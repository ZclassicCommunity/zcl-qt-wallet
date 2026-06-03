#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "precompiled.h"
#include "logger.h"

#include <QProgressBar>
#include <QElapsedTimer>

// Forward declare to break circular dependency.
class RPC;
class Settings;
class QSystemTrayIcon;
class QButtonGroup;
class QPushButton;

using json = nlohmann::json;

// Struct used to hold destination info when sending a Tx. 
struct ToFields {
    QString addr;
    double  amount;
    QString txtMemo;
    QString encodedMemo;
};

// Struct used to represent a Transaction. 
struct Tx {
    QString         fromAddr;
    QList<ToFields> toAddrs;
    double          fee;
};

namespace Ui {
    class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void updateLabelsAutoComplete();
    RPC* getRPC() { return rpc; }

#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAM (L1 widget tests). Compiled in ONLY when ZCL_WIDGET_TEST is
    // defined (tests/widget/tst_widget.pro). NEVER present in the shipped app
    // build. Exposes the private confirmTx() so the E2 de-shield-warning test can
    // drive the real confirm dialog, and seeds an empty RPC balances map so
    // confirmTx's rpc->getAllBalances() deref is safe without a live daemon.
    bool testConfirmTx(Tx tx)  { return confirmTx(tx); }
    void testSeedBalances();
#endif

    QString doSendTxValidations(Tx tx);
    void setDefaultPayFrom();

    void balancesReady();
    void payZClassicURI(QString uri = "");

    void updateLabels();
    void updateTAddrCombo(bool checked);
    void updateFromCombo();

    Ui::MainWindow*     ui;

    QLabel*             statusLabel;
    QLabel*             statusIcon;
    QLabel*             loadingLabel;
    QWidget*            zclassicdtab;

    // P0-6: prominent sync banner on the Balance/main tab. Called by rpc.cpp
    // from the getblockchaininfo poll (setSyncStatus) and from noConnection
    // (setSyncStatusConnecting) so a non-technical user always knows the state.
    QWidget*            syncBanner       = nullptr;
    QLabel*             syncStatusLabel  = nullptr;
    QProgressBar*       syncProgressBar  = nullptr;

    void setSyncStatus(bool isSyncing, int blockNumber, int estimatedHeight, double progress);
    void setSyncStatusConnecting();

    // Phase-3b redesign (Quiet+): the modern Home DASHBOARD on the Balance page.
    // updateHomeFixIt() is called from rpc.cpp's refreshBalances() (and the cached-
    // balance restore) on every balance update, mirroring the existing balSheilded/
    // balTransparent/balTotal label-update pattern. It:
    //   * refreshes the privacy-forward HERO (the big "Private balance" number,
    //     read back from balSheilded, with the Total shown secondary), and
    //   * shows/hides the amber "Shield public funds" FIX-IT card: visible ONLY when
    //     transparent > 0, hidden when transparent == 0. The caller already has the
    //     transparent amount (balT) and passes it here rather than re-querying.
    // Public so the RPC poller can call it. No-op until setupHomeDashboard() has run.
    void updateHomeFixIt(double transparent);
    // longStretch=true after a LONG peerless stretch (~3min): adds a stronger
    // "check your internet / try again later" hint (SELF-HEAL SYNCED-ZERO-PEERS).
    void setSyncStatusWaitingForPeers(bool longStretch = false);
    // Bootstrap snapshot download in progress (getbootstrapinfo phase==active): show a
    // determinate "Downloading blockchain snapshot — X%" (with GB + MB/s when known)
    // instead of the misleading "waiting for peers" -- the node is actively
    // downloading, not stuck (it reports 0 normal P2P peers during this phase).
    void setSyncStatusBootstrapSnapshot(int pct, qint64 received, qint64 total, double mbps);

    // SELF-HEAL (B-side) runtime validation surfacing. Non-blocking informational
    // banner reusing the existing syncBanner widget; empty string clears it. Set
    // from the getblockchaininfo poll when the daemon reports bootstrap_validation.
    void setBootstrapValidationBanner(const QString& msg);

    // Non-modal user notification: a system-tray balloon when the tray icon is
    // available (e.g. while running hidden in the background), otherwise a status-
    // bar message. Used so a backgrounded node-crash recovery can inform the user
    // without popping a modal dialog over a hidden window.
    void notify(const QString& title, const QString& body);

    // RUNTIME STUB AUTO-HEAL entry, called from the post-connection sync poller in
    // rpc.cpp ONLY when ALL of its conservative triggers hold (sustained 0 peers AND
    // a stub-sized blocks/ dir AND not already auto-healed this run/cooldown). It is
    // the runtime analogue of Help -> Repair: it reuses the SAME launch path
    // (launchBlockchainRepair -> startManualRepair -> redownloadChain), so every
    // existing safety guard (daemon-dead confirm, wallet.dat backup, disk floor,
    // reversible set-aside) applies. Non-interactive but surfaces a clear status.
    void autoHealStubChain();

    // P0-2 FUND-SAFETY: prompt the user (once per run) to back up wallet.dat. There
    // is no seed phrase for this wallet, so an un-backed-up wallet.dat is permanent,
    // unrecoverable loss. Invoked from rpc.cpp's balance refresh on the first fully-
    // synced poll with a positive balance (a per-run one-shot there avoids re-firing
    // on every poll). Permanently silenced after a successful backup via the
    // persisted options/walletbackedup flag. Public so the RPC poller can call it.
    void promptWalletBackup();

