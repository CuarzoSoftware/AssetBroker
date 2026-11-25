#include <CZ/AssetBroker/ABRasterizer.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/AssetBroker/ABGenerators.h>
#include <CZ/Ream/SK/RSKFormat.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>

#include <CZ/skia/core/SkBitmap.h>
#include <CZ/skia/core/SkCanvas.h>
#include <CZ/skia/core/SkData.h>
#include <CZ/skia/core/SkImage.h>
#include <CZ/skia/core/SkStream.h>
#include <CZ/skia/effects/SkImageFilters.h>
#include <CZ/skia/modules/svg/include/SkSVGDOM.h>
#include <CZ/skia/modules/svg/include/SkSVGSVG.h>

#include <algorithm>
#include <cmath>

using namespace CZ;

struct ABRasterizer::SVG
{
    sk_sp<SkSVGDOM> dom;

    /// Size of the root <svg> from its absolute width/height (0 if missing or relative)
    SkSize intrinsicSize;

    /// Size used when no box is given: the intrinsic size, else the viewBox size
    SkSize naturalSize;
};

SkISize ABRasterizer::FitSize(SkSize source, SkISize box) noexcept
{
    if (source.isEmpty())
        return SkISize::MakeEmpty();

    SkScalar scale { 1.f };

    if (box.width() > 0 && box.height() > 0)
        scale = std::min(box.width() / source.width(), box.height() / source.height());
    else if (box.width() > 0)
        scale = box.width() / source.width();
    else if (box.height() > 0)
        scale = box.height() / source.height();

    // Snap to the box dimension it was fitted to, so float error never adds a pixel row/column
    auto snap = [](SkScalar value, Int32 limit) -> Int32
    {
        const Int32 rounded { static_cast<Int32>(std::ceil(value - 0.001f)) };
        return std::max(1, limit > 0 ? std::min(rounded, limit) : rounded);
    };

    return SkISize::Make(snap(source.width() * scale, box.width()),
                         snap(source.height() * scale, box.height()));
}

size_t ABRasterizer::svgCount() noexcept
{
    std::erase_if(m_svgs, [](const auto &pair){ return pair.second.expired(); });
    return m_svgs.size();
}

std::shared_ptr<ABRasterizer::SVG> ABRasterizer::svg(const std::filesystem::path &path) noexcept
{
    if (auto it { m_svgs.find(path.string()) }; it != m_svgs.end())
    {
        if (auto existing { it->second.lock() })
            return existing;

        m_svgs.erase(it);
    }

    auto stream { SkMemoryStream::MakeFromFile(path.c_str()) };

    if (!stream)
        return {};

    auto dom { SkSVGDOM::MakeFromStream(*stream) };

    if (!dom || !dom->getRoot())
        return {};

    auto svg { std::make_shared<SVG>() };
    svg->dom = dom;

    // Captured before any setContainerSize() call, which changes it
    svg->intrinsicSize = dom->containerSize();
    svg->naturalSize = svg->intrinsicSize;

    // Many icons only declare a viewBox (or relative width/height): use its extents
    const SkTLazy<SkRect> &viewBox { dom->getRoot()->getViewBox() };

    if (viewBox.isValid())
    {
        if (svg->naturalSize.fWidth <= 0)  svg->naturalSize.fWidth  = viewBox->width();
        if (svg->naturalSize.fHeight <= 0) svg->naturalSize.fHeight = viewBox->height();
    }

    if (svg->naturalSize.isEmpty())
    {
        ABLog(CZDebug, CZLN, "SVG without size or viewBox: {}", path.c_str());
        return {};
    }

    std::erase_if(m_svgs, [](const auto &pair){ return pair.second.expired(); });
    m_svgs[path.string()] = svg;
    return svg;
}

static bool HasSVGExtension(const std::filesystem::path &path) noexcept
{
    auto ext { path.extension().string() };
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    return ext == ".svg";
}

static std::shared_ptr<RImage> Upload(const SkBitmap &bitmap, const RDRMFormat &format, const RImageConstraints &constraints) noexcept
{
    if (!bitmap.getPixels())
        return {};

    RPixelBufferInfo info {};
    info.pixels = static_cast<UInt8*>(bitmap.getPixels());
    info.stride = bitmap.rowBytes();
    info.format = format.format();
    info.size = SkISize::Make(bitmap.width(), bitmap.height());
    return RImage::MakeFromPixels(info, format, &constraints);
}

ABRasterizer::Result ABRasterizer::generate(const ABGenKey &key, RDevice *device, const RImageConstraints *constraints) noexcept
{
    auto core { RCore::Get() };

    if (!core)
        return {};

    if (!device)
        device = core->mainDevice();

    const auto generator { ABGenerators::Find(key.generator) };

    if (!generator)
    {
        ABLog(CZDebug, CZLN, "Unknown generator: {}", key.generator);
        return {};
    }

    const auto format { ABUtils::IconFormat(device) };

    if (!format)
        return {};

    const SkColorType colorType { RSKFormat::FromDRM(format->format()) };

    if (colorType == kUnknown_SkColorType)
        return {};

    SkBitmap bitmap;

    if (!generator(key, colorType, bitmap))
    {
        ABLog(CZDebug, CZLN, "Generator {} failed (invalid parameters?)", key.generator);
        return {};
    }

    RImageConstraints cons {};

    if (constraints)
        cons = *constraints;

    cons.allocator = device;

    Result result;
    result.image = Upload(bitmap, *format, cons);
    return result;
}

