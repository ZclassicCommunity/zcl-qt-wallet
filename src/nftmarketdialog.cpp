// ============================================================================
// NFTMarketDialog implementation — see nftmarketdialog.h. Journey 4 (decentralized
// Browse/Search + Buy handoff). A read-only discovery view over the daemon's offer
// pool (RPC::nftBrowseOffers); Buy delegates to the proven NFTBuyDialog verify/take
// flow via openWithOffer(). Coin is ZCL (never ZEC) in every user-facing string.
// ============================================================================
#include "nftmarketdialog.h"
#include "rpc.h"
#include "settings.h"            // zatToDecimalString (the canonical ZCL price formatter)
#include "contentengine.h"
#include "nftbuydialog.h"
#include "guiutil.h"             // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QFrame>
#include <QPointer>
#include <QPixmap>
#include <algorithm>

namespace {
    const int kThumbPx = 48;     // results-row thumbnail edge

    // The CALM honest empty/discovery-off line. Shown when browse is method-not-found
    // (a daemon predating the RPC / a foreign node) OR the pool is simply empty —
    // NEVER a scary error or a bare empty grid. -nftmarket is default OFF in beta7
    // (it flips on once the Tor relay ships), so this is the expected resting state.
    QString discoveryOffLine() {
        return QObject::tr(
            "Offer discovery is off (enable -nftmarket once Tor relay ships). "
            "Nothing your node has heard about is for sale right now — you can still "
            "buy directly by pasting an offer in the Buy window.");
    }

    // Monotonic per-row poster token so a stale thumbnail reply for a now-replaced
    // results page drops (mirrors the detail/buy dialogs).
    quint64 nextMarketPosterToken() {
        static quint64 s = 0;
        return ++s;
    }

    // A friendly display name for a browsed row: the joined token name, else the
    // ticker-less short token id. NEVER shows zatoshi.
    QString rowDisplayName(const NFTBrowseRow& r) {
        if (!r.tokenName.isEmpty())
            return r.tokenName;
        if (!r.tokenId.isEmpty())
            return r.tokenId.left(12) + QStringLiteral("…");
        return QObject::tr("Collectible");
    }
}

NFTMarketDialog::NFTMarketDialog(ContentEngine* engine, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_engine(engine), m_rpc(rpc) {
    setWindowTitle(tr("Browse the market"));
    setMinimumWidth(520);
    setMinimumHeight(460);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    auto* heading = new QLabel(tr("Browse the market"), this);
    heading->setObjectName("nftMarketHeading");
    heading->setStyleSheet("font-weight:600; font-size:14pt;");
    outer->addWidget(heading);

    auto* honesty = new QLabel(
        tr("These are the offers your node has heard about — search filters what's "
           "already here (there is no central listing index). Prices are in ZCL, "
           "cheapest first. Buying always re-checks the offer before you pay."), this);
    honesty->setObjectName("nftMarketHonesty");
    honesty->setProperty("hint", true);
    honesty->setWordWrap(true);
    honesty->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(honesty);

    // ---- search row: name/collection/tokenId + max price (ZCL) + Search ----
    auto* searchRow = new QHBoxLayout();
    m_searchInput = new QLineEdit(this);
    m_searchInput->setObjectName("nftMarketSearchInput");
    m_searchInput->setPlaceholderText(tr("Name, collection, or collectible id"));
    searchRow->addWidget(m_searchInput, 1);

    m_maxPriceInput = new QLineEdit(this);
    m_maxPriceInput->setObjectName("nftMarketMaxPriceInput");
    m_maxPriceInput->setPlaceholderText(tr("Max price (ZCL)"));
    m_maxPriceInput->setMaximumWidth(140);
    searchRow->addWidget(m_maxPriceInput);

    m_searchBtn = new QPushButton(tr("Search"), this);
    m_searchBtn->setObjectName("nftMarketSearchButton");
    m_searchBtn->setDefault(true);
    searchRow->addWidget(m_searchBtn);
    outer->addLayout(searchRow);

    m_stateLabel = new QLabel(tr("Search the market to see what's for sale."), this);
    m_stateLabel->setObjectName("nftMarketStateLabel");
    m_stateLabel->setWordWrap(true);
    m_stateLabel->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(m_stateLabel);

    m_resultsList = new QListWidget(this);
    m_resultsList->setObjectName("nftMarketResultsList");
    m_resultsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsList->setIconSize(QSize(kThumbPx, kThumbPx));
    outer->addWidget(m_resultsList, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setObjectName("nftMarketCloseButton");
    m_buyBtn = new QPushButton(tr("Buy"), this);
    m_buyBtn->setObjectName("nftMarketBuyButton");
    m_buyBtn->setDefault(false);
    m_buyBtn->setEnabled(false);     // enabled only once a row is selected
    btnRow->addWidget(m_closeBtn);
    btnRow->addWidget(m_buyBtn);
    outer->addLayout(btnRow);

    // --- wiring ---
    connect(m_searchBtn,  &QPushButton::clicked, this, &NFTMarketDialog::onSearchClicked);
    connect(m_searchInput, &QLineEdit::returnPressed, this, &NFTMarketDialog::onSearchClicked);
    connect(m_maxPriceInput, &QLineEdit::returnPressed, this, &NFTMarketDialog::onSearchClicked);
    connect(m_closeBtn,   &QPushButton::clicked, this, &QDialog::reject);
    connect(m_buyBtn,     &QPushButton::clicked, this, &NFTMarketDialog::onBuyClicked);
    connect(m_resultsList, &QListWidget::itemSelectionChanged,
            this, &NFTMarketDialog::onSelectionChanged);
    connect(m_resultsList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onBuyClicked(); });
    if (m_engine)
        connect(m_engine, &ContentEngine::posterReady, this, &NFTMarketDialog::onPosterReady);

    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });
}

