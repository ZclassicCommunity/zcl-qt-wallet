// ============================================================================
// ContentEngine implementation — see contentengine.h for the full contract.
//
// The ONE streaming content/verify/poster engine. Key efficiency property: NO
// entry point ever calls QIODevice::readAll() on the asset; every hash is taken
// streaming through a single reused 1 MiB (kHashBufBytes) buffer, so a 2 GB
// video hashes in ~1 MiB of RAM. Cancellation is checked every block so an
// in-flight multi-GB hash aborts promptly on shutdown.
// ============================================================================
#include "contentengine.h"
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
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainter>
#include <QPen>
#include <QColor>

namespace {
    // Decode-time guards (image path, unchanged from nftimagecache): a source
    // wider than this, or a file bigger than this, is down-scaled AT DECODE so we
    // never inflate a huge image into RAM.
    const int     kMaxSourceWidth = 4096;
    const qint64  kMaxFileBytes   = 10LL * 1024 * 1024;   // 10 MB
    const int     kDecodeCapPx    = 1024;                 // decode-scaled max edge

    // Hard cap on a sanitized cache key length (a SHA-256 hex is 64 chars; allow
    // a little slack for size-suffixed poster names handled by the caller).
    const int     kMaxKeyLen      = 80;

    // Dark-theme tokens (mirror dark.qss) used by the typed glyph posters so a
    // non-image NFT card looks native, not blank.
    const QColor  kInsetBg(0x1d, 0x20, 0x27);   // #1d2027 inset
    const QColor  kGlyphFg(0xc8, 0xcc, 0xd4);   // soft light glyph
}

// ===========================================================================
// PURE / STATIC primitives (no GUI, unit-testable)
// ===========================================================================

QByteArray ContentEngine::merkleLeaf(const QByteArray& chunkBytes) {
    // leaf_i = SHA256(0x00 || chunk_bytes)  — domain-separated (CVE-2012-2459 fix).
    QCryptographicHash h(QCryptographicHash::Sha256);
    const char zero = 0x00;
    h.addData(&zero, 1);
    h.addData(chunkBytes);
    return h.result();
}

QByteArray ContentEngine::merkleRootFromLeaves(const QVector<QByteArray>& leaves) {
    if (leaves.isEmpty())
        return QByteArray();                 // empty content => no leaves
    if (leaves.size() == 1)
        return leaves.first();               // 1-leaf degenerate: root == leaf_0

    QVector<QByteArray> level = leaves;
    while (level.size() > 1) {
        QVector<QByteArray> next;
        next.reserve((level.size() + 1) / 2);
        for (int i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size()) {
                // node = SHA256(0x01 || left || right)
                QCryptographicHash h(QCryptographicHash::Sha256);
                const char one = 0x01;
                h.addData(&one, 1);
                h.addData(level[i]);
                h.addData(level[i + 1]);
                next.push_back(h.result());
            } else {
                // ODD NODE: PROMOTE unchanged (do NOT duplicate — anti-malleability).
                next.push_back(level[i]);
            }
        }
        level = next;
    }
    return level.first();
}

bool ContentEngine::streamDescribe(QIODevice& in, quint32 chunkSize,
                                   ContentDescriptor& outDesc,
                                   QVector<QByteArray>* leavesOut,
                                   std::atomic<bool>* cancel) {
    if (chunkSize == 0)
        chunkSize = kChunkBytes;

    ContentDescriptor d;
    d.chunkSize = chunkSize;

    QCryptographicHash whole(QCryptographicHash::Sha256);   // streaming whole-file SHA-256
    QCryptographicHash leaf(QCryptographicHash::Sha256);    // current-chunk leaf hasher
    QVector<QByteArray> leaves;

    // The leaf hasher is domain-separated: prime it with the 0x00 leaf prefix.
    const char zero = 0x00;
    bool leafPrimed = false;
    auto primeLeaf = [&]() {
        leaf.reset();
        leaf.addData(&zero, 1);
        leafPrimed = true;
    };
    primeLeaf();

    QByteArray buf;
    buf.resize(kHashBufBytes);               // ONE reusable 1 MiB buffer

    quint64 total = 0;
    quint64 chunkFilled = 0;                  // bytes accumulated into the current leaf

    while (true) {
        if (cancel && cancel->load())
            return false;                     // cancellation checked every block

        const qint64 n = in.read(buf.data(), buf.size());
        if (n < 0)
            return false;                     // read error
        if (n == 0)
            break;                            // EOF

        whole.addData(buf.constData(), static_cast<int>(n));
        total += static_cast<quint64>(n);

        // Feed bytes into CHUNK_SIZE-boundaried leaves. A single buffer fill can
        // span a chunk boundary only if chunkSize < kHashBufBytes; handle the
        // general case so an arbitrary chunkSize (tests) still tiles correctly.
        qint64 off = 0;
        while (off < n) {
            const quint64 room  = static_cast<quint64>(chunkSize) - chunkFilled;
            const quint64 avail = static_cast<quint64>(n - off);
            const quint64 take  = (avail < room) ? avail : room;

            leaf.addData(buf.constData() + off, static_cast<int>(take));
            chunkFilled += take;
            off         += static_cast<qint64>(take);

            if (chunkFilled == chunkSize) {
                leaves.push_back(leaf.result());
                primeLeaf();
                chunkFilled = 0;
            }
        }
    }

    // Flush a final partial chunk (the last leaf, < chunkSize) — but ONLY if the
    // file is non-empty. An empty file has zero leaves (root = empty).
    if (total > 0 && chunkFilled > 0) {
        leaves.push_back(leaf.result());
    } else if (total > 0 && chunkFilled == 0 && leaves.isEmpty()) {
        // Exact multiple already pushed above; nothing to flush. (Unreachable for
        // total>0 since the loop pushes on boundary; kept for clarity.)
    }

    d.sha256Whole = whole.result();
    d.fileSize    = total;
    d.chunkCount  = static_cast<quint32>(leaves.size());
    d.merkleRoot  = merkleRootFromLeaves(leaves);
    d.ok          = true;

    if (leavesOut)
        *leavesOut = leaves;
    outDesc = d;
    return true;
}

