// ============================================================================
// nftcommon — small shared free functions for the NFT dialogs, extracted to kill
// copy-pasted strings (translation drift) and a duplicated validator.
//
//   * nftValidateTAddrInto(status, addr) — the ONE 4-state transparent-address
//     validator (empty / not-an-address / valid t-address / valid-but-shielded)
//     that send + sell both used verbatim. Paints `status` (success-green for a
//     usable address, red otherwise) and returns true ONLY for a usable public
//     (transparent) address.
//
//   * nftPublicTradeNote() — the verbatim public-settlement honesty sentence that
//     sell + buy both showed. Single source so the wording can never drift.
//
// HONESTY (load-bearing): ZSLP ownership is ALWAYS PUBLIC (transparent UTXOs).
// These strings must never imply private/anonymous ownership. C++14 only.
// ============================================================================
#ifndef NFTCOMMON_H
#define NFTCOMMON_H

#include "precompiled.h"

#include <QString>

class QLabel;

// 4-state transparent-address validator shared by the send + sell dialogs.
// Paints `status` success-green for a usable address (red otherwise) and returns
// true ONLY when `addr` is a valid PUBLIC (transparent) t-address (the only kind the
// public ZSLP transfer/sale path can use). An empty addr clears the status and
// returns false. `notSupportedHint` lets each caller phrase the valid-but-
// shielded case in its own voice ("Private gifts…" vs "A sale needs…").
bool nftValidateTAddrInto(QLabel* status, const QString& addr,
                          const QString& notSupportedHint);

// The verbatim public-settlement honesty sentence (sell + buy). Marked for
// translation in one place.
QString nftPublicTradeNote();

// Resolve a LOCAL bytes path for an NFT whose on-chain fingerprint is
// `docHashHex` (lowercase SHA-256 hex), WITHOUT any network access — the ONE
// privacy-safe rule shared by the gallery feed and the detail/buy views.
//
// Lookup order (all local; never a remote documenturl):
//   1) the content-addressed blob store (ContentEngine::cacheGet) — bytes the
//      user already minted (cachePut) or attached + verified in the detail view;
//   2) a BUNDLED app resource whose own bytes hash to docHashHex — the wallet
//      ships a handful of sample images, so a collectible minted from one of them
//      renders from inside the app (no fetch, no leak). The map is built once and
//      hashes the :/nft resources at first use.
//
// Returns "" (empty sentinel) when no local bytes are known for that hash — the
// caller then shows its honest "image not on this computer" fallback. An empty or
// non-hex docHashHex returns "" too.
QString nftResolveLocalBytes(const QString& docHashHex);

#endif // NFTCOMMON_H
