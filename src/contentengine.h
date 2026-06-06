// ============================================================================
// ContentEngine — the ONE bounded, GUI-thread-correct streaming content engine
// for the NFT subsystem (the general refactor of nftimagecache, Phase C1).
//
// It turns ANY local file's raw bytes (image, video, document, arbitrary bytes)
// into:
//   * a verifyState (0 pending / 1 verified / 2 mismatch) by recomputing the
//     content fingerprint and comparing to the on-chain anchor — STREAMING, so a
//     multi-GB video hashes in ~1 MiB of RAM (NEVER readAll the whole file);
//   * a poster/thumbnail QImage for the gallery (image => decoded+downscaled;
//     video/document/bytes => a typed glyph poster — never a faked video frame);
//   * a chunked Merkle root (1 MiB leaves) that enables incremental / streamed
//     verification of large files without ever holding the whole file.
//
// THREADING CONTRACT (load-bearing — preserved verbatim from nftimagecache):
//   * request()/posterFor()/hashFile()/verify()/deliver() run on the GUI thread.
//   * The QRunnable worker runs on a bounded pool thread and touches ONLY
//     QByteArray / QCryptographicHash / QImageReader / QImage — NEVER QPixmap.
//   * It crosses back via QMetaObject::invokeMethod(..., Qt::QueuedConnection) to
//     deliver(), where (on the GUI thread) the QPixmap is built and the model's
//     onImageReady() slot is invoked.
//
// PRIVACY (hard rule): the engine has ZERO network code. Every entry point takes
// a LOCAL filesystem path or a ":/resource" ONLY; a remote http(s):// bytesPath
// is REJECTED. Bytes come from local cache or an explicit user action — the
// engine never auto-fetches a documenturl.
//
// EFFICIENCY: a single reused kHashBufBytes (1 MiB) read buffer per worker; a
// std::atomic<bool> cancel flag is tested every block so a 2 GB / 100 GB hash
// aborts promptly on shutdown or a wrong-file drop. The dtor cancelAll()s — a
// QThreadPool::clear() alone only drops not-yet-started runnables and cannot stop
// an already-running multi-GB hash.
//
// C++14 only: no std::optional / std::string_view. Sentinels are empty QString /
// int verifyState, exactly as the existing code.
// ============================================================================
#ifndef CONTENTENGINE_H
#define CONTENTENGINE_H

#include "precompiled.h"

#include <QObject>
#include <QThreadPool>
#include <QSet>
#include <QHash>
#include <QString>
#include <QMutex>
#include <QPointer>
#include <QImage>
#include <QByteArray>
#include <QVector>
#include <QSharedPointer>
#include <QIODevice>

#include <atomic>

class NFTGalleryModel;

// ---------------------------------------------------------------------------
// ContentDescriptor — a value-copyable POD describing a piece of content. POD
// aggregate so brace-init keeps working under C++14 (no methods, in-class member
// initializers only). Returned by descriptorReady() / used by verifyChunk().
// ---------------------------------------------------------------------------
struct ContentDescriptor {
    bool       ok          = false;       // false => unreadable / empty / cancelled
    QByteArray merkleRoot;                // 32B chunk-tree root (== anchor, large mode)
    QByteArray sha256Whole;               // 32B whole-file SHA-256 (cross-check / small mode)
    quint64    fileSize    = 0;           // observed byte length
    quint32    chunkSize   = 1048576;     // PINNED 1 MiB leaf size (wire commitment)
    quint32    chunkCount  = 0;           // ceil(fileSize / chunkSize); 0 for empty
    QString    mime;                      // sniffed MIME, "" if sniff failed
    QString    filename;                  // basename only (never a path)
    QByteArray posterHash;                // 32B or empty (unused in v1 local path)
    bool       isPrivate   = false;       // ciphertext-anchor flag (Tier 3; off here)
};

// verifyState values shared with nft.h:29 (kept as plain ints across the
// thread boundary to match the existing onImageReady contract).
enum ContentVerifyState { CE_Pending = 0, CE_Verified = 1, CE_Mismatch = 2 };

// Content kind classification (coarse MIME family) for the typed poster + UI.
enum ContentKind { CK_Image = 0, CK_Video = 1, CK_Document = 2, CK_Bytes = 3 };

class ContentEngine : public QObject {
    Q_OBJECT
public:
    // `model` is the delivery target for posterFor()/request(); onImageReady()
    // is invoked on it (it lives on the GUI thread). Held as a QPointer so a
    // deleted model can never be dereferenced by a late-finishing worker. `model`
    // may be null when the engine is used purely for hashFile()/verify().
    explicit ContentEngine(NFTGalleryModel* model, QObject* parent = nullptr);
    ~ContentEngine() override;

