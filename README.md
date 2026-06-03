ZClassic wallet — a z-addr-first, Sapling-compatible wallet with a **full ZClassic node built in**, for **Linux, Windows, and macOS**. One self-contained file: no separate node to install, no config, no command line.

![Screenshot](docs/screenshot-main.png?raw=true)
![Screenshots](docs/screenshot-sub.png?raw=true)

# Download & run

Grab the latest release: **https://github.com/ZclassicCommunity/zcl-qt-wallet/releases/latest**

Download the **one** file for your computer and open it:

| Your computer | Download | First-time open |
|---|---|---|
| **Windows** | `zclwallet-…-win64.exe` | Double-click. If you see **"Windows protected your PC"**, click **More info → Run anyway**. |
| **macOS** (Apple Silicon) | `macOS-zclwallet-…dmg` | Open the `.dmg`, drag **ZclWallet** out, then **right-click the app → Open**. On **macOS 15+** if there's no "Open" button, go to **System Settings → Privacy & Security → Open Anyway**. (Signed ad-hoc, not Apple-notarized.) |
| **Linux** | `linux-zclwallet-…` | `chmod +x linux-zclwallet-… && ./linux-zclwallet-…` — or right-click → Properties → *Allow executing as program*, then double-click. (glibc ≥ 2.29; no Qt install needed.) |

On the **first launch** the wallet automatically downloads the zk-security-parameters and a blockchain snapshot (a few GB) from the network, showing a progress bar, then opens to your wallet. **That's the entire setup.** Your coins live in `wallet.dat`, which the app only ever *backs up* — never modifies.

Verify a download against `SHA256SUMS.txt`: `sha256sum <file>` (Linux), `shasum -a 256 <file>` (macOS), `certutil -hashfile <file> SHA256` (Windows).

# The node (`zclassicd`)

The wallet runs its **embedded** `zclassicd` automatically — you don't need to install or configure anything. If you already run a ZClassic node, the wallet connects to it instead. To run a **node only** (no GUI, e.g. a server), download `zclassicd` from the [zclassic release](https://github.com/ZclassicCommunity/zclassic/releases/latest) and run it — **no flags or config needed**; it fetches params + a snapshot and syncs by itself.

Pass `--headless` to run the wallet without the GUI.

# Build from source

This wallet is C++17 + Qt 5.15. Full, current, copy-pasteable build instructions for **all three platforms** (native + cross-compile, the single-file packaging, code-signing notes) are in:

- **`docs/BUILDING.md`** — the wallet (GUI + single-file).
- **`AGENTS.md`** — the cross-repo build quickstart (also in the [zclassic](https://github.com/ZclassicCommunity/zclassic) node repo, with `doc/building-daemon-from-source.md`).

Quick Linux dev build:
```
git clone https://github.com/ZclassicCommunity/zcl-qt-wallet.git
cd zcl-qt-wallet
/path/to/qt5/bin/qmake zcl-qt-wallet.pro CONFIG+=release && make -j$(nproc)
```
(Building from source produces the GUI without an embedded node by default — run an external `zclassicd`, or use `contrib/make-allinone.sh` to embed one. See `docs/BUILDING.md`.)

# Help

[File an issue](https://github.com/ZclassicCommunity/zcl-qt-wallet/issues) for support or bug reports.

_ZclWallet is a community wallet and is not affiliated with the Electric Coin Company._
