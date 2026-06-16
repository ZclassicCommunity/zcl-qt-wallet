// ============================================================================
// ShieldSendDialog — native (no-browser) "Send a private file" dialog over the
// ZDC1 shielded data-channel (doc/nft/NATIVE_DISPLAY_UX.md §6.4,
// PRIVACY_TECH.md §5.3). Programmatic QDialog (no .ui), inherits NftAsyncDialog
// so the in-flight latch + [X]-swallow + Done/Try-again terminal transitions +
// the load-bearing QPointer UAF guard are single-sourced.
//
// HONESTY (load-bearing, NON-NEGOTIABLE):
//   * This makes only the FILE'S CONTENTS private. It does NOT make NFT ownership
//     private — who holds a token is ALWAYS public on the ledger.
//   * The encrypted file is stored PERMANENTLY and PUBLICLY on every node — it can
//     never be deleted, only kept unreadable. The Send button stays DISABLED until
//     the user actively ticks the permanence-consent checkbox (which maps to the
//     daemon's REQUIRED acknowledge_permanent=true; the daemon refuses without it).
//   * "Private" is NOT "undetectable": the existence/size/timing of the transfer is
//     visible on-chain — only the contents are confidential. We say so.
//
// FLOW (one form, gated): pick a LOCAL file (binary-safe read -> hex; hard 40000-
// byte cap enforced UP FRONT), pick a Sapling FROM z-addr (combo of the wallet's
// own zs.. addresses), paste the recipient's Sapling (zs..) address (live green/red
// validation), tick the permanence consent, Send -> RPC::sendDataFile. On success
// surface the content fingerprint (= NFT document_hash) + the per-transfer
// disclosure key, both copyable, honestly labeled. C++14 only (no std::optional).
// ============================================================================
#ifndef SHIELDSENDDIALOG_H
#define SHIELDSENDDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"   // shared in-flight latch + [X]-swallow + Done/Try-again

#include <QString>
#include <QByteArray>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class RPC;

class ShieldSendDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    // `rpc` issues z_senddatafile and provides the wallet's own Sapling z-addresses
    // for the From combo. An optional `prefillFingerprint` (an NFT document_hash) is
    // shown as context when the send is launched from a token's detail view.
    explicit ShieldSendDialog(RPC* rpc, QWidget* parent = nullptr);

#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAM: run the SAME read->hex->cap path as the file picker but with a
    // caller-supplied path (no QFileDialog under offscreen QPA). Never shipped.
    void testPickFile(const QString& path) { beginPick(path); }
#endif

private slots:
    void onPickFile();
    void onRecipientChanged(const QString& text);
    void onConsentToggled(bool checked);
    void onSendClicked();
    void onCopyFingerprint();
    void onCopyKey();

private:
    void beginPick(const QString& path);   // read+cap+hex a local file
    void refreshSendEnabled();
    QString inferContentType(const QString& path) const;

    RPC*         m_rpc       = nullptr;
    bool         m_succeeded = false;

    // chosen file state (set by beginPick; hex is the binary-safe payload)
    QString      m_filePath;
    QString      m_fileName;
    QString      m_hexData;       // lowercase hex of the file bytes (empty == none)
    int          m_fileBytes = 0;

    // ---- widgets ----
    QLabel*      m_fileLine    = nullptr;   // chosen filename + size / cap error
    QPushButton* m_pickBtn     = nullptr;
    QComboBox*   m_fromCombo   = nullptr;   // wallet's own Sapling z-addresses
    QLineEdit*   m_toEdit      = nullptr;
    QLabel*      m_toStatus    = nullptr;   // live green/red recipient validation
    QCheckBox*   m_consent     = nullptr;   // mandatory permanence consent
    QLabel*      m_resultLine  = nullptr;   // success/error line (reserved height)
    QPushButton* m_sendBtn     = nullptr;
    QPushButton* m_cancelBtn   = nullptr;

    // success-only disclosure widgets (built lazily on success)
    QLineEdit*   m_fpField     = nullptr;
    QLineEdit*   m_keyField    = nullptr;
};

#endif // SHIELDSENDDIALOG_H
