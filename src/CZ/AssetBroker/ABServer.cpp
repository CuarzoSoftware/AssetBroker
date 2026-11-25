#include <CZ/AssetBroker/ABServer.h>
#include <CZ/AssetBroker/ABProtocol.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/AssetBroker/ABGenerators.h>
#include <CZ/Ream/GBM/RGBMBo.h>
#include <CZ/Ream/RDMABufferInfo.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RSync.h>
#include <CZ/Core/CZCore.h>
#include <CZ/XDG/XDGKit.h>
#include <CZ/XDG/XDGIconThemeManager.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstring>
#include <format>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>

using namespace CZ;

static constexpr std::uintmax_t MaxFileSize { 64 * 1024 * 1024 };

struct ABServer::Client
{
    UInt64 id;
    pid_t pid;
    std::shared_ptr<CZEventSource> source; // Owns the socket
    std::unordered_map<UInt64, UInt32> refs; // Asset id → references
};

struct ABServer::Asset
{
    UInt64 id;
    std::string key;
    RDevice *device;
    std::shared_ptr<RImage> image;
    std::shared_ptr<void> keepAlive;
    UInt64 refs {};
    UInt64 idleSince {};
};

std::shared_ptr<ABServer> ABServer::Make(const std::filesystem::path &socketPath) noexcept
{
    if (!CZCore::Get())
    {
        ABLog(CZError, CZLN, "Missing CZCore");
        return {};
    }

    if (!RCore::Get())
    {
        ABLog(CZError, CZLN, "Missing RCore");
        return {};
    }

    auto server { std::shared_ptr<ABServer>(new ABServer()) };
    server->m_gracePeriodMs = std::max<Int64>(0, ABUtils::EnvInt("CZ_ASSET_BROKER_SERVER_GRACE_MS", 10000));
    server->m_xdgKit = XDGKit::Make();

    if (!server->m_xdgKit)
    {
        ABLog(CZError, CZLN, "Failed to create the XDGKit");
        return {};
    }

    server->m_pruneTimer = std::make_unique<CZTimer>([s = server.get()](CZTimer *){ s->prune(); });

    if (!server->listen(socketPath.empty() ? ABProtocol::SocketPath() : socketPath))
        return {};

    return server;
}

ABServer::~ABServer() noexcept
{
    m_pruneTimer.reset();

    for (auto &[id, client] : m_clients)
        shutdown(client->source->fd(), SHUT_RDWR);

    m_clients.clear();
    m_assetsById.clear();
    m_assets.clear();

    if (m_listener)
    {
        m_listener.reset();

        // A socket-activated socket belongs to systemd
        if (!m_activated)
            unlink(m_socketPath.c_str());
    }
}

int ABServer::ActivationSocket() noexcept
{
    // The sd_listen_fds() protocol: the first passed fd is 3
    const char *pid { getenv("LISTEN_PID") };
    const char *count { getenv("LISTEN_FDS") };

    if (!pid || !count || strtol(pid, nullptr, 10) != getpid() || strtol(count, nullptr, 10) < 1)
        return -1;

    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");

    constexpr int fd { 3 };
    int type { 0 };
    socklen_t size { sizeof(type) };

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET)
    {
        ABLog(CZError, CZLN, "The socket passed by systemd isn't a SOCK_SEQPACKET socket (use ListenSequentialPacket=)");
        return -1;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

bool ABServer::listen(const std::filesystem::path &path) noexcept
{
    if (const int fd { ActivationSocket() }; fd >= 0)
    {
        sockaddr_un addr {};
        socklen_t size { sizeof(addr) };

        if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size) == 0 && addr.sun_path[0])
            m_socketPath = addr.sun_path;

        m_activated = true;
        m_listener = CZEventSource::Make(fd, EPOLLIN, CZOwn::Own, [this](int, UInt32, CZEventSource *){ accept(); });

        if (!m_listener)
            return false;

        ABLog(CZInfo, CZLN, "Listening on {} (socket activated)", m_socketPath.c_str());
        return true;
    }

    if (path.empty())
    {
        ABLog(CZError, CZLN, "No socket path: set XDG_RUNTIME_DIR or CZ_ASSET_BROKER_SOCKET");
        return false;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;

    if (path.native().size() >= sizeof(addr.sun_path))
    {
        ABLog(CZError, CZLN, "Socket path too long: {}", path.c_str());
        return false;
    }

    strcpy(addr.sun_path, path.c_str());

    const int fd { socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) };

    if (fd < 0)
    {
        ABLog(CZError, CZLN, "socket() failed: {}", strerror(errno));
        return false;
    }

    // A socket file is left behind if a previous instance crashed: remove it unless it's alive
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        const int probe { socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0) };
        const bool alive { probe >= 0 && connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 };

        if (probe >= 0)
            close(probe);

        if (alive)
        {
            ABLog(CZError, CZLN, "Another instance is already listening on {}", path.c_str());
            close(fd);
            return false;
        }

        unlink(path.c_str());
    }

    // Only this user can connect
    const mode_t oldMask { umask(0077) };
    const int bound { bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) };
    umask(oldMask);

    if (bound != 0 || ::listen(fd, 128) != 0)
    {
        ABLog(CZError, CZLN, "Failed to listen on {}: {}", path.c_str(), strerror(errno));
        close(fd);
        return false;
    }

    m_listener = CZEventSource::Make(fd, EPOLLIN, CZOwn::Own, [this](int, UInt32, CZEventSource *){ accept(); });

    if (!m_listener)
    {
        unlink(path.c_str());
        return false;
    }

    m_socketPath = path;
    ABLog(CZInfo, CZLN, "Listening on {}", path.c_str());
    return true;
}