bool ContentEngine::streamDescribe(const QString& path, quint32 chunkSize,
                                   ContentDescriptor& outDesc,
                                   QVector<QByteArray>* leavesOut,
                                   std::atomic<bool>* cancel) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return streamDescribe(f, chunkSize, outDesc, leavesOut, cancel);
}

QByteArray ContentEngine::streamingSha256(QIODevice& in, std::atomic<bool>* cancel) {
    QCryptographicHash whole(QCryptographicHash::Sha256);
    QByteArray buf;
    buf.resize(kHashBufBytes);                // ONE reusable 1 MiB buffer
    while (true) {
        if (cancel && cancel->load())
            return QByteArray();              // cancelled
        const qint64 n = in.read(buf.data(), buf.size());
        if (n < 0)
            return QByteArray();              // read error
        if (n == 0)
            break;
        whole.addData(buf.constData(), static_cast<int>(n));
    }
    return whole.result();
}

QByteArray ContentEngine::streamingSha256(const QString& path, std::atomic<bool>* cancel) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return streamingSha256(f, cancel);
}

bool ContentEngine::verifyChunk(const ContentDescriptor& d,
                                const QVector<QByteArray>& leaves,
                                int idx, const QByteArray& chunkBytes) {
    // `d` is part of the stable API (a future descriptor-only overload may carry
    // the leaves inline); v1 verifies against the explicit leaves vector.
    Q_UNUSED(d);
    if (idx < 0 || idx >= leaves.size())
        return false;
    return merkleLeaf(chunkBytes) == leaves[idx];
}

// ---------------------------------------------------------------------------
ContentKind ContentEngine::kindForMime(const QString& mime) {
    if (mime.startsWith("image/"))            return CK_Image;
    if (mime.startsWith("video/"))            return CK_Video;
    if (mime.startsWith("text/")
        || mime == "application/pdf"
        || mime.contains("document")
        || mime.contains("officedocument")
        || mime == "application/json"
        || mime == "application/xml")         return CK_Document;
    return CK_Bytes;
}

ContentKind ContentEngine::classifyKind(const QString& path, QString& mimeOut) {
    // QMimeDatabase sniffs the file header (and falls back to extension) WITHOUT
    // reading the whole file — header-only, RAM-safe even for a 2 GB video.
    QMimeDatabase db;
    const QMimeType mt = db.mimeTypeForFile(path);
    mimeOut = mt.isValid() ? mt.name() : QString();
    return kindForMime(mimeOut);
}

QString ContentEngine::humanSize(quint64 bytes) {
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    if (bytes >= static_cast<quint64>(gb))
        return QString::number(bytes / gb, 'f', 1) + " GB";
    if (bytes >= static_cast<quint64>(mb))
        return QString::number(bytes / mb, 'f', 1) + " MB";
    if (bytes >= static_cast<quint64>(kb))
        return QString::number(bytes / kb, 'f', 1) + " KB";
    return QString::number(bytes) + " B";
}

