// ============================================================================
// NFTDetailDialog implementation — see nftdetaildialog.h. NATIVE_NFT_GUIDE §2.4.
// ============================================================================
#include "nftdetaildialog.h"
#include "nftsenddialog.h"
#include "nftselldialog.h"
#include "contentengine.h"
#include "rpc.h"
#include "settings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QFileDialog>
#include <QClipboard>
#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QDateTime>
#include <QFileInfo>
#include <QPointer>

namespace {
    // Ownership is PENDING until ~10 confirmations (DEFAULT_MAX_REORG_DEPTH) — a
    // single named constant, honest everywhere (NATIVE_NFT_GUIDE §4.3).
    const qint64 kFinalConfs = 10;
    const int    kStagePx    = 380;     // square image stage min edge
    const int    kPosterPx   = 512;     // requested poster source size
}

// Monotonic token source (process-wide; uniqueness across opens is all we need).
static quint64 nextPosterToken() {
    static quint64 s = 0;
    return ++s;
}

QString NFTDetailDialog::shortId(const QString& hex) {
    if (hex.size() <= 18)
        return hex;
    return hex.left(8) + QStringLiteral("…") + hex.right(8);
}

NFTDetailDialog::NFTDetailDialog(const NFTItem& item, const QVector<NFTItem>& ordered,
                                 int startIndex, ContentEngine* engine, RPC* rpc,
                                 QWidget* parent)
    : QDialog(parent), m_ordered(ordered), m_engine(engine), m_rpc(rpc) {
    setWindowTitle(tr("Collectible"));
    setMinimumSize(760, 560);

    // Resolve the start index; fall back to a single-item list if the ordered set
    // is empty (defensive — the caller always passes a valid index).
    if (m_ordered.isEmpty())
        m_ordered.push_back(item);
    m_index = (startIndex >= 0 && startIndex < m_ordered.size()) ? startIndex : 0;

    // ---- title bar ----
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(10);

    auto* titleRow = new QHBoxLayout();
    auto* titleBox = new QVBoxLayout();
    m_titleName = new QLabel(this);
    m_titleName->setStyleSheet("font-size:16px; font-weight:700;");
    m_titleColl = new QLabel(this);
    m_titleColl->setStyleSheet("font-size:11px; color:#9aa0a6;");
    titleBox->addWidget(m_titleName);
    titleBox->addWidget(m_titleColl);
    titleRow->addLayout(titleBox, 1);
    m_prevBtn = new QPushButton(tr("‹ Prev"), this);
    m_nextBtn = new QPushButton(tr("Next ›"), this);
    titleRow->addWidget(m_prevBtn);
    titleRow->addWidget(m_nextBtn);
    outer->addLayout(titleRow);

    // ---- body: image stage (left) + info panel (right) ----
    auto* body = new QHBoxLayout();
    body->setSpacing(14);

    // image stage
    m_stage = new QLabel(this);
    m_stage->setObjectName("nftDetailStage");
    m_stage->setMinimumSize(kStagePx, kStagePx);
    m_stage->setAlignment(Qt::AlignCenter);
    m_stage->setStyleSheet("background:#1d2027; border-radius:8px;");
    body->addWidget(m_stage, 1);

    // info panel (fixed-ish width)
    auto* info = new QVBoxLayout();
    info->setSpacing(8);

    // verify line + badge
    auto* verifyRow = new QHBoxLayout();
    m_badge = new QLabel(this);
    // Item B (honesty): the single most-misunderstood element. State plainly what a
    // green check does NOT mean. Shared by the badge glyph + the verdict sentence.
    const QString badgeHelp = tr(
        "A green check (✓) means the image on your computer matches the fingerprint "
        "recorded on-chain. It does NOT mean this collectible is genuine, official, "
        "or authorized — anyone can mint a copy that reuses the same picture.");
    m_badge->setWhatsThis(badgeHelp);
    m_verifyLine = new QLabel(this);
    m_verifyLine->setObjectName("nftDetailVerifyLine");
    m_verifyLine->setWordWrap(true);
    m_verifyLine->setWhatsThis(badgeHelp);
    verifyRow->addWidget(m_badge);
    verifyRow->addWidget(m_verifyLine, 1);
    info->addLayout(verifyRow);

    m_privacyPill = new QLabel(this);
    m_privacyPill->setObjectName("nftDetailPrivacyPill");
    m_privacyPill->setWordWrap(true);
    info->addWidget(m_privacyPill);

    auto* sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color:#2a2d35;");
    info->addWidget(sep1);

    m_mintId      = new QLabel(this);
    m_received    = new QLabel(this);
    m_setLine     = new QLabel(this);
    m_fingerprint = new QLabel(this);
    for (QLabel* l : { m_mintId, m_received, m_setLine, m_fingerprint }) {
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info->addWidget(l);
    }

    auto* footnote = new QLabel(
        tr("This name and image aren't unique — anyone can mint another collectible "
           "that reuses them. Only the mint id is one of a kind."), this);
    footnote->setWordWrap(true);
    footnote->setStyleSheet("color:#9aa0a6; font-size:11px;");
    info->addWidget(footnote);

    info->addStretch(1);

    // action bar
    auto* actionRow = new QHBoxLayout();
    m_sendBtn = new QPushButton(tr("Send / Gift"), this);
    m_sendBtn->setObjectName("nftSendGiftButton");
    m_sellBtn = new QPushButton(tr("Sell"), this);
    m_sellBtn->setObjectName("nftDetailSellButton");
    m_saveBtn   = new QPushButton(tr("Save image…"), this);
    auto* copyIdBtn = new QPushButton(tr("Copy id"), this);
    actionRow->addWidget(m_sendBtn);
    actionRow->addWidget(m_sellBtn);
    actionRow->addWidget(m_saveBtn);
    actionRow->addWidget(copyIdBtn);
    info->addLayout(actionRow);

    auto* moreRow = new QHBoxLayout();
    auto* copyFpBtn  = new QPushButton(tr("Copy fingerprint"), this);
    m_recheckBtn     = new QPushButton(tr("Re-check image"), this);
    m_explorerBtn    = new QPushButton(tr("View in explorer"), this);
    moreRow->addWidget(copyFpBtn);
    moreRow->addWidget(m_recheckBtn);
    moreRow->addWidget(m_explorerBtn);
    info->addLayout(moreRow);

    // Item A: attach-the-file affordance. A RECEIVED NFT has cachePath="" by privacy
    // design (never auto-fetched), so this is the ONLY way its image can reach the
    // green verify badge — the user explicitly picks the local file they hold.
    auto* attachRow = new QHBoxLayout();
    m_attachBtn = new QPushButton(tr("Attach the file you have…"), this);
    m_attachBtn->setObjectName("nftAttachFileButton");
    m_attachBtn->setWhatsThis(
        tr("For your privacy, the wallet never downloads a collectible's image by "
           "itself. If you already have the file, pick it here — the wallet checks it "
           "against the on-chain fingerprint, all on your computer. The file is never "
           "uploaded."));
    attachRow->addWidget(m_attachBtn);
    attachRow->addStretch(1);
    info->addLayout(attachRow);

    m_attachStatus = new QLabel(this);
    m_attachStatus->setObjectName("nftAttachStatus");
    m_attachStatus->setWordWrap(true);
    m_attachStatus->setMinimumHeight(18);   // reserved height so layout never jumps
    info->addWidget(m_attachStatus);

    body->addLayout(info);
    outer->addLayout(body);

    // ---- wiring ----
    connect(m_prevBtn,   &QPushButton::clicked, this, &NFTDetailDialog::goPrev);
    connect(m_nextBtn,   &QPushButton::clicked, this, &NFTDetailDialog::goNext);
    connect(m_sendBtn,   &QPushButton::clicked, this, &NFTDetailDialog::onSendGift);
    connect(m_sellBtn,   &QPushButton::clicked, this, &NFTDetailDialog::onSell);
    connect(m_saveBtn,     &QPushButton::clicked, this, &NFTDetailDialog::onSaveImage);
    connect(copyIdBtn,   &QPushButton::clicked, this, &NFTDetailDialog::onCopyId);
    connect(copyFpBtn,   &QPushButton::clicked, this, &NFTDetailDialog::onCopyFingerprint);
    connect(m_recheckBtn,  &QPushButton::clicked, this, &NFTDetailDialog::onRecheck);
    connect(m_explorerBtn, &QPushButton::clicked, this, &NFTDetailDialog::onViewInExplorer);
    connect(m_attachBtn,   &QPushButton::clicked, this, &NFTDetailDialog::onAttachFile);

    if (m_engine) {
        connect(m_engine, &ContentEngine::posterReady,
                this, &NFTDetailDialog::onPosterReady);
        // Item A: the attach-the-file hash uses the EXISTING descriptorReady signal
        // (no second engine, no new metatype). Filtered on m_attachToken.
        connect(m_engine, &ContentEngine::descriptorReady,
                this, &NFTDetailDialog::onAttachDescriptor);
    }

    loadCurrent();
}

