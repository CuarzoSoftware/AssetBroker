#include <CZ/AssetBroker/ABCore.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RImage.h>
#include <CZ/Core/CZCore.h>
#include <CZ/Core/CZTimer.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <format>
#include <sstream>

using namespace CZ;

/*
 * Usage: cz-asset-broker-bench <auto|local|remote> <hold ms> <sizes, e.g. 256,128,64> [names file]
 *
 * Loads every icon name of the file (one per line) at each size, keeps the images alive for
 * <hold ms> (to measure the process memory) and exits. Prints "READY <images> <ms>" once loaded.
 * Without a names file, loads nothing (baseline).
 */
int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <auto|local|remote> <hold ms> <sizes> [names file]\n";
        return 1;
    }

    ABMode mode { ABMode::Auto };
    if (strcmp(argv[1], "local") == 0) mode = ABMode::Local;
    else if (strcmp(argv[1], "remote") == 0) mode = ABMode::Remote;

    const UInt32 holdMs { static_cast<UInt32>(std::stoul(argv[2])) };

    std::vector<Int32> sizes;
    {
        std::stringstream ss { argv[3] };
        std::string item;
        while (std::getline(ss, item, ','))
            sizes.emplace_back(std::stoi(item));
    }

    std::vector<std::string> names;

    if (argc > 4)
    {
        std::ifstream file { argv[4] };
        std::string line;
        while (std::getline(file, line))
            if (!line.empty())
                names.emplace_back(line);
    }

    auto cz { CZCore::GetOrMake() };
    auto ream { ABUtils::MakeHeadlessRCore() };

    if (!ream)
        return 1;

    const UInt64 start { ABUtils::NowMs() };
    auto ab { ABCore::GetOrMake(mode) };

    if (!ab)
        return 1;

    std::vector<std::shared_ptr<RImage>> images;

    for (const auto &name : names)
    {
        // Like Shelf: the app's .desktop Icon, then the name itself, then generic icons
        ABIconKey key;
        key.appId = name;
        key.names = { name, "application-x-executable" };
        key.size = 256;
        key.themes = { "WhiteSur-light", "" };

        for (Int32 size : sizes)
            if (auto image { ab->icon(key, { size, size }) })
                images.emplace_back(image);
    }

    std::cout << "READY " << images.size() << " " << (ABUtils::NowMs() - start) << " " << (ab->remote() ? "remote" : "local") << std::endl;

    bool done { false };
    CZTimer::OneShot(holdMs, [&](CZTimer *){ done = true; });

    while (!done)
        cz->dispatch(-1);

    return 0;
}