void ABServer::accept() noexcept
{
    while (true)
    {
        const int fd { accept4(m_listener->fd(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK) };

        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                ABLog(CZWarning, CZLN, "accept4() failed: {}", strerror(errno));

            if (errno == EINTR)
                continue;

            return;
        }

        ucred cred {};
        socklen_t credSize { sizeof(cred) };

        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credSize) != 0 || cred.uid != getuid())
        {
            ABLog(CZWarning, CZLN, "Rejected a client of another user (pid {}, uid {})", cred.pid, cred.uid);
            close(fd);
            continue;
        }

        const UInt64 id { m_nextClientId++ };
        auto client { std::make_unique<Client>() };
        client->id = id;
        client->pid = cred.pid;
        client->source = CZEventSource::Make(fd, EPOLLIN | EPOLLRDHUP, CZOwn::Own,
            [this, id](int, UInt32 events, CZEventSource *){ dispatch(id, events); });

        if (!client->source)
            continue;

        ABLog(CZDebug, CZLN, "Client {} connected (pid {})", id, cred.pid);
        m_clients[id] = std::move(client);
    }
}

void ABServer::dispatch(UInt64 clientId, UInt32 events) noexcept
{
    auto it { m_clients.find(clientId) };

    if (it == m_clients.end())
        return;

    Client &client { *it->second };

    if (events & EPOLLIN)
    {
        alignas(UInt64) char buffer[ABProtocol::MaxMessageSize];
        std::vector<CZSpFd> fds;

        while (true)
        {
            const ssize_t size { ABProtocol::Recv(client.source->fd(), buffer, sizeof(buffer), fds) };

            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;

            // Disconnected, error, or clients sending fds (they never should)
            if (size <= 0 || !fds.empty() || !handleMessage(client, buffer, size))
            {
                removeClient(clientId);
                return;
            }
        }
    }

    if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
        removeClient(clientId);
}

bool ABServer::handleMessage(Client &client, const char *data, size_t size) noexcept
{
    switch (ABProtocol::PeekOp(data, size))
    {
    case ABProtocol::Op::Hello:
    {
        if (size != sizeof(ABProtocol::Hello))
            return false;

        const ABProtocol::HelloReply reply {};
        return ABProtocol::Send(client.source->fd(), &reply, sizeof(reply));
    }
    case ABProtocol::Op::Load:
        return handleLoad(client, data, size);
    case ABProtocol::Op::LoadIcon:
        return handleLoadIcon(client, data, size);
    case ABProtocol::Op::Generate:
        return handleGenerate(client, data, size);
    case ABProtocol::Op::Release:
        return handleRelease(client, data, size);
    default:
        ABLog(CZWarning, CZLN, "Client {} (pid {}) sent an invalid message", client.id, client.pid);
        return false;
    }
}

