# ZClassic Wallet

A private, single-file ZClassic wallet — with a full ZClassic node built right in. One download, no setup, no command line. For **Windows, macOS, and Linux**.

> **New here?** Download the one file for your computer below, open it, and wait for the progress bar. That's the whole setup.

![ZclWallet — your balance, shielded and transparent](docs/screenshot-wallet.png?raw=true)
![Friendly first-run setup](docs/screenshot-firstrun.png?raw=true)

## Download & run

**Latest release: v2.1.2-beta5** — always grab whatever GitHub marks as **Latest**:
**https://github.com/ZclassicCommunity/zcl-qt-wallet/releases/latest**

Download the **one** file for your computer and open it:

| Your computer | Download | First-time open |
|---|---|---|
| **Windows** | `zclwallet-v2.1.2-beta5-win64.exe` | Double-click. If you see **"Windows protected your PC"**, click **More info → Run anyway**. |
| **macOS** (Apple Silicon) | `macOS-zclwallet-v2.1.2-beta5.dmg` | Open the `.dmg`, drag **ZclWallet** out, then **right-click the app → Open**. On **macOS 15+**, if there's no "Open" button, go to **System Settings → Privacy & Security → Open Anyway**. _(This is normal for community apps.)_ |
| **Linux** | `linux-zclwallet-v2.1.2-beta5` | `chmod +x linux-zclwallet-v2.1.2-beta5 && ./linux-zclwallet-v2.1.2-beta5` — or right-click → Properties → *Allow executing as program*, then double-click. _(glibc ≥ 2.29; no Qt install needed.)_ |

## What happens on first launch

The wallet starts its built-in node, which automatically downloads its security files and a recent copy of the blockchain (a few GB) from the network — all checked for integrity, with a progress bar. Depending on your connection, this can take a little while. When it finishes, your wallet opens. **That is the entire setup** — there's nothing to download or configure by hand.

Your coins live in `wallet.dat`. The app only ever **backs it up** — it never modifies it.

## Your privacy

ZClassic supports **shielded z-addresses**, which keep your balances and transfers private.

- The **Receive** tab gives you a shielded **z-Sapling** address by default.
- The **Balance** tab shows your **Shielded** and **Transparent** funds separately, plus the **Total** — so you always know what's private and what's public.

The tabs are: **Balance · Send · Receive · Transactions · zclassicd**.

## Verify your download

Each release includes `SHA256SUMS.txt`. To check a file matches:

- **Linux:** `sha256sum <file>`
- **macOS:** `shasum -a 256 <file>`
- **Windows:** `certutil -hashfile <file> SHA256`

Compare the result against the matching line in `SHA256SUMS.txt`.

## Run a node only

The wallet runs its own embedded `zclassicd` automatically — most people never think about it. If you already run a ZClassic node, the wallet connects to that instead.

To run a **node only** (no GUI, e.g. on a server), download `zclassicd` from the [zclassic release](https://github.com/ZclassicCommunity/zclassic/releases/latest) and run it — **no flags or config needed**. It fetches the params and a snapshot and syncs by itself.

_(To run the wallet itself without the GUI, pass `--headless`.)_

## Build from source

This wallet is **C++14 + Qt 5.15**. Full, copy-pasteable instructions for all three platforms (native + cross-compile, single-file packaging, code-signing) are in:

- **`docs/BUILDING.md`** — the wallet (GUI + single-file).
- **`AGENTS.md`** — the cross-repo build quickstart (also in the [zclassic](https://github.com/ZclassicCommunity/zclassic) node repo, alongside `doc/building-daemon-from-source.md`).

Quick Linux dev build:

```
git clone https://github.com/ZclassicCommunity/zcl-qt-wallet.git
cd zcl-qt-wallet
/path/to/qt5/bin/qmake zcl-qt-wallet.pro CONFIG+=release && make -j$(nproc)
```

Building from source produces the GUI without an embedded node by default — run an external `zclassicd`, or use `contrib/make-allinone.sh` to embed one. See `docs/BUILDING.md`.

## Help

[File an issue](https://github.com/ZclassicCommunity/zcl-qt-wallet/issues) for support or bug reports.

_ZclWallet is a community wallet and is not affiliated with the Electric Coin Company._
