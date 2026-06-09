// ============================================================================
// NFTBuyDialog implementation — see nftbuydialog.h. NFT_SELL_DESIGN.md §5.
// ============================================================================
#include "nftbuydialog.h"
#include "nftcommon.h"
#include "contentengine.h"
#include "settings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QFrame>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QPointer>

namespace {
    const int kStagePx = 240;   // square image stage min edge
    const int kPosterPx = 384;  // requested poster source size

    // Monotonic per-open token so a stale poster reply drops (mirrors the detail dialog).
    quint64 nextBuyPosterToken() {
        static quint64 s = 0;
        return ++s;
    }
}

NFTBuyDialog::NFTBuyDialog(ContentEngine* engine, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_engine(engine), m_rpc(rpc) {
    setWindowTitle(tr("Buy a collectible"));
    setMinimumWidth(480);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- paste/open ---------------------------------------------------------
    auto* pasteLbl = new QLabel(tr("Paste the offer code, or open an offer file:"), this);
    pasteLbl->setWordWrap(true);
    outer->addWidget(pasteLbl);

    m_offerInput = new QPlainTextEdit(this);
    m_offerInput->setObjectName("nftBuyOfferInput");
    m_offerInput->setPlaceholderText(tr("znftoffer:…"));
    m_offerInput->setMaximumHeight(80);
    outer->addWidget(m_offerInput);

    auto* pasteRow = new QHBoxLayout();
    m_openFileBtn = new QPushButton(tr("Open .znftoffer…"), this);
    m_openFileBtn->setObjectName("nftBuyOpenFileButton");
    m_verifyBtn = new QPushButton(tr("Verify"), this);
    m_verifyBtn->setObjectName("nftBuyVerifyButton");
    pasteRow->addWidget(m_openFileBtn);
    pasteRow->addStretch(1);
    pasteRow->addWidget(m_verifyBtn);
    outer->addLayout(pasteRow);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // --- verified card ------------------------------------------------------
    auto* cardRow = new QHBoxLayout();
    cardRow->setSpacing(14);

    m_stage = new QLabel(this);
    m_stage->setObjectName("nftBuyStage");
    m_stage->setMinimumSize(kStagePx, kStagePx);
    m_stage->setAlignment(Qt::AlignCenter);
    m_stage->setStyleSheet("background:#1d2027; border-radius:8px;");
    m_stage->setWordWrap(true);
    m_stage->setText(tr("Verify an offer to see the collectible."));
    cardRow->addWidget(m_stage);

    auto* cardInfo = new QVBoxLayout();
    cardInfo->setSpacing(6);

    m_nameLbl = new QLabel(this);
    m_nameLbl->setObjectName("nftBuyName");
    m_nameLbl->setStyleSheet("font-size:13pt; font-weight:700;");
    m_nameLbl->setWordWrap(true);

    m_priceLbl = new QLabel(this);
    m_priceLbl->setObjectName("nftBuyPrice");

    m_expiryLbl = new QLabel(this);
    m_expiryLbl->setObjectName("nftBuyExpiry");
    m_expiryLbl->setStyleSheet("color:#9aa0a6;");

    m_verdictLbl = new QLabel(this);
    m_verdictLbl->setObjectName("nftBuyVerdict");
    m_verdictLbl->setWordWrap(true);

    cardInfo->addWidget(m_nameLbl);
    cardInfo->addWidget(m_priceLbl);
    cardInfo->addWidget(m_expiryLbl);
    cardInfo->addWidget(m_verdictLbl);
    cardInfo->addStretch(1);
    cardRow->addLayout(cardInfo, 1);
    outer->addLayout(cardRow);

    // --- confirmation sheet -------------------------------------------------
    m_confirmLine = new QLabel(this);
    m_confirmLine->setObjectName("nftBuyConfirmLine");
    m_confirmLine->setWordWrap(true);
    outer->addWidget(m_confirmLine);

    m_publicNote = new QLabel(nftPublicTradeNote(), this);
    m_publicNote->setObjectName("nftBuyPublicNote");
    m_publicNote->setWordWrap(true);
    m_publicNote->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(m_publicNote);

    m_ackBox = new QCheckBox(
        tr("I understand any extra I pay goes to the network, not the seller."), this);
    m_ackBox->setObjectName("nftBuyAcknowledge");
    outer->addWidget(m_ackBox);

    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("nftBuyResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    // --- action bar ---------------------------------------------------------
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setObjectName("nftBuyCloseButton");
    m_buyBtn = new QPushButton(tr("Buy"), this);
    m_buyBtn->setObjectName("nftBuyButton");
    m_buyBtn->setDefault(true);
    btnRow->addWidget(m_closeBtn);
    btnRow->addWidget(m_buyBtn);
    outer->addLayout(btnRow);

    connect(m_closeBtn,    &QPushButton::clicked, this, &QDialog::reject);
    connect(m_buyBtn,      &QPushButton::clicked, this, &NFTBuyDialog::onBuyClicked);
    connect(m_verifyBtn,   &QPushButton::clicked, this, &NFTBuyDialog::onVerifyClicked);
    connect(m_openFileBtn, &QPushButton::clicked, this, &NFTBuyDialog::onOpenFileClicked);
    connect(m_offerInput,  &QPlainTextEdit::textChanged, this, &NFTBuyDialog::onOfferTextChanged);
    connect(m_ackBox,      &QCheckBox::toggled, this, &NFTBuyDialog::onAcknowledgeToggled);

    if (m_engine)
        connect(m_engine, &ContentEngine::posterReady, this, &NFTBuyDialog::onPosterReady);

    resetVerifyGate();   // seed disabled state
}

void NFTBuyDialog::onOfferTextChanged() {
    if (m_succeeded || isInFlight())
        return;
    // ANY edit invalidates a prior green verdict so a tampered blob can never be bought
    // on a stale verify (the load-bearing anti-stale rule).
    resetVerifyGate();
    m_offerBlob = m_offerInput->toPlainText().trimmed();
    m_verifyBtn->setEnabled(!m_offerBlob.isEmpty());
}

void NFTBuyDialog::resetVerifyGate() {
    m_verified = NFTVerifyResult();
    refreshBuyEnabled();
}

void NFTBuyDialog::onOpenFileClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open an offer"), QString(), tr("Offer file (*.znftoffer);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_resultLine->setText(tr("Couldn't read that offer file."));
        m_resultLine->setStyleSheet("color:#c0392b;");
        return;
    }
    QTextStream in(&f);
    const QString blob = in.readAll().trimmed();
    f.close();
    m_offerInput->setPlainText(blob);   // textChanged resets the gate; auto-verify below
    runVerify();
}

