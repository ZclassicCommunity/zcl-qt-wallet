// ============================================================================
// NftAsyncDialog — shared base for every NFT dialog that fires a single async
// RPC and must survive its reply (mint / send / sell / buy). Centralizes the
// ONE load-bearing safety pattern that was copy-pasted across all of them:
//
//   * an in-flight latch (m_inFlight) so a second click / close is a no-op,
//   * a helper to relabel + disable the primary button while in flight,
//   * the success terminal state: relabel the primary button to "Done",
//     disconnect its old clicked() handler and re-wire it to accept(),
//   * the failure terminal state: relabel the primary button to "Try again",
//   * a closeEvent() that SWALLOWS the window [X] while a request is in flight.
//
// WHY THIS EXISTS (do not delete the guard): a real Use-After-Free shipped once
// when a dialog was torn down by the [X] while its RPC reply was still pending.
// The true safety net is the QPointer<T> guard each subclass wraps around its
// reply lambdas (so a destroyed dialog is a safe no-op); this base makes the
// [X]-swallow + in-flight latch impossible to forget, so no future dialog can
// re-introduce that crash by copy-paste drift.
//
// C++14 only (CONFIG += c++14): no std::optional / std::string_view. The base
// owns NO widgets — subclasses still build their own layout and pass the primary
// button into the helpers below. Subclasses keep their own QPointer-guarded
// lambdas (the guard cannot be hoisted into a non-template base without erasing
// the concrete type), but they call setInFlight()/beginPrimary*/finishPrimary*
// so the behavior stays identical and single-sourced.
// ============================================================================
#ifndef NFTASYNCDIALOG_H
#define NFTASYNCDIALOG_H

#include "precompiled.h"

#include <QDialog>
#include <QString>

class QPushButton;
class QCloseEvent;

class NftAsyncDialog : public QDialog {
    Q_OBJECT
public:
    explicit NftAsyncDialog(QWidget* parent = nullptr) : QDialog(parent) {}

    bool isInFlight() const { return m_inFlight; }

protected:
    // Arm/disarm the in-flight latch. While armed, the window [X] is swallowed
    // (closeEvent) so the dialog can't be destroyed under a pending reply.
    void setInFlight(bool on) { m_inFlight = on; }

    // Enter the in-flight state for a primary action: latch on, relabel the
    // button to a "…ing" gerund, disable it, and disable an optional secondary
    // (Cancel/Close) button so the user can't bail mid-flight (also closes the
    // UAF window the [X] swallow protects). Call from the click handler right
    // before issuing the RPC.
    void beginPrimary(QPushButton* primary, const QString& busyText,
                      QPushButton* secondary = nullptr);

    // SUCCESS terminal state: latch off, retire the primary button into a
    // terminal "Done" that dismisses the dialog (disconnect its old clicked()
    // and re-wire it to accept()), and keep the secondary disabled so ONLY Done
    // dismisses the acknowledged result. The visible confirmation copy is the
    // subclass's job (it must read before this returns / after).
    void finishPrimaryAsDone(QPushButton* primary, QPushButton* secondary = nullptr);

    // FAILURE terminal state: latch off, relabel the primary button to
    // "Try again" (the subclass re-gates its enabled state) and re-enable the
    // optional secondary so the user can back out. The honest daemon error
    // message is rendered by the subclass into its own result line.
    void finishPrimaryAsRetry(QPushButton* primary, QPushButton* secondary = nullptr);

    // The single, shared [X]-swallow. Subclasses that need no extra close logic
    // can rely on this override directly; if a subclass overrides closeEvent it
    // MUST early-return on isInFlight() (call this or replicate the ignore()).
    void closeEvent(QCloseEvent* e) override;

private:
    bool m_inFlight = false;
};

#endif // NFTASYNCDIALOG_H
