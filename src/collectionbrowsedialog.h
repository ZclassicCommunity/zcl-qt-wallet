// ============================================================================
// CollectionBrowseDialog — native (no-browser) "Browse a set" view
// (COLLECTIONS Phase-1). Programmatic modal QDialog (no .ui file). Two phases in
// one window:
//
//   LIST  — the collections this wallet owns (RPC::listMyCollections, derived
//           from zslp_listmytokens). Each row shows the name + slots-left + an
//           "open/full" badge. Pick one to open it.
//   SET   — a zslp_collectioninfo header (member count + open/sealed) plus the
//           AUTHORIZED members (RPC::listCollectionMembers). A "Back" button
//           returns to the list.
//
// HONESTY (load-bearing, COLLECTIONS spec-v2): ONLY authorized members are shown.
// zslp_listcollectionmembers already returns authorized-only (a `group_claimed`
// squatter is never in it), and the wrapper double-checks group_authorized, so a
// non-member can never appear here. The header states plainly "verified members".
//
// LIFETIME: this is a READ-ONLY view (no write RPC, so no in-flight latch needed),
// but its async reply lambdas are QPointer-guarded so a fast close is a safe no-op.
// It still derives from NftAsyncDialog for the shared [X] behavior + style parity.
// C++14 only (empty-QString sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef COLLECTIONBROWSEDIALOG_H
#define COLLECTIONBROWSEDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"
#include "rpc.h"          // CollectionRow / CollectionInfo / CollectionMember by value

#include <QString>
#include <QVector>

class QLabel;
class QListWidget;
class QStackedWidget;
class QPushButton;
class QWidget;

class CollectionBrowseDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit CollectionBrowseDialog(RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seam: the group_id currently opened in the SET phase ("" in LIST).
    QString openedGroupId() const { return m_openedGroupId; }

private slots:
    void onCollectionActivated();   // open the selected collection (LIST -> SET)
    void onBackClicked();           // SET -> LIST

private:
    void loadCollections();                       // populate the LIST phase
    void openCollection(const CollectionRow& c);  // enter the SET phase for one collection

    RPC* m_rpc = nullptr;

    // ---- LIST phase ----
    QListWidget* m_collectionList = nullptr;
    QLabel*      m_listState      = nullptr;   // "loading / none / error" honesty line
    QPushButton* m_openBtn        = nullptr;
    QVector<CollectionRow> m_collections;      // index-aligned with the list rows

    // ---- SET phase ----
    QStackedWidget* m_stack       = nullptr;   // 0 = LIST page, 1 = SET page
    QLabel*      m_setHeader      = nullptr;   // name + member count + open/sealed
    QLabel*      m_setHonesty     = nullptr;   // "Only verified members are shown."
    QListWidget* m_memberList     = nullptr;
    QLabel*      m_memberState    = nullptr;   // "loading / no members / error"
    QPushButton* m_backBtn        = nullptr;
    QString      m_openedGroupId;
};

#endif // COLLECTIONBROWSEDIALOG_H
