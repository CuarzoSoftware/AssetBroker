#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RImage.h>
#include <CZ/Ream/GBM/RGBMBo.h>
#include <CZ/Core/CZBitset.h>
#include <unordered_map>
#include <vector>
#include <CZ/Ream/DRM/RDRMPlatformHandle.h>
#include <CZ/XDG/XDGKit.h>
#include <CZ/XDG/XDGIconThemeManager.h>
#include <CZ/XDG/XDGDesktopEntry.h>
#include <format>
#include <unistd.h>
#include <cstring>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <chrono>
#include <cstdlib>
#include <cerrno>

using namespace CZ;

dev_t ABUtils::RenderNodeId(dev_t dev) noexcept
{
    drmDevicePtr device { nullptr };

    if (drmGetDeviceFromDevId(dev, 0, &device) != 0 || !device)
        return dev;

    dev_t id { dev };

    if ((device->available_nodes & (1 << DRM_NODE_RENDER)) && device->nodes[DRM_NODE_RENDER])
    {
        struct stat st;
        if (stat(device->nodes[DRM_NODE_RENDER], &st) == 0)
            id = st.st_rdev;
    }

    drmFreeDevice(&device);
    return id;
}

RDevice *ABUtils::FindDevice(dev_t dev) noexcept
{
    auto core { RCore::Get() };

    if (!core)
        return nullptr;

    const dev_t wanted { RenderNodeId(dev) };

    for (auto *device : core->devices())
        if (device->id() != 0 && RenderNodeId(device->id()) == wanted)
            return device;

    return nullptr;
}

std::optional<RDRMFormat> ABUtils::IconFormat(RDevice *device) noexcept
{
    if (!device)
        return {};

    const auto &formats { device->textureFormats().formats() };

    for (const RFormat fourcc : { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888 })
    {
        auto it { formats.find(fourcc) };

        if (it != formats.end())
            return *it;
    }

    return {};
}

// Whether an image of @p format on @p device goes all the way to a client: its modifier if so
static std::optional<RModifier> TryShareable(RDevice *device, const RDRMFormat &format) noexcept
{
    static constexpr Int32 Size { 8 };
    UInt32 pixels[Size * Size];

    for (UInt32 &pixel : pixels)
        pixel = 0xFF336699;

    RImageConstraints constraints {};
    constraints.allocator = device;
    constraints.caps[device] = RImageCap_GBMBo;

    RPixelBufferInfo info {};
    info.size = SkISize::Make(Size, Size);
    info.stride = Size * sizeof(UInt32);
    info.format = format.format();
    info.pixels = reinterpret_cast<UInt8*>(pixels);

    // Allocated and written
    auto image { RImage::MakeFromPixels(info, format, &constraints) };
    auto bo { image ? image->gbmBo(device) : nullptr };

    if (!bo)
        return {};

    // Exported
    auto dma { bo->dmaExport() };

    if (!dma)
        return {};

    // Imported, as a client would
    RImageConstraints importConstraints {};
    importConstraints.allocator = device;

    if (!RImage::FromDMA(*dma, CZOwn::Own, &importConstraints))
        return {};

    return bo->modifier();
}

std::optional<RDRMFormat> ABUtils::ShareableFormat(RDevice *device) noexcept
{
    if (!device)
        return {};

    // Devices live as long as the RCore (the service's lifetime)
    static std::unordered_map<RDevice*, std::optional<RDRMFormat>> cache;

    if (auto it { cache.find(device) }; it != cache.end())
        return it->second;

    std::optional<RDRMFormat> chosen;
    const auto &texture { device->textureFormats().formats() };
    const auto &render { device->renderFormats().formats() };

    for (const RFormat fourcc : { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888 })
    {
        const auto tex { texture.find(fourcc) };

        if (tex == texture.end())
            continue;

        // Textured from and rendered to (a client draws them, the service may have to draw into
        // them), then anything textured from
        std::vector<RDRMFormat> candidates;

        if (const auto ren { render.find(fourcc) }; ren != render.end())
        {
            RDRMFormat both { fourcc, {} };

            for (const RModifier modifier : tex->modifiers())
                if (ren->modifiers().contains(modifier))
                    both.addModifier(modifier);

            if (!both.modifiers().empty())
                candidates.emplace_back(std::move(both));
        }

        candidates.emplace_back(*tex);

        for (const RDRMFormat &candidate : candidates)
        {
            if (const auto modifier { TryShareable(device, candidate) })
            {
                chosen = RDRMFormat(fourcc, { *modifier });
                break;
            }
        }

        if (chosen)
            break;
    }

    if (chosen)
        ABLog(CZInfo, CZLN, "Assets of {} ({}) are shared as {} with modifier {}", device->drmNode(), device->drmDriverName(),
              RDRMFormat::FormatName(chosen->format()), RDRMFormat::ModifierName(*chosen->modifiers().begin()));
    else
        ABLog(CZError, CZLN, "No format of {} ({}) can be allocated, written, exported and imported: its assets can't be shared",
              device->drmNode(), device->drmDriverName());

    cache[device] = chosen;
    return chosen;
}