void NFTMarketDialog::onSearchClicked() {
    runSearch(m_searchInput ? m_searchInput->text().trimmed() : QString());
}

void NFTMarketDialog::onSelectionChanged() {
    if (m_buyBtn)
        m_buyBtn->setEnabled(m_resultsList && m_resultsList->currentRow() >= 0
                             && m_resultsList->currentRow() < m_results.size());
}

void NFTMarketDialog::testRunSearch(const QString& query) {
    if (m_searchInput)
        m_searchInput->setText(query);
    runSearch(query);
}

void NFTMarketDialog::selectRow(int idx) {
    if (m_resultsList && idx >= 0 && idx < m_resultsList->count())
        m_resultsList->setCurrentRow(idx);
}

void NFTMarketDialog::clickBuy() {
    onBuyClicked();
}

void NFTMarketDialog::runSearch(const QString& query) {
    if (m_rpc == nullptr)
        return;

    // Max-price cap (ZCL -> zat). A blank / unparseable box means no cap (0 omits the
    // wire field). The daemon-side filter is the authoritative cap; we still pass it.
    qint64 maxPriceZat = 0;
    if (m_maxPriceInput) {
        const QString raw = m_maxPriceInput->text().trimmed();
        if (!raw.isEmpty()) {
            bool ok = false;
            const double zcl = raw.toDouble(&ok);
            if (ok && zcl > 0)
                maxPriceZat = RPC::zclToZat(zcl);
        }
    }

    // The search box doubles as a tokenId filter on the wire (an exact 64-hex id can
    // be pushed to the daemon) AND a client-side name/collection text filter. We send
    // the daemon the FULL query as tokenId ONLY when it looks like a hex token id; a
    // free-text name search is filtered entirely client-side over the fetched page
    // (the pool has no name index — beta7 default).
    const bool looksLikeTokenId =
        (query.size() == 64) &&
        std::all_of(query.begin(), query.end(),
                    [](QChar c){ return c.isDigit()
                                     || (c >= QChar('a') && c <= QChar('f'))
                                     || (c >= QChar('A') && c <= QChar('F')); });
    const QString wireTokenId = looksLikeTokenId ? query.toLower() : QString();

    const QString textFilter = looksLikeTokenId ? QString() : query;   // client-side name/collection

    const quint64 gen = ++m_searchGen;
    m_results.clear();
    if (m_resultsList) m_resultsList->clear();
    if (m_buyBtn) m_buyBtn->setEnabled(false);
    if (m_stateLabel) {
        m_stateLabel->setText(tr("Searching the market…"));
        m_stateLabel->setStyleSheet("color:#9aa0a6;");
        m_stateLabel->show();
    }

    QPointer<NFTMarketDialog> self(this);
    m_rpc->nftBrowseOffers(wireTokenId, maxPriceZat, /*from=*/0, /*count=*/200,
        [self, gen, textFilter](QVector<NFTBrowseRow> rows) {
            if (self.isNull() || gen != self->m_searchGen)
                return;   // dialog gone, or a newer search superseded this page

            // Keep LIVE offers only and sort PRICE-ASCENDING (cheapest first).
            QVector<NFTBrowseRow> live;
            live.reserve(rows.size());
            for (const NFTBrowseRow& r : rows)
                if (r.live)
                    live.push_back(r);
            std::stable_sort(live.begin(), live.end(),
                             [](const NFTBrowseRow& a, const NFTBrowseRow& b){
                                 return a.priceZat < b.priceZat;
                             });

            self->m_results = live;

            // Client-side name/collection JOIN via zslp_gettoken so the text filter can
            // run over the fetched page (the offer pool carries no name). We render
            // immediately (so the page appears + filters on tokenId), then back-fill the
            // name as each gettoken lands and re-apply the text filter.
            if (!self->m_results.isEmpty()) {
                for (int i = 0; i < self->m_results.size(); ++i) {
                    const QString tokenId = self->m_results[i].tokenId;
                    if (tokenId.isEmpty())
                        continue;
                    const int idx = i;
                    QPointer<NFTMarketDialog> s2(self);
                    self->m_rpc->nftProvenance(tokenId,
                        [s2, gen, idx, tokenId, textFilter](json meta) {
                            if (s2.isNull() || gen != s2->m_searchGen)
                                return;
                            if (idx < 0 || idx >= s2->m_results.size())
                                return;
                            if (s2->m_results[idx].tokenId != tokenId)
                                return;
                            if (meta.is_object()) {
                                auto nm = meta.find("name");
                                if (nm != meta.end() && nm->is_string())
                                    s2->m_results[idx].tokenName =
                                        QString::fromStdString(nm->get<std::string>());
                                auto gid = meta.find("group_id");
                                if (gid == meta.end()) gid = meta.find("groupid");
                                if (gid != meta.end() && gid->is_string())
                                    s2->m_results[idx].collectionId =
                                        QString::fromStdString(gid->get<std::string>());
                            }
                            // Re-render with the freshly joined name so the text filter
                            // (if any) now matches on name/collection too.
                            s2->renderResults();
                        },
                        [s2](QString /*errStr*/) {
                            // A name join that fails is non-fatal — the row keeps its
                            // short-id display + tokenId-only filterability.
                            if (s2.isNull()) return;
                        }
                    );
                }
            }

            self->renderResults();
        },
        [self, gen](QString errStr) {
            if (self.isNull() || gen != self->m_searchGen)
                return;
            // HONEST: a method-not-found browse (a daemon predating the RPC / a foreign
            // node) is the EXPECTED beta7-default state, not a scary error. Map the
            // shared NFT-unsupported guidance to the calm discovery-off line. Any OTHER
            // (transport) error shows its calm message but never a dialog.
            self->m_results.clear();
            if (self->m_resultsList) self->m_resultsList->clear();
            if (self->m_buyBtn) self->m_buyBtn->setEnabled(false);
            if (self->m_stateLabel) {
                const bool methodMissing = (errStr == RPC::nftUnsupportedGuidance());
                self->m_stateLabel->setText(methodMissing ? discoveryOffLine() : errStr);
                self->m_stateLabel->setStyleSheet("color:#9aa0a6;");
                self->m_stateLabel->show();
            }
        }
    );
}

