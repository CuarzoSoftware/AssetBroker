#ifndef CZ_ABICONKEY_H
#define CZ_ABICONKEY_H

#include <CZ/AssetBroker/AB.h>
#include <CZ/XDG/XDGIcon.h>
#include <CZ/XDG/XDGIconDirectory.h>
#include <string>
#include <vector>

/**
 * @brief An icon to look up: the parameters of XDGIconThemeManager::findIcon().
 *
 * In remote mode the lookup is done by the service, so the app doesn't need an XDGKit.
 *
 * @ingroup AssetBroker
 */
struct CZ::ABIconKey
{
    /**
     * @brief Icon names in order of preference (the first one found is used).
     *
     * findIcon()'s @c icon, with fallbacks. Absolute paths are accepted too (e.g. the Icon key
     * of some .desktop files).
     */
    std::vector<std::string> names;

    /**
     * @brief App whose icon this is (optional).
     *
     * If set, the Icon of the app's .desktop entry is tried before @ref names (which can then
     * hold fallbacks, e.g. the app id itself and generic app icons).
     */
    std::string appId;

    /// Nominal size (findIcon()'s @c size).
    Int32 size { 64 };

    /// findIcon()'s @c scale.
    Int32 scale { 1 };

    /// Acceptable files, XDGIcon::Extension flags (findIcon()'s @c extensions). SVG is preferred if both are found.
    UInt32 extensions { XDGIcon::PNG | XDGIcon::SVG };

    /// Themes to search in order, "" searches every theme (findIcon()'s @c themes).
    std::vector<std::string> themes { "" };

    /// XDGIconDirectory::Context flags (findIcon()'s @c contexts).
    UInt32 contexts { XDGIconDirectory::AnyContext };

    bool operator==(const ABIconKey &other) const noexcept = default;
};

#endif // CZ_ABICONKEY_H
