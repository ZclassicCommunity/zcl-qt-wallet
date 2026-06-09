// ============================================================================
// NFTSellDialog — native (no-browser) "Sell this collectible" flow
// (NFT_SELL_DESIGN.md §5). Programmatic modal QDialog (no .ui file), styled like
// NFTSendDialog. Two phases in one window:
//
//   COMPOSE — Price (ZCL) + Expiry + buyer's public address -> "List".
//   LISTED  — the base64 offer blob (read-only) + Copy / Save (*.znftoffer) +
//             a "Listed" badge + "Cancel listing".
//
// HONESTY (normative, #119 / §8): this trade settles PUBLICLY on-chain — the
// price and both addresses are visible. The dialog states that plainly and never
// implies a private sale. A mismatch item (verifyState==2) hard-disables List
// (you shouldn't list a tampered collectible) with a red reason, mirroring the
// send dialog. buyerNftAddr is REQUIRED by the daemon (no open listing v1).
//
// LIFETIME: while an RPC is in flight the window [X] is swallowed (closeEvent),
// mirroring NFTSendDialog's UAF guard; the reply lambdas use a QPointer. C++14
// only (empty-QString sentinels, no std::optional). Expiry is the fixed ~7-day
// daemon default in v1 (we pass expiryHeight=0), shown as a read-only line; a
// tip-height-based custom expiry (with a real picker) is a documented follow-up.
// ============================================================================
#ifndef NFTSELLDIALOG_H
#define NFTSELLDIALOG_H

#include "precompiled.h"
#include "nft.h"
#include "nftasyncdialog.h"   // shared in-flight latch + [X]-swallow + Done/Try-again

#include <QString>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class RPC;

class NFTSellDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit NFTSellDialog(const NFTItem& item, RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seams ("" / 0 until a successful List).
    QString lastOfferId()   const { return m_offerId; }
    QString lastOfferBlob() const { return m_offerBlob; }

    // The [X]-swallow while a make/cancel RPC is in flight is inherited from
    // NftAsyncDialog::closeEvent (UAF guard, like the send dialog).

private slots:
    void onComposeChanged();   // re-gate List on a valid price + buyer t-address
    void onListClicked();
    void onCopyClicked();
    void onSaveClicked();
    void onCancelListingClicked();
    void onDoneClicked();

private:
    void refreshListEnabled();
    void enterListedState();   // swap the compose controls for the blob + actions

    NFTItem  m_item;
    RPC*     m_rpc      = nullptr;
    // m_inFlight now lives in NftAsyncDialog (isInFlight()/setInFlight()).
    bool     m_listed   = false;   // an offer has been produced (LISTED phase)

    QString  m_offerId;            // from nft_makeoffer (for Cancel)
    QString  m_offerBlob;          // the "znftoffer:..." blob to share

    // ---- compose-phase widgets ----
    QLineEdit*   m_priceEdit     = nullptr;
    QLabel*      m_priceStatus   = nullptr;
    QLabel*      m_expiryValue   = nullptr;   // read-only "About 7 days." (v1: no picker)
    QLineEdit*   m_buyerEdit     = nullptr;
    QLabel*      m_buyerStatus   = nullptr;
    QLabel*      m_mismatchWarn  = nullptr;   // shown only for verifyState==2
    QPushButton* m_listBtn       = nullptr;
    QLabel*      m_resultLine    = nullptr;

    // ---- listed-phase widgets (created lazily on success) ----
    QPlainTextEdit* m_blobView   = nullptr;
    QPushButton*    m_copyBtn    = nullptr;
    QPushButton*    m_saveBtn    = nullptr;
    QPushButton*    m_cancelListBtn = nullptr;
    QLabel*         m_listedBadge   = nullptr;
};

#endif // NFTSELLDIALOG_H
