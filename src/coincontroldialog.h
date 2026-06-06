#ifndef COINCONTROLDIALOG_H
#define COINCONTROLDIALOG_H

#include "precompiled.h"
#include "mainwindow.h"        // SelectedInput

class RPC;

// COIN CONTROL — modal dialog that lets the user pick EXACTLY which already-valid
// inputs a payment may spend. It is purely a SELECTION restriction over the wallet's
// own UTXO/note set (RPC::getUTXOs(), populated from the daemon's listunspent +
// z_listunspent): it NEVER alters tx/note format, fees, change routing, dust, coinbase,
// turnstile, or any consensus rule. Modeled on the inline memoDialog pattern in
// sendtab.cpp — constructed on the stack, exec()'d, and the resulting selection read
// back via selection().
//
// Money-safety contract:
//   * An EMPTY returned selection means "auto-select" — identical to today's behavior.
//   * Each returned SelectedInput carries only the daemon coordinates (type/txid/
//     index/jsindex) + the input's confirmed value in INTEGER ZATOSHIS, copied straight
//     from the UnspentOutput; the dialog does no money arithmetic beyond summing those
//     integer zatoshis for the live coverage readout.
//   * Shielded-note rows are display-only (and unchecked) unless the caller passes
//     allowShieldedSelection=true (gated behind an Advanced opt-in, with a one-time
//     privacy warning surfaced by the caller before this dialog opens).
class CoinControlDialog {
public:
    // fromAddr  : restrict the listed inputs to this Send-tab "from" address.
    // targetZat : recipients + fee in integer zatoshis (the coverage bar compares the
    //             checked sum against this). 0 => no target known yet (no coverage verdict).
    // allowShielded : enable the checkboxes on shielded (sapling/sprout) rows.
    // initial   : the selection already on the Tx (re-opening preserves prior choices).
    CoinControlDialog(QWidget* parent, RPC* rpc, const QString& fromAddr,
                      qint64 targetZat, bool allowShielded,
                      const QList<SelectedInput>& initial);

    // Run the modal. Returns true iff the user accepted (OK); false on Cancel/close —
    // the caller then leaves tx.selectedInputs untouched.
    bool exec();

    // The user's chosen inputs after an accepted exec(). EMPTY => auto-select.
    QList<SelectedInput> selection() const { return result; }

private:
    QWidget* parent;
    RPC*     rpc;
    QString  fromAddr;
    qint64   targetZat;
    bool     allowShielded;
    QList<SelectedInput> initial;
    QList<SelectedInput> result;
};

#endif // COINCONTROLDIALOG_H
