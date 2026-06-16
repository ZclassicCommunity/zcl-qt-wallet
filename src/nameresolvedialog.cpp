// ============================================================================
// NameResolveDialog implementation — see nameresolvedialog.h. NAMES pillar.
// ============================================================================
#include "nameresolvedialog.h"
#include "namescommon.h"
#include "rpc.h"
#include "guiutil.h"     // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFrame>
#include <QPointer>

NameResolveDialog::NameResolveDialog(RPC* rpc, const QString& prefill, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Look up a name"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    // ---- name + lookup row ----
    outer->addWidget(new QLabel(tr("Name"), this));
    auto* row = new QHBoxLayout();
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("nameResolveNameEdit");
    m_nameEdit->setMaxLength(63);
    m_nameEdit->setPlaceholderText(tr("the name to look up"));
    row->addWidget(m_nameEdit, 1);
    m_lookupBtn = new QPushButton(tr("Look up"), this);
    m_lookupBtn->setObjectName("nameResolveLookupButton");
    m_lookupBtn->setDefault(true);
    row->addWidget(m_lookupBtn);
    outer->addLayout(row);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // ---- state line + results box ----
    m_stateLine = new QLabel(this);
    m_stateLine->setObjectName("nameResolveStateLine");
    m_stateLine->setWordWrap(true);
    m_stateLine->setMinimumHeight(18);
    outer->addWidget(m_stateLine);

    m_resultsBox = new QWidget(this);
    m_resultsBox->setObjectName("nameResolveResultsBox");
    m_resultsLayout = new QVBoxLayout(m_resultsBox);
    m_resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_resultsLayout->setSpacing(4);
    outer->addWidget(m_resultsBox);

    outer->addStretch(1);

    // ---- close bar ----
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setObjectName("nameResolveCloseButton");
    btnRow->addWidget(m_closeBtn);
    outer->addLayout(btnRow);

    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::accept);
    connect(m_lookupBtn, &QPushButton::clicked, this, &NameResolveDialog::onLookupClicked);
    connect(m_nameEdit,  &QLineEdit::returnPressed, this, &NameResolveDialog::onLookupClicked);

    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });

    if (!prefill.trimmed().isEmpty()) {
        m_nameEdit->setText(prefill.trimmed());
        onLookupClicked();
    }
}

void NameResolveDialog::clearResults() {
    if (m_resultsLayout == nullptr)
        return;
    QLayoutItem* item = nullptr;
    while ((item = m_resultsLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void NameResolveDialog::onLookupClicked() {
    if (m_rpc == nullptr)
        return;
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty())
        return;

    clearResults();
    m_stateLine->setText(tr("Looking up…"));
    m_stateLine->setStyleSheet("color:#9aa0a6;");

    QPointer<NameResolveDialog> self(this);
    m_rpc->nameResolve(name,
        [self, name](bool found, ResolvedName r) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            if (found)
                self->renderFound(r);
            else
                self->renderAvailable(name);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            self->m_stateLine->setText(errStr);   // honest daemon message
            self->m_stateLine->setStyleSheet("color:#c0392b;");
        }
    );
}

void NameResolveDialog::renderAvailable(const QString& name) {
    clearResults();
    m_stateLine->setText(
        tr("“%1” is available — it isn't registered yet.").arg(name));
    m_stateLine->setStyleSheet("color:#34c759;");
}

void NameResolveDialog::renderFound(const ResolvedName& r) {
    clearResults();
    m_stateLine->setText(tr("Registered."));
    m_stateLine->setStyleSheet("color:#34c759;");

    // Helper: a read-only, selectable "Label: value" row.
    auto addRow = [this](const QString& label, const QString& value) {
        auto* lbl = new QLabel(
            QString("<b>%1</b> %2").arg(label.toHtmlEscaped(), value.toHtmlEscaped()),
            m_resultsBox);
        lbl->setTextFormat(Qt::RichText);
        lbl->setWordWrap(true);
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_resultsLayout->addWidget(lbl);
    };

    if (!r.owner.isEmpty())  addRow(tr("Owner:"),  r.owner);
    if (!r.status.isEmpty()) addRow(tr("Status:"), r.status);

    if (!r.primaryValue.isEmpty()) {
        const QString tname = r.primaryTypeName.isEmpty()
                                  ? znamTargetLabel(r.primaryType)
                                  : r.primaryTypeName;
        addRow(tr("Points to (%1):").arg(tname), r.primaryValue);
    }

    for (const QPair<int, QString>& rec : r.records)
        addRow(QString("%1:").arg(znamTargetLabel(rec.first)), rec.second);

    for (const QPair<QString, QString>& t : r.text)
        addRow(QString("%1:").arg(t.first), t.second);
}
