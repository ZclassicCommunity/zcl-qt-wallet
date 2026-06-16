// ============================================================================
// NFTListingsDialog implementation — see nftlistingsdialog.h. Journey 3: view
// the sell offers I created + cancel an open one. Read (nft_listoffers) + write
// (nft_canceloffer) only; v1 has NO re-price RPC.
// ============================================================================
#include "nftlistingsdialog.h"
#include "rpc.h"
#include "settings.h"        // zatToDecimalString (the canonical ZCL price formatter)
#include "guiutil.h"         // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QMessageBox>
#include <QPointer>

#include <algorithm>

namespace {
// An offer is OPEN (cancellable) only when its status is the literal "open".
// Everything else (filled / expired / canceled) is terminal: it sorts below the
// open rows and can never be cancelled.
bool isOpenStatus(const QString& status) {
    return status == QStringLiteral("open");
}
}  // namespace

NFTListingsDialog::NFTListingsDialog(RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("My listings"));
    setMinimumWidth(560);
    setMinimumHeight(420);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    auto* heading = new QLabel(tr("Your listings"), this);
    heading->setObjectName("nftListingsHeading");
    heading->setStyleSheet("font-weight:600; font-size:14pt;");
    outer->addWidget(heading);

    auto* sub = new QLabel(
        tr("Offers you created to sell a collectible. Cancelling an open listing "
           "frees the collectible to send or sell again."), this);
    sub->setObjectName("nftListingsSubhead");
    sub->setWordWrap(true);
    sub->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(sub);

    // The honest empty state — shown INSTEAD of the (empty) table when the store
    // has no sell offers (or the index is off and we got nothing back).
    m_emptyLabel = new QLabel(tr("You have no listings yet."), this);
    m_emptyLabel->setObjectName("nftListingsEmptyLabel");
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet("color:#9aa0a6;");
    m_emptyLabel->hide();
    outer->addWidget(m_emptyLabel);

    // --- the table ----------------------------------------------------------
    m_table = new QTableWidget(this);
    m_table->setObjectName("nftListingsTable");
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({
        tr("Collectible"), tr("Price (ZCL)"), tr("Status"), tr("Expires at block")
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    outer->addWidget(m_table, 1);

    // Calm loading / result / index-off honesty line.
    m_statusLine = new QLabel(tr("Loading your listings…"), this);
    m_statusLine->setObjectName("nftListingsStatusLine");
    m_statusLine->setWordWrap(true);
    m_statusLine->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(m_statusLine);

    // --- action bar ---------------------------------------------------------
    auto* btnRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton(tr("Refresh"), this);
    m_refreshBtn->setObjectName("nftListingsRefreshButton");
    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName("nftListingsCloseButton");
    m_cancelBtn = new QPushButton(tr("Cancel listing"), this);
    m_cancelBtn->setObjectName("nftListingsCancelButton");
    m_cancelBtn->setEnabled(false);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(m_cancelBtn);
    outer->addLayout(btnRow);

    connect(closeBtn,     &QPushButton::clicked, this, &QDialog::reject);
    connect(m_refreshBtn, &QPushButton::clicked, this, &NFTListingsDialog::refresh);
    connect(m_cancelBtn,  &QPushButton::clicked, this, &NFTListingsDialog::onCancelSelected);
    connect(m_table,      &QTableWidget::itemSelectionChanged,
            this, &NFTListingsDialog::onSelectionChanged);

    makeLabelsSelectable(this);                                     // copyable text (incl. errors)
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });   // no clipped button labels

    refresh();   // pull on open
}

QString NFTListingsDialog::selectedOfferId() const {
    if (!m_table) return QString();
    const int row = m_table->currentRow();
    if (row < 0) return QString();
    QTableWidgetItem* first = m_table->item(row, 0);
    return first ? first->data(Qt::UserRole).toString() : QString();
}

void NFTListingsDialog::onSelectionChanged() {
    // Cancel is enabled ONLY when the selected row is still "open" (and no RPC is
    // in flight). A terminal (filled/expired/canceled) row cannot be cancelled.
    if (!m_table || !m_cancelBtn) return;
    const int row = m_table->currentRow();
    bool open = false;
    if (row >= 0) {
        QTableWidgetItem* statusItem = m_table->item(row, 2);
        open = statusItem && isOpenStatus(statusItem->data(Qt::UserRole).toString());
    }
    m_cancelBtn->setEnabled(open && !isInFlight());
}

void NFTListingsDialog::refresh() {
    if (m_rpc == nullptr) {
        if (m_statusLine) {
            m_statusLine->setText(tr("Not connected to your node yet."));
            m_statusLine->setStyleSheet("color:#9aa0a6;");
        }
        return;
    }
    if (m_statusLine) {
        m_statusLine->setText(tr("Loading your listings…"));
        m_statusLine->setStyleSheet("color:#9aa0a6;");
    }

    QPointer<NFTListingsDialog> self(this);
    m_rpc->nftListOffers(
        [self](QVector<NFTOfferRow> rows) {
            if (self.isNull())
                return;
            self->populate(rows);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            // Index-off / not-connected etc.: a CALM line, never a crash. We leave
            // any rows we already had in place and don't wipe the table.
            if (self->m_statusLine) {
                self->m_statusLine->setText(errStr);
                self->m_statusLine->setStyleSheet("color:#9aa0a6;");
            }
        }
    );
}

