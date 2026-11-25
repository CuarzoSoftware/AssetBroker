#ifndef CZ_ABSHADOW_H
#define CZ_ABSHADOW_H

#include <CZ/AssetBroker/AB.h>
#include <algorithm>
#include <cmath>

/**
 * @brief A drop shadow drawn under an icon or image (optional, see ABCore::icon()).
 *
 * Icons shown over pictures or busy backgrounds (desktop icons, dock tiles...) usually get a soft
 * shadow following their shape. Drawn by the asset broker, the icon and its shadow are one image,
 * shared by every app like any other asset (no live blur when the app repaints).
 *
 * The image keeps the requested box: the icon is fitted in the box minus @ref padding on each
 * side (centered, at a whole pixel offset so it stays sharp), and the shadow fills that room.
 * The shadow's blur and offset are fractions of the icon's size, so it keeps its look at any size.
 *
 * @code
 * // A 64 px icon in a 94 px image: 15 px of room on each side for its shadow
 * CZ::ABShadow shadow { .padding = 15, .radius = 15.f / 64.f, .offsetY = 2.f / 64.f, .color = 0x66000000 };
 * auto image { ab->icon(key, { 94, 94 }, nullptr, &shadow) };
 * @endcode
 *
 * @ingroup AssetBroker
 */
struct CZ::ABShadow
{
    /// Room around the icon for the shadow, in pixels, on each side.
    Int32 padding { 0 };

    /// Blur radius as a fraction of the icon size (the shadow fades out within it, sigma = radius / 3).
    float radius { 0.f };

    /// Offset of the shadow, as fractions of the icon size.
    float offsetX { 0.f };
    float offsetY { 0.f };

    /// Color (ARGB, unpremultiplied, e.g. 0x66000000).
    UInt32 color { 0x66000000 };

    /**
     * @brief The @ref padding of an image @p pixels wide/tall with an icon whose shadow needs
     *        @p margin (a fraction of the icon size) on each side.
     *
     * The icon is floor(pixels / (1 + 2 * margin)) pixels and the padding half of what's left
     * (rounded down). Used by ABCore::IconProvider() with @ref radius as the margin.
     */
    static Int32 PaddingFor(Int32 pixels, float margin) noexcept
    {
        pixels = std::max(pixels, 1);
        const Int32 icon { std::max(1, Int32(std::floor(float(pixels) / (1.f + 2.f * std::max(0.f, margin))))) };
        return (pixels - icon) / 2;
    }

    bool operator==(const ABShadow &other) const noexcept = default;
};

#endif // CZ_ABSHADOW_H
