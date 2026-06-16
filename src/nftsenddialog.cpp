// ============================================================================
// NFTSendDialog implementation — see nftsenddialog.h. NATIVE_NFT_GUIDE.md §2.6.
// ============================================================================
#include "nftsenddialog.h"
#include "nftcommon.h"
#include "rpc.h"
#include "settings.h"
#include "guiutil.h"
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFrame>
#include <QPointer>

NFTSendDialog::NFTSendDialog(const NFTItem& item, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_item(item), m_rpc(rpc) {
    setWindowTitle(tr("Send a gift"));
    setMinimumWidth(440);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- Card 1: what you're giving (read-only) -----------------------------
    auto* whatTitle = new QLabel(tr("You're giving"), this);
    whatTitle->setStyleSheet("font-weight:600;");
    outer->addWidget(whatTitle);

    QString line2 = m_item.collection.isEmpty()
                        ? tr("Not part of a set")
                        : m_item.collection;
    auto* nameLbl = new QLabel(
        QString("<b>%1</b><br/><span style='color:#9aa0a6'>%2</span>")
            .arg(m_item.name.toHtmlEscaped(), line2.toHtmlEscaped()), this);
    nameLbl->setTextFormat(Qt::RichText);
    outer->addWidget(nameLbl);

    // MISMATCH guard surfaced honestly (verifyState 2 == red mismatch).
    if (m_item.verifyState == 2) {
        auto* warn = new QLabel(
            tr("This picture doesn't match its on-chain fingerprint — we won't send it."), this);
        warn->setObjectName("nftSendMismatchWarning");
        warn->setWordWrap(true);
        warn->setStyleSheet("color:#c0392b;");
        outer->addWidget(warn);
    }

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // --- Card 2: who gets it ------------------------------------------------
    auto* toTitle = new QLabel(tr("Send to"), this);
    toTitle->setStyleSheet("font-weight:600;");
    outer->addWidget(toTitle);

    m_recipient = new QLineEdit(this);
    m_recipient->setObjectName("nftSendRecipientEdit");
    m_recipient->setPlaceholderText(tr("Recipient's public (transparent) address"));
    outer->addWidget(m_recipient);

    m_addrStatus = new QLabel(this);
    m_addrStatus->setObjectName("nftSendAddrStatus");
    m_addrStatus->setWordWrap(true);
    m_addrStatus->setMinimumHeight(18);   // reserved height so layout never jumps
    outer->addWidget(m_addrStatus);

    // --- Card 3: how private (Public wired; Private coming soon) -------------
    auto* privNote = new QLabel(
        tr("This is a public gift. Public gifts ride the transparent ledger and "
           "are linkable."), this);
    privNote->setWordWrap(true);
    privNote->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(privNote);

    if (!RPC::isPrivateMintWired()) {
        auto* soon = new QLabel(tr("Private gift — coming in this release."), this);
        soon->setStyleSheet("color:#9aa0a6;");
        soon->setEnabled(false);
        outer->addWidget(soon);
    }

    // --- result line + action bar -------------------------------------------
    m_resultLine = new QLabel(this);
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_sendBtn = new QPushButton(tr("Send gift"), this);
    m_sendBtn->setObjectName("nftSendButton");
    m_sendBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_sendBtn);
    outer->addLayout(btnRow);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_sendBtn,   &QPushButton::clicked, this, &NFTSendDialog::onSendClicked);
    connect(m_recipient, &QLineEdit::textChanged, this, &NFTSendDialog::onRecipientChanged);

    makeLabelsSelectable(this);                                     // copyable text (incl. errors)
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });   // no clipped button labels

    onRecipientChanged(QString());   // seed the disabled/status state
}

void NFTSendDialog::onRecipientChanged(const QString& text) {
    // Shared 4-state t-address validator (mirrors the sell dialog).
    nftValidateTAddrInto(
        m_addrStatus, text,
        tr("A gift needs a public (transparent) address. Private gifts are coming soon."));
    refreshSendEnabled();
}

void NFTSendDialog::refreshSendEnabled() {
    if (m_succeeded)
        return;   // post-success: Send is gone (replaced by Done) — never re-enable
    const QString addr = m_recipient->text().trimmed();
    const bool recipientOk = Settings::isTAddress(addr);   // public address only
    const bool notMismatch = (m_item.verifyState != 2);
    m_sendBtn->setEnabled(recipientOk && notMismatch && !isInFlight());
}

void NFTSendDialog::onSendClicked() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    const QString addr = m_recipient->text().trimmed();
    if (!Settings::isTAddress(addr))
        return;

    // Shared scaffold: latch in-flight, relabel Send -> "Sending…", disable it +
    // Cancel (can't bail mid-flight — also closes the UAF window).
    beginPrimary(m_sendBtn, tr("Sending…"), m_cancelBtn);
    m_resultLine->clear();

    // LIFETIME (review fix #5): guard the reply with a QPointer and bail before touching
    // any member if the dialog was destroyed while the send RPC was in flight.
    QPointer<NFTSendDialog> self(this);
    m_rpc->sendNFT(m_item.txid, addr,
        [self](QString /*txid*/) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            // VISIBLE SUCCESS (review fix #3): do NOT accept() here — the confirmation
            // would vanish before the user could read it. Switch to a terminal "done"
            // state and let the user explicitly dismiss the acknowledged result.
            // 0-CONF honesty: the token is on its way; the gallery will drop it once it
            // confirms — never imply the user lost it.
            self->m_succeeded = true;
            self->m_resultLine->setText(
                tr("Gift sent. It's on its way — confirming on-chain."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->m_recipient->setEnabled(false);
            self->finishPrimaryAsDone(self->m_sendBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_sendBtn, self->m_cancelBtn);
            self->m_resultLine->setText(errStr);
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshSendEnabled();
        }
    );
}
