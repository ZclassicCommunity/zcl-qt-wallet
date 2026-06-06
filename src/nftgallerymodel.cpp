// ============================================================================
// NFTGalleryModel implementation — see nftgallerymodel.h for intent.
// ============================================================================
#include "nftgallerymodel.h"

#include <QCryptographicHash>

NFTGalleryModel::NFTGalleryModel(QObject* parent)
    : QAbstractListModel(parent) {}

// Fold every render-affecting field of the item list into a compact fingerprint.
// A leading sentinel byte makes an EMPTY list distinct from a never-yet-set
// (null) cached fingerprint, so the first (possibly empty) apply still runs once.
// verifyState is folded in so a pending->verified/mismatch transition flips the
// fingerprint (though in practice those transitions arrive via onImageReady's
// in-place update, not a full setItems).
QByteArray NFTGalleryModel::fingerprint(const QVector<NFTItem>& items) {
    QByteArray buf;
    buf.append('v');                                  // sentinel: non-null even for empty
    buf.append(QByteArray::number(items.size()));
    buf.append('|');
    for (int i = 0; i < items.size(); ++i) {
        const NFTItem& it = items.at(i);
        buf.append(it.name.toUtf8());                 buf.append('\x1f');
        buf.append(it.collection.toUtf8());           buf.append('\x1f');
        buf.append(it.txid.toUtf8());                 buf.append('\x1f');
        buf.append(it.docHashHex.toUtf8());           buf.append('\x1f');
        buf.append(it.cachePath.toUtf8());            buf.append('\x1f');
        buf.append(QByteArray::number(it.receivedHeight)); buf.append('\x1f');
        buf.append(it.isPrivate ? '1' : '0');         buf.append('\x1f');
        buf.append(QByteArray::number(it.verifyState));buf.append('\x1e');
    }
    return QCryptographicHash::hash(buf, QCryptographicHash::Sha1);
}

int NFTGalleryModel::rowCount(const QModelIndex& parent) const {
    // Flat list model: a valid parent has no children.
    if (parent.isValid())
        return 0;
    return _items.size();
}

QVariant NFTGalleryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= _items.size())
        return QVariant();

    const NFTItem& it = _items.at(index.row());
    const bool haveThumb = !_thumbs.value(index.row()).isNull();
    switch (role) {
        case Qt::DisplayRole:      // a plain display fallback (name) for any default view
        case NameRole:             return it.name;
        case CollectionRole:       return it.collection;
        case TxidRole:             return it.txid;
        case Qt::DecorationRole:   // a default view shows the thumbnail too
        case ThumbnailRole:        return _thumbs.value(index.row());   // QPixmap (may be null)
        case Qt::ToolTipRole: {
            // HONEST per-state hover (review fix #4): a confirmed NFT we hold whose
            // image bytes aren't on this computer must NOT read as an endless spinner.
            // The tooltip names the actual state so the card is a stable terminal one.
            QString head = it.name.isEmpty() ? tr("Collectible") : it.name;
            QString state;
            if (haveThumb) {
                if      (it.verifyState == 1) state = tr("Image matches its on-chain fingerprint.");
                else if (it.verifyState == 2) state = tr("Image does NOT match its on-chain fingerprint.");
                else                          state = tr("Checking this image…");
            } else {
                // No local bytes: it's yours, just not pictured here — a fact, not loading.
                state = tr("You hold this collectible. Its image isn't on this computer "
                           "(open it to check it yourself).");
            }
            return head + "\n" + state;
        }
        case VerifyStateRole:      return it.verifyState;
        case IsPrivateRole:        return it.isPrivate;
        case DocHashRole:          return it.docHashHex;
        case CachePathRole:        return it.cachePath;
        case HeightRole:           return QVariant::fromValue<qlonglong>(it.receivedHeight);
        default:                   return QVariant();
    }
}

void NFTGalleryModel::setItems(const QVector<NFTItem>& items) {
    // Flicker-free guard: identical content -> no model churn (mirrors
    // TxTableModel::add*Data). A re-feed with the same fixtures is a no-op.
    const QByteArray fp = fingerprint(items);
    if (fp == _lastFingerprint)
        return;
    _lastFingerprint = fp;

    beginResetModel();
    _items = items;
    // Reset the parallel thumbnail vector to match the new row count (all null
    // until the cache delivers each one); kept index-aligned with _items.
    // QVector::fill(value, size) resizes + fills (Qt5-safe; no QVector::assign).
    _thumbs.fill(QPixmap(), _items.size());
    endResetModel();
}

const NFTItem& NFTGalleryModel::itemAt(int row) const {
    return _items.at(row);
}

bool NFTGalleryModel::isValidRow(int row) const {
    return row >= 0 && row < _items.size();
}

void NFTGalleryModel::onImageReady(QString hash, QPixmap pm, int verifyState) {
    // GUI-thread slot (Qt::QueuedConnection from the worker). The QPixmap was
    // already constructed on the GUI thread by the cache, so it is safe to store.
    // Update EVERY row whose docHashHex matches (an asset may legitimately appear
    // in more than one item) and emit a tight per-row dataChanged().
    for (int row = 0; row < _items.size(); ++row) {
        if (_items[row].docHashHex.compare(hash, Qt::CaseInsensitive) != 0)
            continue;

        // Only replace the thumbnail when a real pixmap arrived; a null delivery
        // (e.g. a missing/undecodable asset) must not wipe an existing good one.
        if (!pm.isNull() && row < _thumbs.size())
            _thumbs[row] = pm;
        _items[row].verifyState = verifyState;

        const QModelIndex idx = index(row, 0);
        // Only the roles that actually changed; the delegate repaints on any of them.
        const QVector<int> roles{ ThumbnailRole, VerifyStateRole, Qt::DecorationRole };
        emit dataChanged(idx, idx, roles);
    }
}
