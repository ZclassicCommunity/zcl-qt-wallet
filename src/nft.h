// ============================================================================
// nft.h — the plain-data record for a single NFT shown in the native
// Collections gallery (Phase C0). A POD aggregate: no Qt object, no behavior,
// no chain access. Populated from FIXTURES today; the same shape will later be
// filled from an indexer/wallet scan without touching the view layer.
//
// verifyState semantics (set by NFTImageCache after a SHA-256 of the on-disk
// bytes is compared to docHashHex):
//   0 = pending   (not yet decoded/verified — shimmer placeholder + amber "?")
//   1 = verified  (bytes hash matches docHashHex — green check)
//   2 = mismatch  (bytes hash does NOT match docHashHex — red x)
// ============================================================================
#ifndef NFT_H
#define NFT_H

#include <QString>

// POD aggregate (C++14-clean: in-class member initializers only, no methods,
// so brace/aggregate initialization keeps working). Copied by value into the
// model; cheap (a handful of implicitly-shared QStrings + two scalars).
struct NFTItem {
    QString name;            // display title, e.g. "Aurora #014"
    QString collection;      // collection / series name, e.g. "Zcl Originals"
    QString txid;            // mint/transfer txid (display + future deep-link)
    QString docHashHex;      // expected SHA-256 of the asset bytes, lowercase hex
    QString cachePath;       // path/resource of the asset bytes to decode + verify
    qint64  receivedHeight = 0;   // block height the item was received at (0 = unknown)
    bool    isPrivate      = true; // shielded provenance -> green privacy pill
    int     verifyState    = 0;    // 0=pending 1=verified 2=mismatch (see header doc)
};

#endif // NFT_H