void NFTBuyDialog::onVerifyClicked() {
    runVerify();
}

#ifdef ZCL_WIDGET_TEST
void NFTBuyDialog::testPasteOffer(const QString& blob) {
    m_offerInput->setPlainText(blob);   // textChanged resets the gate
    runVerify();
}
#endif

void NFTBuyDialog::runVerify() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    m_offerBlob = m_offerInput->toPlainText().trimmed();
    if (m_offerBlob.isEmpty())
        return;
    if (m_verifyInFlight)
        return;

    m_verifyInFlight = true;
    m_verifyBtn->setEnabled(false);
    m_verifyBtn->setText(tr("Checking…"));
    resetVerifyGate();
    m_verdictLbl->setText(tr("Checking this offer…"));
    m_verdictLbl->setStyleSheet("color:#9aa0a6;");   // neutral while in progress; amber is the refusal

    const QString blobAtRequest = m_offerBlob;
    QPointer<NFTBuyDialog> self(this);
    m_rpc->nftVerifyOffer(m_offerBlob,
        [self, blobAtRequest](NFTVerifyResult vr) {
            if (self.isNull())
                return;
            // Drop a stale reply if the offer changed while verify was in flight.
            if (self->m_offerInput->toPlainText().trimmed() != blobAtRequest)
                return;
            self->m_verifyInFlight = false;
            self->m_verifyBtn->setEnabled(true);
            self->m_verifyBtn->setText(tr("Verify"));
            self->m_verified = vr;
            self->renderVerified();
            self->refreshBuyEnabled();
        },
        [self, blobAtRequest](QString errStr) {
            if (self.isNull())
                return;
            if (self->m_offerInput->toPlainText().trimmed() != blobAtRequest)
                return;
            self->m_verifyInFlight = false;
            self->m_verifyBtn->setEnabled(true);
            self->m_verifyBtn->setText(tr("Verify"));
            self->m_verified = NFTVerifyResult();
            self->m_verdictLbl->setText(errStr);
            self->m_verdictLbl->setStyleSheet("color:#c0392b;");
            self->refreshBuyEnabled();
        }
    );
}

void NFTBuyDialog::renderVerified() {
    const NFTVerifyResult& vr = m_verified;

    // Name: the token id short (the offer carries no human name; provenance backfills it).
    QString shortTok = vr.tokenId;
    if (shortTok.size() > 18)
        shortTok = shortTok.left(8) + QStringLiteral("…") + shortTok.right(8);
    m_nameLbl->setText(shortTok.isEmpty() ? tr("Collectible") : shortTok);

    m_priceLbl->setText(tr("Price: %1 ZCL").arg(Settings::zatToDecimalString(vr.priceZat)));
    m_expiryLbl->setText(vr.expiryHeight > 0
                             ? tr("Expires at block %1").arg(vr.expiryHeight)
                             : QString());

    if (vr.ok) {
        // GREEN — only ever rendered on a real ok==true verdict.
        m_verdictLbl->setText(tr("✓ Verified — you will own this if you pay."));
        m_verdictLbl->setStyleSheet("color:#34c759; font-weight:600;");
        m_confirmLine->setText(tr("You pay %1 ZCL (+ a small network fee). You receive "
                                  "this collectible.").arg(Settings::zatToDecimalString(vr.priceZat)));
    } else {
        // AMBER — the honest reason(s); Buy stays disabled.
        QString why = vr.reasons.isEmpty()
                          ? tr("This offer could not be verified.")
                          : vr.reasons.join(tr("; "));
        m_verdictLbl->setText(tr("⚠ Don't pay — %1").arg(why));
        m_verdictLbl->setStyleSheet("color:#d9822b; font-weight:600;");
        m_confirmLine->clear();
    }

    requestPoster();
}

