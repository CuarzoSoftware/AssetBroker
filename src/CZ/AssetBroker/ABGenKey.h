#ifndef CZ_ABGENKEY_H
#define CZ_ABGENKEY_H

#include <CZ/AssetBroker/AB.h>
#include <string>
#include <vector>

/**
 * @brief A generated asset: the generator and its parameters (see ABGenerators, ABCore::generate()).
 *
 * The same key always produces the same image, so generated assets are shared like files.
 *
 * @ingroup AssetBroker
 */
struct CZ::ABGenKey
{
    /// Generator name, including its version (e.g. ABGenerators::AKRRect9Patch).
    std::string generator;

    /// Integer parameters (colors as SkColor values). Their meaning depends on the generator.
    std::vector<Int64> ints;

    /// String parameters. Their meaning depends on the generator.
    std::vector<std::string> strings;

    bool operator==(const ABGenKey &other) const noexcept = default;
};

#endif // CZ_ABGENKEY_H
