# ZClassic Wallet

A private, single-file ZClassic wallet — with a full ZClassic node built right in. One download, no setup, no command line. For **Windows, macOS, and Linux**.

> **New here?** Download the one file for your computer below, open it, and wait for the progress bar. That's the whole setup.

![ZclWallet — your balance, shielded and transparent](docs/screenshot-wallet.png?raw=true)
![Friendly first-run setup](docs/screenshot-firstrun.png?raw=true)

## Download & run

**Latest release: v2.1.2-beta7** — always grab whatever GitHub marks as **Latest**:
**https://github.com/ZclassicCommunity/zcl-qt-wallet/releases/latest**

Download the **one** file for your computer and open it:

| Your computer | Download | First-time open |
|---|---|---|
| **Windows** | `zclwallet-v2.1.2-beta7-win64.exe` | Double-click. If you see **"Windows protected your PC"**, click **More info → Run anyway**. |
| **macOS** (Apple Silicon) | `macOS-zclwallet-v2.1.2-beta7.dmg` | Open the `.dmg` and drag **ZclWallet** to Applications. On first launch macOS says **"Apple could not verify … is free from malware"** — that's expected (the app is signed but not yet notarized). See **[Opening on macOS](#opening-on-macos)** just below. |
| **Linux** | `linux-zclwallet-v2.1.2-beta7` | `chmod +x linux-zclwallet-v2.1.2-beta7 && ./linux-zclwallet-v2.1.2-beta7` — or right-click → Properties → *Allow executing as program*, then double-click. _(glibc ≥ 2.29; no Qt install needed.)_ |

### Opening on macOS

The first time you open the wallet, macOS shows **"Apple could not verify "ZclWallet" is free from malware that may harm your Mac."** This is **normal** for a community app that isn't notarized by Apple — the download is fine. Do this once:

- **macOS 14 Sonoma / 15 Sequoia:** double-click the app, click **Done**, then open **System Settings → Privacy & Security**, scroll down to *Security*, and click **Open Anyway** next to the ZclWallet message → confirm with Touch ID / password.
- **macOS 13 and earlier:** **right-click (Control-click) the app → Open → Open.**
- **Any version (Terminal):** `xattr -dr com.apple.quarantine /Applications/ZclWallet.app`, then open it normally.

After the first time, it opens like any other app. _(A future build will be notarized so this prompt goes away.)_

## What happens on first launch

The wallet starts its built-in node, which automatically downloads its security files and a recent copy of the blockchain (a few GB) from the network — all checked for integrity, with a progress bar. Depending on your connection, this can take a little while. When it finishes, your wallet opens. **That is the entire setup** — there's nothing to download or configure by hand.

Your coins live in `wallet.dat`. The app only ever **backs it up** — it never modifies it.

## Your privacy

ZClassic supports **shielded z-addresses**, which keep your balances and transfers private.

- The **Receive** tab gives you a shielded **z-Sapling** address by default.
- The **Balance** tab shows your **Shielded** and **Transparent** funds separately, plus the **Total** — so you always know what's private and what's public.

The tabs are: **Balance · Send · Receive · Transactions · zclassicd**.

## NFTs / Collectibles (dev/testnet stage)

The wallet has a native **Collections** tab for ZClassic NFTs — 100% built-in, **no web browser**, no marketplace, no external site. Everything renders and verifies locally.

- **Browse what you own.** The Collections gallery shows each NFT this wallet holds, with its picture, name, and a public/private pill — fed straight from your built-in node.
- **Mint from a file.** "Make a collectible" lets you drag in any image (or other file), give it a name, and create a 1-of-1 NFT. Your file **never leaves your computer** — only a fingerprint (a SHA-256 of the file) goes on-chain. Minting is public and permanent.
- **Verify the image.** Every card shows a badge that checks the picture against its on-chain fingerprint:
  - ✓ **"This image matches its on-chain fingerprint."** — the bytes you have are exactly what was minted.
  - ✗ **"This image does NOT match what was recorded on-chain."** — don't trust it.
  - ? **Checking / Image not downloaded.** — the wallet never auto-fetches a remote file (that would leak your IP and interest); you supply the bytes.

- **Send / gift a collectible.** Open any collectible and choose **Send / Gift** to transfer it to a friend's address. The wallet refuses to send an item whose picture fails its on-chain check, and an ordinary send never accidentally spends (burns) a collectible's carrier output.

The badge means only "these bytes match the on-chain fingerprint" — **never** genuine / official / original. The name and image aren't unique (anyone can mint another reusing them); only the mint id is one of a kind. NFTs ride a **non-consensus overlay** (ZSLP) that the node re-derives from the chain — a forgery can be mined but credits nobody. Treat the feature as **dev/testnet-stage**, not mainnet-ready. (The node's NFT index is on by default; for the daemon/CLI side and the full design, see the [zclassic node repo](https://github.com/ZclassicCommunity/zclassic) → `doc/nft/`.)

Two more collectible features — **private files/NFTs** over the shielded data channel (SHIELD) and **selling** a collectible for ZCL via an atomic swap (SELL) — are built on the node today (dev/testnet) and driven from the command line; native wallet screens for them are coming. For the CLI walkthroughs see the zclassic node repo → `doc/nft/` (start with `NATIVE_NFT_GUIDE.md`) and the `qa/zslp/` regtest scripts.

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
