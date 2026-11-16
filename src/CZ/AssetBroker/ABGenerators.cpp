#include <CZ/AssetBroker/ABGenerators.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/skia/core/SkCanvas.h>
#include <CZ/skia/core/SkMaskFilter.h>
#include <CZ/skia/core/SkBlurTypes.h>
#include <CZ/skia/core/SkRRect.h>
#include <CZ/skia/ports/SkFontMgr_fontconfig.h>
#include <CZ/skia/modules/skparagraph/include/FontCollection.h>
#include <CZ/skia/modules/skparagraph/src/ParagraphBuilderImpl.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace CZ;

/*
 * Drawing code ported from Kay's AKTheme (and AKIconFont): the same shapes, drawn on the CPU into
 * a bitmap of size * scale pixels, with the canvas scaled so coordinates stay in logical pixels
 * (like Kay's RSurface passes).
 *
 * Changing what a generator draws requires a new name (version), see ABGenerators.h.
 */

static constexpr Int64 MaxSize { 4096 };

static bool Alloc(SkBitmap &out, Int64 width, Int64 height, SkColorType colorType) noexcept
{
    if (width <= 0 || height <= 0 || width > MaxSize || height > MaxSize)
        return false;

    return out.tryAllocPixels(SkImageInfo::Make(width, height, colorType, kPremul_SkAlphaType));
}

static bool Ints(const ABGenKey &key, size_t count) noexcept
{
    return key.ints.size() == count && key.strings.empty();
}

static Int32 Scale(Int64 scale) noexcept
{
    return static_cast<Int32>(std::clamp<Int64>(scale, 1, 8));
}

/* ------------------------------------------------------------------------------------------- */

static bool RRect9Patch(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 8))
        return false;

    const Int32 radTL { static_cast<Int32>(std::clamp<Int64>(key.ints[0], 0, MaxSize)) };
    const Int32 radTR { static_cast<Int32>(std::clamp<Int64>(key.ints[1], 0, MaxSize)) };
    const Int32 radBR { static_cast<Int32>(std::clamp<Int64>(key.ints[2], 0, MaxSize)) };
    const Int32 radBL { static_cast<Int32>(std::clamp<Int64>(key.ints[3], 0, MaxSize)) };
    const Int32 scale { Scale(key.ints[4]) };
    const SkColor backgroundColor { static_cast<SkColor>(key.ints[5]) };
    const SkColor strokeColor { static_cast<SkColor>(key.ints[7]) };
    Int32 strokeWidth { static_cast<Int32>(std::clamp<Int64>(key.ints[6], 0, MaxSize)) };

    if (SkColorGetA(strokeColor) == 0)
        strokeWidth = 0;

    // Each 9-slice cell must fit both of its corners and the stroke; the center stays a 1x1
    // stretchable pixel (see AKTheme::RRect9Patch)
    const Int32 leftW   { std::max({ radTL, radBL, strokeWidth }) };
    const Int32 rightW  { std::max({ radTR, radBR, strokeWidth }) };
    const Int32 topH    { std::max({ radTL, radTR, strokeWidth }) };
    const Int32 bottomH { std::max({ radBL, radBR, strokeWidth }) };
    const SkISize size { leftW + 1 + rightW, topH + 1 + bottomH };

    if (!Alloc(out, Int64(size.width()) * scale, Int64(size.height()) * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrc);

    const auto rect { SkRect::Make(size) };

    if (SkColorGetA(backgroundColor) > 0)
    {
        paint.setStroke(false);
        paint.setColor(backgroundColor);

        SkVector radii[4];
        radii[SkRRect::kUpperLeft_Corner].set(radTL, radTL);
        radii[SkRRect::kUpperRight_Corner].set(radTR, radTR);
        radii[SkRRect::kLowerRight_Corner].set(radBR, radBR);
        radii[SkRRect::kLowerLeft_Corner].set(radBL, radBL);

        SkRRect rrect;
        rrect.setRectRadii(rect, radii);
        c.drawRRect(rrect, paint);
    }

    if (strokeWidth > 0)
    {
        paint.setStroke(true);
        paint.setColor(strokeColor);
        paint.setStrokeWidth(strokeWidth);

        const SkScalar inset { SkScalar(strokeWidth) * 0.5f };
        const auto sRad { [inset](Int32 r) { return std::max(0.f, SkScalar(r) - inset); } };

        SkVector radii[4];
        radii[SkRRect::kUpperLeft_Corner].set(sRad(radTL), sRad(radTL));
        radii[SkRRect::kUpperRight_Corner].set(sRad(radTR), sRad(radTR));
        radii[SkRRect::kLowerRight_Corner].set(sRad(radBR), sRad(radBR));
        radii[SkRRect::kLowerLeft_Corner].set(sRad(radBL), sRad(radBL));

        SkRRect rrect;
        rrect.setRectRadii(rect.makeInset(inset, inset), radii);
        c.drawRRect(rrect, paint);
    }

    return true;
}

