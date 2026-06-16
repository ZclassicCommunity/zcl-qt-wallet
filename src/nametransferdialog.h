// ============================================================================
// NameTransferDialog — native (no-browser) "Transfer a name" dialog (NAMES
// pillar / ZNAM). Programmatic modal QDialog (no .ui file). Transfers ownership
// of one of the wallet's names to a new owner via RPC::nameTransfer ->
// name_transfer.
//
// The name to transfer is chosen from a QComboBox seeded by the caller with the
// wallet's owned names. The new-owner field is validated t-address-ONLY (a
// shielded z-addr is rejected, because a name's owner rides the transparent
// ledger). On success we show "transferred — confirming" (0-conf honest), never
// an instant "Done". Coin is ZCL, never ZEC. C++14 only (empty-QString
// sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef NAMETRANSFERDIALOG_H
#define NAMETRANSFERDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"

#include <QStringList>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class RPC;

class NameTransferDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    // `myNames` seeds the name picker (the wallet's owned names). May be empty.
    explicit NameTransferDialog(RPC* rpc, const QStringList& myNames,
                                QWidget* parent = nullptr);

    QString lastTxid() const { return m_lastTxid; }

private slots:
    void onNewOwnerChanged(const QString& text);
    void onTransferClicked();

private:
    void refreshTransferEnabled();

    RPC*         m_rpc       = nullptr;
    bool         m_succeeded = false;
    QString      m_lastTxid;

    QComboBox*   m_nameCombo   = nullptr;
    QLineEdit*   m_ownerEdit   = nullptr;
    QLabel*      m_ownerStatus = nullptr;   // live valid/invalid (green / red)
    QLabel*      m_resultLine  = nullptr;
    QPushButton* m_transferBtn = nullptr;
    QPushButton* m_cancelBtn   = nullptr;
};

#endif // NAMETRANSFERDIALOG_H
