#include <CZ/AssetBroker/ABRemoteBackend.h>
#include <CZ/AssetBroker/ABProtocol.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/Ream/RDMABufferInfo.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RSync.h>
#include <CZ/Core/CZCore.h>
#include <CZ/Core/CZTimer.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <poll.h>
#include <cstring>

using namespace CZ;

// Held by the image's Result::keepAlive: releases the service's reference when destroyed
struct ABRemoteBackend::Reference
{
    std::weak_ptr<ABRemoteBackend> backend;
    UInt64 assetId;

    ~Reference() noexcept
    {
        if (auto b { backend.lock() })
            b->queueRelease(assetId);
    }
};

std::shared_ptr<ABRemoteBackend> ABRemoteBackend::Connect() noexcept
{
    if (!CZCore::Get())
    {
        ABLog(CZError, CZLN, "Missing CZCore");
        return {};
    }

    const auto path { ABProtocol::SocketPath() };

    if (path.empty())
    {
        ABLog(CZDebug, CZLN, "Neither CZ_ASSET_BROKER_SOCKET nor XDG_RUNTIME_DIR are set");
        return {};
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;

    if (path.string().size() >= sizeof(addr.sun_path))
    {
        ABLog(CZError, CZLN, "Socket path too long: {}", path.c_str());
        return {};
    }

    strcpy(addr.sun_path, path.c_str());

    const int fd { socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0) };

    if (fd < 0)
        return {};

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ABLog(CZDebug, CZLN, "Service not available at {}: {}", path.c_str(), strerror(errno));
        close(fd);
        return {};
    }

    auto backend { std::shared_ptr<ABRemoteBackend>(new ABRemoteBackend()) };
    std::weak_ptr<ABRemoteBackend> weak { backend };

    // Only to notice the service going away between requests (replies are read synchronously)
    backend->m_source = CZEventSource::Make(fd, EPOLLRDHUP, CZOwn::Own,
        [weak](int, UInt32 events, CZEventSource *)
        {
            if (auto b { weak.lock() }; b && (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)))
            {
                ABLog(CZWarning, CZLN, "Lost connection to the service");
                b->disconnect();
            }
        });

    if (!backend->m_source || !backend->handshake())
        return {};

    ABLog(CZDebug, CZLN, "Connected to the service at {}", path.c_str());
    return backend;
}

ABRemoteBackend::~ABRemoteBackend() noexcept
{
    flushReleases();
    disconnect();
}

bool ABRemoteBackend::handshake() noexcept
{
    const ABProtocol::Hello hello {};

    if (!ABProtocol::Send(fd(), &hello, sizeof(hello)))
    {
        disconnect();
        return false;
    }

    ABProtocol::HelloReply reply {};
    std::vector<CZSpFd> fds;

    if (!waitReply(&reply, sizeof(reply), fds) || reply.op != ABProtocol::Op::HelloReply)
    {
        disconnect();
        return false;
    }

    if (reply.version != ABProtocol::Version)
    {
        ABLog(CZWarning, CZLN, "Protocol version mismatch (service: {}, library: {})", reply.version, ABProtocol::Version);
        disconnect();
        return false;
    }

    return true;
}

bool ABRemoteBackend::waitReply(void *data, size_t size, std::vector<CZSpFd> &fds) noexcept
{
    static const int timeoutMs { static_cast<int>(ABUtils::EnvInt("CZ_ASSET_BROKER_TIMEOUT_MS", 2000)) };

    if (!connected())
        return false;

    pollfd pfd { fd(), POLLIN, 0 };
    int ret;

    do { ret = poll(&pfd, 1, timeoutMs); }
    while (ret < 0 && errno == EINTR);

    if (ret <= 0 || !(pfd.revents & POLLIN))
    {
        if (ret == 0)
            ABLog(CZWarning, CZLN, "The service didn't reply in time");
        else
            ABLog(CZWarning, CZLN, "Lost connection to the service");
        disconnect();
        return false;
    }

    // Exactly one message of this size is expected
    alignas(UInt64) char buffer[ABProtocol::MaxMessageSize];
    const ssize_t received { ABProtocol::Recv(fd(), buffer, sizeof(buffer), fds) };

    if (received != static_cast<ssize_t>(size))
    {
        ABLog(CZWarning, CZLN, "Invalid reply from the service");
        disconnect();
        return false;
    }

    memcpy(data, buffer, size);
    return true;
}