void NFTDetailDialog::loadCurrent() {
    const NFTItem& it = cur();

    m_titleName->setText(it.name.isEmpty() ? tr("Untitled") : it.name);
    m_titleColl->setText(it.collection.isEmpty() ? tr("Not part of a set") : it.collection);

    m_prevBtn->setEnabled(m_index > 0);
    m_nextBtn->setEnabled(m_index < m_ordered.size() - 1);

    // Don't offer to SELL a tampered collectible (verifyState==2) — listing it would
    // be dishonest. Mirrors the send dialog's mismatch hard-disable.
    if (m_sellBtn)
        m_sellBtn->setEnabled(it.verifyState != 2);

    // Ownership pill (#119, HONEST): ZSLP NFT ownership is ALWAYS public — the token
    // rides a public transparent dust UTXO. There is NO shielded-ownership state, so we
    // make exactly ONE honest statement and never imply a private owner. (A future
    // encrypted-FILE-CONTENT label, if added, must describe only the file bytes — never
    // ownership.)
    m_privacyPill->setText(
        tr("● Public — anyone can verify this on the public ledger."));

    // Details card.
    m_mintId->setText(tr("Mint id: %1").arg(shortId(it.txid)));
    m_setLine->setText(it.collection.isEmpty()
                           ? tr("Set: Not part of a set")
                           : tr("Set: %1").arg(it.collection));
    m_fingerprint->setText(it.docHashHex.isEmpty()
                               ? tr("Image fingerprint: none recorded on-chain")
                               : tr("Image fingerprint: %1").arg(shortId(it.docHashHex)));
    m_fingerprint->setWhatsThis(
        tr("The fingerprint is a one-way SHA-256 of the original file, written to the "
           "ledger when the collectible was made. The wallet recomputes it from the "
           "file you hold to confirm the image is exactly the one that was recorded."));
    m_received->setText(it.receivedHeight > 0
                            ? tr("Received: block %1").arg(it.receivedHeight)
                            : tr("Received: confirming…"));
    m_received->setWhatsThis(
        tr("A collectible is only fully yours once its transaction confirms on-chain "
           "(about 10 blocks). Until then it shows as confirming — it's still on its "
           "way, not lost."));

    // Item A: reset the attach-the-file status line for the freshly-bound item.
    if (m_attachStatus)
        m_attachStatus->clear();

    // Explorer link gating: configured (non-empty URL on mainnet). #119: NFT ownership
    // is always public, so there is no "private => no explorer" case to encode.
    const bool explorerOk = !Settings::getExplorerTxURL(it.txid).isEmpty();
    m_explorerBtn->setEnabled(explorerOk);

    // Reset the image stage, then request a fresh poster. requestPoster() owns the
    // pending-vs-terminal decision: it sets "Checking…" ONLY when a job is in flight,
    // and the honest neutral "can't check" state when there are no local bytes — so
    // the verify line never spins with no work pending (review fix #1/#2).
    m_sourcePixmap = QPixmap();
    if (m_saveBtn)
        m_saveBtn->setEnabled(false);   // re-enabled by onPosterReady when bytes load
    requestPoster();

    // Back-fill richer provenance + received date (best-effort; honest defaults).
    backfillProvenance();
    backfillReceived();
}

