#ifndef CZ_ABCORE_H
#define CZ_ABCORE_H

#include <CZ/AssetBroker/ABIconKey.h>
#include <CZ/AssetBroker/ABGenKey.h>
#include <CZ/AssetBroker/ABShadow.h>
#include <CZ/Core/CZTimer.h>
#include <CZ/Ream/Ream.h>
#include <CZ/skia/core/SkSize.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace CZ { class XDGKit; }

/**
 * @brief Entry point of the library: loads icons and images, in-process or through the service.
 *
 * ABCore is a singleton, created with GetOrMake() after the CZCore (the connection to the service
 * and the cache use its event loop) and the RCore (images are Ream images).
 *
 * In remote mode icons are looked up and rasterized by the service: the app doesn't load icon
 * themes (no XDGKit) nor parse SVGs, and images are imported from DMA-bufs shared with every
 * other app. In local mode the same is done in-process.
 *
 * Every image is for a DRM device (Ream's main device by default) and shared: while some copy of
 * it is alive, requesting it again returns the same image. Once the last copy is destroyed it's
 * kept for a grace period (see setGracePeriod()), so momentarily dropped images (e.g. rebuilt
 * menus) are not generated again. After that it's released (in remote mode, the service's
 * reference is dropped; the service frees it once no app uses it).
 *
 * Not thread-safe: use it from the thread of the CZCore.
 *
 * @ingroup AssetBroker
 */
class CZ::ABCore final
{
public:
    /// Same signature as AKImageFrame::ImageProvider: the image for a pixel size and screen scale.
    using ImageProvider = std::function<std::shared_ptr<RImage>(Int32 pixelSize, Int32 scale)>;

    /**
     * @brief Returns the instance, creating it with @p mode if it doesn't exist.
     *
     * Requires a CZCore and an RCore. With ABMode::Remote, fails if the service isn't reachable
     * or the RCore can't import DMA-bufs (the Raster API).
     *
     * @param mode Ignored if the instance already exists (see mode()). Overridden by
     *             $CZ_ASSET_BROKER_MODE (auto, local or remote) if set.
     * @return nullptr on failure.
     */
    static std::shared_ptr<ABCore> GetOrMake(ABMode mode = ABMode::Auto) noexcept;

    /// The instance, nullptr if not created yet (or destroyed).
    static std::shared_ptr<ABCore> Get() noexcept;

    ~ABCore() noexcept;

    /// The mode it was created with.
    ABMode mode() const noexcept { return m_mode; }

    /**
     * @brief Whether images currently come from the service.
     *
     * In ABMode::Auto this changes over time: if the service goes away images are generated
     * locally, and reconnecting is retried (at most every 2 s) on later requests.
     */
    bool remote() const noexcept;

    /**
     * @brief Looks up an icon (see ABIconKey) and rasterizes it.
     *
     * @param box    Fits the icon in this box (keeping its aspect ratio), in pixels. A 0 width or
     *               height leaves that dimension unconstrained. {0, 0}: a square of
     *               key.size * key.scale pixels.
     * @param device DRM device the image is for. nullptr: Ream's main device.
     * @param shadow Draws the icon over a drop shadow (see ABShadow): the image is still @p box,
     *               the icon fitted in it minus the shadow's padding. nullptr: no shadow.
     * @return nullptr if no icon is found or it can't be loaded.
     */
    std::shared_ptr<RImage> icon(const ABIconKey &key, SkISize box = {0, 0}, RDevice *device = nullptr,
                                 const ABShadow *shadow = nullptr) noexcept;

    /**
     * @brief For AKImageFrame::setImageProvider(): @p key at the size it's drawn at.
     *
     * For each pixel size and scale, looks up the icon with size = ceil(pixelSize / scale) and that
     * scale (the theme may have artwork made for it) and fits it in a pixelSize square.
     *
     * With a @p shadow, the image is the pixelSize square with the icon and its shadow: the icon is
     * smaller, the shadow's padding is ABShadow::PaddingFor(pixelSize, shadow->radius) (the
     * @ref ABShadow::padding given is ignored). The frame should then extend past the icon's area
     * by @ref ABShadow::radius times the icon size on each side.
     */
    static ImageProvider IconProvider(const ABIconKey &key, RDevice *device = nullptr,
                                      std::optional<ABShadow> shadow = std::nullopt) noexcept;

    /**
     * @brief Draws a generated asset (see ABGenerators), e.g. Kay's theme assets.
     *
     * In ABMode::Auto, generators the service doesn't have (e.g. a service from an older version)
     * are drawn in-process.
     *
     * @param device DRM device the image is for. nullptr: Ream's main device.
     * @return nullptr if the generator is unknown or its parameters are invalid.
     */
    std::shared_ptr<RImage> generate(const ABGenKey &key, RDevice *device = nullptr) noexcept;

    /**
     * @brief Loads an image file (SVG or any raster format Skia decodes).
     *
     * @param box    Fits the image in this box (keeping its aspect ratio), in pixels. A 0 width
     *               or height leaves that dimension unconstrained; {0, 0} uses the file's size.
     * @param device DRM device the image is for. nullptr: Ream's main device.
     * @param shadow Draws the image over a drop shadow (see ABShadow, icon()). nullptr: none.
     * @return nullptr on failure.
     */
    std::shared_ptr<RImage> image(const std::filesystem::path &path, SkISize box = {0, 0}, RDevice *device = nullptr,
                                  const ABShadow *shadow = nullptr) noexcept;

    /**
     * @brief The XDGKit used for local icon lookups (created on first use if none was set).
     *
     * Not used while remote() (the service looks icons up).
     */
    std::shared_ptr<XDGKit> xdgKit() noexcept;

    /// Shares the app's XDGKit for local lookups.
    void setXDGKit(std::shared_ptr<XDGKit> kit) noexcept { m_xdgKit = kit; }

    /// How long released images are kept (ms). Initially $CZ_ASSET_BROKER_GRACE_MS or 3000.
    UInt32 gracePeriod() const noexcept { return m_gracePeriodMs; }
    void setGracePeriod(UInt32 ms) noexcept { m_gracePeriodMs = ms; }

    /// Number of images cached (alive or within the grace period).
    size_t cachedImages() const noexcept { return m_images.size(); }

private:
    struct Entry
    {
        std::weak_ptr<RImage> handed;       // What users hold (released → onReleased())
        std::shared_ptr<RImage> backing;    // The actual image, also kept during the grace period
        std::shared_ptr<void> keepAlive;    // SVG document / service reference
        UInt64 idleSince {};                // When the last handed copy was released (0: in use)
    };

    ABCore(ABMode mode) noexcept;
    bool canImportDMA() const noexcept;
    bool tryConnect() noexcept;
    bool shouldFallBack() const noexcept;
    std::shared_ptr<RImage> cached(const std::string &key) noexcept;
    std::shared_ptr<RImage> store(const std::string &key, std::shared_ptr<RImage> image, std::shared_ptr<void> keepAlive) noexcept;
    std::shared_ptr<RImage> handOut(const std::string &key, Entry &entry) noexcept;
    void onReleased(const std::string &key) noexcept;
    void prune() noexcept;

    ABMode m_mode;
    std::shared_ptr<ABLocalBackend> m_local;
    std::shared_ptr<ABRemoteBackend> m_remote;
    UInt64 m_lastConnectAttempt {};
    std::shared_ptr<XDGKit> m_xdgKit;
    std::unordered_map<std::string, Entry> m_images;
    std::unique_ptr<CZTimer> m_pruneTimer;
    UInt32 m_gracePeriodMs;
};

#endif // CZ_ABCORE_H
