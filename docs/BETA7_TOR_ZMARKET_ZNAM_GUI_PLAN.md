# v2.1.2-beta7 GUI plan: Tor, ZMARKET, ZNAM, NFT file mirrors

Engineering plan for the wallet GUI. This is not a release note and not user
documentation.

## Scope

Beta7 adds a native, privacy-aware path for market discovery, names, and NFT
file discovery without changing the core NFT safety rule already present in the
wallet: a displayed file is local bytes that match the recorded fingerprint.
It also leaves room for an optional AI assistant that can explain wallet state
and draft actions. The AI assistant plan is `docs/BETA7_AI_ASSISTANT_GUI_PLAN.md`
and must follow the same approval, privacy, and no-automatic-hosting rules.

Work stays GUI-side where possible, with daemon/RPC assumptions called out
explicitly. The current wallet already has useful primitives:

- `ContentEngine` streams local files, computes SHA-256 and chunked Merkle
  descriptors, rejects remote URLs, and writes only verified local blobs to the
  content cache.
- `RPC::refreshNFTs()` deliberately ignores `documenturl` and feeds gallery rows
  with an empty `cachePath`, so the gallery never auto-fetches an NFT image.
- `NftMintDialog` records only the file fingerprint on-chain and says the file is
  never uploaded.
- `NFTSellDialog` and `NFTBuyDialog` use daemon offer verification before a buy
  can proceed; editing an offer invalidates the green state.
- Collection dialogs already frame collections as public, permanent,
  authorization-gated ledger state.
- Tor is currently opt-in for the embedded daemon through `proxy=127.0.0.1:9050`;
  update/price requests are clear-net surfaces that should be skipped under Tor
  unless they move to a dedicated privacy-aware client.

## Non-Negotiables

- **No files on-chain.** Beta7 must not send plaintext, ciphertext, thumbnails,
  avatars, market media, or mirrored file bytes through ZClassic transactions or
  Sapling memos. On-chain data may include token ids, offer ids, names, hashes,
  Merkle roots, signatures, and compact pointers, but not file bytes.
- **No automatic remote fetch.** Opening Collections, ZMARKET, ZNAM, or a detail
  dialog must not issue a remote HTTP/Tor/IPFS/mirror request for arbitrary
  content. Discovery may fetch small signed indexes only when the corresponding
  feature is enabled; file bytes require an explicit user action.
- **No automatic hosting.** The GUI must not host wallet caches, minted files, or
  arbitrary filesystem paths. Node operators opt in and explicitly choose each
  file to host.
- **Verify before display.** Remote content is never shown from downloaded bytes
  until its bytes match the expected hash/Merkle root and the containing metadata
  or pointer signature verifies. Failed verification stays a terminal refusal,
  not a yellow preview.
- **Keep `ContentEngine` network-free.** New remote code downloads into a
  quarantine/temp path, verifies there, then hands a local path to `ContentEngine`
  only after the file is eligible to enter the verified blob cache.
- **AI cannot bypass approvals.** AI output may summarize and draft actions, but
  cannot sign, broadcast, publish listings, host/unhost content, post social
  records, or change settings without an explicit GUI approval and the existing
  wallet confirmation path.

## Required Pivot From Current Data-Channel UI

The current SHIELD data-channel UI and RPC wrappers describe encrypted file bytes
stored on-chain forever. That conflicts with beta7's hard "no files on-chain"
requirement.

Beta7 must therefore do one of these before shipping:

1. Remove the "Send private file" / "Receive private file" entry points from the
   production Collections UI and Settings.
2. Or hide them behind a clearly named legacy/dev flag that cannot be enabled in
   the beta7 release build.

Do not extend `z_senddatafile`, `z_listdatatransfers`, or `z_getdatatransfer` for
beta7 file delivery. The replacement is off-chain verified mirror fetch and
explicit mirror hosting.

## Product Model

### Tor

Tor remains opt-in because the wallet does not bundle Tor. The GUI should add a
small network status model for remote beta7 features:

- `Tor off`: remote market/name/mirror actions are available only after the user
  accepts clear-net disclosure for that action or enables Tor.