std::vector<int> ABUtils::OpenRenderNodes() noexcept
{
    std::vector<int> fds;
    drmDevicePtr devices[64];
    const int count { drmGetDevices2(0, devices, 64) };

    if (count < 0)
    {
        ABLog(CZError, CZLN, "drmGetDevices2 failed: {}", strerror(-count));
        return fds;
    }

    for (int i = 0; i < count; i++)
    {
        if ((devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) && devices[i]->nodes[DRM_NODE_RENDER])
        {
            const int fd { open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC) };

            if (fd >= 0)
                fds.emplace_back(fd);
            else
                ABLog(CZWarning, CZLN, "Failed to open {}: {}", devices[i]->nodes[DRM_NODE_RENDER], strerror(errno));
        }
    }

    drmFreeDevices(devices, count);
    return fds;
}

std::shared_ptr<RCore> ABUtils::MakeHeadlessRCore() noexcept
{
    const auto fds { OpenRenderNodes() };

    if (fds.empty())
    {
        ABLog(CZError, CZLN, "No DRM render nodes available");
        return {};
    }

    std::unordered_set<RDRMFdHandle> handles;

    for (int fd : fds)
        handles.insert({ fd, nullptr });

    RCore::Options options {};
    options.platformHandle = RDRMPlatformHandle::Make(handles); // Duplicates the fds

    for (int fd : fds)
        close(fd);

    if (!options.platformHandle)
        return {};

    return RCore::Make(options);
}

std::filesystem::path ABUtils::FindIconFile(XDGKit &kit, const ABIconKey &key) noexcept
{
    std::vector<std::string> extra;

    if (!key.appId.empty())
        if (auto entry { kit.desktopEntry(key.appId) }; entry && !entry->icon.empty())
            extra.emplace_back(entry->icon);

    const std::vector<std::string> *lists[] { &extra, &key.names };

    for (const auto *list : lists)
    for (const auto &name : *list)
    {
        if (name.empty())
            continue;

        if (name.front() == '/')
        {
            std::error_code ec;

            if (std::filesystem::is_regular_file(name, ec))
                return name;

            continue;
        }

        const auto *icon { kit.iconThemeManager().findIcon(name, key.size, key.scale, key.extensions,
                                                           key.themes.empty() ? std::vector<std::string>{ "" } : key.themes,
                                                           key.contexts) };

        if (!icon)
            continue;

        const UInt32 found { icon->extensions() & key.extensions };

        if (found & XDGIcon::SVG)
            return icon->getPath(XDGIcon::SVG);

        if (found & XDGIcon::PNG)
            return icon->getPath(XDGIcon::PNG);

        if (found & XDGIcon::XPM)
            return icon->getPath(XDGIcon::XPM);
    }

    return {};
}

std::string ABUtils::IconKeyString(const ABIconKey &key) noexcept
{
    std::string str { std::format("{}:{}:{}:{}\na:{}", key.size, key.scale, key.extensions, key.contexts, key.appId) };

    for (const auto &theme : key.themes)
        str.append("\nt:").append(theme);

    for (const auto &name : key.names)
        str.append("\nn:").append(name);

    return str;
}

std::string ABUtils::ShadowString(const ABShadow *shadow) noexcept
{
    if (!shadow)
        return {};

    return std::format("s{}:{}:{}:{}:{:08x}", shadow->padding, shadow->radius, shadow->offsetX, shadow->offsetY, shadow->color);
}

std::string ABUtils::GenKeyString(const ABGenKey &key) noexcept
{
    std::string str { key.generator };

    for (const Int64 value : key.ints)
        str.append(std::format("\ni:{}", value));

    for (const auto &value : key.strings)
        str.append("\ns:").append(value);

    return str;
}

UInt64 ABUtils::NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Int64 ABUtils::EnvInt(const char *name, Int64 fallback) noexcept
{
    const char *value { getenv(name) };

    if (!value || !*value)
        return fallback;

    char *end { nullptr };
    const long long result { strtoll(value, &end, 10) };

    if (!end || *end != '\0')
        return fallback;

    return result;
}