static bool CircleQuadrant(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 2))
        return false;

    const Int32 radius { static_cast<Int32>(std::clamp<Int64>(key.ints[0], 1, MaxSize)) };
    const Int32 scale { Scale(key.ints[1]) };

    if (!Alloc(out, Int64(radius) * scale, Int64(radius) * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    SkPaint paint;
    paint.setStroke(false);
    paint.setAntiAlias(true);
    paint.setColor(SK_ColorWHITE);
    paint.setBlendMode(SkBlendMode::kSrc);
    c.drawCircle(SkPoint::Make(radius, radius), radius, paint);
    return true;
}

static bool TextFieldRoundH3Patch(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 3))
        return false;

    const Int64 width { key.ints[0] }, height { key.ints[1] };
    const Int32 scale { Scale(key.ints[2]) };

    if (!Alloc(out, width * scale, height * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    SkPaint paint;
    const float borderRadius { 5.f };
    SkRect roundRect { SkRect::MakeWH(width * 2, height) };

    // Shadow
    paint.setAntiAlias(true);
    roundRect.inset(2.5f, 2.5f);
    roundRect.offset(0.f, 0.5f);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setColor(SkColorSetARGB(82, 0, 0, 0));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1.f));
    c.drawRoundRect(roundRect, borderRadius, borderRadius, paint);
    paint.setMaskFilter(nullptr);
    roundRect.offset(0.f, -0.5f);

    // Border
    paint.setBlendMode(SkBlendMode::kSrcOver);
    paint.setStroke(true);
    paint.setStrokeWidth(1.f);
    paint.setColor(SkColorSetARGB(9, 0, 0, 0));
    c.drawRoundRect(roundRect, borderRadius, borderRadius, paint);

    // Fill
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setStroke(false);
    paint.setColor(SK_ColorWHITE);
    c.drawRoundRect(roundRect, borderRadius, borderRadius, paint);
    return true;
}

static bool TextCaretV3Patch(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 3))
        return false;

    const Int64 width { key.ints[0] }, height { key.ints[1] };
    const Int32 scale { Scale(key.ints[2]) };

    if (!Alloc(out, width * scale, height * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    const SkScalar borderRadius { width * 0.5f };
    SkRect roundRect { SkRect::MakeWH(width, height * 2) };
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setStroke(false);
    paint.setColor(SK_ColorWHITE);
    c.drawRoundRect(roundRect, borderRadius, borderRadius, paint);
    return true;
}

static bool RoundLine3Patch(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 3))
        return false;

    const bool vertical { key.ints[0] != 0 };
    const Int32 diam { static_cast<Int32>(std::clamp<Int64>(key.ints[1], 1, MaxSize)) };
    const Int32 scale { Scale(key.ints[2]) };
    const SkScalar rad { SkScalar(diam) * 0.5f };

    // Same truncation as Kay's SkISize(float, float)
    const Int32 length { static_cast<Int32>(rad + 2.f) };
    const SkISize size { vertical ? diam : length, vertical ? length : diam };

    if (!Alloc(out, Int64(size.width()) * scale, Int64(size.height()) * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setStroke(false);
    paint.setColor(SK_ColorWHITE);

    if (vertical)
    {
        c.drawCircle(SkPoint(rad, rad + 1.f), rad, paint);
        c.drawRect(SkRect::MakeXYWH(0, rad + 1.f, diam, 1.f), paint);
    }
    else
    {
        c.drawCircle(SkPoint(rad + 1.f, rad), rad, paint);
        c.drawRect(SkRect::MakeXYWH(rad + 1.f, 0.f, 1.f, diam), paint);
    }

    return true;
}

static bool ScrollRail3Patch(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 4))
        return false;

    const bool vertical { key.ints[0] != 0 };
    const Int32 scale { Scale(key.ints[1]) };
    const SkColor railBackground { static_cast<SkColor>(key.ints[2]) };
    const SkColor railBorder { static_cast<SkColor>(key.ints[3]) };

    if (!Alloc(out, Int64(vertical ? 1 : 3) * scale, Int64(vertical ? 3 : 1) * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(railBackground);
    c.scale(scale, scale);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrcOver);
    paint.setStroke(true);
    paint.setStrokeWidth(1.f);
    paint.setColor(railBorder);

    if (vertical)
        c.drawLine(-5.f, 0.f, 5.f, 0.f, paint);
    else
        c.drawLine(0.f, -5.f, 0.f, 5.f, paint);

    return true;
}

