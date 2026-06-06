// ============================================================================
// NFTSendDialog — native (no-browser) "Send a gift" dialog for one NFT
// (NATIVE_NFT_GUIDE.md §2.6). Modal QDialog (programmatic, no .ui file).
//
// Requires an NFTItem by value (no empty state). It transfers the 1-of-1 token
// to a recipient PUBLIC (transparent) address via RPC::sendNFT -> zslp_send.
// ZSLP ownership rides transparent 546-sat dust, so the recipient is a public
// (transparent) t-address; a shielded recipient is honestly NOT supported by
// this public path (the Private gift tile is "Coming soon", gated off by
// RPC::isPrivateMintWired()==false).
//
// HONEST states: a MISMATCH item ("This picture doesn't match its fingerprint")
// disables Send; a successful send shows "Gift sent — confirming on-chain" (the
// confirmed-only indexer drops it from the gallery once it confirms — never
// "you don't own this"). C++14 only (empty-QString sentinels, no std::optional).
// ============================================================================
#ifndef NFTSENDDIALOG_H
#define NFTSENDDIALOG_H

#include "precompiled.h"
#include "nft.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class RPC;

class NFTSendDialog : public QDialog {
    Q_OBJECT
public:
    // `item` is the NFT being gifted (by value). `rpc` issues the zslp_send.
    explicit NFTSendDialog(const NFTItem& item, RPC* rpc, QWidget* parent = nullptr);

protected:
    // While the send RPC is in flight, swallow the window [X] so the dialog can't be
    // destroyed out from under the in-flight reply (review fix #5 / UAF).
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onRecipientChanged(const QString& text);
    void onSendClicked();
    void onDoneClicked();   // after success: explicit dismiss (accept) of the confirmation

private:
    void refreshSendEnabled();

    NFTItem      m_item;
    RPC*         m_rpc       = nullptr;
    bool         m_inFlight  = false;
    bool         m_succeeded = false;   // send returned; dialog now shows the confirmation

    QLineEdit*   m_recipient = nullptr;
    QLabel*      m_addrStatus= nullptr;   // live valid/invalid line (green/amber/red)
    QLabel*      m_resultLine= nullptr;   // success/error line (reserved height)
    QPushButton* m_sendBtn   = nullptr;
    QPushButton* m_cancelBtn = nullptr;   // disabled while in flight
};

#endif // NFTSENDDIALOG_H
