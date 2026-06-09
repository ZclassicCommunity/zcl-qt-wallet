# Beta7 GUI Release Candidate Handoff

Status: GUI release-prep handoff. This is not a shipped release note.

Daemon candidate:

- Repository: `/home/rhett/github/zclassic-2`
- Branch: `origin/beta7/zmarket-spider-router-index`
- Current pushed head: `b0e20626ba491055fefb06d712d8ad2cd3bb2ea5`
- Base master before beta7: `14a83d510ffd109d3fa09bf74ebf8c28854a263f`
- Master dry-run fast-forward has passed, but master has not been pushed.

## Safe GUI Commit Set

The GUI tree currently has unrelated modified source files and untracked object
files. Do not sweep them into a release commit accidentally.

Safe beta7 planning/safety files:

- `docs/BETA7_TOR_ZMARKET_ZNAM_GUI_PLAN.md`
- `docs/BETA7_AI_ASSISTANT_GUI_PLAN.md`
- `docs/BETA7_RELEASE_CANDIDATE_GUI.md`
- `contrib/check-ai-denylist.sh`

Do not include `test_build.o`, `test_fprintf.o`, or broad source changes unless
they are reviewed as part of the GUI implementation itself.

## Required GUI Gates

Before beta7 GUI release:

1. Merge or otherwise pin the daemon candidate that passed full daemon build and
   gtests.
2. Build the daemon from the final beta7 daemon head.
3. Embed/package that daemon into the GUI all-in-one artifact.
4. Run `contrib/check-ai-denylist.sh`.
5. Run `git diff --check` on the exact GUI changes being shipped.
6. Confirm the production GUI hides or dev-gates legacy private file transport;
   beta7 file delivery is off-chain verified mirror fetch only.
7. Build portable Linux through `/home/rhett/zclbuild` using the proot pipeline,
   not a host glibc build.
8. Run the real-display matrix from `/home/rhett/zclbuild/focal/build/run.sh all`.
9. Verify Linux footer magic `ZQWDMON1`, glibc floor, no stray Qt dynamic deps,
   and sha sidecar.
10. Build Windows with the stripped daemon manually embedded using the same
    footer layout.
11. Build macOS with the daemon as an app sibling, then codesign after
    `macdeployqt` and before DMG creation.

## Beta7 GUI Product Boundaries

- GUI is a thin client for daemon-owned Tor/ZMARKET/ZNAM state.
- GUI must not implement a peer spider, router, market indexer, or offer DB.
- GUI must not auto-download or auto-host NFT files.
- Mirror downloads are user-initiated, quarantined, verified by hash/Merkle root,
  then moved into local verified cache.
- Mirror hosting is explicit allowlist-only.
- ZMARKET buy/sell remains gated by daemon offer verification and existing wallet
  confirmation dialogs.
- AI is disabled by default, stores provider credentials only in secure storage
  or session memory, and cannot sign, broadcast, publish, host, unhost, post, or
  change settings.

## Release Blockers To Clear

- Final daemon candidate must get one clean full build/gtest pass after the latest
  consolidation commits.
- GUI source changes must be reviewed separately from docs/safety-gate changes.
- Legacy on-chain file-transfer UI must be hidden or release-build gated.
- AI implementation, if included in beta7, must pass the denylist and prompt
  injection tests described in `docs/BETA7_AI_ASSISTANT_GUI_PLAN.md`.
- All published asset hashes must be recomputed from the gated build artifacts,
  not copied from older beta5/beta6 release notes.
