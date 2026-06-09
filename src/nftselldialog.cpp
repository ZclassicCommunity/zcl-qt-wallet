// ============================================================================
// NFTSellDialog implementation — see nftselldialog.h. NFT_SELL_DESIGN.md §5.
// ============================================================================
#include "nftselldialog.h"
#include "nftcommon.h"
#include "rpc.h"
#include "settings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFrame>
#include <QFileDialog>
#include <QClipboard>
#include <QApplication>
#include <QDoubleValidator>
#include <QMessageBox>
#include <QPointer>
#include <QFile>
#include <QTextStream>

NFTSellDialog::NFTSellDialog(const NFTItem& item, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_item(item), m_rpc(rpc) {
    setWindowTitle(tr("Sell this collectible"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- header: what you're selling (read-only) ----------------------------
    auto* title = new QLabel(this);
    title->setObjectName("nftSellTitle");
    QString line2 = m_item.collection.isEmpty() ? tr("Not part of a set") : m_item.collection;
    title->setText(QString("<b>%1</b><br/><span style='color:#9aa0a6'>%2</span>")
                       .arg(m_item.name.toHtmlEscaped(), line2.toHtmlEscaped()));
    title->setTextFormat(Qt::RichText);
    outer->addWidget(title);

    // MISMATCH guard: never list a tampered collectible (mirrors the send dialog).
    if (m_item.verifyState == 2) {
        m_mismatchWarn = new QLabel(
            tr("This picture doesn't match its on-chain fingerprint — we won't list it."), this);
        m_mismatchWarn->setObjectName("nftSellMismatchWarning");
        m_mismatchWarn->setWordWrap(true);
        m_mismatchWarn->setStyleSheet("color:#c0392b;");
        outer->addWidget(m_mismatchWarn);
    }

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // --- price --------------------------------------------------------------
    auto* priceLbl = new QLabel(tr("Price (ZCL)"), this);
    priceLbl->setStyleSheet("font-weight:600;");
    outer->addWidget(priceLbl);

    m_priceEdit = new QLineEdit(this);
    m_priceEdit->setObjectName("nftSellPriceEdit");
    m_priceEdit->setPlaceholderText(tr("e.g. 100"));
    auto* pv = new QDoubleValidator(0.0, 21000000.0, 8, m_priceEdit);
    pv->setNotation(QDoubleValidator::StandardNotation);
    m_priceEdit->setValidator(pv);
    outer->addWidget(m_priceEdit);

    m_priceStatus = new QLabel(this);
    m_priceStatus->setObjectName("nftSellPriceStatus");
    m_priceStatus->setWordWrap(true);
    m_priceStatus->setMinimumHeight(18);
    outer->addWidget(m_priceStatus);

    // --- expiry -------------------------------------------------------------
    // v1: the daemon's ~7-day default is the ONLY expiry (we pass expiryHeight=0).
    // A 1-choice picker implies choices that don't exist, so show a read-only line.
    // Reintroduce a combo only when real custom-expiry (needs the chain tip) lands.
    auto* expiryLbl = new QLabel(tr("Expires in"), this);
    expiryLbl->setStyleSheet("font-weight:600;");
    outer->addWidget(expiryLbl);

    m_expiryValue = new QLabel(tr("About 7 days."), this);
    m_expiryValue->setObjectName("nftSellExpiryValue");
    outer->addWidget(m_expiryValue);

    // --- buyer's public address (REQUIRED by the daemon) --------------------
    auto* buyerLbl = new QLabel(tr("Buyer's public address"), this);
    buyerLbl->setStyleSheet("font-weight:600;");
    outer->addWidget(buyerLbl);

    m_buyerEdit = new QLineEdit(this);
    m_buyerEdit->setObjectName("nftSellBuyerAddrEdit");
    m_buyerEdit->setPlaceholderText(tr("Paste the buyer's public (transparent) address"));
    outer->addWidget(m_buyerEdit);

    m_buyerStatus = new QLabel(this);
    m_buyerStatus->setObjectName("nftSellBuyerStatus");
    m_buyerStatus->setWordWrap(true);
    m_buyerStatus->setMinimumHeight(18);
    outer->addWidget(m_buyerStatus);

    // --- honest public-settlement note --------------------------------------
    m_resultLine = new QLabel(this);   // also reused as the in-flight/result line
    m_resultLine->setObjectName("nftSellResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);

    auto* publicNote = new QLabel(nftPublicTradeNote(), this);
    publicNote->setObjectName("nftSellPublicNote");
    publicNote->setWordWrap(true);
    publicNote->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(publicNote);

    outer->addWidget(m_resultLine);

    // --- action bar ---------------------------------------------------------
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName("nftSellCloseButton");
    m_listBtn = new QPushButton(tr("List"), this);
    m_listBtn->setObjectName("nftSellListButton");
    m_listBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(m_listBtn);
    outer->addLayout(btnRow);

    connect(closeBtn,    &QPushButton::clicked, this, &QDialog::reject);
    connect(m_listBtn,   &QPushButton::clicked, this, &NFTSellDialog::onListClicked);
    connect(m_priceEdit, &QLineEdit::textChanged, this, &NFTSellDialog::onComposeChanged);
    connect(m_buyerEdit, &QLineEdit::textChanged, this, &NFTSellDialog::onComposeChanged);

    onComposeChanged();   // seed disabled/status state
}

void NFTSellDialog::onComposeChanged() {
    if (m_listed)
        return;
    // Price hint.
    const QString priceStr = m_priceEdit->text().trimmed();
    if (priceStr.isEmpty()) {
        m_priceStatus->clear();
    } else {
        bool ok = false;
        const double v = priceStr.toDouble(&ok);
        if (!ok || v <= 0.0) {
            m_priceStatus->setText(tr("Enter a price greater than zero."));
            m_priceStatus->setStyleSheet("color:#c0392b;");
        } else {
            m_priceStatus->clear();
        }
    }
    // Buyer-address hint — shared 4-state t-address validator (mirrors the send dialog).
    nftValidateTAddrInto(
        m_buyerStatus, m_buyerEdit->text(),
        tr("A sale needs the buyer's public (transparent) address."));
    refreshListEnabled();
}

void NFTSellDialog::refreshListEnabled() {
    if (m_listed || !m_listBtn)
        return;
    bool priceOk = false;
    const double price = m_priceEdit->text().trimmed().toDouble(&priceOk);
    priceOk = priceOk && price > 0.0;
    const bool buyerOk    = Settings::isTAddress(m_buyerEdit->text().trimmed());
    const bool notMismatch = (m_item.verifyState != 2);
    m_listBtn->setEnabled(priceOk && buyerOk && notMismatch && !isInFlight());
}

void NFTSellDialog::onListClicked() {
    if (isInFlight() || m_listed || m_rpc == nullptr)
        return;
    bool ok = false;
    const double price = m_priceEdit->text().trimmed().toDouble(&ok);
    if (!ok || price <= 0.0)
        return;
    const QString buyer = m_buyerEdit->text().trimmed();
    if (!Settings::isTAddress(buyer))
        return;

    // Shared scaffold: latch in-flight + relabel List -> "Listing…" (this flow has
    // no secondary button to disable; Close is a plain reject and the [X] is
    // swallowed by NftAsyncDialog while in flight).
    beginPrimary(m_listBtn, tr("Listing…"));
    m_resultLine->clear();

    // v1: payoutAddr "" (daemon picks a fresh own address); expiryHeight 0 (daemon
    // default ~7d) — the only expiry in v1. QPointer guards the reply.
    QPointer<NFTSellDialog> self(this);
    m_rpc->nftMakeOffer(m_item.txid, price, buyer, /*payoutAddr=*/QString(),
        /*expiryHeight=*/0,
        [self](QString offerBlob, QString offerId, QString /*nftOutpoint*/) {
            if (self.isNull())
                return;
            // Success transitions into the LISTED phase (blob + Copy/Save/Cancel),
            // NOT a terminal Done — just clear the in-flight latch.
            self->setInFlight(false);
            self->m_offerBlob = offerBlob;
            self->m_offerId   = offerId;
            self->enterListedState();
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            self->finishPrimaryAsRetry(self->m_listBtn);
            self->m_resultLine->setText(errStr);
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshListEnabled();
        }
    );
}

void NFTSellDialog::enterListedState() {
    m_listed = true;

    // Lock down the compose inputs (they describe the now-immutable offer).
    if (m_priceEdit)   m_priceEdit->setEnabled(false);
    if (m_buyerEdit)   m_buyerEdit->setEnabled(false);
    if (m_listBtn)     m_listBtn->hide();

    auto* outer = qobject_cast<QVBoxLayout*>(layout());

    m_listedBadge = new QLabel(tr("● Listed — expires in ~7 days."), this);
    m_listedBadge->setObjectName("nftSellListedBadge");
    m_listedBadge->setStyleSheet("color:#34c759; font-weight:600;");

    auto* blobLbl = new QLabel(tr("Share this offer with your buyer:"), this);
    blobLbl->setWordWrap(true);

    m_blobView = new QPlainTextEdit(this);
    m_blobView->setObjectName("nftSellOfferBlob");
    m_blobView->setReadOnly(true);
    m_blobView->setPlainText(m_offerBlob);
    m_blobView->setMaximumHeight(90);

    m_copyBtn = new QPushButton(tr("Copy"), this);
    m_copyBtn->setObjectName("nftSellCopyButton");
    m_saveBtn = new QPushButton(tr("Save…"), this);
    m_saveBtn->setObjectName("nftSellSaveButton");
    m_cancelListBtn = new QPushButton(tr("Cancel listing"), this);
    m_cancelListBtn->setObjectName("nftSellCancelButton");

    auto* shareRow = new QHBoxLayout();
    shareRow->addWidget(m_copyBtn);
    shareRow->addWidget(m_saveBtn);
    shareRow->addStretch(1);
    shareRow->addWidget(m_cancelListBtn);

    // Insert the listed widgets just above the action bar's result line.
    if (outer) {
        const int at = outer->indexOf(m_resultLine);
        const int insertAt = (at >= 0) ? at : outer->count();
        outer->insertWidget(insertAt,     m_listedBadge);
        outer->insertWidget(insertAt + 1, blobLbl);
        outer->insertWidget(insertAt + 2, m_blobView);
        outer->insertLayout(insertAt + 3, shareRow);
    }

    connect(m_copyBtn,       &QPushButton::clicked, this, &NFTSellDialog::onCopyClicked);
    connect(m_saveBtn,       &QPushButton::clicked, this, &NFTSellDialog::onSaveClicked);
    connect(m_cancelListBtn, &QPushButton::clicked, this, &NFTSellDialog::onCancelListingClicked);

    m_resultLine->setText(tr("Offer ready. Send the buyer this code — they verify it "
                             "before paying."));
    m_resultLine->setStyleSheet("color:#34c759;");
}

void NFTSellDialog::onCopyClicked() {
    QApplication::clipboard()->setText(m_offerBlob);
    if (m_resultLine) {
        m_resultLine->setText(tr("Copied. Paste it to your buyer."));
        m_resultLine->setStyleSheet("color:#34c759;");
    }
}

void NFTSellDialog::onSaveClicked() {
    const QString base = m_item.name.isEmpty() ? QStringLiteral("offer") : m_item.name;
    const QString suggested = base + QStringLiteral(".znftoffer");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save offer"), suggested, tr("Offer file (*.znftoffer)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (m_resultLine) {
            m_resultLine->setText(tr("Couldn't save the offer file."));
            m_resultLine->setStyleSheet("color:#c0392b;");
        }
        return;
    }
    QTextStream out(&f);
    out << m_offerBlob;
    f.close();
    if (m_resultLine) {
        m_resultLine->setText(tr("Saved. Send the file to your buyer."));
        m_resultLine->setStyleSheet("color:#34c759;");
    }
}

void NFTSellDialog::onCancelListingClicked() {
    if (isInFlight() || m_rpc == nullptr || m_offerId.isEmpty())
        return;
    const auto r = QMessageBox::question(
        this, tr("Cancel this listing?"),
        tr("This voids the listing and frees your collectible. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes)
        return;

    // Shared scaffold: latch in-flight + relabel the Cancel-listing button to
    // "Cancelling…" (it is this flow's primary action).
    beginPrimary(m_cancelListBtn, tr("Cancelling…"));
    QPointer<NFTSellDialog> self(this);
    m_rpc->nftCancelOffer(m_offerId,
        [self](QString /*txid*/) {
            if (self.isNull())
                return;
            if (self->m_listedBadge) {
                self->m_listedBadge->setText(tr("● Listing cancelled — your collectible is free."));
                self->m_listedBadge->setStyleSheet("color:#9aa0a6; font-weight:600;");
            }
            // Repurpose the Cancel button into a terminal "Done" dismiss (shared scaffold).
            self->finishPrimaryAsDone(self->m_cancelListBtn);
            if (self->m_copyBtn) self->m_copyBtn->setEnabled(false);
            if (self->m_saveBtn) self->m_saveBtn->setEnabled(false);
            if (self->m_resultLine) {
                self->m_resultLine->setText(tr("Listing cancelled."));
                self->m_resultLine->setStyleSheet("color:#34c759;");
            }
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            // Restore the Cancel-listing affordance (not the generic "Try again":
            // the listing still stands, so the original label is the honest one).
            self->setInFlight(false);
            if (self->m_cancelListBtn) {
                self->m_cancelListBtn->setEnabled(true);
                self->m_cancelListBtn->setText(tr("Cancel listing"));
            }
            if (self->m_resultLine) {
                self->m_resultLine->setText(errStr);
                self->m_resultLine->setStyleSheet("color:#c0392b;");
            }
        }
    );
}

void NFTSellDialog::onDoneClicked() {
    accept();
}
