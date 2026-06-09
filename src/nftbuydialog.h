// ============================================================================
// NFTBuyDialog — native (no-browser) "Buy an NFT" flow (NFT_SELL_DESIGN.md §5).
// Programmatic modal QDialog (no .ui file), styled like the detail/send dialogs.
//
// Phases in one window:
//   PASTE/OPEN   — paste a "znftoffer:" blob OR Open a *.znftoffer file.
//   VERIFIED CARD — AUTO-runs nft_verifyoffer; renders the NFT image (via the
//                   EXISTING ContentEngine, never a second engine), name, price,
//                   expiry, and a GREEN check (verify ok) or AMBER warning + the
//                   honest reason. Buy is HARD-disabled until ok && acknowledge.
//   CONFIRM      — "You pay P ZCL (+ ~fee). You receive: <name>." + the honest
//                  public-settlement line + overshoot shown after settlement.
//   SETTLING/RESULT — spinner -> "NFT received — on its way." Buy becomes Done.
//
// HONESTY (normative, #119 / §7-8): verify is MANDATORY before the buyer pays; a
// green verdict NEVER renders without a real ok==true; ANY edit to the offer
// re-disables Buy so a tampered blob can't ride a stale green verdict. The image
// is local-only (never auto-fetched) — an absent image shows an honest "not on
// this computer" placeholder. We never show vout indices / sighash internals.
//
// LIFETIME: [X] swallowed while an RPC is in flight; reply lambdas use a QPointer.
// C++14 only (empty-QString sentinels, no std::optional). The image render needs
// the token's documenthash (via RPC::nftProvenance) -> ContentEngine::posterForToken.
// ============================================================================
#ifndef NFTBUYDIALOG_H
#define NFTBUYDIALOG_H

#include "precompiled.h"
#include "rpc.h"             // NFTVerifyResult by value (held + a getter)
#include "nftasyncdialog.h"  // shared in-flight latch + [X]-swallow + Done/Try-again

#include <QImage>
#include <QPixmap>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class ContentEngine;

class NFTBuyDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit NFTBuyDialog(ContentEngine* engine, RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seams.
    QString lastTxid()          const { return m_lastTxid; }
    qint64  lastOvershootZat()  const { return m_lastOvershoot; }
    NFTVerifyResult verifyResult() const { return m_verified; }

    // The single PUBLIC seam the Market dialog (Journey 4) uses to hand a BROWSED
    // offerBlob into the proven verify/take flow: it drops the blob into the input
    // (which resets the verify gate via textChanged) and auto-runs nft_verifyoffer —
    // exactly what Paste/Open would do. The browsed blob is never reconstructed; the
    // mandatory pre-pay verify still gates Buy. (testPasteOffer just calls this.)
    void openWithOffer(const QString& blob);

#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAM: feed an offer blob the same way Paste/Open would (the file
    // dialog is un-drivable under offscreen QPA). Delegates to openWithOffer().
    void testPasteOffer(const QString& blob);
#endif

    // The [X]-swallow while the take RPC is in flight is inherited from
    // NftAsyncDialog::closeEvent (UAF guard).

private slots:
    void onOfferTextChanged();
    void onOpenFileClicked();
    void onVerifyClicked();
    void onPosterReady(quint64 token, QImage img, int verifyState);
    void onAcknowledgeToggled(bool);
    void onBuyClicked();
    // After success the Buy button is re-wired to QDialog::accept() by
    // NftAsyncDialog::finishPrimaryAsDone — no per-dialog onDoneClicked needed.

private:
    void runVerify();                 // start nft_verifyoffer for the current blob
    void resetVerifyGate();           // any offer edit invalidates a green verdict
    void renderVerified();            // paint the card from m_verified
    void requestPoster();             // resolve documenthash -> posterForToken
    void refreshBuyEnabled();

    ContentEngine* m_engine = nullptr;
    RPC*           m_rpc    = nullptr;

    QString         m_offerBlob;      // the current pasted/opened blob
    NFTVerifyResult m_verified;       // last verify result; m_verified.ok gates Buy
    bool            m_verifyInFlight = false;   // SEPARATE latch for the verify flow
    // The take-RPC in-flight latch now lives in NftAsyncDialog (isInFlight()).
    bool            m_succeeded      = false;
    bool            m_acknowledged   = false;

    quint64  m_posterToken = 0;       // per-open token so a stale neighbor's reply drops
    QString  m_provDocHash;           // documenthash resolved via nftProvenance

    QString  m_lastTxid;
    qint64   m_lastOvershoot = 0;

    // ---- widgets ----
    QPlainTextEdit* m_offerInput   = nullptr;
    QPushButton*    m_openFileBtn  = nullptr;
    QPushButton*    m_verifyBtn    = nullptr;

    QLabel*         m_stage        = nullptr;   // NFT image (or honest placeholder)
    QLabel*         m_nameLbl      = nullptr;
    QLabel*         m_priceLbl     = nullptr;
    QLabel*         m_expiryLbl    = nullptr;
    QLabel*         m_verdictLbl   = nullptr;   // green check / amber reason
    QLabel*         m_confirmLine  = nullptr;   // "You pay P ZCL. You receive: <name>."
    QLabel*         m_publicNote   = nullptr;
    QCheckBox*      m_ackBox       = nullptr;
    QLabel*         m_resultLine   = nullptr;
    QPushButton*    m_buyBtn       = nullptr;
    QPushButton*    m_closeBtn     = nullptr;
};

#endif // NFTBUYDIALOG_H
