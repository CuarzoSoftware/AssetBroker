#include <CZ/AssetBroker/ABCore.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABGenerators.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RImage.h>
#include <CZ/Ream/RSurface.h>
#include <CZ/Ream/RPass.h>
#include <CZ/skia/core/SkCanvas.h>
#include <CZ/Core/CZCore.h>
#include <CZ/Core/CZTimer.h>
#include <cstring>
#include <iostream>
#include <format>

using namespace CZ;

template<typename... Args>
static void Print(std::format_string<Args...> fmt, Args&&... args)
{
    std::cout << std::format(fmt, std::forward<Args>(args)...) << std::endl;
}

// FNV-1a of the image as an app sees it: drawn (sampled) into a local surface, read back
static UInt64 Checksum(const std::shared_ptr<RImage> &image) noexcept
{
    const SkISize size { image->size() };
    auto surface { RSurface::Make(size, 1, true) };

    if (!surface || !image->skImage())
        return 0;

    {
        auto pass { surface->beginPass(RPassCap_SkCanvas) };
        SkCanvas *canvas { pass->getCanvas() };
        canvas->clear(SK_ColorTRANSPARENT);
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas->drawImage(image->skImage(), 0, 0, SkSamplingOptions(), &paint);
    }

    std::vector<UInt8> pixels(size.width() * size.height() * 4);
    RPixelBufferRegion region {};
    region.pixels = pixels.data();
    region.stride = size.width() * 4;
    region.format = surface->image()->formatInfo().format;
    region.region.setRect(SkIRect::MakeSize(size));

    if (!surface->image()->readPixels(region))
        return 0;

    UInt64 hash { 0xcbf29ce484222325ULL };

    for (UInt8 byte : pixels)
        hash = (hash ^ byte) * 0x100000001b3ULL;

    return hash;
}

/*
 * Usage: cz-asset-broker-example [auto|local|remote] [--generators] [icon names or app:<app id>...]
 *
 * Loads icons at a few sizes, shows where they came from and that requesting them again returns
 * the same images, then releases them.
 */
int main(int argc, char **argv)
{
    ABMode mode { ABMode::Auto };
    int firstName { 1 };

    if (argc > 1)
    {
        firstName = 2;

        if (strcmp(argv[1], "local") == 0) mode = ABMode::Local;
        else if (strcmp(argv[1], "remote") == 0) mode = ABMode::Remote;
        else if (strcmp(argv[1], "auto") == 0) mode = ABMode::Auto;
        else firstName = 1;
    }

    std::vector<std::string> names;
    bool generators { false };

    for (int i = firstName; i < argc; i++)
    {
        if (strcmp(argv[i], "--generators") == 0)
            generators = true;
        else
            names.emplace_back(argv[i]);
    }

    if (names.empty() && !generators)
        names = { "firefox", "org.gnome.Nautilus", "user-trash", "preferences-system", "no-such-icon-xyz" };

    // ABCore requires a CZCore and an RCore
    auto cz { CZCore::GetOrMake() };
    auto ream { ABUtils::MakeHeadlessRCore() };

    if (!ream)
        return 1;

    auto ab { ABCore::GetOrMake(mode) };

    if (!ab)
    {
        Print("Failed to create the ABCore");
        return 1;
    }

    Print("Mode: {}, images from: {}, device: {}",
                 mode == ABMode::Auto ? "auto" : mode == ABMode::Local ? "local" : "remote",
                 ab->remote() ? "service" : "this process",
                 ream->mainDevice()->drmNode());

    std::vector<std::shared_ptr<RImage>> images;

    for (const auto &name : names)
    {
        // The same parameters as XDGIconThemeManager::findIcon(), plus the pixel box
        // app:<id> looks up the app's icon (.desktop Icon, then generic app icons)
        ABIconKey key;
        key.size = 64;

        if (name.starts_with("app:"))
        {
            key.appId = name.substr(4);
            key.names = { "application-x-executable" };
        }
        else
            key.names = { name };

        if (!ab->icon(key))
        {
            Print("{}: not found", name);
            continue;
        }

        Print("{}:", name);

        // What AKImageFrame does: ask for the size it's drawn at
        auto provider { ABCore::IconProvider(key) };

        for (Int32 pixels : { 32, 64, 128 })
        {
            auto image { provider(pixels, 1) };

            if (!image)
            {
                Print("  {}px: failed", pixels);
                continue;
            }

            auto again { provider(pixels, 1) };

            Print("  {}px: {}x{} modifier 0x{:x}, shared on repeat: {}, checksum {:016x}", pixels,
                  image->size().width(), image->size().height(), image->modifier(),
                  again == image ? "yes" : "NO", Checksum(image));
            images.emplace_back(image);
        }
    }

    // Generated assets (Kay's theme ones and an icon font glyph)
    if (generators)
    {
        const std::vector<ABGenKey> keys
        {
            { ABGenerators::AKRRect9Patch,           { 8, 8, 8, 8, 2, 0xFFFFFFFF, 1, 0x20000000 }, {} },
            { ABGenerators::AKCircleQuadrant,        { 12, 2 }, {} },
            { ABGenerators::AKTextFieldRoundH3Patch, { 9, 24, 2 }, {} },
            { ABGenerators::AKTextCaretV3Patch,      { 2, 3, 2 }, {} },
            { ABGenerators::AKRoundLine3Patch,       { 1, 7, 2 }, {} },
            { ABGenerators::AKScrollRail3Patch,      { 1, 2, 0x10000000, 0x30000000 }, {} },
            { ABGenerators::AKEdgeShadow,            { 2, 2 }, {} },
            { ABGenerators::Glyph,                   { 48 }, { "Material Icons Round", "\xee\x96\x8a" } }, // U+E58A
            { "no-such-generator@1",                 {}, {} },
        };

        for (const auto &key : keys)
        {
            auto image { ab->generate(key) };

            if (!image)
            {
                Print("{}: failed", key.generator);
                continue;
            }

            Print("{}: {}x{}, shared on repeat: {}, checksum {:016x}", key.generator,
                  image->size().width(), image->size().height(),
                  ab->generate(key) == image ? "yes" : "NO", Checksum(image));
            images.emplace_back(image);
        }
    }

    Print("Cached images: {}", ab->cachedImages());
    images.clear();
    Print("Released, cached during the grace period: {}", ab->cachedImages());

    bool done { false };
    CZTimer::OneShot(ab->gracePeriod() + 200, [&](CZTimer *){ done = true; });

    while (!done)
        cz->dispatch(-1);

    Print("After the grace period: {}", ab->cachedImages());
    return 0;
}
