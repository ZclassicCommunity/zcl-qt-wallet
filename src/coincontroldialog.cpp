#include "coincontroldialog.h"
#include "ui_coincontroldialog.h"
#include "rpc.h"
#include "settings.h"
#include "balancestablemodel.h"

#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QCheckBox>

// Convert a <=8-decimal-place ZCL amount string to integer zatoshis. Mirrors
// sendtab.cpp's toZat(): the UnspentOutput amount is the daemon's own FormatMoney
// decimal string, so llround is exact within MAX_MONEY (no half-zatoshi tie). Money is
// summed in integer zatoshis so the coverage readout never drifts by IEEE-754 dust.
static inline qint64 ccToZat(const QString& amountStr) {
    return static_cast<qint64>(llround(amountStr.toDouble() * 1e8));
}

// Pool label for the Type column + the SelectedInput.type the daemon needs. Derived
// from the address (the wallet's own classifier) so a row's type is single-sourced.
static QString ccTypeOf(const QString& addr) {
    if (Settings::isTAddress(addr))                          return "transparent";
    if (Settings::getInstance()->isSaplingAddress(addr))     return "sapling";
    if (Settings::getInstance()->isSproutAddress(addr))      return "sprout";
    // Fallback: a z-address we couldn't finely classify. Treat as sapling for display
    // only (selection of such a row stays gated behind allowShielded). Never affects
    // money math — the typed-inputs builder keys off this string but the daemon
    // validates the coordinates regardless.
    return Settings::isZAddress(addr) ? "sapling" : "transparent";
}

CoinControlDialog::CoinControlDialog(QWidget* parent, RPC* rpc, const QString& fromAddr,
                                     qint64 targetZat, bool allowShielded,
                                     const QList<SelectedInput>& initial)
    : parent(parent), rpc(rpc), fromAddr(fromAddr), targetZat(targetZat),
      allowShielded(allowShielded), initial(initial) {}

// Column layout for the inputs table.
enum CcCol { CC_CHECK = 0, CC_ADDRESS, CC_AMOUNT, CC_CONFS, CC_TYPE, CC_CHANGE, CC_NCOLS };

