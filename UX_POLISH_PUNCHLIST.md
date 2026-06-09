# UX Polish Punch-List — zcl-qt-wallet

One prioritized, de-duplicated list to execute in order. **Honesty is non-negotiable and outranks brevity:** coin is ZCL never ZEC/Zcash; NFT ownership is ALWAYS public — only file CONTENT is private; a verified/green badge means ONLY "bytes match the on-chain fingerprint," never "genuine/official"; permanence/public consequences must never be hidden. GUI is **C++14** (no `std::optional`/`string_view`).

All paths are absolute. Copy-only fixes touch a string literal; fixes that imply C++/objectName/QSS work are flagged.

---

## A. Top 10 "Don't-Make-Me-Think" Wins (highest impact first)

1. **Settings opens on the scary tab.** `settings.ui` `currentIndex 2 → 1` so a newcomer's first view is Options, not Reindex/Rescan/"hours to days".
2. **Rewrite the always-on Send privacy badge** from a 10-word instruction to a short cue: "Add a recipient to see if this send is private" (fix the `.ui` string AND `sendtab.cpp:1502` together).
3. **Kill `zclassicd` jargon on money screens:** confirm-dialog no-peers warning + the node-stats tab title ("zclassicd" → "Advanced") + several mainwindow messages.
4. **Shorten the 4 Collections entry buttons** to 1-2 words and unify the noun: "Send file / Receive file / Buy / Make" (drop "NFT" from the visible label).
5. **Collapse the Collections empty view** (subhead + 5-line intro + state line) into ONE line: "No collectibles yet. Make one or buy one to get started." Move honesty to existing WhatsThis.
6. **Collapse the NFT mint dialog's 3 stacked helper paragraphs** into one amber public/permanence line + one grey "file never uploaded" line.
7. **Tame the NFT detail view's 11-button cluster:** promote Send/Gift + Sell (primary), demote utilities to a quiet row/More menu, merge "Re-check"/"Attach the file you have…" into one state-aware "Check my file…".
8. **De-paragraph the Settings Options/Network helpers** (Tor, tray, NAT-PMP, data-channel, shield-change, rescan/reindex): each to one short line — keep the data-channel public/permanent honesty verbatim.
9. **Replace the single-option "Expires in: ~7 days (recommended)" combo** with a read-only line — a 1-choice picker implies choices that don't exist.
10. **Make valid-address feedback GREEN consistently** — the NFT t-address validator paints a usable address amber (a warning color) while the sibling shield dialog uses green.

---

## B. Per-Surface Checklists

### Collections gallery

**P0**
- `src/mainwindow.cpp:3099-3108` (nft action-row buttons) — shorten to **"Send file" / "Receive file" / "Buy" / "Make"**. Move the "private file" honesty into each button's tooltip (shield dialogs already explain content-only privacy).
- `src/mainwindow.cpp:3105` (nftBuyBtn) — **"Buy an NFT" → "Buy"**. Use "collectible" as the only user-facing noun everywhere; drop "NFT" from visible labels.
- `src/mainwindow.cpp:3113-3137` subhead + intro + `src/rpc.cpp:3406-3414` state lines — when empty, show ONE centered line: **"No collectibles yet. Make one or buy one to get started."** Hide the subhead while the grid is empty. Keep file-stays-local / fingerprint-on-chain / new-optional-feature honesty in the subhead WhatsThis + Mint dialog. *(C++: visibility toggle + objectName.)*

