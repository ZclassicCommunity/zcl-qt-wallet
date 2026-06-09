// ============================================================================
// NameTransferDialog implementation — see nametransferdialog.h. NAMES pillar.
// Mirrors NFTSendDialog's t-address-only validation + the shared NftAsyncDialog
// in-flight / Done / Try-again scaffold.
// ============================================================================
#include "nametransferdialog.h"
#include "rpc.h"
#include "settings.h"
#include "guiutil.h"     // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QFrame>
#include <QPointer>

NameTransferDialog::NameTransferDialog(RPC* rpc, const QStringList& myNames, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Transfer a name"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    // ---- which name ----
    outer->addWidget(new QLabel(tr("Name to transfer"), this));
    m_nameCombo = new QComboBox(this);
    m_nameCombo->setObjectName("nameTransferNameCombo");
    for (const QString& n : myNames)
        m_nameCombo->addItem(n, n);
    outer->addWidget(m_nameCombo);

    if (myNames.isEmpty()) {
        auto* none = new QLabel(
            tr("You don't own any names yet. Register one first."), this);
        none->setObjectName("nameTransferNoNamesLabel");
        none->setWordWrap(true);
        none->setStyleSheet("color:#9aa0a6;");
        outer->addWidget(none);
    }

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // ---- new owner (transparent t-address only) ----
    outer->addWidget(new QLabel(tr("New owner"), this));
    m_ownerEdit = new QLineEdit(this);
    m_ownerEdit->setObjectName("nameTransferNewOwnerEdit");
    m_ownerEdit->setPlaceholderText(tr("the new owner's public (transparent) address"));
    outer->addWidget(m_ownerEdit);

    m_ownerStatus = new QLabel(this);
    m_ownerStatus->setObjectName("nameTransferOwnerStatus");
    m_ownerStatus->setWordWrap(true);
    m_ownerStatus->setMinimumHeight(18);   // reserved height so layout never jumps
    outer->addWidget(m_ownerStatus);

    // ---- honesty: public + irreversible ----
    auto* note = new QLabel(
        tr("A name's owner is always a public (transparent) address. Once transferred, "
           "only the new owner can manage this name."), this);
    note->setObjectName("nameTransferNote");
    note->setWordWrap(true);
    note->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(note);

    // ---- result + action bar ----
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("nameTransferResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setObjectName("nameTransferCancelButton");
    m_transferBtn = new QPushButton(tr("Transfer name"), this);
    m_transferBtn->setObjectName("nameTransferButton");
    m_transferBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_transferBtn);
    outer->addLayout(btnRow);

    connect(m_cancelBtn,   &QPushButton::clicked, this, &QDialog::reject);
    connect(m_transferBtn, &QPushButton::clicked, this, &NameTransferDialog::onTransferClicked);
    connect(m_ownerEdit,   &QLineEdit::textChanged, this, &NameTransferDialog::onNewOwnerChanged);

    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });

    onNewOwnerChanged(QString());   // seed the disabled/status state
}

void NameTransferDialog::onNewOwnerChanged(const QString& text) {
    const QString addr = text.trimmed();
    if (addr.isEmpty()) {
        m_ownerStatus->clear();
    } else if (Settings::isTAddress(addr)) {
        m_ownerStatus->setText(tr("Public (transparent) address — OK."));
        m_ownerStatus->setStyleSheet("color:#34c759;");
    } else if (Settings::isZAddress(addr)) {
        // A valid z-addr is the load-bearing rejection: a name's owner must be public.
        m_ownerStatus->setText(
            tr("A name's owner must be a public (transparent) address, not a shielded one."));
        m_ownerStatus->setStyleSheet("color:#c0392b;");
    } else {
        m_ownerStatus->setText(tr("That doesn't look like a ZClassic address."));
        m_ownerStatus->setStyleSheet("color:#c0392b;");
    }
    refreshTransferEnabled();
}

void NameTransferDialog::refreshTransferEnabled() {
    if (m_succeeded)
        return;   // post-success: Transfer retired to Done — never re-enable
    const bool haveName = (m_nameCombo->count() > 0);
    const bool ownerOk  = Settings::isTAddress(m_ownerEdit->text().trimmed());
    m_transferBtn->setEnabled(haveName && ownerOk && !isInFlight());
}

void NameTransferDialog::onTransferClicked() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    if (m_nameCombo->count() == 0)
        return;
    const QString name  = m_nameCombo->currentData().toString();
    const QString owner = m_ownerEdit->text().trimmed();
    if (name.isEmpty() || !Settings::isTAddress(owner))
        return;

    beginPrimary(m_transferBtn, tr("Transferring…"), m_cancelBtn);
    m_resultLine->clear();

    QPointer<NameTransferDialog> self(this);
    m_rpc->nameTransfer(name, owner,
        [self](QString txid) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->m_lastTxid  = txid;
            self->m_succeeded = true;
            // 0-CONF honesty: the name is on its way to the new owner; the My-Names list
            // drops it once it confirms — never imply it's already gone or lost.
            self->m_resultLine->setText(
                tr("Name transferred — confirming on-chain."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->m_ownerEdit->setEnabled(false);
            self->m_nameCombo->setEnabled(false);
            self->finishPrimaryAsDone(self->m_transferBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_transferBtn, self->m_cancelBtn);
            self->m_resultLine->setText(errStr);   // honest daemon message
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshTransferEnabled();
        }
    );
}
