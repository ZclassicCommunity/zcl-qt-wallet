// ============================================================================
// CollectionCreateDialog implementation — see collectioncreatedialog.h.
// COLLECTIONS Phase-1. Mirrors NftMintDialog's structure + the shared
// NftAsyncDialog in-flight / Done / Try-again scaffold.
// ============================================================================
#include "collectioncreatedialog.h"
#include "rpc.h"
#include "guiutil.h"     // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QFrame>
#include <QPointer>

CollectionCreateDialog::CollectionCreateDialog(RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Create a collection"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    // ---- what a collection is (honest, up-front) ----
    auto* intro = new QLabel(
        tr("A collection groups cards (collectibles) into a set. You stay in control: "
           "you keep the authority to add cards, and each card you add uses up one slot."),
        this);
    intro->setObjectName("collectionCreateIntro");
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(intro);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // ---- name ----
    outer->addWidget(new QLabel(tr("Name"), this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("collectionCreateNameEdit");
    m_nameEdit->setMaxLength(50);
    m_nameEdit->setPlaceholderText(tr("Required — e.g. Series 1"));
    outer->addWidget(m_nameEdit);

    // ---- ticker (optional short symbol) ----
    outer->addWidget(new QLabel(tr("Short symbol (optional)"), this));
    m_tickerEdit = new QLineEdit(this);
    m_tickerEdit->setObjectName("collectionCreateTickerEdit");
    m_tickerEdit->setMaxLength(20);
    m_tickerEdit->setPlaceholderText(tr("e.g. S1"));
    outer->addWidget(m_tickerEdit);

    // ---- card count (= quantity of authority units) ----
    outer->addWidget(new QLabel(tr("Number of cards you can issue"), this));
    m_countSpin = new QSpinBox(this);
    m_countSpin->setObjectName("collectionCreateCountSpin");
    m_countSpin->setRange(1, 1000000);   // daemon clamps the real limit; 1M is a sane UI cap
    m_countSpin->setValue(10);
    outer->addWidget(m_countSpin);

    auto* countHint = new QLabel(
        tr("Each card you add later uses one of these. You can issue more later because "
           "you keep this collection's authority."),
        this);
    countHint->setObjectName("collectionCreateCountHint");
    countHint->setProperty("hint", true);
    countHint->setWordWrap(true);
    countHint->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(countHint);

    // ---- honesty block: public + permanent ----
    auto* permLine = new QLabel(
        tr("Public — the collection's name and symbol go on the public ledger "
           "permanently and can't be removed."), this);
    permLine->setObjectName("collectionCreatePermanenceLabel");
    permLine->setWordWrap(true);
    permLine->setStyleSheet("color:#d9822b; font-size:12pt;");
    outer->addWidget(permLine);

    // ---- result + action bar ----
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("collectionCreateResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setObjectName("collectionCreateCancelButton");
    m_createBtn = new QPushButton(tr("Create collection"), this);
    m_createBtn->setObjectName("collectionCreateButton");
    m_createBtn->setDefault(true);
    m_createBtn->setWhatsThis(
        tr("Creating a collection writes its name and symbol to the public ledger "
           "permanently. This can't be undone, edited, or hidden later. You keep the "
           "authority to add cards; each card you add uses one slot, and you can issue "
           "more later."));
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_createBtn);
    outer->addLayout(btnRow);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createBtn, &QPushButton::clicked, this, &CollectionCreateDialog::onCreate);
    connect(m_nameEdit,  &QLineEdit::textChanged, this, [this]{ refreshCreateEnabled(); });

    refreshCreateEnabled();

    // Copy-pasteable text (esp. the result/error line + the new collection id) and no
    // clipped button labels (deferred so sizeHint() is post-style).
    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });
}

void CollectionCreateDialog::refreshCreateEnabled() {
    if (m_succeeded)
        return;   // post-success: Create retired to Done — never re-enable
    const bool nameOk = !m_nameEdit->text().trimmed().isEmpty();
    m_createBtn->setEnabled(nameOk && !isInFlight());
}

void CollectionCreateDialog::onCreate() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    const QString name   = m_nameEdit->text().trimmed();
    const QString ticker = m_tickerEdit->text().trimmed();
    const qint64  count  = (qint64) m_countSpin->value();
    if (name.isEmpty())
        return;

    // Shared scaffold: latch in-flight, relabel Create -> "Creating…", disable it +
    // Cancel (can't bail mid-flight — also closes the UAF window).
    beginPrimary(m_createBtn, tr("Creating…"), m_cancelBtn);
    m_resultLine->clear();

    // QPointer guard: the modal can still be torn down under a pending reply.
    QPointer<CollectionCreateDialog> self(this);
    m_rpc->createCollection(name, ticker, count,
        [self](QString txid, QString groupId) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->m_lastTxid    = txid;
            self->m_lastGroupId = groupId;
            self->m_succeeded   = true;
            // VISIBLE SUCCESS: show the honest 0-conf line, then retire Create -> "Done".
            self->m_resultLine->setText(
                tr("Collection created — it'll appear once confirmed. Then you can add cards to it."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->finishPrimaryAsDone(self->m_createBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_createBtn, self->m_cancelBtn);
            self->m_resultLine->setText(errStr);   // honest daemon message, never fabricated
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshCreateEnabled();
        }
    );
}
