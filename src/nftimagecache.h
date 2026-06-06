// ============================================================================
// NFTImageCache — a bounded, GUI-thread-correct background pipeline that turns
// an NFT asset's raw bytes into a verified thumbnail for the Collections gallery
// (Phase C0). Native only; no network, no web engine.
//
// For each request (hash, bytesPath, onChainHashHex, sizePx) a worker thread:
//   1. reads the asset bytes (a file path OR a Qt resource, e.g. ":/nft/x.png");
//   2. SHA-256s them and compares to onChainHashHex  -> verifyState
//        (0 pending / 1 verified / 2 mismatch);
//   3. decodes via QImageReader, down-scaling huge sources at decode time
//      (setScaledSize) so we never inflate a multi-MB image into RAM;
//   4. scales to a sizePx-wide thumbnail (QImage, SmoothTransformation);
//   5. atomically writes a PNG cache file under AppData/nft_thumbs/.
// The worker hands back a QImage ONLY. The QPixmap is built on the GUI thread in
// the model's onImageReady() slot, reached via QMetaObject::invokeMethod with
// Qt::QueuedConnection — the one safe way to cross threads here, because QPixmap
// must never be constructed or touched off the GUI thread.
//
// An in-flight QSet<QString> dedupes concurrent requests for the same hash, and
// QThreadPool::setMaxThreadCount(4) bounds CPU/IO. A previously written cache
// file is reused (re-verified cheaply) instead of re-decoding from scratch.
// ============================================================================
#ifndef NFTIMAGECACHE_H
#define NFTIMAGECACHE_H

#include "precompiled.h"

#include <QObject>
#include <QThreadPool>
#include <QSet>
#include <QString>
#include <QMutex>
#include <QPointer>
#include <QImage>

class NFTGalleryModel;

class NFTImageCache : public QObject {
    Q_OBJECT
public:
    // `model` is the delivery target; onImageReady() is invoked on it (it lives
    // on the GUI thread). Held as a QPointer so a deleted model can never be
    // dereferenced by a late-finishing worker.
    explicit NFTImageCache(NFTGalleryModel* model, QObject* parent = nullptr);
    ~NFTImageCache() override;

    // Queue a decode+verify for one asset. Idempotent per (hash,sizePx): a
    // request already in flight is dropped. Safe to call from the GUI thread.
    void request(const QString& hash, const QString& bytesPath,
                 const QString& onChainHashHex, int sizePx);

    // Directory where thumbnails are cached (AppData/nft_thumbs, created lazily).
    static QString cacheDir();

private:
    // GUI-THREAD landing point for a finished worker. The worker produces a
    // QImage (never a QPixmap) and queues this; here, on the GUI thread, we build
    // the QPixmap, forward to the model's onImageReady(), and clear the in-flight
    // key. This is the single thread-boundary crossing for image data.
    Q_INVOKABLE void deliver(QString hash, QImage img, int verifyState, QString inflightKey);

    static QString inflightKeyFor(const QString& hash, int sizePx);

    QPointer<NFTGalleryModel> _model;
    QThreadPool               _pool;        // bounded to 4 worker threads
    QSet<QString>             _inflight;    // (hash@sizePx) keys currently queued
    QMutex                    _inflightMtx; // guards _inflight (GUI-thread only in
                                            // practice, but cheap insurance)
};

#endif // NFTIMAGECACHE_H
