// ============================================================================
// NFTListingsDialog — native (no-browser) "Manage my listings" view
// (Journey 3: see the sell offers I created + cancel an open one). Programmatic
// modal QDialog (no .ui file), styled like the other NFT dialogs.
//
//   A QTableWidget lists every SELL offer in the local store — cols:
//   Collectible (a short tokenId), Price (ZCL), Status, Expires at block N.
//   The "Cancel listing" button is enabled ONLY when the selected row is still
//   "open"; cancelling self-spends the NFT UTXO, voiding the listing and freeing
//   the collectible (the daemon's nft_canceloffer), then the list re-refreshes.
//
// HONESTY (load-bearing): every record in the local store is the caller's own,
// and an offer's role is the LITERAL daemon string "sell" (nftoffer.cpp) — a
// "buyer"/"buy" row is filtered out (defensive; the store is sell-only). v1 is
// view + cancel ONLY: there is NO re-price RPC, so the dialog never offers one.
//
// LIFETIME: this fires write RPCs (cancel) + a read RPC (list). It derives from
// NftAsyncDialog for the shared in-flight latch + [X]-swallow-while-in-flight UAF
// guard; every reply lambda is QPointer-guarded so a destroyed dialog is a safe
// no-op. C++14 only (empty-QString sentinels, no std::optional/string_view).
// ============================================================================
#ifndef NFTLISTINGSDIALOG_H
#define NFTLISTINGSDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"   // shared in-flight latch + [X]-swallow + QPointer reply discipline
#include "rpc.h"              // NFTOfferRow by value

#include <QString>

class QLabel;
class QPushButton;
class QTableWidget;
class RPC;

class NFTListingsDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit NFTListingsDialog(RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seam: the offerId of the currently selected row ("" if none).
    QString selectedOfferId() const;

private slots:
    void onSelectionChanged();    // re-gate Cancel on the selected row's status
    void onCancelSelected();      // confirm -> nft_canceloffer -> refresh

private:
    void refresh();               // (re-)pull nft_listoffers and repopulate the table
    void populate(const QVector<NFTOfferRow>& rows);

    RPC* m_rpc = nullptr;

    QTableWidget* m_table      = nullptr;   // "nftListingsTable"
    QLabel*       m_emptyLabel = nullptr;   // shown when there are no sell offers
    QLabel*       m_statusLine = nullptr;   // calm loading / result / index-off line
    QPushButton*  m_cancelBtn  = nullptr;   // enabled only for an "open" selection
    QPushButton*  m_refreshBtn = nullptr;
};

#endif // NFTLISTINGSDIALOG_H
