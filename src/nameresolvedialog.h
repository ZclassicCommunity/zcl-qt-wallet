// ============================================================================
// NameResolveDialog — native (no-browser) "Look up a name" view (NAMES pillar /
// ZNAM). Programmatic modal QDialog (no .ui file). READ-ONLY: type a name ->
// RPC::nameResolve -> name_resolve. Renders owner / status / primary target +
// every record[] / text[] row into read-only, selectable labels.
//
// HONESTY: a null resolve (the name is not registered) is rendered as the calm
// "available / not registered" state — never a scary error. Coin is ZCL.
// LIFETIME: read-only (no write RPC, so no in-flight latch), but the async reply
// lambdas are QPointer-guarded so a fast close is a safe no-op. Derives from
// NftAsyncDialog for the shared [X] behavior + style parity.
// C++14 only (empty-QString sentinels, no std::optional / std::string_view).
// ============================================================================
#ifndef NAMERESOLVEDIALOG_H
#define NAMERESOLVEDIALOG_H

#include "precompiled.h"
#include "nftasyncdialog.h"
#include "namescommon.h"   // ResolvedName by value

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class RPC;

class NameResolveDialog : public NftAsyncDialog {
    Q_OBJECT
public:
    // `prefill` (optional) seeds + immediately looks up a name (e.g. a row the user
    // double-clicked in the My-Names list). Empty => the user types one.
    explicit NameResolveDialog(RPC* rpc, const QString& prefill = QString(),
                               QWidget* parent = nullptr);

private slots:
    void onLookupClicked();

private:
    void clearResults();
    void renderFound(const ResolvedName& r);
    void renderAvailable(const QString& name);

    RPC*         m_rpc        = nullptr;

    QLineEdit*   m_nameEdit   = nullptr;
    QPushButton* m_lookupBtn  = nullptr;
    QPushButton* m_closeBtn   = nullptr;
    QLabel*      m_stateLine  = nullptr;   // "looking up…" / "available" / error
    QWidget*     m_resultsBox = nullptr;   // holds the rendered record rows
    QVBoxLayout* m_resultsLayout = nullptr;
};

#endif // NAMERESOLVEDIALOG_H
