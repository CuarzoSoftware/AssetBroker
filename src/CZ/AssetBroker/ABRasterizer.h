#ifndef CZ_ABRASTERIZER_H
#define CZ_ABRASTERIZER_H

#include <CZ/AssetBroker/ABGenKey.h>
#include <CZ/Ream/Ream.h>
#include <CZ/Ream/RImage.h>
#include <CZ/Core/CZBitset.h>
#include <CZ/skia/core/SkSize.h>
#include <CZ/skia/core/SkRefCnt.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

class SkSVGDOM;

/**
 * @brief Turns image files (SVG and the raster formats Skia decodes) into RImages.
 *
 * Parsed SVG documents are kept while any image rendered from them is alive (see Result::keepAlive),
 * so rendering the same SVG at another size skips reading and parsing it again.
 *
 * Used by ABLocalBackend and by the service (ABServer).
 *
 * @ingroup AssetBroker
 */
class CZ::ABRasterizer
{
public:
    struct Result
    {
        std::shared_ptr<RImage> image;

        /// Keeps the parsed source (SVG document) alive. Hold it as long as the image.
        std::shared_ptr<void> keepAlive;
    };

    /**
     * @brief Rasterizes @p path fitted in @p box (keeping its aspect ratio) for @p device.
     *
     * @param box         Box the image is fitted in, in pixels. A 0 width or height leaves that
     *                    dimension unconstrained; {0, 0} uses the file's own size.
     * @param device      Device the image is allocated on (nullptr: Ream's main device).
     * @param constraints Extra allocation constraints (the allocator is always @p device).
     */
    Result load(const std::filesystem::path &path, SkISize box, RDevice *device,
                const RImageConstraints *constraints = nullptr) noexcept;

    /**
     * @brief Draws a generated asset (see ABGenerators) for @p device.
     *
     * @return Empty if the generator is unknown or the parameters are invalid.
     */
    Result generate(const ABGenKey &key, RDevice *device, const RImageConstraints *constraints = nullptr) noexcept;

    /// @p source scaled to fit in @p box (see load()), rounded up and at least 1x1. Empty if @p source is.
    static SkISize FitSize(SkSize source, SkISize box) noexcept;

    /// Number of SVG documents currently parsed.
    size_t svgCount() noexcept;

private:
    struct SVG;
    std::shared_ptr<SVG> svg(const std::filesystem::path &path) noexcept;
    std::unordered_map<std::string, std::weak_ptr<SVG>> m_svgs;
};

#endif // CZ_ABRASTERIZER_H
