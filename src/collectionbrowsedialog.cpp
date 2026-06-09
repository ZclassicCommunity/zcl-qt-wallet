// ============================================================================
// CollectionBrowseDialog implementation — see collectionbrowsedialog.h.
// COLLECTIONS Phase-1. A read-only two-phase view (LIST collections -> SET
// members). ONLY authorized members are ever shown.
// ============================================================================
#include "collectionbrowsedialog.h"
#include "rpc.h"
#include "guiutil.h"     // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QStackedWidget>
#include <QPushButton>
#include <QFrame>
#include <QPointer>

CollectionBrowseDialog::CollectionBrowseDialog(RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Browse a collection"));
    setMinimumWidth(480);
    setMinimumHeight(420);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack, 1);

    // ===================== LIST page (your collections) =====================
    auto* listPage = new QWidget(m_stack);
    auto* listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(8);

    auto* listHeading = new QLabel(tr("Your collections"), listPage);
    listHeading->setObjectName("collectionBrowseListHeading");
    listHeading->setStyleSheet("font-weight:600; font-size:14pt;");
    listLayout->addWidget(listHeading);

    m_listState = new QLabel(tr("Loading your collections…"), listPage);
    m_listState->setObjectName("collectionBrowseListState");
    m_listState->setWordWrap(true);
    m_listState->setStyleSheet("color:#9aa0a6;");
    listLayout->addWidget(m_listState);

    m_collectionList = new QListWidget(listPage);
    m_collectionList->setObjectName("collectionBrowseList");
    m_collectionList->setSelectionMode(QAbstractItemView::SingleSelection);
    listLayout->addWidget(m_collectionList, 1);

    auto* listBtnRow = new QHBoxLayout();
    listBtnRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"), listPage);
    closeBtn->setObjectName("collectionBrowseCloseButton");
    m_openBtn = new QPushButton(tr("Open"), listPage);
    m_openBtn->setObjectName("collectionBrowseOpenButton");
    m_openBtn->setDefault(true);
    m_openBtn->setEnabled(false);
    listBtnRow->addWidget(closeBtn);
    listBtnRow->addWidget(m_openBtn);
    listLayout->addLayout(listBtnRow);

    m_stack->addWidget(listPage);   // index 0

    // ========================= SET page (members) ==========================
    auto* setPage = new QWidget(m_stack);
    auto* setLayout = new QVBoxLayout(setPage);
    setLayout->setContentsMargins(0, 0, 0, 0);
    setLayout->setSpacing(8);

    m_setHeader = new QLabel(setPage);
    m_setHeader->setObjectName("collectionBrowseSetHeader");
    m_setHeader->setWordWrap(true);
    m_setHeader->setTextFormat(Qt::RichText);
    setLayout->addWidget(m_setHeader);

    m_setHonesty = new QLabel(
        tr("Only verified members are shown — cards whose creation spent one of this "
           "collection's slots. Anyone can name a collection, but only the owner can add "
           "a real member."), setPage);
    m_setHonesty->setObjectName("collectionBrowseSetHonesty");
    m_setHonesty->setProperty("hint", true);
    m_setHonesty->setWordWrap(true);
    m_setHonesty->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    setLayout->addWidget(m_setHonesty);

    auto* sep = new QFrame(setPage);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    setLayout->addWidget(sep);

    m_memberState = new QLabel(setPage);
    m_memberState->setObjectName("collectionBrowseMemberState");
    m_memberState->setWordWrap(true);
    m_memberState->setStyleSheet("color:#9aa0a6;");
    setLayout->addWidget(m_memberState);

    m_memberList = new QListWidget(setPage);
    m_memberList->setObjectName("collectionBrowseMemberList");
    m_memberList->setSelectionMode(QAbstractItemView::NoSelection);
    setLayout->addWidget(m_memberList, 1);

    auto* setBtnRow = new QHBoxLayout();
    m_backBtn = new QPushButton(tr("Back"), setPage);
    m_backBtn->setObjectName("collectionBrowseBackButton");
    setBtnRow->addWidget(m_backBtn);
    setBtnRow->addStretch(1);
    auto* setCloseBtn = new QPushButton(tr("Close"), setPage);
    setCloseBtn->setObjectName("collectionBrowseSetCloseButton");
    setBtnRow->addWidget(setCloseBtn);
    setLayout->addLayout(setBtnRow);

    m_stack->addWidget(setPage);   // index 1
    m_stack->setCurrentIndex(0);

    // --- wiring ---
    connect(closeBtn,    &QPushButton::clicked, this, &QDialog::reject);
    connect(setCloseBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_backBtn,   &QPushButton::clicked, this, &CollectionBrowseDialog::onBackClicked);
    connect(m_openBtn,   &QPushButton::clicked, this, &CollectionBrowseDialog::onCollectionActivated);
    connect(m_collectionList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onCollectionActivated(); });
    connect(m_collectionList, &QListWidget::itemSelectionChanged, this, [this]() {
        if (m_openBtn)
            m_openBtn->setEnabled(m_collectionList->currentRow() >= 0);
    });

    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });

    loadCollections();
}

