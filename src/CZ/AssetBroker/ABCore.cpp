#include <CZ/AssetBroker/ABCore.h>
#include <CZ/AssetBroker/ABLocalBackend.h>
#include <CZ/AssetBroker/ABRemoteBackend.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Core/CZCore.h>
#include <CZ/XDG/XDGKit.h>
#include <format>
#include <cstring>
#include <cstdlib>

using namespace CZ;

static std::weak_ptr<ABCore> s_core;
static constexpr UInt64 ReconnectInterval { 2000 };

std::shared_ptr<ABCore> ABCore::GetOrMake(ABMode mode) noexcept
{
    if (auto core { s_core.lock() })
        return core;

    if (!CZCore::Get())
    {
        ABLog(CZError, CZLN, "Missing CZCore: create it before the ABCore");
        return {};
    }

    if (!RCore::Get())
    {
        ABLog(CZError, CZLN, "Missing RCore: create it before the ABCore");
        return {};
    }

    // Forced from outside (testing, or disabling the service for an app)
    if (const char *env { getenv("CZ_ASSET_BROKER_MODE") }; env && *env)
    {
        if (strcmp(env, "local") == 0)       mode = ABMode::Local;
        else if (strcmp(env, "remote") == 0) mode = ABMode::Remote;
        else if (strcmp(env, "auto") == 0)   mode = ABMode::Auto;
        else ABLog(CZWarning, CZLN, "Invalid CZ_ASSET_BROKER_MODE '{}' (auto, local or remote)", env);
    }

    auto core { std::shared_ptr<ABCore>(new ABCore(mode)) };

    if (mode != ABMode::Local)
    {
        core->tryConnect();

        if (mode == ABMode::Remote && !core->remote())
        {
            ABLog(CZError, CZLN, "ABMode::Remote: the service is not available");
            return {};
        }
    }

    ABLog(CZDebug, CZLN, "Created ({})", core->remote() ? "remote" : "local");
    s_core = core;
    return core;
}

std::shared_ptr<ABCore> ABCore::Get() noexcept
{
    return s_core.lock();
}

ABCore::ABCore(ABMode mode) noexcept :
    m_mode(mode),
    m_local(std::make_shared<ABLocalBackend>()),
    m_gracePeriodMs(std::max<Int64>(0, ABUtils::EnvInt("CZ_ASSET_BROKER_GRACE_MS", 3000)))
{
    m_pruneTimer = std::make_unique<CZTimer>([this](CZTimer *){ prune(); });
}

ABCore::~ABCore() noexcept
{
    m_pruneTimer.reset();
    m_images.clear();
}

bool ABCore::remote() const noexcept
{
    return m_remote && m_remote->connected();
}

bool ABCore::canImportDMA() const noexcept
{
    auto core { RCore::Get() };
    return core && core->graphicsAPI() != RGraphicsAPI::RS;
}

bool ABCore::tryConnect() noexcept
{
    if (remote())
        return true;

    m_remote.reset();

    if (m_mode == ABMode::Local || !canImportDMA())
        return false;

    const UInt64 now { ABUtils::NowMs() };

    if (m_lastConnectAttempt != 0 && now - m_lastConnectAttempt < ReconnectInterval)
        return false;

    m_lastConnectAttempt = now;
    m_remote = ABRemoteBackend::Connect();
    return remote();
}

bool ABCore::shouldFallBack() const noexcept
{
    if (m_mode != ABMode::Auto)
        return false;

    // Not when the service just couldn't find/read it: it would fail here too
    return !remote() ||
           m_remote->lastStatus() == ABProtocol::Status::UnsupportedDevice ||
           m_remote->lastStatus() == ABProtocol::Status::UnknownGenerator ||
           m_remote->lastStatus() == ABProtocol::Status::Ok; // But the import failed
}

std::shared_ptr<RImage> ABCore::icon(const ABIconKey &key, SkISize box, RDevice *device, const ABShadow *shadow) noexcept
{
    auto ream { RCore::Get() };

    if (!ream || (key.names.empty() && key.appId.empty()))
        return {};

    if (!device)
        device = ream->mainDevice();

    if (box.isZero())
        box = SkISize::Make(key.size * key.scale, key.size * key.scale);

    box.fWidth = std::max(0, box.fWidth);
    box.fHeight = std::max(0, box.fHeight);

    tryConnect();

    if (remote())
    {
        const std::string cacheKey { std::format("icon:{}:{}x{}:{}:{}", device->id(), box.width(), box.height(),
                                                 ABUtils::ShadowString(shadow), ABUtils::IconKeyString(key)) };

        if (auto image { cached(cacheKey) })
            return image;

        auto result { m_remote->loadIcon(key, box, device, shadow) };

        if (result.image)
            return store(cacheKey, std::move(result.image), std::move(result.keepAlive));

        if (!shouldFallBack())
            return {};
    }
    else if (m_mode == ABMode::Remote)
        return {};

    // Local: look it up here, then share the image of that file
    auto kit { xdgKit() };

    if (!kit)
        return {};

    const auto path { ABUtils::FindIconFile(*kit, key) };

    if (path.empty())
    {
        ABLog(CZDebug, CZLN, "Icon not found: {}", key.appId.empty() ? key.names.front() : key.appId);
        return {};
    }

    const std::string cacheKey { std::format("{}:{}x{}:{}:{}", device->id(), box.width(), box.height(), ABUtils::ShadowString(shadow), path.native()) };

    if (auto image { cached(cacheKey) })
        return image;

    auto result { m_local->load(path, box, device, shadow) };
    return result.image ? store(cacheKey, std::move(result.image), std::move(result.keepAlive)) : nullptr;
}

