#ifndef CZ_ABUTILS_H
#define CZ_ABUTILS_H

#include <CZ/AssetBroker/ABIconKey.h>
#include <CZ/AssetBroker/ABGenKey.h>
#include <CZ/AssetBroker/ABShadow.h>
#include <CZ/Ream/Ream.h>
#include <CZ/Ream/DRM/RDRMFormat.h>
#include <sys/types.h>
#include <optional>
#include <memory>
#include <vector>
#include <filesystem>

namespace CZ { class XDGKit; }

/**
 * @brief Helpers shared by the library and the service.
 *
 * @ingroup AssetBroker
 */
namespace CZ::ABUtils
{
    /**
     * @brief Device id of the render node of the GPU with device id @p dev.
     *
     * The same GPU can be reported by its primary node (/dev/dri/cardN) or its render node
     * (/dev/dri/renderDN). Comparing the ids returned by this function tells whether two
     * device ids refer to the same GPU. Returns @p dev itself if it has no render node.
     */
    dev_t RenderNodeId(dev_t dev) noexcept;

    /// The device of the current RCore whose render node is that of @p dev, nullptr if none.
    RDevice *FindDevice(dev_t dev) noexcept;

    /// The format icons are stored in on @p device (ABGR8888 or ARGB8888 with the device's modifiers).
    std::optional<RDRMFormat> IconFormat(RDevice *device) noexcept;

    /**
     * @brief The format (and the one modifier) the service stores assets in on @p device, so every
     *        client rendering with that device can use them: chosen by the service, not by clients.
     *
     * Found once per device by trying it for real, since what a driver lists isn't always what it
     * can do (e.g. NVIDIA's GBM buffers can't be mapped, and some of the modifiers it textures from
     * can't be rendered to). A small image must be:
     * - allocated as a GBM buffer on the device,
     * - written with pixels (mapped, or drawn into where buffers can't be mapped),
     * - exported as a DMA-buf, and
     * - imported back on the device (what clients do with it).
     *
     * ABGR8888 then ARGB8888, with the modifiers the device both textures from and renders to
     * first, then every one it textures from. The modifier the buffer got is the one kept.
     *
     * @return Nothing if no format passes (logged): the device can't share assets.
     */
    std::optional<RDRMFormat> ShareableFormat(RDevice *device) noexcept;

    /**
     * @brief Opens the render node of every GPU (O_RDWR | O_CLOEXEC).
     *
     * For headless processes (the service, tests) that create an RCore with an RDRMPlatformHandle:
     * render nodes don't need DRM master and are enough to allocate GBM buffers.
     */
    std::vector<int> OpenRenderNodes() noexcept;

    /**
     * @brief Creates an RCore for a headless process, with every GPU's render node.
     *
     * Uses the DRM platform (GL unless CZ_REAM_GAPI says otherwise) without DRM master, so
     * no scanout, but GBM buffers can be allocated and shared. nullptr on failure.
     */
    std::shared_ptr<RCore> MakeHeadlessRCore() noexcept;

    /**
     * @brief The file of the first icon of @p key found (see XDGIconThemeManager::findIcon()).
     *
     * The Icon of the .desktop entry of key.appId (if any) is tried first. Names that are absolute
     * paths are used as is if the file exists. When an icon has both,
     * the SVG is preferred (if accepted by @p key's extensions). Empty if none is found.
     */
    std::filesystem::path FindIconFile(XDGKit &kit, const ABIconKey &key) noexcept;

    /// A string identifying @p key (for cache keys).
    std::string IconKeyString(const ABIconKey &key) noexcept;

    /// A string identifying @p shadow (for cache keys), empty for nullptr.
    std::string ShadowString(const ABShadow *shadow) noexcept;

    /// A string identifying @p key (for cache keys).
    std::string GenKeyString(const ABGenKey &key) noexcept;

    /// Steady clock in milliseconds.
    UInt64 NowMs() noexcept;

    /// Reads an integer environment variable, @p fallback if unset or invalid.
    Int64 EnvInt(const char *name, Int64 fallback) noexcept;
};

#endif // CZ_ABUTILS_H