bool ABServer::handleLoad(Client &client, const char *data, size_t size) noexcept
{
    if (size < sizeof(ABProtocol::Load))
        return false;

    ABProtocol::Load request;
    memcpy(&request, data, sizeof(request));

    if (request.pathSize == 0 || request.pathSize > ABProtocol::MaxPathSize || size != sizeof(request) + request.pathSize)
        return false;

    ABProtocol::LoadReply reply {};
    reply.serial = request.serial;

    const std::filesystem::path path { std::string(data + sizeof(request), request.pathSize) };

    if (!path.is_absolute())
        return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

    return sendAsset(client, reply, path, request.boxWidth, request.boxHeight, request.device, request.shadow);
}

bool ABServer::handleLoadIcon(Client &client, const char *data, size_t size) noexcept
{
    if (size < sizeof(ABProtocol::LoadIcon))
        return false;

    ABProtocol::LoadIcon request;
    memcpy(&request, data, sizeof(request));

    if (request.payloadSize > ABProtocol::MaxIconPayloadSize || size != sizeof(request) + request.payloadSize)
        return false;

    ABProtocol::LoadReply reply {};
    reply.serial = request.serial;

    // Names, then themes, each NUL terminated
    ABIconKey key;
    key.themes.clear();
    key.size = request.size;
    key.scale = request.scale;
    key.extensions = request.extensions;
    key.contexts = request.contexts;

    const char *payload { data + sizeof(request) };
    const char *end { payload + request.payloadSize };

    // App id (possibly empty), names, themes
    for (UInt32 i = 0; i < 1 + request.nameCount + request.themeCount; i++)
    {
        const char *nul { static_cast<const char*>(memchr(payload, '\0', end - payload)) };

        if (!nul)
            return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

        if (i == 0)
            key.appId.assign(payload, nul);
        else
            (i <= request.nameCount ? key.names : key.themes).emplace_back(payload, nul);

        payload = nul + 1;
    }

    if (payload != end || (key.names.empty() && key.appId.empty()) || key.size <= 0 || key.scale <= 0)
        return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

    if (key.themes.empty())
        key.themes = { "" };

    // Notice installed/updated themes (the indexer updates the cache)
    const UInt64 now { ABUtils::NowMs() };

    if (now - m_lastThemesCheck >= 5000)
    {
        m_lastThemesCheck = now;

        if (m_xdgKit->iconThemeManager().reloadThemes(true))
            ABLog(CZInfo, CZLN, "Icon themes reloaded");
    }

    // XDGKit caches .desktop misses: forget them after a while, the app may have been installed since
    if (!key.appId.empty())
    {
        if (auto miss { m_desktopMisses.find(key.appId) }; miss != m_desktopMisses.end() && now - miss->second >= 5000)
        {
            m_xdgKit->forgetDesktopEntry(key.appId);
            m_desktopMisses.erase(miss);
        }
    }

    const auto path { ABUtils::FindIconFile(*m_xdgKit, key) };

    if (!key.appId.empty() && !m_xdgKit->desktopEntry(key.appId))
        m_desktopMisses.try_emplace(key.appId, now);

    if (path.empty())
        return sendFailure(client, reply, ABProtocol::Status::NotFound);

    return sendAsset(client, reply, path, request.boxWidth, request.boxHeight, request.device, request.shadow);
}

bool ABServer::sendFailure(Client &client, ABProtocol::LoadReply &reply, ABProtocol::Status status) noexcept
{
    reply.status = status;
    return ABProtocol::Send(client.source->fd(), &reply, sizeof(reply));
}

bool ABServer::sendAsset(Client &client, ABProtocol::LoadReply &reply, const std::filesystem::path &path, Int32 boxWidth, Int32 boxHeight,
                         UInt64 deviceId, const ABProtocol::Shadow &wireShadow) noexcept
{
    std::optional<ABShadow> shadow;

    if (boxWidth < 0 || boxWidth > ABProtocol::MaxBoxSize || boxHeight < 0 || boxHeight > ABProtocol::MaxBoxSize ||
        !ABProtocol::FromWire(wireShadow, shadow))
        return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

    const ABShadow *shadowPtr { shadow ? &*shadow : nullptr };

    std::error_code ec;
    const auto canonical { std::filesystem::canonical(path, ec) };

    if (ec || !std::filesystem::is_regular_file(canonical, ec) || std::filesystem::file_size(canonical, ec) > MaxFileSize || ec)
        return sendFailure(client, reply, ABProtocol::Status::Failed);

    RDevice *device { ABUtils::FindDevice(deviceId) };

    if (!device || !device->gbmDevice())
        return sendFailure(client, reply, ABProtocol::Status::UnsupportedDevice);

    const std::string key { std::format("{}:{}x{}:{}:{}", device->id(), boxWidth, boxHeight, ABUtils::ShadowString(shadowPtr), canonical.native()) };

    Asset *asset { findOrCreate(key, device, canonical.native(), [&](const RImageConstraints &constraints)
    {
        return m_rasterizer.load(canonical, SkISize::Make(boxWidth, boxHeight), device, &constraints, shadowPtr);
    }) };

    if (!asset)
        return sendFailure(client, reply, ABProtocol::Status::Failed);

    return sendReply(client, reply, *asset);
}