- `Tor configured and reachable`: remote ZMARKET/ZNAM/mirror discovery uses a
  dedicated remote-content network client with SOCKS proxy settings.
- `Tor configured but unreachable`: remote actions are disabled with a direct
  "Tor is not running at 127.0.0.1:9050" line. Never fall back to clear-net.

Do not proxy the existing localhost RPC `QNetworkAccessManager`; that would break
node RPC. Add a separate `RemoteContentClient` or equivalent for all non-localhost
market/name/mirror traffic.

Under Tor, skip legacy update and price refresh unless they are moved to the same
dedicated remote client and respect the same no-fallback policy.

### NFT File Mirror Hosting

Mirrors are an off-chain availability layer for files whose hashes are already
known from NFT metadata, ZMARKET offers, or ZNAM records.

GUI screens:

- **NFT detail:** keep the current "image isn't on this computer" state. Add
  "Find mirrors..." and "Host this file..." actions only when the NFT has a
  fingerprint.
- **Find mirrors:** shows signed mirror manifests and mirror endpoints. It does
  not fetch file bytes automatically. The user selects a mirror and clicks
  "Download and verify".
- **Host this file:** opens a local file picker, streams the descriptor, verifies
  it matches the NFT fingerprint, then adds it to a hosted-file allowlist only
  after explicit confirmation.
- **Settings > File mirrors:** opt-in toggle, hosted-file list, add/remove files,
  per-file hash, size, last-served status, bandwidth cap, and Tor/onion status.

Module plan:

- `MirrorManifest`: parse canonical JSON/CBOR manifest, enforce size limits,
  reject path traversal, reject embedded file bytes, and expose `{hash,
  merkle_root, size, mime, filename, urls, signer, signature}`.
- `MirrorVerifier`: verifies manifest signature, expected signer policy, byte
  hash/Merkle root, and optional chunk proofs.
- `MirrorFetchJob`: user-initiated download to quarantine; streams hash as bytes
  arrive; moves to `ContentEngine::blobCacheDir()` only after verification.
- `MirrorHostModel`: local allowlist of explicitly hosted files. Source paths
  are user-picked local files; records persist by hash and path, never by scanning
  wallet caches.
- `MirrorHostSettingsPage`: GUI for opt-in hosting and the allowlist.

Daemon/RPC assumptions:

- The daemon may expose a mirror server only when a mirror flag is enabled and an
  allowlist file is present.
- The allowlist contains hash, size, path, optional MIME, and host policy. The
  daemon must serve only allowlisted hashes.
- The GUI owns allowlist edits for the embedded daemon. With an external node, it
  shows instructions and does not pretend it can change that node.

### ZMARKET

ZMARKET is a high-performance decentralized marketplace compiled from the local
node's spidering/routing/indexing of signed NFT offer records. That engine is a
daemon-side C hot path, not Qt application code. There is no central market
server, no privileged publisher, and no GUI-trusted index. The local node may
discover offers from peers, relays, mirrors, and operator-chosen sources; the GUI
treats every discovered row as untrusted until signed-offer verification and
daemon offer verification pass.

The current `znftoffer` buy/sell flow stays the transaction authority. ZMARKET
adds discovery, routing status, index health, mirror health, and fast local
querying over signed offers without weakening the existing Buy gate.

Ownership boundary:

- **Daemon C engine:** owns spidering, routing, peer/relay protocol, signed-offer
  parsing, signature verification, local market index storage, pruning,
  rate-limits, scoring, mirror-health probes, and high-performance query
  execution.
- **GUI Qt code:** owns screens, user consent, operator policy toggles, display
  models, pagination/search controls, and calls to daemon RPC or a local bridge.
  It must not implement or own the market indexer, router, peer spider, relay
  protocol, or hot query engine.
- **Bridge contract:** GUI calls bounded RPC/local-bridge methods such as market
  status, query page, verify offer, publish selected offer, set operator policy,
  rebuild index, and mirror health. Responses are already bounded/pageable.

GUI screens:

- **ZMARKET tab:** Buy, Sell, Listings, Local Index, Operator.
- **Buy view:** search/filter/sort local indexed offers; cards show signed-offer
  state, daemon verification state, media/mirror state, price, expiry, seller
  identity/name status, and whether content is local or needs an explicit fetch.
- **Sell view:** create a signed offer from a held NFT using the current sell
  flow, choose whether to publish/serve/relay it, and show propagation/indexing
  status after creation.
- **Listings view:** the user's own listed offers, imported offers, watched
  offers, cancelled/expired offers, and relay/publish status.
- **Local Index view:** indexed offers count, last spider pass, peers contacted,
  routing table health, signature reject count, expired-pruned count, mirror
  health summary, disk size, and "rebuild local market index" action.
- **Operator view:** explicit opt-in controls for serving offers, relaying offers,
  spidering peers, maintaining the local offer index, serving mirror manifests,
  and hosting selected file bytes.
- **Listing row/card:** token id/name, price, expiry, verification state, media
  state ("not on this computer", "verified local", "download available").
- **Offer drawer:** raw offer id/blob, daemon verification result, seller payout,
  buyer address requirement, expiry, reasons when invalid, and public trade note.
- **Buy flow:** unchanged hard gate: Buy enabled only after `nft_verifyoffer`
  returns ok and the user acknowledges the fee/overshoot line.

Module plan:

- `ZMarketModel`: thin Qt model over daemon query pages with state flags:
  `Unverified`, `VerifiedOffer`, `InvalidOffer`, `Expired`, `MediaVerified`,
  `MediaMissing`. It stores UI rows only; it is not an index.
- `ZMarketIndexStatusModel`: status model for local spider/routing/indexing:
  `enabled`, `serving`, `relaying`, `spidering`, `indexed_count`,
  `verified_count`, `rejected_count`, `last_spider_at`, `last_error`,
  `mirror_healthy_count`, `mirror_failing_count`, and disk usage.
- `ZMarketNodeAdapter`: daemon-backed RPC/local-bridge adapter for local market
  queries, signed-offer verification, publish/relay controls, index rebuild, and
  health. It is the only Qt boundary to the C market engine.
- `ZMarketSourcePolicy`: GUI-side policy for enabling local indexing, spidering,
  relaying, and serving. Default off for operator actions; viewing a local index
  can be on only after the user enables ZMARKET.
- `OfferVerifier`: adapter over existing `RPC::nftVerifyOffer`; every visible
  listing reaches a daemon-verified state before Buy is enabled.
- `MarketMediaGate`: integrates mirror manifests with listing cards, but never
  auto-downloads media.

Do not add a Qt `ZMarketIndexer`, Qt peer spider, Qt routing table, Qt offer DB,
or Qt background thread that owns market indexing. Qt tests may use fake RPC
pages, but production indexing belongs to the daemon C engine.

Local market index rows are hints, not truth. The GUI never trusts an indexed
price, owner, expiry, media hash, or status until the signed offer verifies and
the daemon confirms the offer is buyable. Invalid signatures are visible only as
diagnostics in Local Index/Operator views, never as buyable listings.

Operator controls:

- **Serve offers:** publish this wallet's selected signed offers to peers. Off by
  default. Creating an offer does not automatically serve it.
- **Relay offers:** relay third-party signed offers. Off by default. Relayed
  offers are bounded by size, expiry, signature validity, and rate limits.
- **Spider offers:** ask peers/mirrors for signed offers to index locally. Off by
  default until ZMARKET is enabled. No media bytes are fetched by spidering.
- **Maintain local index:** store the signed offer index locally for fast search.
  The index stores offer records and verification metadata, not NFT file bytes.
- **Serve mirror manifests/files:** separate from offer serving and still governed
  by the mirror allowlist. Serving offers never serves files.

Performance requirements:

- Local Buy/List views query the local node index, not a central API.
- The high-throughput spider/router/index/query path runs in C inside the daemon.
  The GUI requests bounded pages/cursors and never scans the full index itself.
- Initial ZMARKET paint must be bounded by local index query time; slow spidering
  continues in the background and updates the status panel.
- Search/filter/sort should be executed by the daemon/local bridge against the
  C-owned index and remain responsive at tens of thousands of offers.