// ---------------------------------------------------------------------------
QString ContentEngine::safeKey(const QString& raw) {
    // PATH-TRAVERSAL NEUTRALIZATION: keep ONLY lowercase hex chars (the universe
    // of a SHA-256 hex digest). This strips '/', '\\', '.', ':' and any other
    // separator, so a hostile "../../etc/passwd" or "a/b" can never escape the
    // cache dir. Length-capped for filesystem safety.
    QString out;
    out.reserve(raw.size());
    for (QChar c : raw) {
        const ushort u = c.unicode();
        if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f'))
            out.append(c);
        else if (u >= 'A' && u <= 'F')
            out.append(c.toLower());          // normalize hex case
        if (out.size() >= kMaxKeyLen)
            break;
    }
    return out;
}

QString ContentEngine::anchorHexFor(const ContentDescriptor& d) {
    // The document_hash anchor = the descriptor's merkleRoot for multi-chunk files,
    // else the whole-file SHA-256 (small/single-leaf), lowercase hex. The engine's
    // verify() accepts EITHER, so this stays consistent with the verify badge. The
    // ONE shared definition for the mint wizard AND the detail attach-gate (DRY).
    const QByteArray anchor = (d.chunkCount > 1 && !d.merkleRoot.isEmpty())
                                  ? d.merkleRoot : d.sha256Whole;
    return QString::fromLatin1(anchor.toHex());
}

bool ContentEngine::isRemoteUrl(const QString& bytesPath) {
    const QString p = bytesPath.trimmed().toLower();
    return p.startsWith("http://")  || p.startsWith("https://")
        || p.startsWith("ftp://")   || p.startsWith("ipfs://")
        || p.startsWith("zdc1://")  || p.startsWith("zdm:")
        || p.startsWith("//")                 // protocol-relative (forward-slash)
        || p.startsWith("\\\\");              // Windows UNC \\server\share (backslash)
}

// ===========================================================================
// On-disk content-addressed cache
// ===========================================================================
QString ContentEngine::cacheDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir  = base + "/nft_posters";
    QDir().mkpath(dir);                       // idempotent; creates parents
    return dir;
}

QString ContentEngine::blobCacheDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir  = base + "/nft_content";
    QDir().mkpath(dir);
    return dir;
}

QString ContentEngine::cacheGet(const QString& hashHex) {
    const QString key = safeKey(hashHex);
    if (key.isEmpty())
        return QString();                     // empty sentinel: never resolves
    const QString p = blobCacheDir() + "/" + key;
    return QFileInfo::exists(p) ? p : QString();
}

bool ContentEngine::cachePut(const QString& hashHex, const QString& srcPath) {
    const QString key = safeKey(hashHex);
    if (key.isEmpty() || srcPath.isEmpty())
        return false;
    // Refuse to "store" a remote URL (privacy: never auto-fetch).
    if (isRemoteUrl(srcPath))
        return false;

    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly))
        return false;

    const QString dst = blobCacheDir() + "/" + key;
    // Defense-in-depth: the resolved path MUST stay inside blobCacheDir even after
    // canonicalization. safeKey already guarantees this, but assert it.
    const QString canonDir = QFileInfo(blobCacheDir()).absoluteFilePath();
    if (!QFileInfo(dst).absoluteFilePath().startsWith(canonDir + "/"))
        return false;

    if (QFileInfo::exists(dst))
        return true;                          // store-once: already cached

    // Atomic streaming copy (NEVER readAll — could be a multi-GB blob).
    QSaveFile out(dst);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    QByteArray buf;
    buf.resize(kHashBufBytes);
    while (true) {
        const qint64 n = src.read(buf.data(), buf.size());
        if (n < 0) { out.cancelWriting(); return false; }
        if (n == 0) break;
        if (out.write(buf.constData(), n) != n) { out.cancelWriting(); return false; }
    }
    return out.commit();
}

// ===========================================================================
// The worker. A plain QRunnable so it auto-deletes after run(). It owns only
// value-type copies of its inputs + a guarded pointer back to the engine for the
// queued deliver(). Three jobs in one task type, branched by `_op`.
// ===========================================================================
enum CtTaskOp { Op_Poster = 0, Op_Hash = 1, Op_Verify = 2 };

class ContentTask : public QRunnable {
public:
    ContentTask(QPointer<ContentEngine> owner, int op,
                QString hash, QString bytesPath, QString onChainHashHex,
                int sizePx, quint64 token, QString inflightKey,
                QSharedPointer<std::atomic<bool>> cancel)
        : _owner(owner), _op(op), _hash(hash), _bytesPath(bytesPath),
          _onChainHashHex(onChainHashHex.toLower()), _sizePx(sizePx),
          _token(token), _inflightKey(inflightKey), _cancel(cancel),
          _delivered(false) {}

