# v2.1.2-beta6 — release readiness checklist

Single source of truth for what's left, who owns it, and status. Cut from canonical `04dba01`.
Scope (decided): **RC + Send-form redesign (#21)**.

Legend: ✅ done · 🔵 in progress · ⏳ blocked/waiting · ⬜ not started

## MUST (release blockers)

| # | Item | Owner | Status |
|---|------|-------|--------|
| 1 | **Version bump** GUI `version.h` + `Info.plist` → beta6 | me | ✅ PR #22 |
| 2 | **Send-form redesign** merged into beta6 candidate | me | ✅ PR #21 → folded into `release/v2.1.2-beta6` |
| 3 | **Reproducible + tagged daemon** | me/dev | 🔵 **daemon PR ZclassicCommunity/zclassic#119** (committed the uncommitted changes, junk excluded, `configure.ac` 4→5; getwalletsummary+NAT-PMP+param-presence). Needs CI build+gtest + the parallel-rescan decision, then merge + `git tag v2.1.2-beta6` |
| 4 | **Live funded `t→z` send-with-change** (3 cases; change lands shielded; shown==sent) | **you** | ⏳ see `BETA6_LIVE_SEND_TEST.md` |
| 5 | **Bob fresh-install end-to-end** (start → connect → balance → first send) on a clean machine | you/bob | 🔵 Send form already confirmed launching on bob@Arch |
| 6 | **Linux** beta6 single-file (version-stamped) | me | ✅ `zclwallet-v2.1.2-beta6-linux-x86_64` sha 20ebb6c6 |
| 7 | **Windows** single-file `.exe` (beta6 GUI; built qtsvg cross-module; embeds win daemon) | me | ✅ `zclwallet-v2.1.2-beta6-win64.exe` sha 38b0ee56 — note: win daemon is pre-getwalletsummary (graceful fallback); refresh for parity when daemon is tagged |
| 8 | **macOS** arm64 `.dmg` — `macdeployqt` + ad-hoc re-sign | **Mac dev** | ⬜ instructions ready: `docs/BETA6_MACOS_HANDOFF.md` (note: their static Qt needs qtsvg too) |
| 9 | **Windows runtime smoke-test** (fresh install + daemon-extract + sync) | you/tester | ⬜ |
| 10 | **GUI master merge** (ff `feature/wallet-redesign` → master, *after* version bump) + **daemon master merge** | dev | ⬜ |
| 11 | **SHA256SUMS + GPG signatures + release notes**, then tag + publish pre-release | me/dev | 🔵 notes drafted (`RELEASE_NOTES_v2.1.2-beta6.md`) |

## SHOULD

| Item | Owner | Status |
|------|-------|--------|
| UI_DIR=src build-hygiene (the shipped binary built clean — `distclean` between app/L1) | me | ✅ release builds do `rm -f src/ui_*.h` + fresh uic |
| fontfix confirmed on a 2nd distro (Fedora 39+ **and** a proprietary-GPU box) | you/tester | 🔵 Arch confirmed; Fedora/NVIDIA pending |
| Mark beta3/beta4 GitHub pre-releases as superseded | dev | ⬜ |
| Commit the proot portable-builder into a repo (reproducible Linux release path) | me/dev | ⬜ |
| Manual smoke of the 4-tier cookie/`/proc` RPC-auth pipeline | you/tester | ⬜ |
| Confirm coinbase-excluded-max daemon-reject message is calm (not scary) | me | ⬜ |

## NICE (don't let these delay)
- NAT-PMP honest-reachability spot-check (only if mentioned in notes; default-OFF).
- Don't advertise "279ms" as cold-start (it's a warm GUI-attach mark).
- GUI startup feel-wins (instant-cached-frame, faster-attach-poll) → fold into a later bundle.

## DEFERRED to beta7+
Parallel-Sapling-rescan engine; daemon-side `z_sendmany` input-pinning (full TOCTOU close);
param-fetch parallelization; growable-v3 bootstrap finish; deferred Send polish (human-readable
From-combo, per-recipient remove, QR-scan, confirm-dialog badge); Receive-form redesign.

---
**Critical path:** #3 (dev tags daemon) → #6/#7 rebuild from tagged daemon → #4/#5/#9 live gates →
#10 master merge → #11 sign + publish. Everything else is parallelizable.