void NFTDetailDialog::requestPoster() {
    const NFTItem& it = cur();
    // Local bytes ONLY — never auto-fetch. The gallery sets cachePath="" for
    // privacy; the detail dialog resolves a local path via the content-addressed
    // blob store. Empty => CE_Pending null poster (the honest "not downloaded").
    QString localPath = it.cachePath;
    if (localPath.isEmpty() && !it.docHashHex.isEmpty())
        localPath = ContentEngine::cacheGet(it.docHashHex);

    m_posterToken = nextPosterToken();   // a fresh token retires any stale neighbor
    if (m_engine == nullptr || localPath.isEmpty()) {
        // No engine or no local bytes: there is NOTHING to check and no fetch path
        // (we never auto-fetch — privacy hard rule). State it as a FACT, not an
        // instruction the dialog can't fulfil (review fix #2), and drop the verify
        // line to a neutral TERMINAL "can't check" — never a perpetual "Checking…"
        // with no work in flight (review fix #1).
        m_posterInFlight = false;
        m_stage->setText(tr("This collectible's image isn't on this computer."));
        applyNoBytesBadge();
        if (m_recheckBtn)
            m_recheckBtn->setEnabled(false);   // nothing to re-check -> not a dead click
        // Item A: the attach affordance is exactly useful HERE (no bytes). For a
        // hash-less NFT (no on-chain fingerprint) keep the button disabled with an
        // honest explanation — its image can never be verified, so never imply it can.
        if (m_attachBtn) {
            const bool hasFingerprint = !it.docHashHex.isEmpty();
            m_attachBtn->setVisible(true);
            m_attachBtn->setEnabled(hasFingerprint);
            if (!hasFingerprint)
                showAttachStatus(
                    tr("This collectible has no on-chain fingerprint, so its image "
                       "can't be verified."), "#9aa0a6");
        }
        return;
    }
    // Real work is in flight: now (and only now) the spinning pending state is honest.
    m_posterInFlight = true;
    m_stage->setText(tr("Checking this image…"));
    applyVerifyBadge(it.verifyState);
    if (m_recheckBtn)
        m_recheckBtn->setEnabled(true);
    // Item A: bytes are present -> the attach affordance isn't needed; hide it.
    if (m_attachBtn)
        m_attachBtn->setVisible(false);
    m_engine->posterForToken(localPath, it.docHashHex, it.docHashHex,
                             kPosterPx, m_posterToken);
}