    // INFLIGHT-KEY LEAK FIX (review #2): EVERY exit path of run() — including a
    // cancel or a null/destroyed _owner — must release the in-flight key, else
    // an identical request can never be re-queued (the key is stuck forever). A
    // successful delivery clears the key on the GUI thread inside deliver*(); the
    // dtor here is the safety net for every NON-delivering path. endJob() locks
    // _inflightMtx so it is safe to call from this worker thread, and remove() of
    // an absent key is a harmless no-op (so a benign double-clear can't corrupt).
    ~ContentTask() override {
        if (!_delivered && !_owner.isNull())
            _owner.data()->endJob(_inflightKey);
    }

    void run() override {
        std::atomic<bool>* cflag = _cancel ? _cancel.data() : nullptr;
        if (_op == Op_Hash)        runHash(cflag);
        else if (_op == Op_Verify) runVerify(cflag);
        else                       runPoster(cflag);
    }

private:
    // ---- compute verifyState by streaming the bytes once ----------------------
    // Accepts EITHER the bare whole-file SHA-256 OR the chunked Merkle root as the
    // anchor (resolves anchor-ambiguity without a manifest). 0 if nothing to check.
    int computeVerify(std::atomic<bool>* cancel, ContentDescriptor* descOut) {
        // DOUBLE-HASH FIX (review #4): only compute what the anchor type requires.
        // When the caller needs the full descriptor (descOut != null, e.g. the
        // poster path that also reports fileSize) we must run the full single-pass
        // streamDescribe (whole-SHA + Merkle). But for a bare verify with NO
        // descriptor consumer and a 64-hex anchor, a single streaming SHA-256 is
        // enough to settle the common (small/image bare-SHA) case WITHOUT also
        // building the Merkle tree. We keep the accept-either-anchor guarantee:
        // if the fast bare-SHA does not match, we fall back to the full Merkle
        // pass so a large-file Merkle-root anchor still verifies.
        if (!descOut) {
            if (_onChainHashHex.isEmpty())
                return CE_Pending;            // nothing to verify against (no read needed)
            const QByteArray bare = ContentEngine::streamingSha256(_bytesPath, cancel);
            if (bare.isEmpty())
                return CE_Pending;            // unreadable / cancelled
            if (QString::fromLatin1(bare.toHex()) == _onChainHashHex)
                return CE_Verified;           // bare-SHA anchor (small/image) matched in ONE pass
            // Bare SHA did not match: the anchor may be a Merkle root. Do the full
            // (second) pass ONLY now, and ONLY for a non-empty file (an empty file
            // has an empty Merkle root that can never equal a 64-hex anchor).
            ContentDescriptor d;
            if (!ContentEngine::streamDescribe(_bytesPath, ContentEngine::kChunkBytes,
                                               d, nullptr, cancel))
                return CE_Pending;
            const QString rootHex = QString::fromLatin1(d.merkleRoot.toHex());
            return (rootHex == _onChainHashHex) ? CE_Verified : CE_Mismatch;
        }

        // Descriptor consumer wants the full single-pass result (whole + Merkle).
        ContentDescriptor d;
        if (!ContentEngine::streamDescribe(_bytesPath, ContentEngine::kChunkBytes,
                                           d, nullptr, cancel))
            return CE_Pending;                // unreadable / cancelled
        *descOut = d;
        if (_onChainHashHex.isEmpty())
            return CE_Pending;                // nothing to verify against

        const QString wholeHex = QString::fromLatin1(d.sha256Whole.toHex());
        const QString rootHex  = QString::fromLatin1(d.merkleRoot.toHex());
        if (wholeHex == _onChainHashHex || rootHex == _onChainHashHex)
            return CE_Verified;
        return CE_Mismatch;
    }

    void runHash(std::atomic<bool>* cancel) {
        ContentDescriptor d;
        QString mime;
        // Sniff MIME header-only (no whole read) before the streaming pass.
        d.ok = false;
        if (!_bytesPath.isEmpty()) {
            ContentEngine::classifyKind(_bytesPath, mime);
            ContentDescriptor sd;
            if (ContentEngine::streamDescribe(_bytesPath, ContentEngine::kChunkBytes,
                                              sd, nullptr, cancel)) {
                d = sd;
                d.mime     = mime;
                d.filename = QFileInfo(_bytesPath).fileName();
            }
        }
        // On cancel/null-owner we do NOT deliver; the dtor's safety net releases
        // the in-flight key (review #2). On a successful queue, mark _delivered so
        // the GUI-thread deliver*() owns the key release (avoids a double-clear).
        if (cancel && cancel->load()) return;
        if (!_owner.isNull()) {
            _delivered = true;
            QMetaObject::invokeMethod(_owner.data(), "deliverDescriptor", Qt::QueuedConnection,
                                      Q_ARG(quint64, _token),
                                      Q_ARG(ContentDescriptor, d),
                                      Q_ARG(QString, _inflightKey));
        }
    }

