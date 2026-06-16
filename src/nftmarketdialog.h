// ============================================================================
// NFTMarketDialog — native (no-browser) decentralized "Browse the market"
// view (Journey 4). Programmatic modal QDialog (no .ui file), modeled on
// CollectionBrowseDialog. The single discovery surface for offers the rest of
// the network has gossiped into the daemon's offer pool.
//
// FLOW (one window):
//   SEARCH  — a name/collection/tokenId text box + a max-price (ZCL) box +
//             "Search". Search calls RPC::nftBrowseOffers(...); each result is
//             joined client-side (zslp_gettoken) for its token name / collection
//             so the NAME/COLLECTION text filter can run over the fetched page.
//   RESULTS — a PRICE-ASCENDING list (cheapest first), each row showing the
//             token name + price in ZCL (NEVER zatoshi/"ZEC") + a thumbnail from
//             the EXISTING ContentEngine (nftImgCache) when the image is local.
//   BUY     — hands the selected row's FULL offerBlob to the proven
//             NFTBuyDialog::openWithOffer() (verify-before-pay flow). We never
//             reconstruct an offer here.
//
// HONESTY (load-bearing): the offer pool has NO name index (beta7 default), so
// the name/collection filter is a CLIENT-SIDE filter over the fetched page, not
// a server search — the user is told the results are "what your node has heard".
// When browse is method-not-found (a daemon that predates the RPC / a foreign
// node) OR the pool is simply empty, we show the CALM honest line "Offer
// discovery is off (enable -nftmarket once Tor relay ships)" — never a scary
// error dialog or a bare empty grid.
//
// LIFETIME: a READ-ONLY view (no write RPC of its own — Buy is delegated to a
// child NFTBuyDialog), so no in-flight latch is needed; its async reply lambdas
// are QPointer-guarded so a fast close is a safe no-op. Derives from
// NftAsyncDialog for the shared [X] behavior + style parity.
// C++14 only (empty-QString sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef NFTMARKETDIALOG_H
#define NFTMARKETDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"
#include "rpc.h"          // NFTBrowseRow by value

#include <QString>
#include <QVector>
#include <QImage>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class ContentEngine;

class NFTMarketDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    explicit NFTMarketDialog(ContentEngine* engine, RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seams (drive the dialog with no daemon, then assert state).
    void testRunSearch(const QString& query);    // type `query` + run the search
    void selectRow(int idx);                      // select a results row (price-ascending order)
    void clickBuy();                              // click Buy on the current selection

    // The offerBlob most recently handed to NFTBuyDialog::openWithOffer (the exact
    // blob the Buy handoff routed). Empty until a Buy is clicked on a real row.
    QString lastHandoffBlob() const { return m_lastHandoffBlob; }

private slots:
    void onSearchClicked();
    void onSelectionChanged();
    void onBuyClicked();
    void onPosterReady(quint64 token, QImage img, int verifyState);

private:
    void runSearch(const QString& query);             // browse + client-side name/collection filter
    void renderResults();                             // paint m_results (price-ascending) into the list
    void requestThumb(int row, const NFTBrowseRow& r);// per-row local-only thumbnail via ContentEngine

    ContentEngine* m_engine = nullptr;
    RPC*           m_rpc    = nullptr;

    // ---- widgets ----
    QLineEdit*   m_searchInput  = nullptr;   // name / collection / tokenId
    QLineEdit*   m_maxPriceInput = nullptr;  // max price in ZCL (blank = no cap)
    QPushButton* m_searchBtn     = nullptr;
    QLabel*      m_stateLabel    = nullptr;   // loading / empty / discovery-off honesty line
    QListWidget* m_resultsList   = nullptr;
    QPushButton* m_buyBtn        = nullptr;
    QPushButton* m_closeBtn      = nullptr;

    // Results in PRICE-ASCENDING order, index-aligned with the list rows. Filled by
    // runSearch (browse + client-side filter), consumed by Buy.
    QVector<NFTBrowseRow> m_results;

    // The text filter currently applied (so a thumbnail / name-join landing late can
    // tell whether it still belongs to the displayed page). Bumped each search.
    quint64 m_searchGen = 0;

    QString m_lastHandoffBlob;   // the blob the last Buy handed to NFTBuyDialog
};

#endif // NFTMARKETDIALOG_H
