// ============================================================================
// nftcommon implementation — see nftcommon.h. The exact strings here were
// previously duplicated across nftsenddialog/nftselldialog/nftbuydialog; keeping
// them in one TU means a wording change (or a translation) happens once.
// ============================================================================
#include "nftcommon.h"
#include "settings.h"
#include "contentengine.h"

#include <QLabel>
#include <QObject>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QCryptographicHash>

bool nftValidateTAddrInto(QLabel* status, const QString& addrIn,
                          const QString& notSupportedHint) {
    if (status == nullptr)
        return false;
    const QString addr = addrIn.trimmed();
    if (addr.isEmpty()) {
        status->clear();
        return false;
    }
    if (!Settings::isValidAddress(addr)) {
        status->setText(QObject::tr("That doesn't look like a ZClassic address."));
        status->setStyleSheet("color:#c0392b;");
        return false;
    }
    if (Settings::isTAddress(addr)) {
        status->setText(QObject::tr("Looks good — a public (transparent) address."));
        status->setStyleSheet("color:#34c759;");   // success text (matches the shield dialog)
        return true;
    }
    // A valid shielded (z-) address: the public ZSLP path can't deliver to it.
    status->setText(notSupportedHint);
    status->setStyleSheet("color:#c0392b;");
    return false;
}

QString nftPublicTradeNote() {
    return QObject::tr(
        "This trade is public — the price and both addresses are visible on the ledger.");
}

// ---------------------------------------------------------------------------
// Bundled-resource-by-hash index. The wallet ships a small set of :/nft sample
// images; a collectible minted from one of them carries that file's SHA-256 as
// its on-chain document_hash, so the wallet can render the picture from inside
// the app with ZERO network access (no fetch, no IP/interest leak). We hash each
// bundled resource ONCE (lazily) into a hash->resourcePath map; the streaming
// SHA-256 of a tiny embedded PNG is microseconds, so this is cheap and runs on
// the GUI thread on first lookup only.
// ---------------------------------------------------------------------------
static const QHash<QString, QString>& nftBundledByHash() {
    static QHash<QString, QString> map;
    static bool built = false;
    if (built)
        return map;
    built = true;

    // The bundled assets we are willing to render from (the qrc /nft prefix).
    const QStringList resources = {
        QStringLiteral(":/nft/res/nft/sample1.png"),
        QStringLiteral(":/nft/res/nft/sample2.png"),
        QStringLiteral(":/nft/res/nft/sample3.png"),
        QStringLiteral(":/nft/res/nft/sample4.png"),
    };
    for (const QString& res : resources) {
        QFile f(res);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        // Bare whole-file SHA-256 — the SAME anchor a small (single-leaf) file's
        // mint records (ContentEngine::anchorHexFor returns sha256Whole for
        // chunkCount<=1), so it matches the on-chain document_hash exactly.
        const QByteArray bytes = f.readAll();   // bounded: embedded sample PNGs are KB
        const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const QString hex = QString::fromLatin1(digest.toHex());
        if (!hex.isEmpty())
            map.insert(hex, res);
    }
    return map;
}

QString nftResolveLocalBytes(const QString& docHashHexIn) {
    const QString docHashHex = docHashHexIn.trimmed().toLower();
    if (docHashHex.isEmpty())
        return QString();                       // no fingerprint -> nothing to resolve

    // 1) Bytes the user already holds on this computer (minted / attached + cached).
    const QString cached = ContentEngine::cacheGet(docHashHex);
    if (!cached.isEmpty())
        return cached;

    // 2) A bundled app resource whose bytes hash to this fingerprint (offline, safe).
    return nftBundledByHash().value(docHashHex);   // "" if not one of ours
}