bool CoinControlDialog::exec() {
    Ui_CoinControlDialog cc;
    QDialog dialog(parent);
    cc.setupUi(&dialog);
    Settings::saveRestore(&dialog);

    QTableWidget* tbl = cc.inputsTable;
    tbl->setColumnCount(CC_NCOLS);
    QStringList headers;
    headers << QObject::tr("Use") << QObject::tr("Address") << QObject::tr("Amount")
            << QObject::tr("Confirmations") << QObject::tr("Type") << QObject::tr("Change?");
    tbl->setHorizontalHeaderLabels(headers);
    tbl->verticalHeader()->setVisible(false);
    tbl->horizontalHeader()->setSectionResizeMode(CC_ADDRESS, QHeaderView::Stretch);

    // Build the candidate row list = the wallet's CONFIRMED, spendable UTXOs/notes for
    // the chosen from-address. We mirror the send guards' confirmed (confirmations > 0)
    // + spendable filter so the dialog can never offer an input the daemon would reject.
    // We hold a parallel UnspentOutput list so we can read back each checked row's exact
    // coordinates + integer-zatoshi value (the row carries an index into it).
    QList<UnspentOutput> rows;
    if (rpc && rpc->getUTXOs()) {
        for (const UnspentOutput& u : *rpc->getUTXOs()) {
            if (u.address == fromAddr && u.confirmations > 0 && u.spendable)
                rows.append(u);
        }
    }

    tbl->setRowCount(rows.size());

    // Pre-checked set: anything in `initial` (a re-open) whose coordinates still match a
    // live row. We compare on (txid, type, index, jsindex) — the exact key the daemon
    // pins on — so a since-spent input simply drops out of the restored selection.
    auto isInitiallyChecked = [&](const UnspentOutput& u, const QString& type, int index, int jsindex) -> bool {
        for (const SelectedInput& s : initial) {
            if (s.txid == u.txid && s.type == type && s.index == index && s.jsindex == jsindex)
                return true;
        }
        return false;
    };

    // The checkbox lives inside a centered cell-widget wrapper, so reading it back is
    // always cellWidget()->findChild<QCheckBox*>() (NOT a direct cast of cellWidget()).
    auto fnCellCheck = [tbl](int r) -> QCheckBox* {
        QWidget* w = tbl->cellWidget(r, CC_CHECK);
        return w ? w->findChild<QCheckBox*>() : nullptr;
    };

    // running coverage readout: re-summed from the checkboxes on every toggle.
    auto fnUpdateSummary = [=]() {
        qint64 sumZat = 0;
        for (int r = 0; r < tbl->rowCount(); r++) {
            auto* chk = fnCellCheck(r);
            if (chk && chk->isChecked()) {
                // amountZat is stashed on the checkbox via a dynamic property so the
                // sum reads the SAME integer zatoshi value parsed at build time (no
                // re-parse drift).
                sumZat += chk->property("amountZat").toLongLong();
            }
        }

        QString sumStr = Settings::getZCLDisplayFormat((double)sumZat / 1e8);
        if (sumZat == 0) {
            cc.lblSummary->setText(QObject::tr("No inputs selected — the wallet will choose for you."));
            cc.lblSummary->setStyleSheet("");
        } else if (targetZat <= 0) {
            // No known send target yet (amount/fee not entered). Show the sum only.
            cc.lblSummary->setText(QObject::tr("Selected: %1").arg(sumStr));
            cc.lblSummary->setStyleSheet("");
        } else {
            QString tgtStr = Settings::getZCLDisplayFormat((double)targetZat / 1e8);
            if (sumZat >= targetZat) {
                cc.lblSummary->setText(QObject::tr("Selected: %1  ✓ covers %2 (amount + fee)")
                                       .arg(sumStr).arg(tgtStr));
                cc.lblSummary->setStyleSheet("color: green;");
            } else {
                cc.lblSummary->setText(QObject::tr("Selected: %1  — NOT enough for %2 (amount + fee)")
                                       .arg(sumStr).arg(tgtStr));
                cc.lblSummary->setStyleSheet("color: red;");
            }
        }
    };

    for (int r = 0; r < rows.size(); r++) {
        const UnspentOutput& u = rows[r];
        const QString type = ccTypeOf(u.address);
        const bool isShielded = (type != "transparent");

        // Resolve the daemon coordinates per pool (see SelectedInput / processUnspent).
        int index   = -1;
        int jsindex = -1;
        if (type == "transparent")  { index = u.vout; }
        else if (type == "sapling") { index = u.outindex; }
        else /* sprout */           { index = u.jsoutindex; jsindex = u.jsindex; }

        const qint64 amtZat = ccToZat(u.amount);

        // Col 0: the checkbox, hosted in a centered cell widget. Shielded rows are
        // checkable ONLY when allowShielded (the Advanced manual-note opt-in); otherwise
        // the row is shown for VISIBILITY but disabled (auto-selection stays privacy-
        // optimized). The integer-zatoshi value rides as a dynamic property so the
        // coverage sum never re-parses.
        auto* chk = new QCheckBox();
        chk->setProperty("amountZat", (qlonglong)amtZat);
        bool checkable = !isShielded || allowShielded;
        chk->setEnabled(checkable);
        chk->setChecked(checkable && isInitiallyChecked(u, type, index, jsindex));
        if (isShielded && !allowShielded)
            chk->setToolTip(QObject::tr("Turn on \"manual shielded note selection\" in "
                                        "Settings to pick individual shielded notes. "
                                        "Auto-selection is privacy-optimized."));
        QObject::connect(chk, &QCheckBox::toggled, [=](bool) { fnUpdateSummary(); });

        auto* cellW = new QWidget();
        auto* cellL = new QHBoxLayout(cellW);
        cellL->addWidget(chk);
        cellL->setAlignment(Qt::AlignCenter);
        cellL->setContentsMargins(0, 0, 0, 0);
        tbl->setCellWidget(r, CC_CHECK, cellW);

        auto fnReadOnlyItem = [](const QString& text) {
            auto* it = new QTableWidgetItem(text);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);   // display-only
            return it;
        };

        tbl->setItem(r, CC_ADDRESS, fnReadOnlyItem(u.address));
        auto* amtItem = fnReadOnlyItem(Settings::getZCLDisplayFormat(u.amount.toDouble()));
        amtItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tbl->setItem(r, CC_AMOUNT, amtItem);
        auto* confItem = fnReadOnlyItem(QString::number(u.confirmations));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tbl->setItem(r, CC_CONFS, confItem);
        tbl->setItem(r, CC_TYPE, fnReadOnlyItem(type));
        tbl->setItem(r, CC_CHANGE, fnReadOnlyItem(u.change ? QObject::tr("yes") : QString()));
    }

    tbl->resizeColumnsToContents();
    tbl->horizontalHeader()->setSectionResizeMode(CC_ADDRESS, QHeaderView::Stretch);
    fnUpdateSummary();

    if (dialog.exec() != QDialog::Accepted)
        return false;   // Cancel/close: caller leaves the prior selection untouched.

    // Collect the checked rows into the result selection (integer-zatoshi values copied
    // verbatim from the rows so the send guards size against the exact same numbers).
    result.clear();
    for (int r = 0; r < rows.size(); r++) {
        auto* chk = fnCellCheck(r);
        if (!chk || !chk->isChecked())
            continue;
        const UnspentOutput& u = rows[r];
        const QString type = ccTypeOf(u.address);
        int index   = -1;
        int jsindex = -1;
        if (type == "transparent")  { index = u.vout; }
        else if (type == "sapling") { index = u.outindex; }
        else /* sprout */           { index = u.jsoutindex; jsindex = u.jsindex; }

        result.append(SelectedInput{ type, u.txid, index, jsindex, ccToZat(u.amount) });
    }
    return true;
}