ABServer::Asset *ABServer::findOrCreate(const std::string &key, RDevice *device, const std::string &what,
                                        const std::function<ABRasterizer::Result(const RImageConstraints&)> &create) noexcept
{
    if (auto it { m_assets.find(key) }; it != m_assets.end())
        return it->second.get();

    RImageConstraints constraints {};
    constraints.caps[device] = RImageCap_GBMBo;

    auto result { create(constraints) };

    if (!result.image || !result.image->gbmBo(device))
        return nullptr;

    auto newAsset { std::make_unique<Asset>() };
    newAsset->id = m_nextAssetId++;
    newAsset->key = key;
    newAsset->device = device;
    newAsset->image = std::move(result.image);
    newAsset->keepAlive = std::move(result.keepAlive);
    newAsset->idleSince = ABUtils::NowMs();
    Asset *asset { newAsset.get() };
    m_assetsById[asset->id] = asset;
    m_assets[key] = std::move(newAsset);

    ABLog(CZDebug, CZLN, "Asset {} created: {} ({}x{}, {} assets, {} SVGs)", asset->id, what,
          asset->image->size().width(), asset->image->size().height(), m_assets.size(), m_rasterizer.svgCount());

    // If the client never takes it (the reply fails), the grace period releases it
    if (!m_pruneTimer->running())
        m_pruneTimer->start(m_gracePeriodMs);

    return asset;
}

bool ABServer::handleGenerate(Client &client, const char *data, size_t size) noexcept
{
    if (size < sizeof(ABProtocol::Generate))
        return false;

    ABProtocol::Generate request;
    memcpy(&request, data, sizeof(request));

    if (request.payloadSize > ABProtocol::MaxGeneratePayloadSize || size != sizeof(request) + request.payloadSize)
        return false;

    ABProtocol::LoadReply reply {};
    reply.serial = request.serial;

    // Name, strings (NUL terminated), then the ints
    ABGenKey key;
    const char *payload { data + sizeof(request) };
    const char *end { payload + request.payloadSize };

    for (UInt32 i = 0; i < 1 + request.stringCount; i++)
    {
        const char *nul { static_cast<const char*>(memchr(payload, '\0', end - payload)) };

        if (!nul)
            return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

        if (i == 0)
            key.generator.assign(payload, nul);
        else
            key.strings.emplace_back(payload, nul);

        payload = nul + 1;
    }

    if (static_cast<size_t>(end - payload) != request.intCount * sizeof(Int64))
        return sendFailure(client, reply, ABProtocol::Status::InvalidRequest);

    key.ints.resize(request.intCount);
    memcpy(key.ints.data(), payload, request.intCount * sizeof(Int64));

    if (!ABGenerators::Find(key.generator))
        return sendFailure(client, reply, ABProtocol::Status::UnknownGenerator);

    RDevice *device { ABUtils::FindDevice(request.device) };

    if (!device || !device->gbmDevice())
        return sendFailure(client, reply, ABProtocol::Status::UnsupportedDevice);

    const std::string assetKey { std::format("gen:{}:{}", device->id(), ABUtils::GenKeyString(key)) };

    Asset *asset { findOrCreate(assetKey, device, key.generator, [&](const RImageConstraints &constraints)
    {
        return m_rasterizer.generate(key, device, &constraints);
    }) };

    if (!asset)
        return sendFailure(client, reply, ABProtocol::Status::Failed);

    return sendReply(client, reply, *asset);
}