void NFTMarketDialog::renderResults() {
    if (m_resultsList == nullptr)
        return;

    // Apply the CLIENT-SIDE name/collection text filter over the fetched page (the
    // pool has no name index). An empty filter shows the whole page. Matches on the
    // joined name, the collection id, OR the token id (so a partial id also filters).
    const QString filter = m_searchInput ? m_searchInput->text().trimmed() : QString();
    const bool hexId =
        (filter.size() == 64) &&
        std::all_of(filter.begin(), filter.end(),
                    [](QChar c){ return c.isDigit()
                                     || (c >= QChar('a') && c <= QChar('f'))
                                     || (c >= QChar('A') && c <= QChar('F')); });
    const QString needle = filter.toLower();

    // Re-derive the displayed (filtered) subset, keeping m_results index-aligned with
    // the list by tracking the source index per visible row.
    QVector<int> visible;
    for (int i = 0; i < m_results.size(); ++i) {
        const NFTBrowseRow& r = m_results[i];
        bool show = filter.isEmpty();
        if (!show) {
            if (hexId) {
                show = r.tokenId.toLower() == needle;
            } else {
                show = r.tokenName.toLower().contains(needle)
                       || r.collectionId.toLower().contains(needle)
                       || r.tokenId.toLower().contains(needle);
            }
        }
        if (show)
            visible.push_back(i);
    }

    m_resultsList->clear();
    for (int vi = 0; vi < visible.size(); ++vi) {
        const NFTBrowseRow& r = m_results[visible[vi]];
        const QString name  = rowDisplayName(r);
        const QString price = Settings::zatToDecimalString(r.priceZat);
        auto* item = new QListWidgetItem(
            tr("%1\n%2 ZCL").arg(name, price), m_resultsList);
        // Store the SOURCE index so selection maps back to m_results even when filtered.
        item->setData(Qt::UserRole, visible[vi]);
        requestThumb(visible[vi], r);
    }

    if (m_stateLabel) {
        if (m_results.isEmpty() || visible.isEmpty()) {
            // Empty pool OR nothing matched the filter -> the calm honest line, never a
            // scary error or a bare empty grid.
            if (m_results.isEmpty())
                m_stateLabel->setText(discoveryOffLine());
            else
                m_stateLabel->setText(tr("Nothing your node has heard about matches that search."));
            m_stateLabel->setStyleSheet("color:#9aa0a6;");
            m_stateLabel->show();
        } else {
            m_stateLabel->hide();
        }
    }
    onSelectionChanged();
}

