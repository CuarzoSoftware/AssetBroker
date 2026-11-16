#ifndef CZ_ABGENERATORS_H
#define CZ_ABGENERATORS_H

#include <CZ/AssetBroker/ABGenKey.h>
#include <CZ/skia/core/SkBitmap.h>
#include <CZ/skia/core/SkRefCnt.h>
#include <string_view>

namespace skia::textlayout { class FontCollection; }

/**
 * @brief Built-in generators of shared assets.
 *
 * A generator draws an image from the parameters of an ABGenKey. They are built into the library
 * (and so into the service), so apps and the service draw exactly the same thing. Names include a
 * version: a generator whose output changes gets a new name, so a service from another version
 * never hands out a different image (unknown generators are drawn in-process, see ABCore::generate()).
 *
 * The ak.* generators are Kay's default theme assets (see AKTheme). Sizes are in logical pixels;
 * images are @c size * @c scale pixels.
 *
 * @ingroup AssetBroker
 */
namespace CZ::ABGenerators
{
    /**
     * @brief Draws the asset into @p out (allocated by the generator, with @p colorType, premultiplied).
     * @return false if the parameters are invalid or drawing failed.
     */
    using Func = bool(*)(const ABGenKey &key, SkColorType colorType, SkBitmap &out);

    /// The generator named @p name, nullptr if unknown.
    Func Find(std::string_view name) noexcept;

    /**
     * @brief Font collection used by text based generators (Glyph).
     *
     * By default one with the fontconfig font manager is created on first use. Apps that already
     * have one (e.g. Kay's AKApp) can share it.
     */
    void SetFontCollection(sk_sp<skia::textlayout::FontCollection> collection) noexcept;

    /// Rounded rect 9-patch. ints: radTL, radTR, radBR, radBL, scale, fill SkColor, stroke width, stroke SkColor.
    inline constexpr const char *AKRRect9Patch          { "ak.rrect9patch@1" };

    /// White-filled first quadrant of a circle. ints: radius, scale.
    inline constexpr const char *AKCircleQuadrant       { "ak.circlequadrant@1" };

    /// AKTextField's horizontal 3-patch. ints: width, height, scale.
    inline constexpr const char *AKTextFieldRoundH3Patch { "ak.textfieldroundh3patch@1" };

    /// AKTextCaret's vertical 3-patch. ints: width, height, scale.
    inline constexpr const char *AKTextCaretV3Patch     { "ak.textcaretv3patch@1" };

    /// Round line 3-patch (scroll bar handles). ints: orientation (0: H, 1: V), diameter, scale.
    inline constexpr const char *AKRoundLine3Patch      { "ak.roundline3patch@1" };

    /// Scroll rail 3-patch. ints: orientation (0: H, 1: V), scale, background SkColor, border SkColor.
    inline constexpr const char *AKScrollRail3Patch     { "ak.scrollrail3patch@1" };

    /// AKEdgeShadow's gradient. ints: radius, scale.
    inline constexpr const char *AKEdgeShadow           { "ak.edgeshadow@1" };

    /// A white glyph stretched to a size x size square (icon fonts). strings: font family, UTF-8 text. ints: size (pixels).
    inline constexpr const char *Glyph                  { "glyph@1" };
};

#endif // CZ_ABGENERATORS_H
