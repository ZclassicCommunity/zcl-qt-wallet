# beta6 — macOS build instructions (for the Mac developer)

The Linux and Windows beta6 builds are produced by the maintainer. **macOS must be built on a
Mac** — please produce a `ZclWallet.app` / `.dmg` for **v2.1.2-beta6**. The version strings are
already bumped on the branch; you just build + sign.

> macOS is structurally different from Linux/Windows: **no embedded daemon**. The signed
> `zclassicd` ships as a **sibling** inside `ZclWallet.app/Contents/MacOS/` (Gatekeeper/hardened
> runtime forbid running a runtime-extracted executable). The committed `mkmacdmg.sh` handles this.

## What to check out
- Repo: `git@github.com:ZclassicCommunity/zcl-qt-wallet.git`, branch **`release/v2.1.2-beta6`**
  (it already has `APP_VERSION` = `2.1.2-beta6` in `src/version.h` and `CFBundleVersion` =
  `2.1.2-beta6` in `res/Info.plist`).
- Daemon repo: `git@github.com:ZclassicCommunity/zclassic.git` — build the **beta6 daemon** (see
  note below about which commit, coordinate with the daemon owner; functionally it must answer
  `getwalletsummary`, i.e. the `be6f0031d`-era daemon).

## Prereqs (Mac)
- Xcode command-line tools (`clang`, `codesign`, `xcrun`)
- A **static Qt 5.15** for macOS (path passed via `-q`)
- `brew install create-dmg`

## Build (3 commands)
```bash
# 1) macOS daemon (native)
cd /path/to/zclassic && ./zcutil/build.sh -j$(sysctl -n hw.ncpu)
#    -> src/zclassicd and src/zclassic-cli  (mkmacdmg.sh expects them there)

# 2) the .app + .dmg, version-stamped beta6
cd /path/to/zcl-qt-wallet && git checkout release/v2.1.2-beta6
src/scripts/mkmacdmg.sh -q /path/to/static/Qt -z /path/to/zclassic -v 2.1.2-beta6
```
`mkmacdmg.sh` runs `qmake CONFIG+=release` → `make` → copies `zclassicd`+`zclassic-cli` into
`Contents/MacOS/` → `macdeployqt` → **ad-hoc codesign (now automated, in the right order)** →
`create-dmg` with an `hdiutil` fallback.

## ⚠️ The two things that bite everyone
1. **Sign AFTER `macdeployqt`, not before.** `macdeployqt` rewrites the Qt frameworks and
   invalidates any earlier signature → the app launches on Intel but is **instantly SIGKILLed on
   Apple Silicon**. The script now signs in the correct order automatically — don't add an earlier
   sign step.
2. **`QT += svg` is required** (the new UI uses SVG icons). Make sure your static Qt has the
   **qtsvg** module, or the build fails with `Unknown module(s) in QT: svg`. (Linux/Windows hit
   this too — qtsvg must be in the static Qt.)

## Signing reality
- No Apple Developer ID here → the script does an **ad-hoc** sign (`-s -`). That fixes the Apple
  Silicon SIGKILL but does **not** notarize, so Gatekeeper still says "unidentified developer."
  Users **right-click → Open** once, or `xattr -dr com.apple.quarantine /Applications/ZclWallet.app`.
- If you have a Developer ID: replace `-s -` with your identity, add `--options runtime`, then
  `xcrun notarytool submit` + `stapler staple`. Your call — tell me if you'll notarize so I word
  the download page accordingly.

## Verify before sending it back
```bash
codesign --verify --deep --strict ZclWallet.app && spctl -a -vv ZclWallet.app
```
Then test on a **clean Mac** (or fresh user): open → connects → balance shows → a small send works.
Send me the `.dmg` + its `sha256` and whether it's ad-hoc or notarized, and I'll fold it into the
beta6 release with the Linux + Windows artifacts.