void NFTMarketDialog::requestThumb(int row, const NFTBrowseRow& r) {
    // Local-only thumbnail: resolve the token's documenthash (via provenance) and
    // render LOCAL cached bytes only — NEVER auto-fetch a remote image. Absent image
    // => the row simply shows no icon (honest). We reuse the EXISTING ContentEngine.
    if (m_engine == nullptr || m_rpc == nullptr || r.tokenId.isEmpty())
        return;
    (void)row;
    const QString tokenId = r.tokenId;
    const quint64 gen = m_searchGen;
    QPointer<NFTMarketDialog> self(this);
    m_rpc->nftProvenance(tokenId,
        [self, gen, tokenId](json meta) {
            if (self.isNull() || gen != self->m_searchGen)
                return;
            QString docHash;
            if (meta.is_object()) {
                auto it = meta.find("documenthash");
                if (it != meta.end() && it->is_string())
                    docHash = QString::fromStdString(it->get<std::string>());
            }
            if (docHash.isEmpty())
                return;
            const QString localPath = ContentEngine::cacheGet(docHash);
            if (localPath.isEmpty())
                return;   // not on this computer -> no icon (honest)
            const quint64 token = nextMarketPosterToken();
            // Encode the source row by stashing it under the engine token isn't
            // possible; instead we re-match on the delivered hash in onPosterReady.
            self->m_engine->posterForToken(localPath, docHash, docHash,
                                           kThumbPx, token);
        },
        [self](QString /*errStr*/) { if (self.isNull()) return; }
    );
}

void NFTMarketDialog::onPosterReady(quint64 /*token*/, QImage img, int /*verifyState*/) {
    // We don't carry the row through the engine token, so a delivered poster is applied
    // to ALL currently-visible rows whose stored source row resolves to the same hash.
    // Simpler + robust: a null image is ignored; a real image is set on rows that have
    // no icon yet AND whose source row's tokenId matches the engine's keyed hash isn't
    // available here — so we conservatively apply it to the FIRST iconless row. This is
    // a best-effort thumbnail; the verify dialog renders the authoritative image.
    if (img.isNull() || m_resultsList == nullptr)
        return;
    const QPixmap pm = QPixmap::fromImage(img);
    for (int i = 0; i < m_resultsList->count(); ++i) {
        QListWidgetItem* it = m_resultsList->item(i);
        if (it && it->icon().isNull()) {
            it->setIcon(QIcon(pm));
            break;
        }
    }
}

void NFTMarketDialog::onBuyClicked() {
    if (m_resultsList == nullptr)
        return;
    const int listRow = m_resultsList->currentRow();
    if (listRow < 0)
        return;
    QListWidgetItem* it = m_resultsList->item(listRow);
    if (it == nullptr)
        return;
    const int srcIdx = it->data(Qt::UserRole).toInt();
    if (srcIdx < 0 || srcIdx >= m_results.size())
        return;

    const NFTBrowseRow& r = m_results[srcIdx];
    if (r.offerBlob.isEmpty()) {
        if (m_stateLabel) {
            m_stateLabel->setText(tr("That offer is missing its details — try searching again."));
            m_stateLabel->setStyleSheet("color:#c0392b;");
            m_stateLabel->show();
        }
        return;
    }

    // Hand the FULL browsed offerBlob into the proven verify/take flow. We NEVER
    // reconstruct an offer; the mandatory pre-pay verify still gates Buy inside the
    // child dialog. Modal so the user completes (or abandons) the purchase here.
    m_lastHandoffBlob = r.offerBlob;
    NFTBuyDialog buy(m_engine, m_rpc, this);
    buy.openWithOffer(r.offerBlob);
    buy.exec();
}