    // ---- back-compat: decode+verify+poster for one asset (the original API) ---
    // Idempotent per (hash,sizePx): a request already in flight is dropped. Safe
    // to call from the GUI thread. bytesPath MUST be local/resource (never a URL).
    void request(const QString& hash, const QString& bytesPath,
                 const QString& onChainHashHex, int sizePx);

    // Explicit alias for new call sites (same behavior as request()).
    void posterFor(const QString& path, const QString& hash,
                   const QString& expectedHashHex, int sizePx);

    // Token-addressed poster for the DETAIL DIALOG (the one and only ContentEngine
    // addition for the native NFT detail/mint/send UI). It decodes + verifies the
    // SAME way as posterFor(), but delivers the LARGE decoded QImage back via the
    // posterReady(token, img, verifyState) signal on the GUI thread INSTEAD of
    // routing the result into the gallery model. `hash` is still the content/anchor
    // key (used for the on-disk poster cache + verify); `token` (>0) routes the
    // result back to one caller so a fast prev/next drops a stale neighbor's reply.
    // Local/resource path ONLY (a remote URL emits posterReady(token, null,
    // CE_Pending) — never fetched). A token==0 (or empty path/non-positive size)
    // also emits posterReady(token, null, CE_Pending) so the dialog never hangs.
    void posterForToken(const QString& path, const QString& hash,
                        const QString& expectedHashHex, int sizePx, quint64 token);

    // ---- async hash + describe (streaming, bounded RAM) -----------------------
    // Emits descriptorReady(token, d) on the GUI thread. NEVER blocks the GUI.
    void hashFile(const QString& path, quint64 token);

    // ---- async verify against an on-chain anchor ------------------------------
    // expectedHashHex is the 64-hex anchor (bare SHA-256 for small files OR the
    // Merkle root for large files — the engine compares BOTH and accepts either,
    // resolving the anchor-ambiguity rule without an external manifest). Emits
    // verifyDone(token, verifyState) on the GUI thread.
    void verify(const QString& path, const QString& expectedHashHex, quint64 token);

    // ---- cancellation ---------------------------------------------------------
    void cancel(const QString& hash);     // cancel a specific in-flight job by key
    void cancelAll();                     // cancel everything (also called by dtor)

    // =========================================================================
    // PURE / STATIC helpers — no I/O on the GUI thread; unit-testable in L0.
    // =========================================================================

    // Streaming SHA-256 of a whole file/device through a fixed kHashBufBytes
    // buffer (NEVER readAll). Returns 32B digest; empty on open failure. The
    // optional cancel flag is tested every block. For a QIODevice overload the
    // device must already be open for reading.
    static QByteArray streamingSha256(const QString& path,
                                      std::atomic<bool>* cancel = nullptr);
    static QByteArray streamingSha256(QIODevice& in,
                                      std::atomic<bool>* cancel = nullptr);

    // Streaming chunked Merkle root over chunkSize-byte leaves, computed in the
    // SAME single pass as the whole-file SHA-256. Rules (PINNED, a wire
    // commitment): leaf_i = SHA256(0x00 || chunk_i); node = SHA256(0x01||L||R);
    // an odd node is PROMOTED unchanged (NOT duplicated). A file <= chunkSize has
    // root == leaf_0 == SHA256(0x00 || bytes) (NOTE: != bare SHA-256(bytes)).
    // Fills the out-descriptor (merkleRoot, sha256Whole, fileSize, chunkCount,
    // optionally the per-chunk leaves). leavesOut may be null. Returns d.ok.
    static bool streamDescribe(QIODevice& in, quint32 chunkSize,
                               ContentDescriptor& outDesc,
                               QVector<QByteArray>* leavesOut = nullptr,
                               std::atomic<bool>* cancel = nullptr);
    static bool streamDescribe(const QString& path, quint32 chunkSize,
                               ContentDescriptor& outDesc,
                               QVector<QByteArray>* leavesOut = nullptr,
                               std::atomic<bool>* cancel = nullptr);

    // The on-chain document_hash anchor for a described file: the descriptor's
    // merkleRoot for multi-chunk files (chunkCount>1 && merkleRoot present), else
    // the whole-file SHA-256 (small/single-leaf), lowercase hex. This is the ONE
    // shared rule used by BOTH the mint wizard (to compute the anchor it writes)
    // and the detail dialog's attach-the-file match-gate (to compare a chosen
    // file against the recorded fingerprint). verify() accepts EITHER form, so an
    // anchor produced here always matches the verify badge. Pure -> L0-testable.
    static QString anchorHexFor(const ContentDescriptor& d);

    // Combine 32B leaf digests into the Merkle root per the PINNED rules above.
    // Empty input => empty root (an empty file has no leaves; handled by callers).
    static QByteArray merkleRootFromLeaves(const QVector<QByteArray>& leaves);

    // A single leaf digest: SHA256(0x00 || chunkBytes). Exposed for verifyChunk.
    static QByteArray merkleLeaf(const QByteArray& chunkBytes);