void NFTListingsDialog::populate(const QVector<NFTOfferRow>& rows) {
    // Filter to SELL offers only. The role is the LITERAL daemon string "sell"
    // (nftoffer.cpp) — NOT "seller" (the daemon help text saying "seller|buyer" is
    // wrong). A buy row would never appear in the local store, but we filter
    // defensively so a stray non-sell record can never show up as a listing.
    QVector<NFTOfferRow> sells;
    sells.reserve(rows.size());
    for (const NFTOfferRow& r : rows) {
        if (r.role == QStringLiteral("sell"))
            sells.push_back(r);
    }

    // Sort open rows above terminal ones (stable on the rest).
    std::stable_sort(sells.begin(), sells.end(),
        [](const NFTOfferRow& a, const NFTOfferRow& b) {
            return isOpenStatus(a.status) && !isOpenStatus(b.status);
        });

    if (m_table) {
        m_table->clearContents();
        m_table->setRowCount(sells.size());
        for (int i = 0; i < sells.size(); ++i) {
            const NFTOfferRow& r = sells.at(i);

            // Col 0 — Collectible (a short tokenId). The full offerId is stashed in
            // the row's first cell UserRole for the cancel path; the short tokenId
            // is just the display.
            const QString shortTok = r.tokenId.left(12)
                                    + (r.tokenId.size() > 12 ? QStringLiteral("…") : QString());
            auto* tokItem = new QTableWidgetItem(shortTok);
            tokItem->setData(Qt::UserRole, r.offerId);   // the cancel target
            tokItem->setToolTip(r.tokenId);
            m_table->setItem(i, 0, tokItem);

            // Col 1 — Price (ZCL = priceZat / 1e8), via the canonical formatter.
            auto* priceItem = new QTableWidgetItem(Settings::zatToDecimalString(r.priceZat));
            m_table->setItem(i, 1, priceItem);

            // Col 2 — Status. The raw status is stashed in UserRole so the cancel
            // gate matches on the exact daemon string regardless of display casing.
            auto* statusItem = new QTableWidgetItem(r.status);
            statusItem->setData(Qt::UserRole, r.status);
            m_table->setItem(i, 2, statusItem);

            // Col 3 — Expires at block N (blank when 0 / unknown).
            auto* expItem = new QTableWidgetItem(
                r.expiryHeight > 0 ? QString::number(r.expiryHeight) : QString());
            m_table->setItem(i, 3, expItem);
        }
    }

    const bool empty = sells.isEmpty();
    if (m_emptyLabel) m_emptyLabel->setVisible(empty);
    if (m_table)      m_table->setVisible(!empty);
    if (m_statusLine) {
        if (empty) {
            // The empty label carries the story; clear the loading line.
            m_statusLine->clear();
        } else {
            m_statusLine->setText(tr("%n listing(s).", "", sells.size()));
            m_statusLine->setStyleSheet("color:#9aa0a6;");
        }
    }
    onSelectionChanged();   // re-gate Cancel (selection cleared by repopulation)
}

void NFTListingsDialog::onCancelSelected() {
    if (isInFlight() || m_rpc == nullptr || !m_cancelBtn)
        return;
    const QString offerId = selectedOfferId();
    if (offerId.isEmpty())
        return;

    const auto r = QMessageBox::question(
        this, tr("Cancel this listing?"),
        tr("This voids the listing and frees your collectible. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes)
        return;

    // Shared scaffold: latch in-flight + relabel Cancel -> "Cancelling…" (also
    // disables Refresh so the user can't re-pull mid-cancel; the [X] is swallowed
    // by NftAsyncDialog while in flight).
    beginPrimary(m_cancelBtn, tr("Cancelling…"), m_refreshBtn);

    QPointer<NFTListingsDialog> self(this);
    m_rpc->nftCancelOffer(offerId,
        [self](QString /*txid*/) {
            if (self.isNull())
                return;
            // Latch off + restore the buttons, then re-pull the list (the cancelled
            // row will come back as "canceled" / terminal). refresh() re-gates Cancel.
            self->setInFlight(false);
            if (self->m_cancelBtn) {
                self->m_cancelBtn->setText(self->tr("Cancel listing"));
                self->m_cancelBtn->setEnabled(false);   // re-enabled by the refresh's selection re-gate
            }
            if (self->m_refreshBtn)
                self->m_refreshBtn->setEnabled(true);
            self->refresh();
            if (self->m_statusLine) {
                self->m_statusLine->setText(self->tr("Listing cancelled."));
                self->m_statusLine->setStyleSheet("color:#34c759;");
            }
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            // The listing still stands — restore the Cancel affordance + a red line.
            self->setInFlight(false);
            if (self->m_cancelBtn)
                self->m_cancelBtn->setText(self->tr("Cancel listing"));
            if (self->m_refreshBtn)
                self->m_refreshBtn->setEnabled(true);
            self->onSelectionChanged();   // re-gate Cancel on the current selection
            if (self->m_statusLine) {
                self->m_statusLine->setText(errStr);
                self->m_statusLine->setStyleSheet("color:#c0392b;");
            }
        }
    );
}
