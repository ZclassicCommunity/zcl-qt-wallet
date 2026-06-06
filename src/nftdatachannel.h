// ============================================================================
// nftdatachannel — pure (header-only, no Qt-object) helpers for the SHIELD
// (private file send/receive) GUI surface over the daemon's ZDC1 shielded
// data-channel. Kept dependency-light (only QByteArray/QString) so the L0
// suite can link + unit-test the load-bearing logic without the heavy app TUs.
//
// HONESTY (load-bearing — see doc/nft/PRIVACY_TECH.md): the data-channel makes
// only FILE CONTENT private. NFT ownership is ALWAYS PUBLIC (transparent UTXOs),
// and the ciphertext is stored PUBLICLY on-chain FOREVER. "Private" means
// unreadable to others, NOT undetectable and NOT deletable. Nothing here may
// imply private/anonymous ownership.
//
// What lives here:
//   * ZDC_MAX_FILE_BYTES — the daemon's principled 40000-byte cap, mirrored so
//     the send dialog can reject an oversize file UP FRONT with a clear message.
//   * looksLikeZdc1Memo() — the BINARY-SAFE ZDC1 magic sniff on the RAW memo
//     bytes, run BEFORE any QString is built from arrived memo data (a QString
//     would mangle the high bytes / embedded NULs of a binary frame).
//   * mapDataTransferError() — collapse the daemon's honest status-string (from
//     zdc::status_str) into ONE of four distinct receive states the UI must
//     never conflate (HASH_MISMATCH / NO_KEY / AEAD_FAIL / INCOMPLETE), plus a
//     generic terminal for anything else. Pure string -> enum so the four-state
//     receive UI is L0-testable with no daemon.
//
// C++14 only (CONFIG += c++14): no std::optional / std::string_view.
// ============================================================================
#ifndef NFTDATACHANNEL_H
#define NFTDATACHANNEL_H

#include <QByteArray>
#include <QString>

namespace nftdc {

// The daemon's principled file cap (src/rpc/datachannel.cpp: ZDC_MAX_FILE_BYTES).
// 87 DATA frames * 464 usable bytes ~= 40368, advertised + enforced at 40000.
// Mirrored here so the GUI rejects an oversize file before any RPC round-trip.
static const int ZDC_MAX_FILE_BYTES = 40000;

// The 4-byte big-endian ZDC1 frame magic ("ZDC1" == 0x5A444331). A real arrived
// frame is a 512-byte Sapling memo whose first 4 bytes are this magic.
static const char kZdc1Magic[4] = { '\x5A', '\x44', '\x43', '\x31' };

// BINARY-SAFE sniff on the RAW memo bytes. MUST be called on the QByteArray of
// the arrived memo BEFORE any QString is constructed from it (a QString decode
// would corrupt the high/NUL bytes of a binary ZDC1 frame and could miss or
// fabricate the magic). Returns true iff the first 4 bytes are the ZDC1 magic.
inline bool looksLikeZdc1Memo(const QByteArray& rawMemo) {
    if (rawMemo.size() < 4)
        return false;
    return rawMemo.at(0) == kZdc1Magic[0]
        && rawMemo.at(1) == kZdc1Magic[1]
        && rawMemo.at(2) == kZdc1Magic[2]
        && rawMemo.at(3) == kZdc1Magic[3];
}

// The four honest receive states the UI must keep distinct (PRIVACY_TECH.md §2.5
// / NATIVE_DISPLAY_UX.md §6.2), plus terminals for success and an unclassified
// failure. NEVER collapse these to a single "failed".
enum DataTransferState {
    DTS_None = 0,          // no error AND not yet verified/complete (still arriving / unknown)
    DTS_Verified,          // verified && complete -> safe to offer Save/open
    DTS_Incomplete,        // ERR_INCOMPLETE / complete==false -> "some pieces haven't confirmed yet"
    DTS_NoKey,             // ERR_NO_KEY -> "not addressed to you / key not on-chain for this wallet"
    DTS_HashMismatch,      // ERR_HASH_MISMATCH -> "on-chain file doesn't match the expected fingerprint" (refused)
    DTS_AeadFail,          // ERR_AEAD_FAIL -> "couldn't decrypt — tampered or wrong key" (refused)
    DTS_OtherError         // any other daemon error string -> generic terminal (still honest, not "try again")
};

// Map the daemon's honest status string (zdc::status_str text carried in the
// z_getdatatransfer "error" field) to a distinct state. The daemon emits human
// substrings — we sniff for the stable, distinguishing phrase of each, so a
// future wording tweak on ONE code never silently reclassifies another. If the
// transfer verified+completed with no error, callers pass an empty errStr and
// this returns DTS_Verified only when (verified && complete) is asserted by the
// caller; this helper classifies the FAILURE space from the error string alone.
inline DataTransferState mapDataTransferError(const QString& errStrIn) {
    const QString e = errStrIn.trimmed().toLower();
    if (e.isEmpty())
        return DTS_None;
    // Order matters only for disjoint phrases; these substrings are mutually
    // exclusive across zdc::status_str, so any order is safe.
    if (e.contains("content hash mismatch") || e.contains("hash mismatch")
        || e.contains("doesn't match") || e.contains("does not match"))
        return DTS_HashMismatch;
    if (e.contains("aead") || e.contains("tamper") || e.contains("wrong key"))
        return DTS_AeadFail;
    if (e.contains("key not yet available") || e.contains("sealed")
        || e.contains("no key") || e.contains("not the recipient"))
        return DTS_NoKey;
    if (e.contains("incomplete") || e.contains("missing frames")
        || e.contains("frames still missing"))
        return DTS_Incomplete;
    return DTS_OtherError;
}

// The honest, plain-language one-liner for each receive state (no jargon; never
// implies private/anonymous ownership; never a fake "try again" on a hard
// refusal). Verbatim from NATIVE_DISPLAY_UX.md §6.5. tr() is applied by callers
// (this header is Qt-object-free); the strings are the canonical source.
inline QString dataTransferStateCopy(DataTransferState s) {
    switch (s) {
        case DTS_Verified:
            return QStringLiteral("File verified and ready to open.");
        case DTS_Incomplete:
            return QStringLiteral(
                "Some pieces haven't confirmed yet. Try again in a few minutes.");
        case DTS_NoKey:
            return QStringLiteral(
                "This file isn't addressed to you, or its key isn't on-chain for this wallet.");
        case DTS_HashMismatch:
            return QStringLiteral(
                "The file on-chain doesn't match the expected fingerprint. Not opened.");
        case DTS_AeadFail:
            return QStringLiteral(
                "Couldn't decrypt — the file may be tampered or the key is wrong. Not opened.");
        case DTS_OtherError:
            return QStringLiteral("This private file couldn't be opened.");
        case DTS_None:
        default:
            return QString();
    }
}

} // namespace nftdc

#endif // NFTDATACHANNEL_H