- Expired and invalid offers are pruned or hidden by default, with diagnostics
  available in Local Index.

### ZNAM

ZNAM is the signed name/profile layer for people, collections, mirrors, and market
publishers. It should make names easier to use without making them authority by
appearance.

GUI screens:

- **ZNAM search/detail:** name, owner key/address, resolved records, status badge,
  expiry/height if available, and raw fingerprint/signature fields.
- **Owned names:** names controlled by this wallet, renewal/update actions, and
  publishing key management.
- **Name use in Collections/ZMARKET:** display a verified-name badge only when the
  ZNAM record signature and on-chain ownership/anchor both verify.

Module plan:

- `ZnamRecord`: canonical record format containing name, owner identifier,
  signing key, record hash, payload hash, expiry, and signature.
- `ZnamResolver`: daemon-backed resolver with local cache; never returns rich
  remote content directly, only verified records and content pointers.
- `ZnamVerifier`: canonical-name normalization, signature verification, expiry
  checks, and binding checks between the name owner and signing key.
- `ZnamBadgeDelegate`: compact display states: `Verified`, `Expired`,
  `Signature mismatch`, `Name unavailable`, `Unverified`.

ZNAM payloads such as avatars, collection banners, profile text, mirror endpoint
lists, and publisher metadata follow the mirror rules: signed pointers first,
explicit user fetch for file bytes, hash verification before display.

## Screen and Navigation Plan

- **Settings > Network:** current embedded-node Tor checkbox becomes a clearer Tor
  status section: installed/reachable test, SOCKS host/port, "Use Tor for remote
  market/name/mirror lookups", and "Never fall back to clear-net" default on.
- **Settings > File mirrors:** new section for mirror hosting. Default off. Shows
  "No files hosted" until the operator adds files. The enable toggle alone hosts
  nothing.
- **Collections detail:** preserve current local-only rendering. Add mirror
  actions below "Check my file..." so local verification remains the primary path.
- **ZMARKET:** add a top-level tab when the feature is enabled. It contains Buy,
  Sell, Listings, Local Index, and Operator views. Empty state should say there
  is no central market server; the local node builds the market from signed
  offers it discovers, serves, relays, or indexes after opt-in.
- **ZNAM:** add as a search/manage dialog first, then promote to a tab only if it
  becomes a frequent workflow. Avoid crowding the current wallet nav rail before
  the product surface is proven.

## Implementation Phases

1. **Stop on-chain file paths.** Hide or remove production SHIELD data-channel
   entry points. Add denylist tests for new beta7 flows to ensure they never call
   `z_senddatafile` or put file bytes into a transaction/memo payload.
2. **Remote content foundation.** Add manifest parser, signature verifier,
   quarantine download job, and `RemoteContentClient` with Tor policy. Keep
   `ContentEngine` unchanged except for any needed public helper reuse.
3. **Mirror hosting.** Add allowlist model and Settings page. Embedded node writes
   allowlist/config; external node shows manual guidance. Host nothing until files
   are picked and verified.
4. **Mirror fetch from NFT detail.** Add "Find mirrors..." and "Download and
   verify" flow. Display remains unchanged until verification succeeds.
5. **ZMARKET local node index.** Add thin Qt models for Buy/Sell/List views,
   daemon RPC/local-bridge adapter for the C spider/router/index status and
   queries, and operator opt-in for serving, relaying, and indexing signed
   offers. Wire existing buy/sell verification gates into the browseable market
   screen.
6. **ZNAM resolver and badges.** Add record verification, local cache, search
   dialog, and verified badges for market publishers/collections/mirrors.

## Tests

### L0 Unit

- `MirrorManifest` rejects oversized manifests, embedded bytes, bad JSON, missing
  hash, non-hex hashes, path traversal filenames, duplicate URLs, and unsupported
  schemes.
- `MirrorVerifier` rejects bad signatures, wrong signing key, wrong hash, wrong
  Merkle root, wrong size, and chunk proof mismatches.
- `RemoteContentPolicy` never falls back to clear-net when Tor is required, and
  disables remote actions when the SOCKS listener is unreachable.
