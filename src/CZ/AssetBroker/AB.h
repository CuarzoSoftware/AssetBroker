#ifndef CZ_AB_H
#define CZ_AB_H

#include <CZ/Core/Cuarzo.h>

/**
 * @defgroup AssetBroker AssetBroker
 * @brief Shared icon and asset loading, optionally served across processes.
 *
 * Apps load icons and images through ABCore. Depending on the ABMode, the assets are either
 * generated in-process (ABMode::Local) or requested from the cz-asset-broker service
 * (ABMode::Remote), which hands them out as DMA-bufs so every app shares the same GPU buffers.
 * ABMode::Auto (the default) uses the service when available and falls back to local loading.
 */

namespace CZ
{
    class ABCore;
    class ABRasterizer;
    class ABBackend;
    class ABLocalBackend;
    class ABRemoteBackend;
    class ABServer;
    struct ABIconKey;
    struct ABGenKey;

    /**
     * @brief Where assets are generated.
     *
     * @ingroup AssetBroker
     */
    enum class ABMode
    {
        /// Use the service if available, otherwise (or if it goes away) generate assets in-process.
        Auto,

        /// Always generate assets in-process.
        Local,

        /// Only use the service. ABCore::GetOrMake() fails if it's not reachable.
        Remote
    };
};

#endif // CZ_AB_H