void NFTBuyDialog::requestPoster() {
    // The offer carries the tokenId; the image anchor is the token's documenthash, which
    // we fetch via provenance, then render LOCAL bytes only (never auto-fetch). An absent
    // image is the honest "not on this computer" placeholder.
    m_stage->setPixmap(QPixmap());
    m_stage->setText(tr("Checking for this image on your computer…"));
    if (m_engine == nullptr || m_rpc == nullptr || m_verified.tokenId.isEmpty()) {
        m_stage->setText(tr("This collectible's image isn't on this computer."));
        return;
    }

    const QString tokenId = m_verified.tokenId;
    QPointer<NFTBuyDialog> self(this);
    m_rpc->nftProvenance(tokenId,
        [self, tokenId](json meta) {
            if (self.isNull())
                return;
            if (self->m_verified.tokenId != tokenId)
                return;   // raced to a newer verify
            QString docHash;
            if (meta.is_object()) {
                auto it = meta.find("documenthash");
                if (it != meta.end() && it->is_string())
                    docHash = QString::fromStdString(it->get<std::string>());
            }
            self->m_provDocHash = docHash;
            if (docHash.isEmpty()) {
                self->m_stage->setText(tr("This collectible's image isn't on this computer."));
                return;
            }
            const QString localPath = ContentEngine::cacheGet(docHash);
            if (localPath.isEmpty()) {
                self->m_stage->setText(tr("This collectible's image isn't on this computer."));
                return;
            }
            self->m_posterToken = nextBuyPosterToken();
            self->m_engine->posterForToken(localPath, docHash, docHash,
                                           kPosterPx, self->m_posterToken);
        },
        [self](QString /*errStr*/) {
            if (self.isNull())
                return;
            self->m_stage->setText(tr("This collectible's image isn't on this computer."));
        }
    );
}

void NFTBuyDialog::onPosterReady(quint64 token, QImage img, int /*verifyState*/) {
    if (token != m_posterToken)
        return;
    if (img.isNull()) {
        m_stage->setText(tr("This collectible's image isn't on this computer."));
        return;
    }
    m_stage->setText(QString());
    const QSize area = m_stage->size().isEmpty() ? QSize(kStagePx, kStagePx) : m_stage->size();
    m_stage->setPixmap(QPixmap::fromImage(img).scaled(area, Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation));
}

void NFTBuyDialog::onAcknowledgeToggled(bool checked) {
    m_acknowledged = checked;
    refreshBuyEnabled();
}

void NFTBuyDialog::refreshBuyEnabled() {
    if (m_succeeded) {
        return;   // post-success: Buy is now "Done" — never re-enable the buy path
    }
    // HARD gate: a real green verify + the overshoot acknowledgement + not in flight.
    m_buyBtn->setEnabled(m_verified.ok && m_acknowledged && !isInFlight());
}

void NFTBuyDialog::onBuyClicked() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    if (!m_verified.ok || !m_acknowledged)
        return;

    // Shared scaffold: latch in-flight, relabel Buy -> "Buying…", disable it + Close.
    beginPrimary(m_buyBtn, tr("Buying…"), m_closeBtn);
    m_resultLine->clear();

    QPointer<NFTBuyDialog> self(this);
    m_rpc->nftTakeOffer(m_offerBlob, /*acknowledge=*/true,
        [self](QString txid, qint64 overshootZat) {
            if (self.isNull())
                return;
            self->m_succeeded    = true;
            self->m_lastTxid     = txid;
            self->m_lastOvershoot = overshootZat;
            QString msg = tr("Collectible bought — it's on its way, confirming on-chain.");
            if (overshootZat > 0)
                msg += tr(" Network fee: %1 ZCL.").arg(Settings::zatToDecimalString(overshootZat));
            self->m_resultLine->setText(msg);
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->finishPrimaryAsDone(self->m_buyBtn, self->m_closeBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            self->finishPrimaryAsRetry(self->m_buyBtn, self->m_closeBtn);
            self->m_resultLine->setText(errStr);
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshBuyEnabled();
        }
    );
}