- `ZnamVerifier` covers canonicalization, homograph/rejected-character cases,
  expired records, wrong owner binding, and bad signatures.
- `ZMarketModel` state transitions never make Buy enabled from index data alone,
  and never require a GUI-owned indexer.
- `ZMarketIndexStatusModel` formats disabled/enabled/serving/relaying/spidering
  states, reject counts, disk usage, last spider time, and mirror health without
  needing a daemon.
- `ZMarketSourcePolicy` defaults serving, relaying, spidering, and file hosting
  to off; enabling one does not imply enabling the others.
- Adapter tests assert GUI requests bounded pages/cursors from the daemon bridge
  and does not scan a full market index in Qt.
- Denylist: beta7 mirror/ZMARKET/ZNAM code must not call `z_senddatafile` or
  serialize file bytes into RPC payloads.

### L1 Widget

- Opening Collections and NFT detail with a `documenturl` present issues no remote
  GET and shows the current "image isn't on this computer" state.
- "Find mirrors..." lists manifests but does not download bytes until "Download
  and verify" is clicked.
- A verified mirror download turns the detail badge green only after hash and
  signature checks pass; a mismatch stays not displayed and does not enter the
  blob cache.
- "Host this file..." requires a file picker and rejects a file whose descriptor
  does not match the NFT fingerprint.
- Enabling mirror hosting with an empty allowlist hosts nothing and says so.
- ZMARKET listing cards stay unbuyable until daemon offer verification returns ok;
  editing/replacing the offer invalidates the state.
- Buy/Sell/List view tabs remain responsive while Local Index reports spidering
  in progress.
- Operator toggles show independent consent for serving own offers, relaying
  third-party offers, spidering/indexing offers, and serving mirrored files.
- Turning on offer serving does not turn on file hosting, and turning on indexing
  does not turn on relaying.
- ZNAM badges show verified only for records with valid signatures and current
  bindings.

### L2/E2E

- Mock mirror server: valid manifest + valid bytes downloads only after click,
  verifies, enters cache, and then displays.
- Mock mirror server: valid manifest + tampered bytes never displays and never
  enters cache.
- Mock market index: index fetch can populate rows, but no media GET happens until
  a user action.
- Mock decentralized market node: local spider/index status updates the ZMARKET
  Local Index view; Buy/List queries come from local indexed records, not a
  central URL.
- Fake bridge pagination: Buy/List views request page/cursor results and render
  incrementally; no Qt test fixture creates or drives a production market indexer.
- Operator opt-in: serving, relaying, indexing, and mirror hosting persist as
  separate flags; all default off on a fresh profile.
- Signed-offer routing: bad signatures are counted in index health and never
  appear as buyable cards; valid signatures still require daemon offer
  verification before Buy.
- Tor-required mode with no SOCKS listener disables remote market/name/mirror
  actions and performs no clear-net request.
- Embedded mirror hosting: adding one verified file writes exactly one allowlist
  entry; removing it stops serving it. No wallet cache scan occurs.
- External node mode: mirror-hosting controls are disabled or explanatory, and the
  GUI does not mutate the external node's config.

## Key Decisions

- Remote files are availability only; authenticity is hash/Merkle plus signature.
- On-chain state remains compact and non-file: hashes, names, signatures, offer
  commitments, and pointers only.
- Discovery is allowed to be remote only through explicit feature enablement and
  Tor policy; media bytes still require an explicit per-file user action.
- Node operators are publishers only for files they explicitly choose and verify.
- Existing offer verification remains the buying authority. ZMARKET indexes are
  decentralized local-node discovery hints.
- ZMARKET has no central party. The market view is compiled from signed offers
  discovered, routed, served, relayed, and indexed by the user's local node under
  explicit operator policy.
- The decentralized marketplace spider/router/indexer is daemon-side C hot-path
  code. GUI Qt code is a thin RPC/local-bridge client and must not own the market
  indexer.
- Serving offers, relaying offers, spidering/indexing offers, and hosting files
  are separate opt-ins. None implies another.
- ZNAM names are labels with cryptographic status, not trust by typography.