bool ABServer::sendReply(Client &client, ABProtocol::LoadReply &reply, Asset &assetRef) noexcept
{
    Asset *asset { &assetRef };
    RDevice *device { asset->device };

    auto dma { asset->image->gbmBo(device)->dmaExport() };

    if (!dma)
        return sendFailure(client, reply, ABProtocol::Status::Failed);

    reply.status = ABProtocol::Status::Ok;
    reply.assetId = asset->id;
    reply.width = dma->width;
    reply.height = dma->height;
    reply.format = dma->format;
    reply.modifier = dma->modifier;
    reply.planeCount = dma->planeCount;

    int fds[ABProtocol::MaxFds];
    int fdCount { 0 };

    for (int i = 0; i < dma->planeCount; i++)
    {
        reply.offset[i] = dma->offset[i];
        reply.stride[i] = dma->stride[i];
        fds[fdCount++] = dma->fd[i];
    }

    // The upload may still be running on the GPU: the client waits for this fence before reading
    CZSpFd fence;

    if (auto sync { asset->image->writeSync() })
        fence = sync->fd();

    if (fence.get() >= 0)
    {
        reply.hasFence = 1;
        fds[fdCount++] = fence.get();
    }

    const bool sent { ABProtocol::Send(client.source->fd(), &reply, sizeof(reply), fds, fdCount) };
    dma->closeFds();

    if (!sent)
        return false;

    asset->refs++;
    asset->idleSince = 0;
    client.refs[asset->id]++;
    return true;
}

bool ABServer::handleRelease(Client &client, const char *data, size_t size) noexcept
{
    if (size < sizeof(ABProtocol::Release))
        return false;

    ABProtocol::Release header;
    memcpy(&header, data, sizeof(header));

    if (header.count > ABProtocol::MaxReleaseIds || size != sizeof(header) + header.count * sizeof(UInt64))
        return false;

    for (UInt32 i = 0; i < header.count; i++)
    {
        UInt64 id;
        memcpy(&id, data + sizeof(header) + i * sizeof(UInt64), sizeof(id));

        auto it { client.refs.find(id) };

        if (it == client.refs.end())
        {
            ABLog(CZWarning, CZLN, "Client {} (pid {}) released asset {} it doesn't hold", client.id, client.pid, id);
            return false;
        }

        if (--it->second == 0)
            client.refs.erase(it);

        unref(id, 1);
    }

    return true;
}

void ABServer::removeClient(UInt64 clientId) noexcept
{
    auto it { m_clients.find(clientId) };

    if (it == m_clients.end())
        return;

    // Moved out first: unref() must not see a half-removed client
    auto client { std::move(it->second) };
    m_clients.erase(it);

    for (const auto &[assetId, count] : client->refs)
        unref(assetId, count);

    shutdown(client->source->fd(), SHUT_RDWR);
    ABLog(CZDebug, CZLN, "Client {} disconnected (pid {}), {} clients left", client->id, client->pid, m_clients.size());
}

void ABServer::unref(UInt64 assetId, UInt32 count) noexcept
{
    auto it { m_assetsById.find(assetId) };

    if (it == m_assetsById.end())
        return;

    Asset &asset { *it->second };
    asset.refs -= std::min<UInt64>(asset.refs, count);

    if (asset.refs > 0)
        return;

    asset.idleSince = ABUtils::NowMs();

    if (!m_pruneTimer->running())
        m_pruneTimer->start(m_gracePeriodMs);
}

void ABServer::prune() noexcept
{
    const UInt64 now { ABUtils::NowMs() };
    UInt64 nextExpiration { 0 };

    for (auto it = m_assets.begin(); it != m_assets.end();)
    {
        Asset &asset { *it->second };

        if (asset.refs > 0 || asset.idleSince == 0)
        {
            ++it;
            continue;
        }

        const UInt64 expiration { asset.idleSince + m_gracePeriodMs };

        if (expiration <= now)
        {
            ABLog(CZDebug, CZLN, "Asset {} released", asset.id);
            m_assetsById.erase(asset.id);
            it = m_assets.erase(it);
            continue;
        }

        if (nextExpiration == 0 || expiration < nextExpiration)
            nextExpiration = expiration;

        ++it;
    }

    if (nextExpiration != 0)
        m_pruneTimer->start(nextExpiration - now);

    ABLog(CZDebug, CZLN, "{} assets, {} SVGs parsed, {} clients", m_assets.size(), m_rasterizer.svgCount(), m_clients.size());
}
