// ============================================================================
// CollectionCreateDialog — native (no-browser) "Create a collection" flow
// (COLLECTIONS Phase-1). Programmatic modal QDialog (no .ui file), styled like
// NftMintDialog / NFTSellDialog. A collection is an ordinary ZSLP token minted
// WITH a mint baton and a quantity of "authority units" (one unit = one card you
// can add). Its tokenid becomes the group_id you pass to child cards.
//
// HONESTY (load-bearing): this writes a PUBLIC, permanent token to the ledger.
// The copy states plainly that (a) you keep the authority to add cards, (b) each
// card you add consumes one unit, and (c) the mint baton lets you add more units
// later. It NEVER implies a private/anonymous collection.
//
// LIFETIME: while the create RPC is in flight the window [X] is swallowed
// (NftAsyncDialog::closeEvent) and the reply lambda is QPointer-guarded — the
// proven UAF-safe pattern shared by every NFT dialog. C++14 only (empty-QString
// sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef COLLECTIONCREATEDIALOG_H
#define COLLECTIONCREATEDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"   // shared in-flight latch + [X]-swallow + Done/Try-again

#include <QString>

class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class RPC;

class CollectionCreateDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit CollectionCreateDialog(RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seams ("" until a successful create).
    QString lastTxid()   const { return m_lastTxid; }
    QString lastGroupId() const { return m_lastGroupId; }

    // The [X]-swallow while the create RPC is in flight is inherited from
    // NftAsyncDialog::closeEvent (UAF guard, like the mint dialog).

private slots:
    void onCreate();
    // After success the primary button is re-wired to QDialog::accept() by
    // NftAsyncDialog::finishPrimaryAsDone.

private:
    void refreshCreateEnabled();

    RPC* m_rpc = nullptr;
    bool m_succeeded = false;   // create returned; the dialog now shows the confirmation

    QString m_lastTxid;
    QString m_lastGroupId;

    QLineEdit*   m_nameEdit   = nullptr;
    QLineEdit*   m_tickerEdit = nullptr;
    QSpinBox*    m_countSpin  = nullptr;
    QLabel*      m_resultLine = nullptr;
    QPushButton* m_createBtn  = nullptr;
    QPushButton* m_cancelBtn  = nullptr;   // disabled while in flight; "Done" after success
};

#endif // COLLECTIONCREATEDIALOG_H