bool ABRasterizer::DrawShadow(const SkBitmap &content, SkISize box, const ABShadow &shadow, SkBitmap &out) noexcept
{
    if (content.empty())
        return false;

    const Int32 padding { std::max(0, shadow.padding) };
    const SkISize size { box.width() > 0 ? box.width() : content.width() + 2 * padding,
                         box.height() > 0 ? box.height() : content.height() + 2 * padding };

    if (size.isEmpty() || !out.tryAllocPixels(content.info().makeDimensions(size)))
        return false;

    SkCanvas canvas { out };
    canvas.clear(SK_ColorTRANSPARENT);

    // Its blur and offset follow the icon's size (its larger side)
    const SkScalar iconSize { SkScalar(std::max(content.width(), content.height())) };
    const SkScalar sigma { shadow.radius * iconSize / 3.f };

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setImageFilter(SkImageFilters::DropShadow(shadow.offsetX * iconSize, shadow.offsetY * iconSize,
                                                    sigma, sigma, shadow.color, nullptr));

    // Centered at whole pixels: drawn 1:1, so it stays sharp
    canvas.drawImage(content.asImage(), SkScalar((size.width() - content.width()) / 2), SkScalar((size.height() - content.height()) / 2),
                     SkSamplingOptions(), &paint);
    return true;
}

ABRasterizer::Result ABRasterizer::load(const std::filesystem::path &path, SkISize box, RDevice *device, const RImageConstraints *constraints, const ABShadow *shadow) noexcept
{
    auto core { RCore::Get() };

    if (!core)
    {
        ABLog(CZError, CZLN, "Missing RCore");
        return {};
    }

    if (!device)
        device = core->mainDevice();

    const auto format { ABUtils::IconFormat(device) };

    if (!format)
    {
        ABLog(CZError, CZLN, "Device {} has no ABGR8888/ARGB8888 texture format", device->drmNode());
        return {};
    }

    const SkColorType colorType { RSKFormat::FromDRM(format->format()) };

    if (colorType == kUnknown_SkColorType)
        return {};

    RImageConstraints cons {};

    if (constraints)
        cons = *constraints;

    cons.allocator = device;

    box.fWidth = std::clamp(box.fWidth, 0, 16384);
    box.fHeight = std::clamp(box.fHeight, 0, 16384);

    // With a shadow, the content is fitted in the box minus the room for it (the result keeps the box)
    const SkISize outBox { box };

    if (shadow)
    {
        const Int32 room { 2 * std::max(0, shadow->padding) };

        if (box.width() > 0)  box.fWidth  = std::max(1, box.width() - room);
        if (box.height() > 0) box.fHeight = std::max(1, box.height() - room);
    }

    // The content as is, or over its shadow
    const auto finish = [&](const SkBitmap &bitmap, std::shared_ptr<void> keepAlive) -> Result
    {
        Result result;

        if (shadow)
        {
            SkBitmap shadowed;

            if (!DrawShadow(bitmap, outBox, *shadow, shadowed))
                return {};

            result.image = Upload(shadowed, *format, cons);
        }
        else
            result.image = Upload(bitmap, *format, cons);

        if (result.image)
            result.keepAlive = std::move(keepAlive);

        return result;
    };

    const bool svgFirst { HasSVGExtension(path) };

    for (int attempt = 0; attempt < 2; attempt++)
    {
        const bool trySVG { (attempt == 0) == svgFirst };

        if (trySVG)
        {
            auto source { svg(path) };

            if (!source)
                continue;

            const SkISize size { FitSize(source->naturalSize, box) };
            SkBitmap bitmap;

            if (size.isEmpty() || !bitmap.tryAllocPixels(SkImageInfo::Make(size, colorType, kPremul_SkAlphaType)))
                return {};

            SkCanvas canvas { bitmap };
            canvas.clear(SK_ColorTRANSPARENT);

            // Roots with relative sizes or only a viewBox are mapped to the viewport by
            // setContainerSize(). Roots with absolute dimensions ignore it and render at their
            // intrinsic size, so they are scaled instead.
            source->dom->setContainerSize(SkSize::Make(size.width(), size.height()));

            if (!source->intrinsicSize.isEmpty())
                canvas.scale(size.width() / source->intrinsicSize.width(),
                             size.height() / source->intrinsicSize.height());

            source->dom->render(&canvas);
            return finish(bitmap, source);
        }
        else
        {
            auto data { SkData::MakeFromFileName(path.c_str()) };

            if (!data)
                return {};

            auto image { SkImages::DeferredFromEncodedData(data) };

            if (!image)
                continue;

            const SkISize size { FitSize(SkSize::Make(image->width(), image->height()), box) };
            SkBitmap bitmap;

            if (size.isEmpty() || !bitmap.tryAllocPixels(SkImageInfo::Make(size, colorType, kPremul_SkAlphaType)))
                return {};

            SkCanvas canvas { bitmap };
            canvas.clear(SK_ColorTRANSPARENT);

            // Downscaling through mipmaps (antialiased), upscaling bilinear
            const bool smaller { size.width() < image->width() || size.height() < image->height() };
            const SkSamplingOptions sampling { smaller ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                                       : SkSamplingOptions(SkFilterMode::kLinear) };
            SkPaint paint;
            paint.setAntiAlias(true);
            canvas.drawImageRect(image, SkRect::MakeIWH(image->width(), image->height()), SkRect::Make(size),
                                 sampling, &paint, SkCanvas::kFast_SrcRectConstraint);
            return finish(bitmap, nullptr);
        }
    }

    ABLog(CZDebug, CZLN, "Unsupported or invalid image: {}", path.c_str());
    return {};
}
