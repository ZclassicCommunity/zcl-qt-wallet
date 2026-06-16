// ============================================================================
// NftAsyncDialog implementation — see nftasyncdialog.h. The whole point is that
// the in-flight latch + [X]-swallow + Done/Try-again terminal transitions live
// in ONE place so no NFT dialog can drift away from the proven UAF-safe pattern.
// ============================================================================
#include "nftasyncdialog.h"

#include <QPushButton>
#include <QCloseEvent>

void NftAsyncDialog::beginPrimary(QPushButton* primary, const QString& busyText,
                                  QPushButton* secondary) {
    m_inFlight = true;
    if (primary) {
        primary->setText(busyText);
        primary->setEnabled(false);
    }
    if (secondary)
        secondary->setEnabled(false);   // can't bail mid-flight (closes the UAF window)
}

void NftAsyncDialog::finishPrimaryAsDone(QPushButton* primary, QPushButton* secondary) {
    m_inFlight = false;
    if (primary) {
        // Retire the primary action into a terminal "Done" dismiss: drop the old
        // clicked() wiring and re-point it at accept() so the user explicitly
        // dismisses the acknowledged result (the visible success copy stays put).
        primary->setText(tr("Done"));
        primary->setEnabled(true);
        primary->disconnect(SIGNAL(clicked()));
        connect(primary, &QPushButton::clicked, this, &QDialog::accept);
    }
    if (secondary)
        secondary->setEnabled(false);   // only Done dismisses now
}

void NftAsyncDialog::finishPrimaryAsRetry(QPushButton* primary, QPushButton* secondary) {
    m_inFlight = false;
    if (primary)
        primary->setText(tr("Try again"));   // subclass re-gates enabled state
    if (secondary)
        secondary->setEnabled(true);
}

void NftAsyncDialog::closeEvent(QCloseEvent* e) {
    // Swallow the window [X] while a request is in flight so the dialog can't be
    // destroyed under the in-flight reply. The subclass's QPointer guard is the
    // true safety net; this is the honest UX (the button reads "…ing").
    if (m_inFlight) {
        e->ignore();
        return;
    }
    QDialog::closeEvent(e);
}
