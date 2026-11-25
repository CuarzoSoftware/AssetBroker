#ifndef CZ_ABREMOTEBACKEND_H
#define CZ_ABREMOTEBACKEND_H

#include <CZ/AssetBroker/ABBackend.h>
#include <CZ/AssetBroker/ABIconKey.h>
#include <CZ/AssetBroker/ABGenKey.h>
#include <CZ/AssetBroker/ABProtocol.h>
#include <CZ/Core/CZEventSource.h>
#include <CZ/Core/CZSpFd.h>
#include <memory>
#include <vector>

/**
 * @brief Gets images from the cz-asset-broker service, imported from DMA-bufs.
 *
 * Requests are synchronous (the reply is waited for, with a timeout of
 * CZ_ASSET_BROKER_TIMEOUT_MS, 2000 by default). The DMA-buf fds are closed once imported.
 *
 * Each image holds a reference in the service, released (in batches, on the next event loop
 * iteration) when the image's Result::keepAlive is destroyed. All of them are released if the
 * connection closes, but already imported images stay valid.
 *
 * @ingroup AssetBroker
 */
class CZ::ABRemoteBackend final : public ABBackend, public std::enable_shared_from_this<ABRemoteBackend>
{
public:
    /// Connects to the service. nullptr if it's not running or the handshake fails. Requires a CZCore.
    static std::shared_ptr<ABRemoteBackend> Connect() noexcept;

    ~ABRemoteBackend() noexcept;

    /// Fails (empty Result) if the service couldn't load it or the connection is lost (see connected()).
    Result load(const std::filesystem::path &path, SkISize box, RDevice *device, const ABShadow *shadow = nullptr) noexcept override;

    /// Looks up and rasterizes an icon in the service. Fails like load().
    Result loadIcon(const ABIconKey &key, SkISize box, RDevice *device, const ABShadow *shadow = nullptr) noexcept;

    /// Asks the service to draw a generated asset. Fails like load().
    Result generate(const ABGenKey &key, RDevice *device) noexcept;

    bool remote() const noexcept override { return true; }

    /// Status of the last request (Failed also if the connection was lost).
    ABProtocol::Status lastStatus() const noexcept { return m_lastStatus; }

    /// False once the connection is lost (the backend can't be reused).
    bool connected() const noexcept { return m_source != nullptr; }

private:
    struct Reference;
    ABRemoteBackend() noexcept = default;
    bool handshake() noexcept;
    Result request(const void *message, size_t size, UInt64 serial, RDevice *device, const std::string &what) noexcept;
    bool waitReply(void *data, size_t size, std::vector<CZSpFd> &fds) noexcept;
    void disconnect() noexcept;
    void queueRelease(UInt64 assetId) noexcept;
    void flushReleases() noexcept;
    int fd() const noexcept { return m_source ? m_source->fd() : -1; }

    std::shared_ptr<CZEventSource> m_source; // Owns the socket
    UInt64 m_serial {};
    std::vector<UInt64> m_pendingReleases;
    bool m_flushScheduled { false };
    ABProtocol::Status m_lastStatus { ABProtocol::Status::Ok };
};

#endif // CZ_ABREMOTEBACKEND_H
