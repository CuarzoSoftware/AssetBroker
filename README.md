# AssetBroker

> This project is part of the [Cuarzo Framework](https://github.com/CuarzoSoftware/Cuarzo). Follow the instructions there to build and install it.

Icon and image loading for Cuarzo apps. Assets are either generated in each app or, when the `cz-asset-broker` service is running, generated once by the service and shared with every app as DMA-bufs, so apps use the same GPU buffers.

## Library

```cpp
#include <CZ/AssetBroker/ABCore.h>

auto cz { CZ::CZCore::GetOrMake() };        // ABCore needs a CZCore...
// ... and an RCore (e.g. created by Marco)

auto ab { CZ::ABCore::GetOrMake() };        // ABMode::Auto by default

// The same parameters XDGIconThemeManager::findIcon() takes, plus the pixel box
CZ::ABIconKey key;
key.names = { "firefox", "application-x-executable" }; // Fallbacks in order
key.size = 64;
key.themes = { "WhiteSur", "" };
key.appId = "org.mozilla.firefox";                     // Optional: its .desktop Icon is tried first

auto image { ab->icon(key, { 128, 128 }) };              // For Ream's main device
auto other { ab->icon(key, { 128, 128 }, someRDevice) }; // For a specific DRM device

// For AKImageFrame::setImageProvider(): the icon at the size it's drawn at
frame.setImageProvider(CZ::ABCore::IconProvider(key));

// Any image file (SVG or raster), fitted in a box
auto picture { ab->image("/path/to/picture.png", { 512, 0 }) };
```

Images are shared: requesting the same icon, size and device again returns the same `RImage` while some copy is alive. When the last copy is destroyed the image is kept for a short grace period, then released.

| Mode | Behavior |
|---|---|
| `ABMode::Auto` | Uses the service if it's running; otherwise (or if it goes away) generates assets in-process. Reconnecting is retried on later requests. |
| `ABMode::Local` | Always generates assets in-process. |
| `ABMode::Remote` | Only uses the service: `GetOrMake()` fails if it's not running. |

In remote mode icons are looked up by the service, so the app doesn't load icon themes or parse SVGs.

## Service

`cz-asset-broker` runs in the user session and listens on `$XDG_RUNTIME_DIR/cz-asset-broker` (a `SOCK_SEQPACKET` Unix socket only the user can connect to). It uses every GPU's render node (no DRM master needed). See `ABProtocol.h` for the protocol.

It's started on demand by systemd (socket activation). The units are installed to `<prefix>/lib/systemd/user` (meson option `systemd_user_unit_dir`, `no` to skip). Enable the socket once:

```
$ systemctl --user daemon-reload
$ systemctl --user enable --now cz-asset-broker.socket
```

The first app that connects starts the service. It can also be run by hand (without systemd), in which case it creates the socket itself.

It keeps parsed SVGs while images rendered from them exist, counts each app's references, drops all of an app's references when it disconnects, and releases assets nobody uses after a grace period.

## Environment variables

| Variable | Default | |
|---|---|---|
| `CZ_ASSET_BROKER_MODE` | | Forces the mode of apps: `auto`, `local` or `remote` |
| `CZ_ASSET_BROKER_SOCKET` | `$XDG_RUNTIME_DIR/cz-asset-broker` | Socket path |
| `CZ_ASSET_BROKER_GRACE_MS` | `3000` | How long apps keep released images |
| `CZ_ASSET_BROKER_SERVER_GRACE_MS` | `10000` | How long the service keeps unused assets |
| `CZ_ASSET_BROKER_TIMEOUT_MS` | `2000` | How long apps wait for a reply |
| `CZ_ASSET_BROKER_LOG_LEVEL` | | Log verbosity (0-6) |

## Example

`cz-asset-broker-example [auto|local|remote] [icon names...]` loads icons at a few sizes and prints where they came from and a checksum of their pixels (equal in every mode).
