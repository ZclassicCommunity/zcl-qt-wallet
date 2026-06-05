# v2.1.2-beta6 — release readiness checklist

Single source of truth for what's left, who owns it, and status. Cut from canonical `04dba01`.
Scope (decided): **RC + Send-form redesign (#21)**.

Legend: ✅ done · 🔵 in progress · ⏳ blocked/waiting · ⬜ not started

## MUST (release blockers)

| # | Item | Owner | Status |
|---|------|-------|--------|
| 1 | **Version bump** GUI `version.h` + `Info.plist` → beta6 | me | ✅ PR #22 |
| 2 | **Send-form redesign** merged into beta6 candidate | me | ✅ PR #21 → folded into `release/v2.1.2-beta6` |
| 3 | **Reproducible + tagged daemon** (commit the 16 uncommitted changes; `configure.ac` build 4→5; tag `v2.1.2-beta6` pinned to `be6f0031d`, drop parallel-rescan) | **dev** | ⏳ see `BETA6_DAEMON_HANDOFF.md` |
| 4 | **Live funded `t→z` send-with-change** (3 cases; change lands shielded; shown==sent) | **you** | ⏳ see `BETA6_LIVE_SEND_TEST.md` |
| 5 | **Bob fresh-install end-to-end** (start → connect → balance → first send) on a clean machine | you/bob | 🔵 Send form already confirmed launching on bob@Arch |
| 6 | **Linux** beta6 bundle (proot builder, version-stamped) | me | 🔵 building (builder bumped to beta6) |
| 7 | **Windows** `.exe` rebuilt from `04dba01` + tagged daemon (currently STALE) | me/dev | ⬜ after daemon tag |
| 8 | **macOS** arm64 `.dmg` (ABSENT) — `macdeployqt` + ad-hoc re-sign | **Mac dev** | ⬜ |
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
