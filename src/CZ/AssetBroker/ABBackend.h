#ifndef CZ_ABBACKEND_H
#define CZ_ABBACKEND_H

#include <CZ/AssetBroker/ABRasterizer.h>

/**
 * @brief Where ABCore gets images from: in-process (ABLocalBackend) or the service (ABRemoteBackend).
 *
 * @ingroup AssetBroker
 */
class CZ::ABBackend
{
public:
    using Result = ABRasterizer::Result;

    virtual ~ABBackend() noexcept = default;

    /**
     * @brief Rasterizes @p path fitted in @p box for @p device (never nullptr).
     *
     * @p path must be canonical. Result::keepAlive must be held as long as the image.
     */
    virtual Result load(const std::filesystem::path &path, SkISize box, RDevice *device) noexcept = 0;

    /// Whether images come from the service.
    virtual bool remote() const noexcept = 0;
};

#endif // CZ_ABBACKEND_H