    void runVerify(std::atomic<bool>* cancel) {
        const int vs = computeVerify(cancel, nullptr);
        if (cancel && cancel->load()) return;
        if (!_owner.isNull()) {
            _delivered = true;
            QMetaObject::invokeMethod(_owner.data(), "deliverVerify", Qt::QueuedConnection,
                                      Q_ARG(quint64, _token),
                                      Q_ARG(int, vs),
                                      Q_ARG(QString, _inflightKey));
        }
    }

    void runPoster(std::atomic<bool>* cancel) {
        int    verifyState = CE_Pending;
        QImage thumb;                          // worker produces ONLY a QImage

        // ---- 1) classify (header-only sniff; RAM-safe) --------------------------
        QString mime;
        const ContentKind kind = _bytesPath.isEmpty()
                                   ? CK_Bytes
                                   : ContentEngine::classifyKind(_bytesPath, mime);

        // ---- 2) streaming verify (single pass, bounded RAM) --------------------
        // For IMAGES we still verify by streaming the bytes; this replaces the old
        // readAll()+one-shot hash. computeVerify reads via a 1 MiB buffer.
        ContentDescriptor desc;
        bool haveDesc = false;
        if (!_bytesPath.isEmpty()) {
            verifyState = computeVerify(cancel, &desc);
            haveDesc = desc.ok;
        }
        if (cancel && cancel->load()) return;

        // IMAGE-CLASSIFICATION-REGRESSION FIX (review #7): the original
        // nftimagecache ALWAYS attempted QImageReader::read(); the MIME-gated
        // rewrite only decoded when kind==CK_Image, so a valid image with an odd
        // or missing extension (and an unsniffable header) would never get a
        // thumbnail. Restore the fall-back: ATTEMPT a decode for a confirmed
        // image OR when the kind is AMBIGUOUS (mime empty => sniff failed, kind
        // collapsed to CK_Bytes). NEVER attempt a decode for a CLEARLY non-image
        // kind (a sniffed video/document) — that still gets the typed glyph and
        // we never spin QImageReader on a multi-GB video.
        const bool ambiguousKind = mime.isEmpty();      // sniff failed -> try decode
        const bool tryDecode = haveDesc && (kind == CK_Image || ambiguousKind);
        if (tryDecode) {
            // ---- 3a) IMAGE decode (down-scaling huge sources at decode time) ----
            // Re-open the file for the decode pass. QImageReader::size() is
            // header-only, so setScaledSize() decodes downscaled — no full inflate.
            QImageReader reader(_bytesPath);
            reader.setAutoTransform(true);
            const QSize src = reader.size();
            const bool tooBig =
                (src.isValid() && src.width() > kMaxSourceWidth) ||
                (desc.fileSize > static_cast<quint64>(kMaxFileBytes));
            if (tooBig && src.isValid()) {
                QSize scaled = src;
                scaled.scale(kDecodeCapPx, kDecodeCapPx, Qt::KeepAspectRatio);
                if (!scaled.isEmpty())
                    reader.setScaledSize(scaled);
            }
            const QImage decoded = reader.read();
            if (!decoded.isNull())
                thumb = decoded.scaledToWidth(_sizePx, Qt::SmoothTransformation);
        }
        if (thumb.isNull() && haveDesc) {
            // ---- 3b) NON-IMAGE (or a failed/ambiguous decode): typed glyph poster
            // (no decode, no fake frame). For an ambiguous kind whose decode just
            // failed, fall back to the generic bytes glyph rather than leaving the
            // card blank.
            thumb = renderTypedPoster(kind, _sizePx);
        }

        // ---- 4) atomic PNG write to the on-disk poster cache -------------------
        if (!thumb.isNull() && !_hash.isEmpty()) {
            const QString key = ContentEngine::safeKey(_hash);
            if (!key.isEmpty()) {
                const QString out = ContentEngine::cacheDir()
                                  + "/" + key + "_" + QString::number(_sizePx) + ".png";
                QSaveFile sf(out);
                if (sf.open(QIODevice::WriteOnly)) {
                    if (thumb.save(&sf, "PNG")) sf.commit();
                    else                        sf.cancelWriting();
                }
            }
        }

        // ---- hand back to the GUI thread (QImage only; NO QPixmap here) ---------
        if (cancel && cancel->load()) return;
        if (!_owner.isNull()) {
            _delivered = true;
            QMetaObject::invokeMethod(_owner.data(), "deliver", Qt::QueuedConnection,
                                      Q_ARG(QString, _hash),
                                      Q_ARG(QImage, thumb),
                                      Q_ARG(int, verifyState),
                                      Q_ARG(QString, _inflightKey),
                                      Q_ARG(quint64, _token));
        }
    }

