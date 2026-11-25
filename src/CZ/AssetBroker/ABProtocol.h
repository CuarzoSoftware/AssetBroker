#ifndef CZ_ABPROTOCOL_H
#define CZ_ABPROTOCOL_H

#include <CZ/AssetBroker/AB.h>
#include <CZ/Core/CZSpFd.h>
#include <filesystem>
#include <optional>
#include <vector>

/**
 * @brief Wire protocol between the library (ABRemoteBackend) and the service (ABServer).
 *
 * Transport: a SOCK_SEQPACKET Unix socket (message boundaries are kept, fds travel with
 * SCM_RIGHTS). Every message starts with an Op. Requests are answered in order, and the server
 * never sends unsolicited messages.
 *
 * - Hello      (client → server): first message, carries the protocol version.
 * - HelloReply (server → client): the server's version. The client disconnects if it differs.
 * - Load       (client → server): rasterize a file fitted in a box (with an optional shadow, see
 *                                  ABShadow), for a DRM device.
 *                                  Followed by the path bytes (not NUL terminated).
 * - LoadIcon   (client → server): look up an icon (the parameters of
 *                                  XDGIconThemeManager::findIcon(), see ABIconKey) and rasterize
 *                                  it fitted in a box, for a DRM device. Followed by the app id,
 *                                  the names and then the themes, each NUL terminated.
 * - Generate   (client → server): draw a generated asset (see ABGenerators), for a DRM device.
 *                                  Followed by the generator name and the strings (each NUL
 *                                  terminated), then the ints (Int64 each).
 * - LoadReply  (server → client): the result (of Load, LoadIcon or Generate). On success its fds are the DMA-buf planes,
 *                                  plus a sync_file (write fence) if hasFence. Each successful
 *                                  LoadReply adds a reference to assetId for this client.
 * - Release    (client → server): drops one reference per id. Followed by the ids.
 *
 * All the references of a client are dropped when it disconnects.
 *
 * @ingroup AssetBroker
 */
namespace CZ::ABProtocol
{
    inline constexpr UInt32 Version { 2 }; // 2: shadows (Shadow in Load and LoadIcon)
    inline constexpr size_t MaxMessageSize { 8192 };
    inline constexpr size_t MaxPathSize { 4096 };
    inline constexpr Int32  MaxBoxSize { 4096 };
    inline constexpr int    MaxFds { 5 }; // 4 planes + fence

    enum class Op : UInt32
    {
        Hello = 1,
        HelloReply,
        Load,
        LoadReply,
        Release,
        LoadIcon,
        Generate
    };

    enum class Status : Int32
    {
        Ok = 0,
        Failed,             ///< The file couldn't be read or rasterized
        UnsupportedDevice,  ///< The server has no such DRM device
        InvalidRequest,
        NotFound,           ///< No icon found (LoadIcon)
        UnknownGenerator    ///< The service doesn't have that generator (Generate)
    };

    struct Hello
    {
        Op op { Op::Hello };
        UInt32 version { Version };
    };

    struct HelloReply
    {
        Op op { Op::HelloReply };
        UInt32 version { Version };
    };

    /// An optional drop shadow (see ABShadow), part of Load and LoadIcon.
    struct Shadow
    {
        Int32 enabled {};       ///< 0: no shadow (the other fields are ignored)
        Int32 padding {};
        float radius {};
        float offsetX {};
        float offsetY {};
        UInt32 color {};
    };

    struct Load
    {
        Op op { Op::Load };
        UInt32 pathSize {};
        UInt64 serial {};
        UInt64 device {};       ///< dev_t of the DRM device the image is for
        Int32 boxWidth {};      ///< Fit the image in this box (0: unconstrained)
        Int32 boxHeight {};
        Shadow shadow {};
    };

    struct LoadIcon
    {
        Op op { Op::LoadIcon };
        UInt32 payloadSize {};  ///< Bytes of names + themes that follow
        UInt64 serial {};
        UInt64 device {};       ///< dev_t of the DRM device the image is for
        Int32 boxWidth {};      ///< Fit the image in this box (0: unconstrained)
        Int32 boxHeight {};
        Int32 size {};          ///< ABIconKey fields
        Int32 scale {};
        UInt32 extensions {};
        UInt32 contexts {};
        UInt32 nameCount {};
        UInt32 themeCount {};
        Shadow shadow {};
    };

    inline constexpr size_t MaxIconPayloadSize { MaxMessageSize - sizeof(LoadIcon) };

    struct Generate
    {
        Op op { Op::Generate };
        UInt32 payloadSize {};  ///< Bytes of name + strings + ints that follow
        UInt64 serial {};
        UInt64 device {};       ///< dev_t of the DRM device the image is for
        UInt32 intCount {};
        UInt32 stringCount {};
    };

    inline constexpr size_t MaxGeneratePayloadSize { MaxMessageSize - sizeof(Generate) };

    struct LoadReply
    {
        Op op { Op::LoadReply };
        Status status { Status::Failed };
        UInt64 serial {};
        UInt64 assetId {};
        Int32 width {}, height {};
        UInt32 format {};
        Int32 planeCount {};
        UInt64 modifier {};
        UInt32 offset[4] {};
        UInt32 stride[4] {};
        Int32 hasFence {};
        Int32 padding {};
    };

    struct Release
    {
        Op op { Op::Release };
        UInt32 count {};
    };

    inline constexpr size_t MaxReleaseIds { (MaxMessageSize - sizeof(Release)) / sizeof(UInt64) };

    /// @p shadow (nullptr: none) for a message.
    Shadow ToWire(const ABShadow *shadow) noexcept;

    /// A received shadow. False if it's invalid (e.g. a negative padding). @p out is empty if not enabled.
    bool FromWire(const Shadow &wire, std::optional<ABShadow> &out) noexcept;

    /// $CZ_ASSET_BROKER_SOCKET if set, otherwise $XDG_RUNTIME_DIR/cz-asset-broker. Empty if neither is set.
    std::filesystem::path SocketPath() noexcept;

    /// Sends one message (and @p fdCount fds). Doesn't take ownership of the fds.
    bool Send(int socket, const void *data, size_t size, const int *fds = nullptr, int fdCount = 0) noexcept;

    /**
     * @brief Receives one message.
     *
     * @param fds Receives the attached fds (with O_CLOEXEC).
     * @return The message size, 0 if the peer disconnected, -1 on error (EAGAIN included).
     */
    ssize_t Recv(int socket, void *data, size_t capacity, std::vector<CZSpFd> &fds) noexcept;

    /// The op of a received message, or 0 if it's too short.
    Op PeekOp(const void *data, size_t size) noexcept;
};

#endif // CZ_ABPROTOCOL_H