    // Pure streamed-verify primitive: does chunk `idx`'s bytes match the leaf the
    // descriptor expects? Used by a future chunked-transfer pipeline and tests.
    // Requires d to carry the per-chunk leaves (built via streamDescribe with a
    // leavesOut), else returns false. idx out of range => false.
    static bool verifyChunk(const ContentDescriptor& d,
                            const QVector<QByteArray>& leaves,
                            int idx, const QByteArray& chunkBytes);

    // MIME / kind classification (header sniff + extension; QMimeDatabase — no
    // full read). mimeOut receives the MIME string ("" if unknown).
    static ContentKind classifyKind(const QString& path, QString& mimeOut);
    static ContentKind kindForMime(const QString& mime);

    // Human-readable byte size, e.g. 12.3 MB / 900 KB / 42 B.
    static QString humanSize(quint64 bytes);

    // ---- content-addressed on-disk cache (store once, dedupe across NFTs) -----
    // Poster (thumbnail) cache dir: AppData/nft_posters (created lazily). The
    // legacy AppData/nft_thumbs name is kept working via cacheDir() for the
    // existing gallery test; cacheDir() returns the poster dir.
    static QString cacheDir();            // poster/thumbnail dir
    static QString blobCacheDir();        // verified-bytes dir (opt-in only)

    // Verified-bytes store, keyed by hash. cachePut copies srcPath into the blob
    // store atomically; cacheGet returns the stored path or "" (empty sentinel).
    // PATH-TRAVERSAL SAFE: the hash is sanitized to [0-9a-f] and length-capped, so
    // a hostile "../../etc/passwd" hash can never escape blobCacheDir().
    static QString cacheGet(const QString& hashHex);
    static bool    cachePut(const QString& hashHex, const QString& srcPath);

    // Sanitize an arbitrary string into a safe filename component (lowercase hex
    // only, length-capped). Public for unit tests of path-traversal neutralization.
    static QString safeKey(const QString& raw);

    // Is `bytesPath` a remote URL we must REFUSE (privacy guard)?
    static bool isRemoteUrl(const QString& bytesPath);

    // PINNED constants (wire commitments / efficiency knobs).
    static const int    kHashBufBytes = 1 << 20;   // 1 MiB streaming read buffer
    static const quint32 kChunkBytes  = 1u << 20;  // 1 MiB Merkle leaf size

signals:
    // Emitted on the GUI thread when hashFile() finishes.
    void descriptorReady(quint64 token, ContentDescriptor d);
    // Emitted on the GUI thread when verify() finishes.
    void verifyDone(quint64 token, int verifyState);
    // Emitted on the GUI thread when a poster requested via posterForToken()
    // finishes: the LARGE decoded image (or a typed glyph poster) for ONE caller
    // token, delivered WITHOUT touching the gallery model. token==0 NEVER emits
    // this (the gallery's hash-addressed request()/posterFor() path), so the
    // existing gallery sees zero new signals. `img` may be null (undecodable / no
    // local bytes) -> the dialog shows its own honest fallback. verifyState is
    // CE_* (fingerprint-match ONLY — never genuine/official), matching verifyDone.
    void posterReady(quint64 token, QImage img, int verifyState);

private:
    // GUI-THREAD landing point for a finished poster worker. The worker produces
    // a QImage (never a QPixmap) and queues this; here, on the GUI thread, we
    // build the QPixmap, forward to onImageReady(), and clear the in-flight key.
    Q_INVOKABLE void deliver(QString hash, QImage img, int verifyState,
                             QString inflightKey, quint64 token);
    // GUI-thread landing for hashFile()/verify() (no QPixmap involved).
    Q_INVOKABLE void deliverDescriptor(quint64 token, ContentDescriptor d, QString inflightKey);
    Q_INVOKABLE void deliverVerify(quint64 token, int verifyState, QString inflightKey);

    static QString inflightKeyFor(const QString& hash, int sizePx);

    // Register/clear a cancel flag for an in-flight key (under _inflightMtx).
    QSharedPointer<std::atomic<bool>> beginJob(const QString& key);
    void endJob(const QString& key);

    QPointer<NFTGalleryModel> _model;
    QThreadPool               _pool;        // bounded to 4 worker threads
    QSet<QString>             _inflight;    // keys currently queued (dedupe)
    QHash<QString, QSharedPointer<std::atomic<bool>>> _cancelFlags;  // per-job cancel
    QMutex                    _inflightMtx; // guards _inflight + _cancelFlags

    friend class ContentTask;
};

// ContentDescriptor crosses thread boundaries as a queued-connection argument
// (descriptorReady signal + deliverDescriptor invokable), so it must be a
// registered metatype. It is a value-copyable POD of QByteArray/QString/scalars.
Q_DECLARE_METATYPE(ContentDescriptor)

#endif // CONTENTENGINE_H