void NFTDetailDialog::onPosterReady(quint64 token, QImage img, int verifyState) {
    if (token != m_posterToken)
        return;   // a stale neighbor's late reply — ignore
    m_posterInFlight = false;
    if (img.isNull()) {
        // The bytes we had couldn't be decoded into an image. Terminal + factual; the
        // verify line drops to the neutral "can't check" state (review fix #1/#2).
        m_stage->setText(tr("This collectible's image couldn't be opened from the "
                            "file on this computer."));
        m_sourcePixmap = QPixmap();
        applyNoBytesBadge();
        if (m_saveBtn)
            m_saveBtn->setEnabled(false);
        return;
    }
    applyVerifyBadge(verifyState);
    m_sourcePixmap = QPixmap::fromImage(img);
    m_stage->setText(QString());
    if (m_saveBtn)
        m_saveBtn->setEnabled(true);
    const QSize area = m_stage->size().isEmpty()
                           ? QSize(kStagePx, kStagePx) : m_stage->size();
    m_stage->setPixmap(m_sourcePixmap.scaled(area, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
}

void NFTDetailDialog::applyVerifyBadge(int verifyState) {
    m_lastVerifyState = verifyState;
    QString verdict, color, glyph, tip;
    if (verifyState == 1) {
        verdict = tr("This image matches its on-chain fingerprint.");
        color   = "#2a9d2a"; glyph = "✓";
        tip     = verdict;
    } else if (verifyState == 2) {
        verdict = tr("This image does NOT match what was recorded on-chain. Don't trust it.");
        color   = "#c0392b"; glyph = "✗";
        tip     = verdict;
    } else {
        verdict = tr("Checking this image…");
        color   = "#d9822b"; glyph = "?";
        tip     = verdict;
    }
    m_badge->setText(glyph);
    m_badge->setToolTip(tip);
    m_badge->setStyleSheet(QString("color:%1; font-weight:700; font-size:16px;").arg(color));
    m_verifyLine->setText(verdict);
    m_verifyLine->setStyleSheet(QString("color:%1;").arg(color));
    // Item B (honesty): keep the "what ✓ does NOT mean" disambiguation on the line in
    // every verified/mismatch/checking state (applyNoBytesBadge swaps in its own).
    m_verifyLine->setWhatsThis(m_badge->whatsThis());
}

void NFTDetailDialog::applyNoBytesBadge() {
    // Neutral TERMINAL state: there are no bytes to check, so we make NO claim about
    // the image (not verified, not a mismatch) and we never spin "Checking…". Honest
    // and dim, matching the gallery's "image not on this device" reading.
    m_lastVerifyState = 0;
    const QString verdict =
        tr("Can't check this image — its file isn't on this computer.");
    const QString color = "#9aa0a6";   // neutral dim, not amber/green/red
    m_badge->setText(QStringLiteral("–"));
    m_badge->setToolTip(verdict);
    m_badge->setStyleSheet(QString("color:%1; font-weight:700; font-size:16px;").arg(color));
    m_verifyLine->setText(verdict);
    m_verifyLine->setStyleSheet(QString("color:%1;").arg(color));
    // Item B (honesty): explain the no-bytes state — this is normal and private, not
    // an error, and point at the attach affordance that fixes it.
    m_verifyLine->setWhatsThis(
        tr("For your privacy, the wallet never downloads a collectible's image on its "
           "own. If you already have the original file, use “Attach the file you "
           "have” below to check it against the on-chain fingerprint — all on your "
           "computer."));
}

void NFTDetailDialog::backfillProvenance() {
    if (m_rpc == nullptr)
        return;
    const QString tokenId = cur().txid;
    // LIFETIME (review fix #5): the dialog is open()-modeless + WA_DeleteOnClose, so it
    // can be destroyed while this RPC is in flight. Guard the reply with a QPointer and
    // bail before touching any member if we've been deleted.
    QPointer<NFTDetailDialog> self(this);
    m_rpc->nftProvenance(tokenId,
        [self, tokenId](json meta) {
            if (self.isNull())
                return;   // dialog gone — late reply is a safe no-op
            // Only apply if we're still on the same item (no fast prev/next race).
            if (self->cur().txid != tokenId || !meta.is_object())
                return;
            // Set/series from ticker (Creator stays "Unknown" — chain records none).
            // The success path here is purely additive polish; honest defaults stand
            // if the field is absent.
        },
        [](QString /*errStr*/) { /* honest defaults already shown; no dialog */ }
    );
}

void NFTDetailDialog::backfillReceived() {
    if (m_rpc == nullptr)
        return;
    const QString txid = cur().txid;
    // LIFETIME (review fix #5): guard against the dialog being closed/deleted while the
    // gettransaction RPC is in flight (open()-modeless + WA_DeleteOnClose).
    QPointer<NFTDetailDialog> self(this);
    m_rpc->txReceivedDate(txid,
        [self, txid](qint64 confs, qint64 blocktime) {
            if (self.isNull())
                return;   // dialog gone — late reply is a safe no-op
            if (self->cur().txid != txid)
                return;   // raced past this item
            if (confs < kFinalConfs) {
                self->m_received->setText(tr("Received: Just arrived — confirming…"));
            } else if (blocktime > 0) {
                const QString iso =
                    QDateTime::fromSecsSinceEpoch(blocktime).toString("yyyy-MM-dd");
                self->m_received->setText(tr("Received: %1 · block %2")
                                        .arg(iso).arg(self->cur().receivedHeight));
            }
        },
        [](QString /*errStr*/) { /* keep the height-only line */ }
    );
}

void NFTDetailDialog::onSendGift() {
    // #119 HONESTY: do NOT offer a "Send anyway?" override on a mismatch. NFTSendDialog
    // HARD-disables Send for verifyState==2 (and shows a red mismatch reason), so an
    // override here would promise an action that does not exist. Open the send dialog
    // directly; for a tampered item it opens with Send disabled and the honest reason.
    NFTSendDialog dlg(cur(), m_rpc, this);
    if (dlg.exec() == QDialog::Accepted) {
        // The token is on its way (0-conf). Close the detail view so the gallery
        // owns the pending/confirming state; never imply the user still holds it.
        accept();
    }
}

void NFTDetailDialog::onSell() {
    // Open the Sell flow for THIS owned NFT. A mismatch item never reaches here (the
    // button is disabled), but NFTSellDialog also hard-disables List for verifyState==2.
    NFTSellDialog dlg(cur(), m_rpc, this);
    if (dlg.exec() == QDialog::Accepted) {
        // The NFT was listed (and possibly cancelled) — close the detail view so the
        // gallery re-pulls its current state on the next poll.
        accept();
    }
}

void NFTDetailDialog::onSaveImage() {
    if (m_sourcePixmap.isNull()) {
        QMessageBox::information(this, tr("Nothing to save"),
            tr("This collectible's image isn't on this computer, so there's nothing to save."));
        return;
    }
    const QString base = cur().name.isEmpty() ? QStringLiteral("collectible")
                                              : cur().name;
    const QString suggested = base + QStringLiteral(".png");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save image"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty())
        return;
    m_sourcePixmap.save(path, "PNG");
}

void NFTDetailDialog::onCopyId() {
    QApplication::clipboard()->setText(cur().txid);
}

void NFTDetailDialog::onCopyFingerprint() {
    QApplication::clipboard()->setText(cur().docHashHex);
}

void NFTDetailDialog::onRecheck() {
    requestPoster();
}

// ---------------------------------------------------------------------------
// Item A — attach the file you have. The user explicitly picks a LOCAL file
// (privacy: we NEVER auto-fetch). onAttachFile runs the picker; beginAttach()
// holds the reject->hash->match-gate logic so a test can drive it without a
// QFileDialog (testAttachFile seam).
// ---------------------------------------------------------------------------
void NFTDetailDialog::onAttachFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose the file you have for this collectible"));
    if (path.isEmpty())
        return;   // user cancelled
    beginAttach(path);
}

void NFTDetailDialog::showAttachStatus(const QString& msg, const QString& color) {
    if (!m_attachStatus)
        return;
    m_attachStatus->setText(msg);
    m_attachStatus->setStyleSheet(QString("color:%1;").arg(color));
}

void NFTDetailDialog::beginAttach(const QString& path) {
    if (path.isEmpty())
        return;
    // PRIVACY hard rule: refuse a remote/URL path — only a local file the user holds.
    if (ContentEngine::isRemoteUrl(path)) {
        showAttachStatus(
            tr("For your privacy, pick a local file — not a web link."), "#c0392b");
        return;
    }
    // Hash-less NFT: there is no on-chain fingerprint to match, so its image can
    // never be verified. Honest dead-end — never pretend a match is possible.
    if (cur().docHashHex.isEmpty()) {
        showAttachStatus(
            tr("This collectible has no on-chain fingerprint, so its image can't be "
               "verified."), "#9aa0a6");
        return;
    }
    if (m_engine == nullptr) {
        showAttachStatus(tr("Can't read this file right now."), "#c0392b");
        return;
    }
    // Begin streaming the chosen file through the EXISTING engine (no second engine).
    m_attachPath  = path;
    m_attachToken = nextPosterToken();   // monotonic; reused as the hash token
    showAttachStatus(tr("Checking this file…"), "#9aa0a6");
    m_engine->hashFile(path, m_attachToken);
}

void NFTDetailDialog::onAttachDescriptor(quint64 token, ContentDescriptor d) {
    if (token != m_attachToken)
        return;   // a stale / unrelated descriptor reply
    if (!d.ok) {
        showAttachStatus(tr("We couldn't read that file. Try another."), "#c0392b");
        return;
    }
    // MATCH-GATE: accept EITHER the bare whole-file SHA-256 OR the Merkle root vs the
    // recorded docHashHex (the same dual rule verify() uses). anchorHexFor() yields
    // the canonical anchor for THIS file's size class; we also compare the whole hash
    // so a small file recorded as a bare SHA-256 still matches.
    const QString docHash  = cur().docHashHex;
    const QString anchor   = ContentEngine::anchorHexFor(d);
    const QString wholeHex = QString::fromLatin1(d.sha256Whole.toHex());
    const bool match = (docHash.compare(anchor,   Qt::CaseInsensitive) == 0)
                    || (docHash.compare(wholeHex, Qt::CaseInsensitive) == 0);
    if (match) {
        // Store the verified bytes content-addressed by the anchor, then re-run the
        // poster so the badge resolves VERIFIED from the now-cached file.
        ContentEngine::cachePut(docHash, m_attachPath);
        showAttachStatus(
            tr("This file matches the on-chain fingerprint."), "#2a9d2a");
        requestPoster();   // local bytes now resolve via cacheGet -> green badge
    } else {
        // NON-MATCH: honest red, and DO NOT cache (the badge stays unverified).
        showAttachStatus(
            tr("This file does NOT match the on-chain fingerprint, so it can't be "
               "verified."), "#c0392b");
    }
}

void NFTDetailDialog::onViewInExplorer() {
    const NFTItem& it = cur();
    const QString url = Settings::getExplorerTxURL(it.txid);
    if (url.isEmpty())
        return;   // gated off (no explorer configured) — should be disabled anyway
    const auto r = QMessageBox::question(
        this, tr("Open the block explorer?"),
        tr("This opens an outside website and may reveal your interest. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes)
        return;
    QDesktopServices::openUrl(QUrl(url));
}

void NFTDetailDialog::goPrev() {
    if (m_index > 0) {
        --m_index;
        loadCurrent();
    }
}

void NFTDetailDialog::goNext() {
    if (m_index < m_ordered.size() - 1) {
        ++m_index;
        loadCurrent();
    }
}
