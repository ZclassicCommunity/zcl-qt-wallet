// ============================================================================
// NameRegisterDialog — native (no-browser) "Register a name" dialog (NAMES
// pillar / ZNAM). Programmatic modal QDialog (no .ui file). Registers a ZNAM
// name first-in-first-served via RPC::nameRegister -> name_register.
//
// The name field is LIVE-validated with znamIsValidName (green/red) and gates
// the Register button. The target type comes from a QComboBox seeded from
// znamTargetChoices() (the int target_type stored in itemData). The value field
// is the target payload. An optional owner QComboBox is seeded from the wallet's
// transparent addresses (empty selection => the daemon picks an own address).
//
// HONESTY: a name is registered on the public ledger; the registration is
// 0-conf-invisible to the confirmed-only indexer, so on success we show
// "registered — confirming", never an instant "Done". Coin is ZCL, never ZEC.
// C++14 only (empty-QString sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef NAMEREGISTERDIALOG_H
#define NAMEREGISTERDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"   // shared in-flight latch + [X]-swallow + Done/Try-again

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class RPC;

class NameRegisterDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit NameRegisterDialog(RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seam: the txid the last successful register returned.
    QString lastTxid() const { return m_lastTxid; }

private slots:
    void onNameChanged(const QString& text);
    void onRegisterClicked();

private:
    void refreshRegisterEnabled();

    RPC*         m_rpc       = nullptr;
    bool         m_succeeded = false;
    QString      m_lastTxid;

    QLineEdit*   m_nameEdit    = nullptr;
    QLabel*      m_nameStatus  = nullptr;   // live valid/invalid line (green / red)
    QComboBox*   m_typeCombo   = nullptr;
    QLineEdit*   m_valueEdit   = nullptr;
    QComboBox*   m_ownerCombo  = nullptr;
    QLabel*      m_resultLine  = nullptr;
    QPushButton* m_registerBtn = nullptr;
    QPushButton* m_cancelBtn   = nullptr;
};

#endif // NAMEREGISTERDIALOG_H