ABBackend::Result ABRemoteBackend::load(const std::filesystem::path &path, SkISize box, RDevice *device) noexcept
{
    if (!connected() || !device)
        return {};

    const std::string &pathStr { path.native() };

    if (pathStr.empty() || pathStr.size() > ABProtocol::MaxPathSize)
        return {};

    // Pending releases first, so the service can reuse memory
    flushReleases();

    ABProtocol::Load msg {};
    msg.pathSize = pathStr.size();
    msg.serial = ++m_serial;
    msg.device = device->id();
    msg.boxWidth = box.width();
    msg.boxHeight = box.height();

    alignas(UInt64) char buffer[sizeof(ABProtocol::Load) + ABProtocol::MaxPathSize];
    memcpy(buffer, &msg, sizeof(msg));
    memcpy(buffer + sizeof(msg), pathStr.data(), pathStr.size());
    return request(buffer, sizeof(msg) + pathStr.size(), msg.serial, device, path.native());
}

ABBackend::Result ABRemoteBackend::loadIcon(const ABIconKey &key, SkISize box, RDevice *device) noexcept
{
    if (!connected() || !device || (key.names.empty() && key.appId.empty()))
        return {};

    ABProtocol::LoadIcon msg {};
    std::string payload { key.appId };
    payload.push_back('\0');

    for (const auto &name : key.names)
        payload.append(name).push_back('\0');

    for (const auto &theme : key.themes)
        payload.append(theme).push_back('\0');

    if (payload.size() > ABProtocol::MaxIconPayloadSize)
    {
        ABLog(CZWarning, CZLN, "Icon request too long");
        return {};
    }

    flushReleases();

    msg.payloadSize = payload.size();
    msg.serial = ++m_serial;
    msg.device = device->id();
    msg.boxWidth = box.width();
    msg.boxHeight = box.height();
    msg.size = key.size;
    msg.scale = key.scale;
    msg.extensions = key.extensions;
    msg.contexts = key.contexts;
    msg.nameCount = key.names.size();
    msg.themeCount = key.themes.size();

    alignas(UInt64) char buffer[ABProtocol::MaxMessageSize];
    memcpy(buffer, &msg, sizeof(msg));
    memcpy(buffer + sizeof(msg), payload.data(), payload.size());
    return request(buffer, sizeof(msg) + payload.size(), msg.serial, device, key.appId.empty() ? key.names.front() : key.appId);
}

ABBackend::Result ABRemoteBackend::generate(const ABGenKey &key, RDevice *device) noexcept
{
    if (!connected() || !device || key.generator.empty())
        return {};

    std::string payload { key.generator };
    payload.push_back('\0');

    for (const auto &value : key.strings)
        payload.append(value).push_back('\0');

    const size_t intsSize { key.ints.size() * sizeof(Int64) };

    if (payload.size() + intsSize > ABProtocol::MaxGeneratePayloadSize)
    {
        ABLog(CZWarning, CZLN, "Generate request too long");
        return {};
    }

    flushReleases();

    ABProtocol::Generate msg {};
    msg.payloadSize = payload.size() + intsSize;
    msg.serial = ++m_serial;
    msg.device = device->id();
    msg.intCount = key.ints.size();
    msg.stringCount = key.strings.size();

    alignas(UInt64) char buffer[ABProtocol::MaxMessageSize];
    memcpy(buffer, &msg, sizeof(msg));
    memcpy(buffer + sizeof(msg), payload.data(), payload.size());
    memcpy(buffer + sizeof(msg) + payload.size(), key.ints.data(), intsSize);
    return request(buffer, sizeof(msg) + msg.payloadSize, msg.serial, device, key.generator);
}

