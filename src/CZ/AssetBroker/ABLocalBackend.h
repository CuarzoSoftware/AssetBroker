#ifndef CZ_ABLOCALBACKEND_H
#define CZ_ABLOCALBACKEND_H

#include <CZ/AssetBroker/ABBackend.h>

/**
 * @brief Generates images in-process.
 *
 * @ingroup AssetBroker
 */
class CZ::ABLocalBackend final : public ABBackend
{
public:
    Result load(const std::filesystem::path &path, SkISize box, RDevice *device) noexcept override
    {
        return m_rasterizer.load(path, box, device);
    }

    Result generate(const ABGenKey &key, RDevice *device) noexcept
    {
        return m_rasterizer.generate(key, device);
    }

    bool remote() const noexcept override { return false; }

private:
    ABRasterizer m_rasterizer;
};

#endif // CZ_ABLOCALBACKEND_H