    // RUNTIME actionable dialog (edit #5). Reached from the sync poller (rpc.cpp) when an
    // ATTACHED FOREIGN node (one this wallet did NOT start: rpc->isEmbedded()==false) has
    // been peerless/stuck for a sustained window. The wallet can only download/repair the
    // chain for a node IT starts, so it cannot heal the foreign node — instead it shows a
    // CLEAR, ACTIONABLE, PERSISTENT, RETRYABLE message centred on STUCK (not "too old"):
    // stop the other node (systemctl --user stop zclassicd, or close the other ZClassic),
    // then reopen this wallet so it manages its own node. Primary relaunches the app; Quit
    // exits; an OPTIONAL "Use the bundled node" appears only when the ports are actually
    // free (re-checked on click). NEVER kills/mutates the foreign node; NEVER a silent hang.
    void showForeignNodeStuck();

    // Sibling of the above for the connect-time CATCH-ALL (edit #2): the node answered
    // neither cleanly nor with a recognized warmup state for too long. Actionable +
    // retryable; Retry re-runs the connect flow (a fresh ConnectionLoader/doAutoConnect)
    // rather than leaving the connect dialog frozen forever.
    void showNodeNotRespondingRetry();

    Logger*      logger;

    void doClose();

    // Opt-in tray-resident mode (Settings -> Options -> "Keep running in the
    // background"). showFromTray() un-hides+raises the window (also used when a
    // second launch hands off to this instance); quitApp() performs the real,
    // clean shutdown (vs. closeEvent hiding to tray). applyTraySetting() wires
    // the tray icon + app-exit semantics to the current setting.
    void showFromTray();
    void quitApp();
    void applyTraySetting(bool enabled);

private:
    void closeEvent(QCloseEvent* event);

    void setupTrayIcon();
    QSystemTrayIcon* trayIcon       = nullptr;
    bool             quitting       = false;
    bool             trayHintShown  = false;

    void setupSendTab();
    void setupTransactionsTab();
    void setupRecieveTab();
    void setupBalancesTab();
    void setupZClassicdTab();

    void setupSettingsModal();
    void setupStatusBar();
    void setupSyncBanner();

    // Phase-3a redesign (Quiet+): a modern left vertical NAV RAIL of large
    // checkable buttons that drive ui->tabWidget->setCurrentIndex(). Built
    // PROGRAMMATICALLY (no .ui structural change): the QTabWidget and all its
    // pages/objectNames are kept intact underneath; only its tabBar is hidden,
    // so every page stays index-selectable (L0/L1 tests untouched).
    void setupNavRail();
    QButtonGroup* navRailGroup = nullptr;

    // Phase-3b redesign (Quiet+): build the Home DASHBOARD programmatically on the
    // existing Balance page (no .ui change). Adds, above the existing Summary rows:
    // a privacy-forward HERO ("Private balance" big + Total secondary), two large
    // quick-action buttons (Send/Receive -> setCurrentIndex), and a hidden amber
    // "Shield public funds" fix-it card surfaced by updateHomeFixIt() when t>0.
    void setupHomeDashboard();
    QLabel*      homeHeroPrivate   = nullptr;   // big private (shielded) number
    QLabel*      homeHeroTotal     = nullptr;   // secondary "Total NN ZCL"
    QFrame*      homeFixItCard     = nullptr;   // amber card (hidden unless t>0)
    QLabel*      homeFixItText     = nullptr;   // "X ZCL is PUBLIC ..."

    // SINGLE destructive launch path shared by the Help -> Repair action and the
    // runtime stub auto-heal: stop the embedded node, then drive a fresh
    // ConnectionLoader through startManualRepair() (the staged re-download ladder).
    void launchBlockchainRepair();

    // P0-6: state used to estimate a sync ETA from observed block rate.
    QElapsedTimer       syncEtaTimer;
    bool                syncEtaStarted = false;
    int                 syncEtaStartBlock = 0;

    void removeExtraAddresses();

    Tx   createTxFromSendPage();
    bool confirmTx(Tx tx);

    void setupTurnstileDialog();
    void turnstileDoMigration(QString fromAddr = "");
    void turnstileProgress();

    void cancelButton();
    void sendButton();
    void inputComboTextChanged(int index);
    void addAddressSection();
    void maxAmountChecked(int checked);

    void editSchedule();

    void addressChanged(int number, const QString& text);
    void amountChanged (int number, const QString& text);

    void addNewZaddr(bool sapling);
    std::function<void(bool)> addZAddrsToComboList(bool sapling);

    void memoButtonClicked(int number, bool includeReplyTo = false);
    void setMemoEnabled(int number, bool enabled);
    
    void addressBook();
    void importPrivKey();
    void exportAllKeys();
    void exportKeys(QString addr = "");
    void backupWalletDat();
    void exportTransactions();

    void doImport(QList<QString>* keys);

    void restoreSavedStates();
    bool eventFilter(QObject *object, QEvent *event);

    bool            uiPaymentsReady    = false;
    QString         pendingURIPayment;

    RPC*         rpc  = nullptr;
    QCompleter*  labelCompleter = nullptr;

    QMovie*      loadingMovie;
};

#endif // MAINWINDOW_H