ABCore::ImageProvider ABCore::IconProvider(const ABIconKey &key, RDevice *device, std::optional<ABShadow> shadow) noexcept
{
    return [key, device, shadow](Int32 pixelSize, Int32 scale) -> std::shared_ptr<RImage>
    {
        auto core { ABCore::Get() };

        if (!core)
            return {};

        ABIconKey sized { key };
        sized.scale = std::max(1, scale);
        pixelSize = std::max(1, pixelSize);

        if (!shadow)
        {
            sized.size = (pixelSize + sized.scale - 1) / sized.scale;
            return core->icon(sized, SkISize::Make(pixelSize, pixelSize), device);
        }

        // The icon is what's left inside the shadow's padding
        ABShadow padded { *shadow };
        padded.padding = ABShadow::PaddingFor(pixelSize, shadow->radius);
        const Int32 iconPixels { std::max(1, pixelSize - 2 * padded.padding) };
        sized.size = (iconPixels + sized.scale - 1) / sized.scale;
        return core->icon(sized, SkISize::Make(pixelSize, pixelSize), device, &padded);
    };
}

std::shared_ptr<RImage> ABCore::generate(const ABGenKey &key, RDevice *device) noexcept
{
    auto ream { RCore::Get() };

    if (!ream || key.generator.empty())
        return {};

    if (!device)
        device = ream->mainDevice();

    const std::string cacheKey { std::format("gen:{}:{}", device->id(), ABUtils::GenKeyString(key)) };

    if (auto image { cached(cacheKey) })
        return image;

    ABBackend::Result result;
    tryConnect();

    if (remote())
    {
        result = m_remote->generate(key, device);

        if (!result.image && shouldFallBack())
            result = m_local->generate(key, device);
    }
    else if (m_mode != ABMode::Remote)
        result = m_local->generate(key, device);

    return result.image ? store(cacheKey, std::move(result.image), std::move(result.keepAlive)) : nullptr;
}

std::shared_ptr<RImage> ABCore::image(const std::filesystem::path &path, SkISize box, RDevice *device, const ABShadow *shadow) noexcept
{
    auto ream { RCore::Get() };

    if (!ream)
        return {};

    if (!device)
        device = ream->mainDevice();

    // Icon themes are full of symlinks: the same file is cached once
    std::error_code ec;
    const auto canonical { std::filesystem::canonical(path, ec) };

    if (ec)
    {
        ABLog(CZDebug, CZLN, "Can't open {}: {}", path.c_str(), ec.message());
        return {};
    }

    box.fWidth = std::max(0, box.fWidth);
    box.fHeight = std::max(0, box.fHeight);

    const std::string key { std::format("{}:{}x{}:{}:{}", device->id(), box.width(), box.height(), ABUtils::ShadowString(shadow), canonical.native()) };

    if (auto image { cached(key) })
        return image;

    ABBackend::Result result;
    tryConnect();

    if (remote())
    {
        result = m_remote->load(canonical, box, device, shadow);

        if (!result.image && shouldFallBack())
            result = m_local->load(canonical, box, device, shadow);
    }
    else if (m_mode != ABMode::Remote)
        result = m_local->load(canonical, box, device, shadow);

    return result.image ? store(key, std::move(result.image), std::move(result.keepAlive)) : nullptr;
}

std::shared_ptr<XDGKit> ABCore::xdgKit() noexcept
{
    if (!m_xdgKit)
        m_xdgKit = XDGKit::Make();

    return m_xdgKit;
}

std::shared_ptr<RImage> ABCore::cached(const std::string &key) noexcept
{
    auto it { m_images.find(key) };

    if (it == m_images.end())
        return {};

    if (auto handed { it->second.handed.lock() })
        return handed;

    return handOut(key, it->second); // Revived within the grace period
}

std::shared_ptr<RImage> ABCore::store(const std::string &key, std::shared_ptr<RImage> image, std::shared_ptr<void> keepAlive) noexcept
{
    Entry &entry { m_images[key] };
    entry.backing = std::move(image);
    entry.keepAlive = std::move(keepAlive);
    return handOut(key, entry);
}

std::shared_ptr<RImage> ABCore::handOut(const std::string &key, Entry &entry) noexcept
{
    entry.idleSince = 0;

    // Same image, but its own control block: when every copy is gone, the deleter tells us
    // (the image itself is kept alive by the entry, or by the deleter if the ABCore is gone)
    auto handed { std::shared_ptr<RImage>(entry.backing.get(), [backing = entry.backing, key](RImage *) mutable
    {
        if (auto core { s_core.lock() })
            core->onReleased(key);
    }) };

    entry.handed = handed;
    return handed;
}

void ABCore::onReleased(const std::string &key) noexcept
{
    auto it { m_images.find(key) };

    if (it == m_images.end() || !it->second.handed.expired())
        return;

    if (m_gracePeriodMs == 0)
    {
        m_images.erase(it);
        return;
    }

    it->second.idleSince = ABUtils::NowMs();

    if (!m_pruneTimer->running())
        m_pruneTimer->start(m_gracePeriodMs);
}

void ABCore::prune() noexcept
{
    const UInt64 now { ABUtils::NowMs() };
    UInt64 nextExpiration { 0 };

    for (auto it = m_images.begin(); it != m_images.end();)
    {
        const Entry &entry { it->second };

        if (entry.idleSince == 0 || !entry.handed.expired())
        {
            ++it;
            continue;
        }

        const UInt64 expiration { entry.idleSince + m_gracePeriodMs };

        if (expiration <= now)
        {
            it = m_images.erase(it);
            continue;
        }

        if (nextExpiration == 0 || expiration < nextExpiration)
            nextExpiration = expiration;

        ++it;
    }

    if (nextExpiration != 0)
        m_pruneTimer->start(nextExpiration - now);
}