ABBackend::Result ABRemoteBackend::request(const void *message, size_t size, UInt64 serial, RDevice *device, const std::string &what) noexcept
{
    m_lastStatus = ABProtocol::Status::Failed;

    if (!ABProtocol::Send(fd(), message, size))
    {
        disconnect();
        return {};
    }

    ABProtocol::LoadReply reply {};
    std::vector<CZSpFd> fds;

    if (!waitReply(&reply, sizeof(reply), fds))
        return {};

    if (reply.op != ABProtocol::Op::LoadReply || reply.serial != serial)
    {
        ABLog(CZWarning, CZLN, "Unexpected reply from the service");
        disconnect();
        return {};
    }

    m_lastStatus = reply.status;

    if (reply.status != ABProtocol::Status::Ok)
    {
        ABLog(CZDebug, CZLN, "The service couldn't load {} (status {})", what, static_cast<Int32>(reply.status));
        return {};
    }

    // From here on the service holds a reference: released by Reference (also if the import fails)
    auto reference { std::make_shared<Reference>() };
    reference->backend = weak_from_this();
    reference->assetId = reply.assetId;

    const size_t expectedFds { static_cast<size_t>(reply.planeCount) + (reply.hasFence ? 1 : 0) };

    if (reply.planeCount < 1 || reply.planeCount > 4 || fds.size() != expectedFds)
    {
        ABLog(CZWarning, CZLN, "Invalid DMA-buf from the service");
        return {};
    }

    RDMABufferInfo info {};
    info.width = reply.width;
    info.height = reply.height;
    info.format = reply.format;
    info.modifier = reply.modifier;
    info.planeCount = reply.planeCount;

    for (int i = 0; i < reply.planeCount; i++)
    {
        info.offset[i] = reply.offset[i];
        info.stride[i] = reply.stride[i];
        info.fd[i] = fds[i].release();
    }

    RImageConstraints constraints {};
    constraints.allocator = device;

    // Takes ownership of the fds: closed once imported (kept as GEM handles)
    auto image { RImage::FromDMA(info, CZOwn::Own, &constraints) };

    if (!image)
    {
        ABLog(CZWarning, CZLN, "Failed to import the DMA-buf of {}", what);
        return {};
    }

    // Don't read before the service's upload finishes
    if (reply.hasFence)
        if (auto sync { RSync::FromExternal(fds[reply.planeCount].release(), device) })
            image->setWriteSync(sync);

    Result result;
    result.image = image;
    result.keepAlive = reference;
    return result;
}

void ABRemoteBackend::disconnect() noexcept
{
    if (!m_source)
        return;

    // The event source closes the fd later (the CZCore may still reference it): tell the service now
    shutdown(m_source->fd(), SHUT_RDWR);
    m_source.reset();
    m_pendingReleases.clear();
}

void ABRemoteBackend::queueRelease(UInt64 assetId) noexcept
{
    if (!connected())
        return;

    m_pendingReleases.emplace_back(assetId);

    if (m_flushScheduled)
        return;

    m_flushScheduled = true;
    std::weak_ptr<ABRemoteBackend> weak { weak_from_this() };

    CZTimer::OneShot(0, [weak](CZTimer *)
    {
        if (auto b { weak.lock() })
        {
            b->m_flushScheduled = false;
            b->flushReleases();
        }
    });
}

void ABRemoteBackend::flushReleases() noexcept
{
    while (connected() && !m_pendingReleases.empty())
    {
        const size_t count { std::min(m_pendingReleases.size(), ABProtocol::MaxReleaseIds) };

        ABProtocol::Release header {};
        header.count = count;

        alignas(UInt64) char buffer[ABProtocol::MaxMessageSize];
        memcpy(buffer, &header, sizeof(header));
        memcpy(buffer + sizeof(header), m_pendingReleases.data(), count * sizeof(UInt64));

        if (!ABProtocol::Send(fd(), buffer, sizeof(header) + count * sizeof(UInt64)))
        {
            disconnect();
            return;
        }

        m_pendingReleases.erase(m_pendingReleases.begin(), m_pendingReleases.begin() + count);
    }
}
