# Cross-build helper scripts (Linux host → Windows)

These build the single-file Windows wallet from a Linux box with the apt
`x86_64-w64-mingw32` toolchain. See `docs/BUILDING.md` (Windows section) for the
full procedure and prerequisites.

- `build-qtbase-win.sh` — from-source **static** Qt 5.15 cross-build (`-xplatform win32-g++`).
- `build-gui-win.sh` — cross-build `release/zclwallet.exe` against that static Qt + the
  daemon's mingw libsodium. **Does NOT embed the daemon** — that is a separate footer
  step (`ZQWDMON1`), see `docs/BUILDING.md`.

⚠️ Both scripts hard-code absolute paths (`/home/rhett/...`) for the Qt prefix, the
daemon `depends/`, and the repos. Edit those to your layout before running (or export
them). They are committed here so the procedure is reproducible, not because the paths
are portable as-is.