static bool EdgeShadow(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (!Ints(key, 2))
        return false;

    const Int32 radius { static_cast<Int32>(std::clamp<Int64>(key.ints[0], 1, MaxSize)) };
    const Int32 scale { Scale(key.ints[1]) };

    if (!Alloc(out, Int64(1) * scale, Int64(radius) * scale, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);
    c.scale(scale, scale);

    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, SkScalar(radius) / 3.f));
    c.drawIRect(SkIRect::MakeXYWH(-100, -100, 200, 100), paint);
    return true;
}

/* ------------------------------------------- Glyph ----------------------------------------- */

static std::mutex s_fontMutex;
static sk_sp<skia::textlayout::FontCollection> s_fontCollection;

void ABGenerators::SetFontCollection(sk_sp<skia::textlayout::FontCollection> collection) noexcept
{
    std::lock_guard lock { s_fontMutex };
    s_fontCollection = std::move(collection);
}

static sk_sp<skia::textlayout::FontCollection> FontCollection() noexcept
{
    std::lock_guard lock { s_fontMutex };

    if (!s_fontCollection)
    {
        s_fontCollection = sk_make_sp<skia::textlayout::FontCollection>();
        s_fontCollection->setDefaultFontManager(SkFontMgr_New_FontConfig(nullptr));
        s_fontCollection->enableFontFallback();
    }

    return s_fontCollection;
}

static bool Glyph(const ABGenKey &key, SkColorType colorType, SkBitmap &out)
{
    if (key.ints.size() != 1 || key.strings.size() != 2 || key.strings[1].empty())
        return false;

    const Int32 size { static_cast<Int32>(std::clamp<Int64>(key.ints[0], 1, MaxSize)) };

    if (!Alloc(out, size, size, colorType))
        return false;

    SkCanvas c { out };
    c.clear(SK_ColorTRANSPARENT);

    skia::textlayout::ParagraphStyle paragraphStyle;
    paragraphStyle.setTextDirection(skia::textlayout::TextDirection::kLtr);

    auto builder { skia::textlayout::ParagraphBuilderImpl::make(paragraphStyle, FontCollection()) };

    if (!builder)
        return false;

    // As AKIconFont::getIconByUTF8()
    SkPaint p;
    p.setBlendMode(SkBlendMode::kSrc);
    p.setAntiAlias(true);
    p.setColor(SK_ColorWHITE);

    skia::textlayout::TextStyle style;
    style.setFontFamilies({ SkString(key.strings[0].c_str()) });
    style.setForegroundColor(p);
    style.setHeight(size);
    style.setFontSize(size);
    builder->pushStyle(style);
    builder->addText(key.strings[1].c_str());

    auto paragraph { builder->Build() };

    if (!paragraph)
        return false;

    paragraph->layout(size);

    if (paragraph->getMaxIntrinsicWidth() <= 0 || paragraph->getHeight() <= 0)
        return false;

    c.scale(size / paragraph->getMaxIntrinsicWidth(), size / paragraph->getHeight());
    paragraph->paint(&c, 0, 0);
    return true;
}

/* ------------------------------------------------------------------------------------------- */

ABGenerators::Func ABGenerators::Find(std::string_view name) noexcept
{
    static const std::unordered_map<std::string_view, Func> generators
    {
        { AKRRect9Patch,           RRect9Patch },
        { AKCircleQuadrant,        CircleQuadrant },
        { AKTextFieldRoundH3Patch, TextFieldRoundH3Patch },
        { AKTextCaretV3Patch,      TextCaretV3Patch },
        { AKRoundLine3Patch,       RoundLine3Patch },
        { AKScrollRail3Patch,      ScrollRail3Patch },
        { AKEdgeShadow,            EdgeShadow },
        { Glyph,                   ::Glyph },
    };

    auto it { generators.find(name) };
    return it == generators.end() ? nullptr : it->second;
}
