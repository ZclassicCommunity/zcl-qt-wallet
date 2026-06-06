// ============================================================================
// NFTImageCache implementation — see nftimagecache.h for the threading contract.
//
// THREADING (the load-bearing part):
//   * request()/deliver()/markDone() run on the GUI thread.
//   * The QRunnable worker runs on a pool thread and touches ONLY QByteArray,
//     QCryptographicHash, QImageReader and QImage — NEVER QPixmap.
//   * The worker crosses back via QMetaObject::invokeMethod(..., QueuedConnection)
//     to deliver(), where (on the GUI thread) the QPixmap is finally built and
//     the model's onImageReady() slot is invoked.
// ============================================================================
#include "nftimagecache.h"
#include "nftgallerymodel.h"

#include <QRunnable>
#include <QImageReader>
#include <QPixmap>
#include <QPixmapCache>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QStandardPaths>
#include <QMetaObject>
#include <QMetaType>

namespace {
    // Decode-time guards: a source wider than this, or a file bigger than this,
    // is down-scaled AT DECODE so we never inflate a huge image into RAM.
    const int     kMaxSourceWidth = 4096;
    const qint64  kMaxFileBytes   = 10LL * 1024 * 1024;   // 10 MB
    const int     kDecodeCapPx    = 1024;                 // decode-scaled max edge
}

// ---------------------------------------------------------------------------
// The worker. A plain QRunnable so it auto-deletes after run() (autoDelete is
// the QThreadPool default). It owns only value-type copies of its inputs +
// a guarded pointer back to the cache object for the queued deliver().
// ---------------------------------------------------------------------------
namespace {
class DecodeTask : public QRunnable {
public:
    DecodeTask(QPointer<NFTImageCache> owner,
               QString hash, QString bytesPath, QString onChainHashHex,
               int sizePx, QString inflightKey)
        : _owner(owner), _hash(hash), _bytesPath(bytesPath),
          _onChainHashHex(onChainHashHex.toLower()), _sizePx(sizePx),
          _inflightKey(inflightKey) {}

    void run() override {
        int     verifyState = 0;     // pending until we have read+hashed the bytes
        QImage  thumb;               // worker produces ONLY a QImage

        // ---- 1) read the asset bytes (file OR Qt resource) -----------------
        QByteArray bytes;
        {
            QFile f(_bytesPath);
            if (f.open(QIODevice::ReadOnly))
                bytes = f.readAll();
        }

        if (!bytes.isEmpty()) {
            // ---- 2) SHA-256 verify ----------------------------------------
            const QByteArray digest =
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
            const QString gotHex = QString::fromLatin1(digest.toHex());
            if (_onChainHashHex.isEmpty())
                verifyState = 0;                         // nothing to verify against
            else
                verifyState = (gotHex == _onChainHashHex) ? 1 : 2;

            // ---- 3) decode (down-scaling huge sources at decode time) ------
            QImageReader reader;
            QBuffer buf(&bytes);
            buf.open(QIODevice::ReadOnly);
            reader.setDevice(&buf);
            reader.setAutoTransform(true);

            const QSize src = reader.size();             // header-only; cheap
            const bool tooBig =
                (src.isValid() && src.width() > kMaxSourceWidth) ||
                (bytes.size() > kMaxFileBytes);
            if (tooBig && src.isValid()) {
                // Preserve aspect, cap the longest edge at kDecodeCapPx.
                QSize scaled = src;
                scaled.scale(kDecodeCapPx, kDecodeCapPx, Qt::KeepAspectRatio);
                if (!scaled.isEmpty())
                    reader.setScaledSize(scaled);
            }

            const QImage decoded = reader.read();
            if (!decoded.isNull()) {
                // ---- 4) final thumbnail (width = sizePx) ------------------
                thumb = decoded.scaledToWidth(_sizePx, Qt::SmoothTransformation);

                // ---- 5) atomic PNG write to the on-disk cache -------------
                const QString out = NFTImageCache::cacheDir()
                                  + "/" + _hash + "_" + QString::number(_sizePx) + ".png";
                QSaveFile sf(out);
                if (sf.open(QIODevice::WriteOnly)) {
                    if (thumb.save(&sf, "PNG"))
                        sf.commit();                     // atomic rename on commit
                    else
                        sf.cancelWriting();
                }
            }
        }

        // ---- hand back to the GUI thread (QImage only; NO QPixmap here) ----
        if (!_owner.isNull()) {
            QMetaObject::invokeMethod(_owner.data(), "deliver", Qt::QueuedConnection,
                                      Q_ARG(QString, _hash),
                                      Q_ARG(QImage, thumb),
                                      Q_ARG(int, verifyState),
                                      Q_ARG(QString, _inflightKey));
        }
    }

private:
    QPointer<NFTImageCache> _owner;
    QString _hash;
    QString _bytesPath;
    QString _onChainHashHex;
    int     _sizePx;
    QString _inflightKey;
};
} // namespace

// ---------------------------------------------------------------------------
NFTImageCache::NFTImageCache(NFTGalleryModel* model, QObject* parent)
    : QObject(parent), _model(model) {
    // QImage crosses the thread boundary as a queued-connection argument; it is a
    // built-in metatype, but register defensively so invokeMethod always resolves
    // it (harmless if already registered).
    qRegisterMetaType<QImage>("QImage");

    _pool.setMaxThreadCount(4);                 // bounded CPU/IO
    QPixmapCache::setCacheLimit(128 * 1024);    // 128 MB (cacheLimit is in KB)
}

NFTImageCache::~NFTImageCache() {
    // Block until in-flight workers finish so none calls back into a half-destroyed
    // object. _owner is a QPointer, so even a straggler is a safe no-op.
    _pool.clear();
    _pool.waitForDone();
}

QString NFTImageCache::cacheDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir  = base + "/nft_thumbs";
    QDir().mkpath(dir);                         // idempotent; creates parents
    return dir;
}

QString NFTImageCache::inflightKeyFor(const QString& hash, int sizePx) {
    return hash + "@" + QString::number(sizePx);
}

void NFTImageCache::request(const QString& hash, const QString& bytesPath,
                            const QString& onChainHashHex, int sizePx) {
    if (hash.isEmpty() || bytesPath.isEmpty() || sizePx <= 0)
        return;

    const QString key = inflightKeyFor(hash, sizePx);
    {
        QMutexLocker lock(&_inflightMtx);
        if (_inflight.contains(key))
            return;                              // already queued -> dedupe
        _inflight.insert(key);
    }

    auto* task = new DecodeTask(QPointer<NFTImageCache>(this),
                                hash, bytesPath, onChainHashHex, sizePx, key);
    task->setAutoDelete(true);
    _pool.start(task);
}

void NFTImageCache::deliver(QString hash, QImage img, int verifyState, QString inflightKey) {
    // GUI thread. Build the QPixmap HERE (never in the worker), seed the global
    // QPixmapCache, then notify the model.
    QPixmap pm;
    if (!img.isNull()) {
        pm = QPixmap::fromImage(img);
        if (!pm.isNull())
            QPixmapCache::insert(inflightKey, pm);
    }

    // Clear the in-flight marker so a later refresh can re-request if needed.
    {
        QMutexLocker lock(&_inflightMtx);
        _inflight.remove(inflightKey);
    }

    if (!_model.isNull())
        _model->onImageReady(hash, pm, verifyState);
}