void CollectionBrowseDialog::loadCollections() {
    if (m_rpc == nullptr)
        return;
    QPointer<CollectionBrowseDialog> self(this);
    m_rpc->listMyCollections(
        [self](QVector<CollectionRow> rows) {
            if (self.isNull())
                return;
            self->m_collections = rows;
            self->m_collectionList->clear();
            for (const CollectionRow& c : rows) {
                const QString name = c.name.isEmpty()
                                         ? (c.ticker.isEmpty() ? c.tokenId.left(10) + QStringLiteral("…")
                                                               : c.ticker)
                                         : c.name;
                // "open" here means the wallet still holds a slot to add a card; we phrase it
                // as slots-left so it's honest about what the user can do.
                const QString badge = (c.balance > 0)
                    ? QObject::tr("%1 card slot(s) left").arg(c.balance)
                    : QObject::tr("full");
                self->m_collectionList->addItem(QStringLiteral("%1  —  %2").arg(name, badge));
            }
            if (rows.isEmpty()) {
                self->m_listState->setText(QObject::tr(
                    "You don't own any collections yet. Create one to group cards into a set."));
                self->m_listState->show();
            } else {
                self->m_listState->hide();
            }
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            // Honest error line (e.g. old node / index off), never a dialog.
            self->m_listState->setText(errStr);
            self->m_listState->setStyleSheet("color:#c0392b;");
            self->m_listState->show();
        }
    );
}

void CollectionBrowseDialog::onCollectionActivated() {
    const int row = m_collectionList->currentRow();
    if (row < 0 || row >= m_collections.size())
        return;
    openCollection(m_collections.at(row));
}

void CollectionBrowseDialog::onBackClicked() {
    m_openedGroupId.clear();
    if (m_stack)
        m_stack->setCurrentIndex(0);
}

void CollectionBrowseDialog::openCollection(const CollectionRow& c) {
    m_openedGroupId = c.tokenId;
    if (m_stack)
        m_stack->setCurrentIndex(1);

    const QString displayName = c.name.isEmpty()
                                    ? (c.ticker.isEmpty() ? c.tokenId.left(10) + QStringLiteral("…")
                                                          : c.ticker)
                                    : c.name;
    // Provisional header until collectioninfo lands.
    m_setHeader->setText(QStringLiteral("<b>%1</b>").arg(displayName.toHtmlEscaped()));
    m_memberState->setText(tr("Loading members…"));
    m_memberState->setStyleSheet("color:#9aa0a6;");
    m_memberState->show();
    m_memberList->clear();

    const QString groupId = c.tokenId;
    QPointer<CollectionBrowseDialog> self(this);

    // 1) Header: zslp_collectioninfo (member count + open/sealed).
    m_rpc->collectionInfo(groupId,
        [self, groupId, displayName](CollectionInfo info) {
            if (self.isNull() || self->m_openedGroupId != groupId)
                return;   // dialog gone, or the user already went back / opened another
            const QString name = info.name.isEmpty() ? displayName : info.name;
            const QString openLabel = info.open
                ? QObject::tr("open — more cards can be added")
                : QObject::tr("sealed — no more cards can be added");
            const QString count = QObject::tr("%1 verified member(s)").arg(info.memberCount);
            self->m_setHeader->setText(
                QStringLiteral("<b>%1</b><br/><span style='color:#9aa0a6'>%2 · %3</span>")
                    .arg(name.toHtmlEscaped(), count, openLabel));
        },
        [self, groupId](QString /*errStr*/) {
            if (self.isNull() || self->m_openedGroupId != groupId)
                return;
            // Leave the provisional header; the member list still loads independently.
        }
    );

    // 2) Members: zslp_listcollectionmembers (AUTHORIZED children only).
    m_rpc->listCollectionMembers(groupId, /*from=*/0, /*count=*/1000,
        [self, groupId](QVector<CollectionMember> members) {
            if (self.isNull() || self->m_openedGroupId != groupId)
                return;
            self->m_memberList->clear();
            for (const CollectionMember& m : members) {
                const QString name = m.name.isEmpty()
                                         ? m.tokenId.left(12) + QStringLiteral("…")
                                         : m.name;
                self->m_memberList->addItem(
                    QStringLiteral("%1\n%2").arg(name, m.tokenId));
            }
            if (members.isEmpty()) {
                self->m_memberState->setText(QObject::tr(
                    "No cards have been added to this collection yet."));
                self->m_memberState->show();
            } else {
                self->m_memberState->hide();
            }
        },
        [self, groupId](QString errStr) {
            if (self.isNull() || self->m_openedGroupId != groupId)
                return;
            self->m_memberState->setText(errStr);
            self->m_memberState->setStyleSheet("color:#c0392b;");
            self->m_memberState->show();
        }
    );
}
