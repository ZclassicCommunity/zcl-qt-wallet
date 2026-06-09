// ============================================================================
// NFTGalleryModel — a QAbstractListModel of NFTItem rows feeding the native
// Collections gallery QListView (Phase C0). Native widgets only; no HTML/web.
//
// Flicker-free refresh: setItems() folds the incoming list into a content
// fingerprint (mirrors TxTableModel's Phase-4 technique) and NO-OPs when the
// fingerprint is unchanged, so a re-feed with identical data emits no model
// signals (no relayout, no thumbnail re-request).
//
// Thumbnails arrive asynchronously from NFTImageCache on the GUI thread via the
// onImageReady() slot (a QPixmap is built on the GUI thread there and stored on
// the row, then dataChanged() repaints just that index). The worker thread never
// touches a QPixmap — see nftimagecache.h for the Qt-threading contract.
// ============================================================================
#ifndef NFTGALLERYMODEL_H
#define NFTGALLERYMODEL_H

#include "precompiled.h"
#include "nft.h"

#include <QAbstractListModel>
#include <QPixmap>
#include <QVector>
#include <QByteArray>

class NFTGalleryModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit NFTGalleryModel(QObject* parent = nullptr);

    // Custom roles exposed to the delegate. Start well above Qt::UserRole so we
    // never collide with the framework's reserved roles.
    enum Roles {
        NameRole = Qt::UserRole + 1,
        CollectionRole,
        TxidRole,
        ThumbnailRole,    // QPixmap (null until the cache delivers it)
        VerifyStateRole,  // int 0=pending 1=verified 2=mismatch
        IsPrivateRole,    // bool
        DocHashRole,      // QString (lowercase hex) — used by the cache pipeline
        CachePathRole,    // QString — bytes path/resource for the cache pipeline
        HeightRole        // qint64 receivedHeight
    };

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    // Flicker-free, fingerprint-guarded full refresh: replaces the row set only
    // when the incoming content differs from what is already applied.
    void setItems(const QVector<NFTItem>& items);

    // Read-only access for callers (e.g. the tab wiring kicking off thumbnail
    // requests, and the unit test asserting verify state).
    const NFTItem& itemAt(int row) const;
    bool           isValidRow(int row) const;

public slots:
    // Delivered by NFTImageCache (Qt::QueuedConnection, GUI thread). `hash`
    // matches NFTItem::docHashHex; updates every matching row's thumbnail +
    // verifyState and emits dataChanged() for just those rows. The QPixmap is
    // constructed on the GUI thread by the cache before this fires, so storing
    // it here is thread-correct.
    void onImageReady(QString hash, QPixmap pm, int verifyState);

private:
    // Cheap content fingerprint of an item list (every render-affecting field,
    // in order), so an identical re-feed is a single hash compare. Mirrors
    // TxTableModel::fingerprint(); a leading sentinel keeps an empty list
    // distinct from a never-set cache.
    static QByteArray fingerprint(const QVector<NFTItem>& items);

    // NFTItem stays a chain-shaped POD (no QPixmap member); the async-delivered
    // thumbnails live in a parallel vector kept index-aligned with _items, so a
    // setItems() rebuild and an onImageReady() update never fight over the POD.
    QVector<NFTItem> _items;
    QVector<QPixmap> _thumbs;          // index-aligned with _items
    QByteArray       _lastFingerprint;
};

#endif // NFTGALLERYMODEL_H