    // A small native typed glyph (film-strip for video, doc sheet for documents,
    // bytes box otherwise) on the dark inset — so a non-image NFT is never blank,
    // and we NEVER fake a video frame (no codec in the static bundle).
    static QImage renderTypedPoster(ContentKind kind, int sizePx) {
        if (sizePx <= 0) sizePx = 1;
        QImage img(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
        img.fill(kInsetBg);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(kGlyphFg);
        pen.setWidthF(qMax(1.0, sizePx / 48.0));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        const qreal m = sizePx * 0.22;        // margin
        const QRectF box(m, m, sizePx - 2 * m, sizePx - 2 * m);

        if (kind == CK_Video) {
            // film-strip: a rounded rect + a centered play triangle
            p.drawRoundedRect(box, sizePx * 0.04, sizePx * 0.04);
            const qreal ph = box.height() * 0.42;
            const qreal pw = ph * 0.86;
            const QPointF c = box.center();
            QPolygonF tri;
            tri << QPointF(c.x() - pw / 2, c.y() - ph / 2)
                << QPointF(c.x() - pw / 2, c.y() + ph / 2)
                << QPointF(c.x() + pw / 2, c.y());
            p.setBrush(kGlyphFg);
            p.drawPolygon(tri);
        } else if (kind == CK_Document) {
            // document sheet with a folded corner + ruled lines
            const qreal fold = box.width() * 0.28;
            QPolygonF sheet;
            sheet << box.topLeft()
                  << QPointF(box.right() - fold, box.top())
                  << QPointF(box.right(), box.top() + fold)
                  << box.bottomRight()
                  << box.bottomLeft();
            p.drawPolygon(sheet);
            const qreal y0 = box.top() + box.height() * 0.45;
            for (int i = 0; i < 3; ++i) {
                const qreal y = y0 + i * box.height() * 0.16;
                p.drawLine(QPointF(box.left() + box.width() * 0.16, y),
                           QPointF(box.right() - box.width() * 0.16, y));
            }
        } else {
            // generic bytes: a box with a small binary tick mark
            p.drawRect(box);
            p.drawLine(box.center() - QPointF(box.width() * 0.18, 0),
                       box.center() + QPointF(box.width() * 0.18, 0));
        }
        p.end();
        return img;
    }

    QPointer<ContentEngine> _owner;
    int     _op;
    QString _hash;
    QString _bytesPath;
    QString _onChainHashHex;
    int     _sizePx;
    quint64 _token;
    QString _inflightKey;
    QSharedPointer<std::atomic<bool>> _cancel;
    bool    _delivered;   // true once a deliver*() has been queued (GUI thread owns key release)
};

// ===========================================================================
// ContentEngine — GUI-thread methods
// ===========================================================================
ContentEngine::ContentEngine(NFTGalleryModel* model, QObject* parent)
    : QObject(parent), _model(model) {
    // QImage + ContentDescriptor cross the thread boundary as queued-connection
    // arguments; register them so invokeMethod / signals always resolve them.
    qRegisterMetaType<QImage>("QImage");
    qRegisterMetaType<ContentDescriptor>("ContentDescriptor");

    _pool.setMaxThreadCount(4);                 // bounded CPU/IO
    QPixmapCache::setCacheLimit(128 * 1024);    // 128 MB (cacheLimit is in KB)
}

ContentEngine::~ContentEngine() {
    // Signal every in-flight job to stop, THEN block until workers drain. clear()
    // alone only drops not-yet-started runnables; the atomic stops a running
    // multi-GB hash promptly. _owner is a QPointer, so a straggler is a safe no-op.
    cancelAll();
    _pool.clear();
    _pool.waitForDone();
}

QString ContentEngine::inflightKeyFor(const QString& hash, int sizePx) {
    return hash + "@" + QString::number(sizePx);
}

QSharedPointer<std::atomic<bool>> ContentEngine::beginJob(const QString& key) {
    // Caller holds _inflightMtx.
    auto flag = QSharedPointer<std::atomic<bool>>::create(false);
    _inflight.insert(key);
    _cancelFlags.insert(key, flag);
    return flag;
}

void ContentEngine::endJob(const QString& key) {
    QMutexLocker lock(&_inflightMtx);
    _inflight.remove(key);
    _cancelFlags.remove(key);
}

void ContentEngine::cancel(const QString& target) {
    QMutexLocker lock(&_inflightMtx);
    // CANCEL-NAMESPACING FIX (review #5). The three in-flight key schemes are:
    //   * poster   : "<contentHash>@<sizePx>"   (content-hash addressed)
    //   * hashFile : "hashjob#<token>"          (token addressed)
    //   * verify   : "verify#<token>"           (token addressed)
    // `target` cancels by EITHER:
    //   (a) an exact full-key match (cancel one precise job — works for ALL three
    //       schemes, including a token job whose full key the caller holds), OR
    //   (b) a content-hash match for poster jobs: target == the content hash =>
    //       cancel every "<hash>@<px>" size variant of that asset.
    // We deliberately do NOT do an over-broad startsWith(target + "#") match: that
    // is what let cancel("hashjob") / cancel("verify") sweep UNRELATED token jobs,
    // and a content hash could never legitimately be the literal "hashjob"/"verify"
    // prefix anyway. A token job is therefore cancellable only by its exact key
    // (precise) — never accidentally swept by a content-hash cancel.
    const QString posterPrefix = target + "@";
    for (auto it = _cancelFlags.begin(); it != _cancelFlags.end(); ++it) {
        const QString& key = it.key();
        if (key == target || key.startsWith(posterPrefix)) {
            if (it.value()) it.value()->store(true);
        }
    }
}

void ContentEngine::cancelAll() {
    QMutexLocker lock(&_inflightMtx);
    for (auto it = _cancelFlags.begin(); it != _cancelFlags.end(); ++it)
        if (it.value()) it.value()->store(true);
}

void ContentEngine::request(const QString& hash, const QString& bytesPath,
                            const QString& onChainHashHex, int sizePx) {
    if (hash.isEmpty() || bytesPath.isEmpty() || sizePx <= 0)
        return;
    // PRIVACY GUARD (hard): never accept a remote URL as bytes; the engine has no
    // network code and must never be coaxed into fetching one.
    if (isRemoteUrl(bytesPath))
        return;

    const QString key = inflightKeyFor(hash, sizePx);

    // CACHE SHORT-CIRCUIT (review #3): the 'cache avoids rehashing' claim is about
    // the CONTENT-ADDRESSED BLOB STORE (cacheGet/cachePut, keyed by the true
    // content hash and populated ONLY by a verified cachePut). When the on-chain
    // anchor IS the cache key (hash == onChainHashHex — the production call) AND a
    // blob is already stored under that hash, the bytes were already proven to
    // hash to the anchor, so the content is Verified BY CONSTRUCTION: re-queuing a
    // worker to re-stream+re-hash the same bytes is pure waste. In that case we
    // deliver the poster image from the on-disk poster cache (or a typed glyph if
    // none) with CE_Verified, skipping the worker entirely.
    //
    // IMPORTANT (the unsound trap we must NOT fall into): a poster PNG by itself
    // is NOT proof of verification — runPoster() writes a thumbnail for ANY
    // decodable image regardless of verifyState (incl. a MISMATCH). So we gate the
    // short-circuit on the content-addressed BLOB (cacheGet), never on mere poster
    // existence; otherwise a cached mismatch poster would be mis-reported Verified.
    const bool trustedHash = !onChainHashHex.isEmpty()
                          && hash.compare(onChainHashHex, Qt::CaseInsensitive) == 0;
    if (trustedHash && !cacheGet(hash).isEmpty()) {
        // Blob present for the trusted hash => content already verified; no rehash.
        QPixmap pm;
        // Prefer the in-RAM QPixmapCache, then the on-disk poster PNG (image only).
        if (!QPixmapCache::find(key, &pm) || pm.isNull()) {
            const QString safe = safeKey(hash);
            if (!safe.isEmpty()) {
                const QString posterPng = cacheDir() + "/" + safe
                                        + "_" + QString::number(sizePx) + ".png";
                if (QFileInfo::exists(posterPng)) {
                    QPixmap disk(posterPng);
                    if (!disk.isNull()) {
                        pm = disk;
                        QPixmapCache::insert(key, pm);   // seed RAM cache
                    }
                }
            }
        }
        if (!_model.isNull())
            _model->onImageReady(hash, pm, CE_Verified);  // pm may be null (glyph deferred)
        return;                                           // delivered from cache; NO re-hash
    }

    QSharedPointer<std::atomic<bool>> flag;
    {
        QMutexLocker lock(&_inflightMtx);
        if (_inflight.contains(key))
            return;                              // already queued -> dedupe
        flag = beginJob(key);
    }

    auto* task = new ContentTask(QPointer<ContentEngine>(this), Op_Poster,
                                 hash, bytesPath, onChainHashHex, sizePx,
                                 /*token*/0, key, flag);
    task->setAutoDelete(true);
    _pool.start(task);
}

void ContentEngine::posterFor(const QString& path, const QString& hash,
                              const QString& expectedHashHex, int sizePx) {
    request(hash, path, expectedHashHex, sizePx);
}

void ContentEngine::posterForToken(const QString& path, const QString& hash,
                                   const QString& expectedHashHex, int sizePx,
                                   quint64 token) {
    // Never leave the dialog hanging: any reject path emits a CE_Pending null
    // poster so the caller can render its honest fallback immediately.
    if (token == 0 || path.isEmpty() || sizePx <= 0) {
        emit posterReady(token, QImage(), CE_Pending);
        return;
    }
    // PRIVACY GUARD (same as request()): the engine has zero network code and must
    // never be coaxed into fetching a remote URL.
    if (isRemoteUrl(path)) {
        emit posterReady(token, QImage(), CE_Pending);
        return;
    }

    // Namespaced, token-addressed key (review #5 discipline): a content-hash
    // cancel() can never sweep "poster#<token>", and it can never collide with /
    // dedupe against a gallery thumbnail's "<hash>@<px>" key. Cancellable only by
    // its exact key.
    const QString key = "poster#" + QString::number(token);
    QSharedPointer<std::atomic<bool>> flag;
    {
        QMutexLocker lock(&_inflightMtx);
        if (_inflight.contains(key))
            return;                              // already queued -> dedupe
        flag = beginJob(key);
    }
    auto* task = new ContentTask(QPointer<ContentEngine>(this), Op_Poster,
                                 hash, path, expectedHashHex, sizePx,
                                 /*token*/token, key, flag);
    task->setAutoDelete(true);
    _pool.start(task);
}

void ContentEngine::hashFile(const QString& path, quint64 token) {
    if (path.isEmpty() || isRemoteUrl(path)) {
        // Emit an empty (ok=false) descriptor so callers never hang.
        ContentDescriptor d;
        emit descriptorReady(token, d);
        return;
    }
    // Namespaced, token-addressed key (review #5): a distinct "hashjob#" prefix so
    // a content-hash cancel can never accidentally sweep it, and it is cancellable
    // by its exact key.
    const QString key = "hashjob#" + QString::number(token);
    QSharedPointer<std::atomic<bool>> flag;
    {
        QMutexLocker lock(&_inflightMtx);
        if (_inflight.contains(key))
            return;
        flag = beginJob(key);
    }
    auto* task = new ContentTask(QPointer<ContentEngine>(this), Op_Hash,
                                 /*hash*/QString(), path, /*anchor*/QString(),
                                 /*sizePx*/0, token, key, flag);
    task->setAutoDelete(true);
    _pool.start(task);
}

void ContentEngine::verify(const QString& path, const QString& expectedHashHex, quint64 token) {
    if (path.isEmpty() || isRemoteUrl(path)) {
        emit verifyDone(token, CE_Pending);
        return;
    }
    const QString key = "verify#" + QString::number(token);
    QSharedPointer<std::atomic<bool>> flag;
    {
        QMutexLocker lock(&_inflightMtx);
        if (_inflight.contains(key))
            return;
        flag = beginJob(key);
    }
    auto* task = new ContentTask(QPointer<ContentEngine>(this), Op_Verify,
                                 /*hash*/QString(), path, expectedHashHex,
                                 /*sizePx*/0, token, key, flag);
    task->setAutoDelete(true);
    _pool.start(task);
}

// ---- GUI-thread landings --------------------------------------------------
void ContentEngine::deliver(QString hash, QImage img, int verifyState,
                            QString inflightKey, quint64 token) {
    // GUI thread. Build the QPixmap HERE (never in the worker), seed the global
    // QPixmapCache, then notify the consumer.
    QPixmap pm;
    if (!img.isNull()) {
        pm = QPixmap::fromImage(img);
        if (!pm.isNull())
            QPixmapCache::insert(inflightKey, pm);
    }
    endJob(inflightKey);

    if (token != 0) {
        // Token-addressed (detail dialog): hand back the LARGE decoded image; do
        // NOT touch the gallery model. img may be null (undecodable / glyph) -> the
        // dialog shows its own fallback. verifyState is fingerprint-match only.
        emit posterReady(token, img, verifyState);
        return;
    }
    // Gallery path (token==0): UNCHANGED original behavior.
    if (!_model.isNull())
        _model->onImageReady(hash, pm, verifyState);
}

void ContentEngine::deliverDescriptor(quint64 token, ContentDescriptor d, QString inflightKey) {
    endJob(inflightKey);
    emit descriptorReady(token, d);
}

void ContentEngine::deliverVerify(quint64 token, int verifyState, QString inflightKey) {
    endJob(inflightKey);
    emit verifyDone(token, verifyState);
}
