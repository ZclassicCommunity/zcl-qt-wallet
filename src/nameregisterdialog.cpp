// ============================================================================
// NameRegisterDialog implementation — see nameregisterdialog.h. NAMES pillar.
// Mirrors CollectionCreateDialog/NFTSendDialog + the shared NftAsyncDialog
// in-flight / Done / Try-again scaffold.
// ============================================================================
#include "nameregisterdialog.h"
#include "namescommon.h"
#include "rpc.h"
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

NameRegisterDialog::NameRegisterDialog(RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Register a name"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    // ---- what a name is (honest, up-front) ----
    auto* intro = new QLabel(
        tr("Register a short, human name that points to an address or other target. "
           "Names are first-come, first-served and are recorded on the public ZCL "
           "ledger."),
        this);
    intro->setObjectName("nameRegisterIntro");
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(intro);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // ---- name (live-validated) ----
    outer->addWidget(new QLabel(tr("Name"), this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("nameRegisterNameEdit");
    m_nameEdit->setMaxLength(63);
    m_nameEdit->setPlaceholderText(tr("lowercase letters, digits, and hyphens"));
    outer->addWidget(m_nameEdit);

    m_nameStatus = new QLabel(this);
    m_nameStatus->setObjectName("nameRegisterNameStatus");
    m_nameStatus->setWordWrap(true);
    m_nameStatus->setMinimumHeight(18);   // reserved height so layout never jumps
    outer->addWidget(m_nameStatus);

    // ---- target type (combo seeded from znamTargetChoices) ----
    outer->addWidget(new QLabel(tr("What does this name point to?"), this));
    m_typeCombo = new QComboBox(this);
    m_typeCombo->setObjectName("nameRegisterTypeCombo");
    for (const ZNAMTargetChoice& c : znamTargetChoices())
        m_typeCombo->addItem(c.label, c.code);   // store the int code in itemData
    outer->addWidget(m_typeCombo);

    // ---- value (target payload) ----
    outer->addWidget(new QLabel(tr("Target"), this));
    m_valueEdit = new QLineEdit(this);
    m_valueEdit->setObjectName("nameRegisterValueEdit");
    m_valueEdit->setPlaceholderText(tr("the address, onion, or hash this name points to"));
    outer->addWidget(m_valueEdit);

    // ---- owner (optional; default = let the node pick an own address) ----
    outer->addWidget(new QLabel(tr("Owner address (optional)"), this));
    m_ownerCombo = new QComboBox(this);
    m_ownerCombo->setObjectName("nameRegisterOwnerCombo");
    // Empty sentinel selection => the daemon picks a fresh own address.
    m_ownerCombo->addItem(tr("Let my wallet choose"), QString());
    if (m_rpc != nullptr) {
        const QList<QString>* taddrs = m_rpc->getAllTAddresses();
        if (taddrs != nullptr)
            for (const QString& a : *taddrs)
                m_ownerCombo->addItem(a, a);
    }
    outer->addWidget(m_ownerCombo);

    // ---- honesty: public + permanent ----
    auto* permLine = new QLabel(
        tr("Public — the name and what it points to go on the public ledger and "
           "can be looked up by anyone."), this);
    permLine->setObjectName("nameRegisterPermanenceLabel");
    permLine->setWordWrap(true);
    permLine->setStyleSheet("color:#d9822b; font-size:12pt;");
    outer->addWidget(permLine);

    // ---- result + action bar ----
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("nameRegisterResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setObjectName("nameRegisterCancelButton");
    m_registerBtn = new QPushButton(tr("Register name"), this);
    m_registerBtn->setObjectName("nameRegisterButton");
    m_registerBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_registerBtn);
    outer->addLayout(btnRow);

    connect(m_cancelBtn,   &QPushButton::clicked, this, &QDialog::reject);
    connect(m_registerBtn, &QPushButton::clicked, this, &NameRegisterDialog::onRegisterClicked);
    connect(m_nameEdit,    &QLineEdit::textChanged, this, &NameRegisterDialog::onNameChanged);

    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });

    onNameChanged(QString());   // seed the disabled/status state
}

void NameRegisterDialog::onNameChanged(const QString& text) {
    const QString name = text.trimmed();
    if (name.isEmpty()) {
        m_nameStatus->clear();
    } else {
        QString reason;
        if (znamIsValidName(name, &reason)) {
            m_nameStatus->setText(tr("Looks good."));
            m_nameStatus->setStyleSheet("color:#34c759;");   // success green
        } else {
            m_nameStatus->setText(reason);
            m_nameStatus->setStyleSheet("color:#c0392b;");    // red
        }
    }
    refreshRegisterEnabled();
}

void NameRegisterDialog::refreshRegisterEnabled() {
    if (m_succeeded)
        return;   // post-success: Register retired to Done — never re-enable
    const bool nameOk = znamIsValidName(m_nameEdit->text().trimmed());
    m_registerBtn->setEnabled(nameOk && !isInFlight());
}

void NameRegisterDialog::onRegisterClicked() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    const QString name  = m_nameEdit->text().trimmed();
    if (!znamIsValidName(name))
        return;
    const QString value = m_valueEdit->text().trimmed();
    const int     type  = m_typeCombo->currentData().toInt();
    const QString owner = m_ownerCombo->currentData().toString();   // "" => node picks

    // Shared scaffold: latch in-flight, relabel Register -> "Registering…", disable it
    // + Cancel (can't bail mid-flight — also closes the UAF window).
    beginPrimary(m_registerBtn, tr("Registering…"), m_cancelBtn);
    m_resultLine->clear();

    // QPointer guard: the modal can still be torn down under a pending reply.
    QPointer<NameRegisterDialog> self(this);
    m_rpc->nameRegister(name, type, value, owner,
        [self](QString txid, QString /*owner*/) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->m_lastTxid  = txid;
            self->m_succeeded = true;
            // 0-CONF honesty: the name is registered on its way; the My-Names list will
            // show it once it confirms — never imply it's instantly live.
            self->m_resultLine->setText(
                tr("Name registered — it'll appear once confirmed on-chain."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->m_nameEdit->setEnabled(false);
            self->m_valueEdit->setEnabled(false);
            self->m_typeCombo->setEnabled(false);
            self->m_ownerCombo->setEnabled(false);
            self->finishPrimaryAsDone(self->m_registerBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_registerBtn, self->m_cancelBtn);
            self->m_resultLine->setText(errStr);   // honest daemon message, never fabricated
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshRegisterEnabled();
        }
    );
}
