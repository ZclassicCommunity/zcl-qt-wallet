// ============================================================================
// ShieldReceiveDialog — native (no-browser) "Receive a private file" view over
// the ZDC1 shielded data-channel (doc/nft/NATIVE_DISPLAY_UX.md §6.5,
// PRIVACY_TECH.md §5.4). Programmatic QDialog (no .ui).
//
// Two entry modes in one dialog:
//   * A LIST of this session's data transfers (z_listdatatransfers) — labeled
//     honestly as "sent this session", NOT a durable or received history. Picking
//     a row pre-fills its fingerprint and runs the lookup.
//   * Cross-wallet OPEN by transfer_id OR fingerprint (paste), with an optional
//     receiving Sapling address (defaults to scanning all viewable addrs) and an
//     optional out-of-band disclosure fingerprint.
//
// SAFETY (load-bearing): the daemon VERIFIES BEFORE DECRYPT and returns plaintext
// ONLY when verified+complete. This dialog surfaces the four distinct honest
// states (HASH_MISMATCH / NO_KEY / AEAD_FAIL / INCOMPLETE) — never a generic
// "failed" or a fake "try again" on a hard refusal — and offers "Save decrypted
// file…" ONLY after the fingerprint verifies. The hexdata is decoded binary-safe
// to bytes (QByteArray::fromHex), never round-tripped through a QString.
//
// HONESTY: receiving a private file makes only the CONTENT private; ownership is
// public. This dialog does NOT issue an async write/spend, so it is a plain
// QDialog (no in-flight/Done transition is needed); each lookup is QPointer-guarded.
// C++14 only. The coin is ZCL.
// ============================================================================
#ifndef SHIELDRECEIVEDIALOG_H
#define SHIELDRECEIVEDIALOG_H

#include "precompiled.h"

#include <QDialog>
#include <QString>
#include <QByteArray>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class RPC;

class ShieldReceiveDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShieldReceiveDialog(RPC* rpc, QWidget* parent = nullptr);

    // Pre-fill the lookup with an NFT's fingerprint (Mode A: open the file linked to
    // a token you hold) and immediately run the verify-before-decrypt lookup.
    void prefillFingerprint(const QString& fingerprint64);

private slots:
    void onRefreshList();
    void onListRowActivated();
    void onLookup();
    void onSaveDecrypted();

private:
    void runLookup(const QString& transferId, const QString& fingerprint);
    void clearResult();

    RPC*         m_rpc = nullptr;

    // last verified plaintext (decoded binary-safe; non-empty only on a verified open)
    QByteArray   m_plaintext;
    QString      m_lastFilename;

    // ---- widgets ----
    QListWidget* m_list        = nullptr;
    QPushButton* m_refreshBtn  = nullptr;
    QLineEdit*   m_idEdit      = nullptr;   // transfer_id OR fingerprint
    QLineEdit*   m_addrEdit    = nullptr;   // optional receiving zs-addr
    QLineEdit*   m_verifyEdit  = nullptr;   // optional out-of-band verify fingerprint
    QPushButton* m_lookupBtn   = nullptr;
    QLabel*      m_stateLine   = nullptr;   // the honest verify/decrypt verdict
    QLabel*      m_metaLine    = nullptr;   // filename / size on success
    QPushButton* m_saveBtn     = nullptr;   // enabled ONLY after verify+decrypt
};

#endif // SHIELDRECEIVEDIALOG_H
