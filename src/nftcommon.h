// ============================================================================
// nftcommon — small shared free functions for the NFT dialogs, extracted to kill
// copy-pasted strings (translation drift) and a duplicated validator.
//
//   * nftValidateTAddrInto(status, addr) — the ONE 4-state transparent-address
//     validator (empty / not-an-address / valid t-address / valid-but-shielded)
//     that send + sell both used verbatim. Paints `status` (green/amber/red) and
//     returns true ONLY for a usable public (transparent) address.
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
// Paints `status` with the matching green/amber/red copy and returns true ONLY
// when `addr` is a valid PUBLIC (transparent) t-address (the only kind the
// public ZSLP transfer/sale path can use). An empty addr clears the status and
// returns false. `notSupportedHint` lets each caller phrase the valid-but-
// shielded case in its own voice ("Private gifts…" vs "A sale needs…").
bool nftValidateTAddrInto(QLabel* status, const QString& addr,
                          const QString& notSupportedHint);

// The verbatim public-settlement honesty sentence (sell + buy). Marked for
// translation in one place.
QString nftPublicTradeNote();

#endif // NFTCOMMON_H