**P1**
- `src/rpc.cpp:903-907` (nftUnsupportedGuidance) — **"Collectibles need ZClassic v2.1.2-beta7 or newer. Quit any other ZClassic node, then restart this wallet to use its built-in node."** (Drops the redundant "or upgrade your node".)
- `src/rpc.cpp:3406-3409` (index-off state line) — **"Collectibles tracking is off. Add the line below to your node config, then restart — the wallet catches up once."** (Points at the copyable `zslpindex=1` hint below it; "catches up once" keeps the one-time-scan honesty.)
- `src/nftgallerydelegate.cpp:66-71` **(honesty-risk, C++)** — `privacyLabel()` can still return "Private" / `privacyColor()` green though the painted pill is hardcoded "Public" (#119 rule: ZSLP ownership is always public). Make both helpers always return "Public"/neutral, **or delete them** now that the pill is hardcoded. Don't leave a "Private" path that contradicts the rendered truth.

**P2**
- `src/mainwindow.cpp:3113-3115` (nftGallerySubhead) — **"Each card's image is checked against its on-chain fingerprint."** (hide while empty; keep the green-check WhatsThis verbatim).
- `src/nftgallerymodel.cpp:68-69` (no-bytes tooltip) — **"You hold this. Open it to download and check the image."**

### NFT create / detail

**P0**
- `src/nftmintdialog.cpp:84-111` (m_visLabel + honest + permanence) — collapse 3 stacked labels into TWO:
  - Line 1 (m_visLabel, amber): **"Public — name, collection and fingerprint go on the public ledger permanently and can't be removed."**
  - Line 2 (grey, replaces both other labels): **"Your file is never uploaded — only its fingerprint is recorded."**
  - Drop the "Private coming soon" parenthetical from the always-visible line.
- `src/nftdetaildialog.cpp:169-174` (footnote) — **remove the footnote label** (the badge WhatsThis already says "anyone can mint a copy that reuses the same picture"). Fold the unique-id nuance into the mint-id line: `m_mintId` → **"Mint id: %1 (the one unique part)"**.
- `src/nftdetaildialog.cpp:178-234` (11 buttons across 4 rows) **(visual/flow, C++)** — establish hierarchy: (1) Send/Gift + Sell primary on top row, Send/Gift `setDefault(true)`/accent; (2) demote Save image / Copy id / Copy fingerprint / Re-check into one compact row or a "⋯ More" menu; (3) keep the shield pair (Send file privately / Open private file) as its own labeled row; (4) merge Re-check + Attach (see P1 below). Target: ≤2 prominent buttons + a tidy utility row.

**P1**
- `src/nftdetaildialog.cpp:194 + 225` **(make-you-think, C++)** — one state-aware verify button: bytes on disk → **"Re-check"**; no bytes → **"Check my file…"** (replaces "Attach the file you have…"; "Attach" wrongly implies upload). Set the text in `requestPoster()`; only one is ever shown.
- `src/nftdetaildialog.cpp:457-471` (no-bytes badge) — keep the verdict line; trim the WhatsThis to **"Use Check my file to verify it against the on-chain fingerprint — nothing is uploaded."**

**P2**
- `src/nftmintdialog.cpp:74` (Collection label) + `src/nftdetaildialog.cpp:306-308` (m_setLine) **(consistency)** — use **"Collection"** everywhere; change m_setLine to **"Collection: %1" / "Collection: none"**. Do not introduce "Set".
- `src/nftmintdialog.cpp:78-80` (Link) — label **"Link (optional)"**, placeholder **"https://… (a hint — we never open it)"** (one caveat, one place).
- `src/nftmintdialog.cpp:265` (mint success) — **"Created — it'll appear once confirmed."**
- `src/nftdetaildialog.cpp:312-315` (fingerprint WhatsThis) — **"A unique fingerprint of the original file, recorded on-chain — the wallet recomputes it from your copy to confirm it's the same file."** (drops SHA-256/one-way jargon).
- `src/nftdetaildialog.cpp:207-210` (m_sendFileBtn WhatsThis) — lead with the load-bearing honesty: **"Encrypts the file's CONTENTS so only your recipient can read it. Ownership stays public — only the file is private. The encrypted file is stored on-chain permanently."**
- `src/nftdetaildialog.cpp:434` (mismatch verdict) **(honesty-adjacent — do NOT soften)** — **"This image does NOT match the on-chain fingerprint — it's not the recorded file."** (red color + ✗ already signal danger).

### NFT trade / shield

**P0**
- `src/shieldsenddialog.cpp:37-42` (honesty banner) — split the 5-line wall into:
  - Lead (normal): **"Only this file's contents are encrypted — just the person you send it to can open it."**
  - Caveat (grey, smaller): **"It does NOT hide who owns an NFT — that's always public. The encrypted file is stored on every node forever; it can never be deleted, only kept unreadable."**
- `src/nftselldialog.cpp:79-88` (Expires-in combo) **(make-you-think, C++)** — drop the single-item combo; show read-only **label "Expires in" + value "About 7 days."** Reintroduce the combo only when real custom-expiry lands.
- `src/nftcommon.cpp:27-28` (valid t-address line) **(consistency, C++)** — keep the words "Looks good — a public (transparent) address." but paint it **GREEN (#34c759)** to match the shield dialog and Listed/Verified states. (See visual-system note: standardize on #34c759, not #2a9d2a, for success text.)

**P1**
- `src/nftbuydialog.cpp:120` (overpayment ack) — **"I understand any extra I pay goes to the network, not the seller."** (keeps the honest "extra isn't refunded / not to seller"; drops jargon).
- `src/nftcommon.cpp:38-40` (nftPublicTradeNote) — **"This trade is public — the price and both addresses are visible on the ledger."** (drops the abstract "negotiation can be private").

**P2**
- `src/shieldsenddialog.cpp:181-182` (too-large error) — **"Too large: %1 bytes. The limit is 40,000."**
- `src/shieldsenddialog.cpp:68-120` (capNote + changeNote + leakNote) **(flow, C++)** — keep the cap note at the picker; move the metadata-leak line under the banner as part of the honesty block (or demote to a tooltip on "Send privately"); drop the standalone changeNote. Goal: one honesty block up top, one cap hint at the picker, then a clean form.
- `src/nftbuydialog.cpp:212-213 vs 269-273` (color) **(consistency, C++)** — paint the in-progress "Checking this offer…/Checking…" states NEUTRAL grey (#9aa0a6); reserve amber (#d9822b) for the "⚠ Don't pay" refusal only; green stays for "✓ Verified".
- `src/shieldreceivedialog.cpp:47-50` (listNote) — **"Sent this session only — not a full receive history."**

### Core tabs (Home / Send / Receive)

**P0**
- `src/mainwindow.ui:249` (sendPrivacyBadge default) **AND** `src/sendtab.cpp:1502` — **both** to **"Add a recipient to see if this send is private"**. (Same string in two places; change together to stay in sync.)
- `src/confirm.ui:155` (nopeersWarning) — **"No network peers found — check your internet. This send may not go through."** (Drops "zclassicd"; one actionable line.)

**P1**
- `src/mainwindow.ui:881` (tab_5 title `<string>zclassicd</string>`) **(consistency)** — **"Advanced"** to match the nav-rail label.
- `src/confirm.ui:168` (syncingWarning) — **"Still catching up with the network — this send may fail. Best to wait until syncing finishes."**
- `src/mainwindow.cpp:3956-3960` (advCaption, Receive) — **"⚠ Transparent (t) addresses are PUBLIC — anyone can see the balance. Use a shielded (z) address for privacy."** (drops the redundant Sprout sentence).
- `src/mainwindow.ui:782` (Sprout security-announcement HTML) **(honesty-risk / ZCL-not-Zcash)** — **"Legacy Sprout addresses are deprecated. Receive to a shielded (z) Sapling address instead."** Drop the z.cash link (gated pre-2.0.4 only); if a link is wanted, point to the ZClassic repo, never z.cash.

**P2**
- `src/sendtab.cpp:1507-1513` (privacy badge verdicts) — **"● Private" / "● Shielding → private" / "● Public — visible to everyone" / "● De-shield — becomes public forever"** (keep the two load-bearing tails; color tone carries the rest).
- `src/mainwindow.ui:808` (receive caption "Your address") — drop it (subhead at `mainwindow.cpp:3914` already labels it); if kept, "Address".
- `src/mainwindow.cpp:926-928` (importText) — **"Importing your keys and rescanning (about 10–20 min). Your balance updates when it's done — keep the app open."**
- `src/mainwindow.ui:469` (memo button) — **"Add note"** (note privacy follows the send's privacy, shown by the badge).
- `src/mainwindow.ui:749` (Sprout radio) — **"Legacy Sprout (z)"** (adds the missing space; aligns with the "t-Addr (PUBLIC)" pattern).
- `src/mainwindow.cpp:878-880` (homeBackupText) — **"Back up your wallet. There's no recovery phrase — lose this computer with no backup and your coins are gone forever."** (consequence preserved).

### Settings / first-run / connect

**P0**
- `src/settings.ui:28-30` (tabWidget currentIndex) — **`2 → 1`** (open on Options, not Troubleshooting). Better: reorder so Options is index 0 and Advanced last.
- `src/settings.ui:33` (tab title) — **"zclassicd connection" → "Advanced"**, and add a top QLabel near confMsg (line 37): **"Most people never need this. ZClassic runs its own node for you automatically. Only change these to connect to a node you run yourself."** Leave Host/Port/RPC fields untouched. *(C++/.ui: new QLabel.)*
- `src/settings.ui:222` (label_tray) — **"Closing the window keeps ZClassic in the tray so it reopens instantly. Quit fully from the tray icon or File > Exit."**
- `src/settings.ui:239` (label_natpmp) — **"Asks your router to let other peers reach you, strengthening the network. Uses safe NAT-PMP/PCP (never UPnP); if your router doesn't support it, nothing happens. Default: off."** (keep "never UPnP" + "off by default").

**P1**
- `src/settings.ui:263` (chkDataChannel helper) **(honesty — keep both clauses verbatim)** — trim ONLY the config mechanic: **"Lets you send and receive files whose contents are private (encrypted). It does NOT make NFT ownership private — who holds a token is always public — and the encrypted file is stored on every node permanently and can never be deleted. Restart to apply. Default: off."** Do NOT shorten further.
- `src/settings.ui:205` (label_7, shield-change) — **"Sends your change to a shielded (private) address instead of a public one. More private."**
- `src/settings.ui:168` (label_5, remember-tx) — **"Saves private transactions locally so they show in the Transactions tab."** (remove the double space + the inverse restatement).
- `src/settings.ui:232` (NAT-PMP checkbox label) — **"Help the network (open my port)"** (protocol detail lives in label_natpmp).
- `src/settings.ui:338` (rescan) + `src/settings.ui:375` (reindex) — rescan: **"Re-checks the blockchain for missing wallet transactions and corrects your balance. Takes a while; restart to apply."** reindex: **"Rebuilds the blockchain from scratch. Can take a long time; restart to apply."** (drop the "hours to days, depending on hardware" dramatization).

**P2**
- `src/settings.ui:151` (lblTor) — **"Routes your connection through Tor for more anonymity. You must install and run Tor yourself."**
- `src/settings.ui:286` (custom-fees) — **"Lets you set your own fee when sending. Custom fees are public and may reduce your privacy."** (drop trailing space).
- `src/createzclassicconfdialog.ui:14` (window title) — **"Configure zclassic.conf" → "Set up ZClassic"**; make the advanced-toggle text consistent with `connection.cpp` ("Advanced settings" / "Hide advanced settings").
- `src/connection.cpp:46-48` (first-run intro) — **"Your first run downloads the blockchain and security files, then catches up with the network. This takes a while but happens only once — you can leave it running and come back later."**
- `src/about.ui:20/52` — de-duplicate: drop the redundant body description (title already says it) OR shorten the title to "ZclWallet". Keep the license. Low priority.

### Global wordiness (other surfaces)

**P2**
- `src/mainwindow.cpp:1555-1556` (waiting-for-peers long stretch) — **"Still no peers — please check your internet connection."** (drop "try again later"; the app retries automatically).
- `src/sendtab.cpp:90/107` (feeHint) — **"Default network fee. Change it in Settings > Allow custom fees."**
- `src/sendtab.cpp:734-737` and `:939-942` (coinbase-must-shield) — **"Some of these are mined (coinbase) coins, which must be shielded to a private address before sending.\n\nUse 'Shield my public funds' first, then send again."** (keep the shield-first requirement).
- `src/turnstile.cpp:120` — title "Locked funds"/body — **"Can't migrate yet. Your funds may be unconfirmed, or the balance is too low to migrate automatically."**
- `src/addressbook.cpp:128` — title **"Invalid address"**; body **"That isn't a valid ZClassic address."** (address on its own line).
- `src/confirm.ui:194` (custom-fee warning) **(consistency)** — text **"Custom fee set. Fees are public, so this gives up some privacy."** AND change the static `color: red;` to **`color: #d9822b;`** (amber). Reserve red strictly for the de-shield quadrant.
- `src/mainwindow.cpp:1926` (delete-saved-shielded confirm) — **"Your private (shielded) transaction history is saved on this computer only. Delete it now?"** (drops "zclassicd").
- `src/mainwindow.cpp:2159` (rescan/reindex restart) — **"ZClassic will close now. Reopen it to finish the rescan."**
- `src/mainwindow.cpp:2426` (memo-not-added) — **"No memo added — memos only work with shielded (z) addresses, not public (t) ones."**
- `src/rpc.cpp:2944` (update-available prompt) — **"Update available: v%1 (you have v%2). Open the releases page?"**
- `src/connection.cpp:256-257/457` (first-run download size) — **"One-time ~1.7 GB download (about 10–20 min on home internet)."** (also worth deduping the two call sites to one string).

---

## C. Visual-System Recommendation

The central `res/styles/dark.qss` is an excellent, documented token system (pt type scale; semantic colors green=private `#1f7a1f`/text `#34c759`, amber=public `#d9822b`, red=de-shield-only `#c0392b`; 8px spacing grid). **The newest, most safety-sensitive surfaces bypass it entirely**, so fix three things by routing the NFT (mint/buy/sell/send/detail) and shield (send/receive) dialogs through `objectName` + QSS instead of inline `setStyleSheet`:
1. **Kill px fine print.** ~11 labels use `font-size:11px` (~8pt — the smallest text in the app) and it carries the honesty-critical copy (permanence, "does NOT upload", privacy-leak). Add one rule `QLabel[hint="true"] { color:#9aa0a6; font-size:12pt; }` (pt, never px) at `nftdetaildialog.cpp:98,173`, `nftmintdialog.cpp:99,110`, `shieldsenddialog.cpp:70,110,119,324`, `shieldreceivedialog.cpp:51`, `mainwindow.cpp:3138`.
2. **Collapse four greens to two.** ~15 inline `#2a9d2a` "verified/success" greens contradict the system (which reserves `#2a9d2a` for accent-HOVER and specifies `#34c759` for success text). Replace all success/verified/listed text with **`#34c759`** so verified-green equals the private badge/hero; confine `#2a9d2a` to hover.
3. **Stop hardcoding tokens.** Route hairline `#2a2d35`, inset `#1d2027`, dim `#9aa0a6` through `objectName` + QSS so future token changes don't silently skip these dialogs. Also: detail-dialog **title** is `16px` (~12pt) — smaller than the 14pt body — give it `QLabel#nftDetailTitle { font-size:18pt; font-weight:700; }` to match `nftGalleryHeading`; and fix `confirm.ui` custom-fee `color: red` → amber (red reserved for de-shield only). **Recommended set:** type scale 11/12/13/14/16/18/40 pt (never px); green=private, amber=public, red=de-shield-only; 8px spacing grid.
