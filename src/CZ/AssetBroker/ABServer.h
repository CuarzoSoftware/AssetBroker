#ifndef CZ_ABSERVER_H
#define CZ_ABSERVER_H

#include <CZ/AssetBroker/ABRasterizer.h>
#include <CZ/AssetBroker/ABProtocol.h>
#include <CZ/Core/CZEventSource.h>
#include <CZ/Core/CZTimer.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace CZ { class XDGKit; }

/**
 * @brief The service side: rasterizes assets on behalf of clients and shares them as DMA-bufs.
 *
 * Listens on ABProtocol::SocketPath() (see ABProtocol for the messages). Only processes of the
 * same user are accepted.
 *
 * Icons are looked up here (LoadIcon), with the service's own XDGKit, so apps don't need one.
 * Themes are reloaded when the indexer updates their cache (checked at most every 5 s).
 *
 * Assets are cached by file, size and device, and shared by every client. Each client's
 * references are counted; an asset no client references is kept for a grace period
 * ($CZ_ASSET_BROKER_SERVER_GRACE_MS, 10000 by default) and then released. Memory is actually
 * freed once no process has it imported either.
 *
 * Requires a CZCore and an RCore whose devices can allocate GBM buffers (e.g. GL on render nodes,
 * see ABUtils::OpenRenderNodes()).
 *
 * @ingroup AssetBroker
 */
class CZ::ABServer final
{
public:
    /**
     * @brief Starts listening. nullptr on failure (e.g. another instance is running).
     *
     * If the process was socket-activated by systemd (see cz-asset-broker.socket), the socket
     * passed is used and @p socketPath is ignored.
     */
    static std::shared_ptr<ABServer> Make(const std::filesystem::path &socketPath = {}) noexcept;

    /// Stops listening (removes the socket file) and disconnects every client.
    ~ABServer() noexcept;

    const std::filesystem::path &socketPath() const noexcept { return m_socketPath; }
    size_t clientCount() const noexcept { return m_clients.size(); }
    size_t assetCount() const noexcept { return m_assets.size(); }

private:
    struct Client;
    struct Asset;
    ABServer() noexcept = default;
    bool listen(const std::filesystem::path &path) noexcept;
    static int ActivationSocket() noexcept;
    void accept() noexcept;
    void dispatch(UInt64 clientId, UInt32 events) noexcept;
    bool handleMessage(Client &client, const char *data, size_t size) noexcept;
    bool handleLoad(Client &client, const char *data, size_t size) noexcept;
    bool handleLoadIcon(Client &client, const char *data, size_t size) noexcept;
    bool handleRelease(Client &client, const char *data, size_t size) noexcept;
    bool sendAsset(Client &client, ABProtocol::LoadReply &reply, const std::filesystem::path &path,
                   Int32 boxWidth, Int32 boxHeight, UInt64 deviceId) noexcept;
    bool sendFailure(Client &client, ABProtocol::LoadReply &reply, ABProtocol::Status status) noexcept;
    bool handleGenerate(Client &client, const char *data, size_t size) noexcept;
    bool sendReply(Client &client, ABProtocol::LoadReply &reply, Asset &asset) noexcept;
    Asset *findOrCreate(const std::string &key, RDevice *device, const std::string &what,
                        const std::function<ABRasterizer::Result(const RImageConstraints&)> &create) noexcept;
    void removeClient(UInt64 clientId) noexcept;
    void unref(UInt64 assetId, UInt32 count) noexcept;
    void prune() noexcept;

    std::filesystem::path m_socketPath;
    std::shared_ptr<CZEventSource> m_listener;
    std::unordered_map<UInt64, std::unique_ptr<Client>> m_clients;
    std::unordered_map<std::string, std::unique_ptr<Asset>> m_assets; // By key
    std::unordered_map<UInt64, Asset*> m_assetsById;
    UInt64 m_nextClientId { 1 };
    UInt64 m_nextAssetId { 1 };
    ABRasterizer m_rasterizer;
    std::shared_ptr<XDGKit> m_xdgKit;
    UInt64 m_lastThemesCheck {};
    bool m_activated { false };
    std::unordered_map<std::string, UInt64> m_desktopMisses; // App id → when its .desktop wasn't found
    std::unique_ptr<CZTimer> m_pruneTimer;
    UInt32 m_gracePeriodMs { 10000 };
};

#endif // CZ_ABSERVER_H
