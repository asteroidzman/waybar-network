# waybar-network

<p align="center"><img src="assets/icon.png" width="128" alt="waybar-network icon"></p>

A [waybar](https://github.com/Alexays/Waybar) **CFFI plugin** for network status:
a wifi-strength / ethernet / disconnected icon (+ wifi signal%) in the bar, and a
click-through popover with connection details, live throughput and a wifi list.

## Features

- **Bar pill:** signal-graded wifi glyph + signal%, or an ethernet / disconnected
  icon.
- **Click → popover:** SSID, signal, IP, live ↓/↑ throughput, a Wi-Fi toggle, and
  a scannable list of networks — click one to connect (password prompt appears for
  secured networks).
- State via `nmcli`; throughput from `/proc/net/dev`. Refresh default 3s.

## Build & install

Arch Linux: `yay -S waybar-network` (AUR).

Requires `gtk3`, `glib2` (+dev headers), `NetworkManager` (`nmcli`) and a C compiler.

```sh
make
make install                 # → ~/.local/lib/waybar/libnetwork.so
```

## waybar config

```jsonc
"modules-right": ["cffi/network"],

"cffi/network": {
    "module_path": "/home/YOU/.local/lib/waybar/libnetwork.so",
    "interval": 3
}
```

| key | default | meaning |
|-----|---------|---------|
| `module_path` | *(required)* | path to `libnetwork.so` |
| `interval` | 3 | state/throughput refresh seconds |
| `icon-size` | 26 | bar icon pixel size |
| `icon-dir` | `$XDG_DATA_HOME/waybar-network` | dir holding the wifi/ethernet SVGs (installed by `make install`) |

The bar shows crafted image icons: signal-graded wifi (`wifi1`–`wifi3`), an
ethernet plug, and a disconnected state.

## style.css

Bar: `#network` (state class `.connected` / `.disconnected`) with `.nw-icon` /
`.nw-label`. Popover: `.nw-pop`, `.nw-head`, `.nw-key`, `.nw-val`, `.nw-thru`,
`.nw-wifi` (`.nw-active` on the current network).

## License

MIT
