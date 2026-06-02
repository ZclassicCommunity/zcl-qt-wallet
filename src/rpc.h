#ifndef RPCCLIENT_H
#define RPCCLIENT_H

#include "precompiled.h"

#include "balancestablemodel.h"
#include "txtablemodel.h"
#include "ui_mainwindow.h"
#include "mainwindow.h"
#include "connection.h"

using json = nlohmann::json;

class Turnstile;

struct TransactionItem {
    QString         type;
    qint64            datetime;
    QString         address;
    QString         txid;
    double          amount;
    unsigned long   confirmations;
    QString         fromAddr;
    QString         memo;
};

struct WatchedTx {
    QString opid;
    Tx tx;
    std::function<void(QString, QString)> completed;
    std::function<void(QString, QString)> error;
};

class RPC
{
public:
    RPC(MainWindow* main);
    ~RPC();

    void setConnection(Connection* c);
    void setEZClassicd(QProcess* p);
    const QProcess* getEZClassicD() { return ezclassicd; }

    void refresh(bool force = false);

    void refreshAddresses();    
    
    void checkForUpdate(bool silent = true);
    void refreshZCLPrice();

    void executeTransaction(Tx tx, 
        const std::function<void(QString opid)> submitted,
        const std::function<void(QString opid, QString txid)> computed,
        const std::function<void(QString opid, QString errStr)> error);

    void fillTxJsonParams(json& params, Tx tx);
    void sendZTransaction(json params, const std::function<void(json)>& cb, const std::function<void(QString)>& err);
    void watchTxStatus();

    const QMap<QString, WatchedTx> getWatchingTxns() { return watchingOps; }
    void addNewTxToWatch(const QString& newOpid, WatchedTx wtx); 

    const TxTableModel*               getTransactionsModel() { return transactionsTableModel; }
    const QList<QString>*             getAllZAddresses()     { return zaddresses; }
    const QList<QString>*             getAllTAddresses()     { return taddresses; }
    const QList<UnspentOutput>*       getUTXOs()             { return utxos; }
    const QMap<QString, double>*      getAllBalances()       { return allBalances; }
    const QMap<QString, bool>*        getUsedAddresses()     { return usedAddresses; }

    void newZaddr(bool sapling, const std::function<void(json)>& cb);
    void newTaddr(const std::function<void(json)>& cb);

    void getZPrivKey(QString addr, const std::function<void(json)>& cb);
    void getTPrivKey(QString addr, const std::function<void(json)>& cb);
    void importZPrivKey(QString addr, bool rescan, const std::function<void(json)>& cb);
    void importTPrivKey(QString addr, bool rescan, const std::function<void(json)>& cb);

    void shutdownZClassicd();

    // SAFETY GATE for the manual Help -> Repair path: positively confirm the
    // RPC-owned embedded node is NOT running before any datadir mutation. Polls
    // state(); if still alive, terminate() + waitForFinished, then escalate to
    // kill() + waitForFinished. Returns true only when confirmed NotRunning (or
    // there is no embedded node / it's an external daemon we don't own). The 30s
    // soft cap in shutdownZClassicd() means "quit promptly", NOT "confirmed
    // exited", so the repair path must call this before touching blocks/chainstate.
    bool confirmEmbeddedStopped();

    void noConnection();
    bool isEmbedded() { return ezclassicd != nullptr; }

    QString getDefaultSaplingAddress();
    QString getDefaultTAddress();

    void getAllPrivKeys(const std::function<void(QList<QPair<QString, QString>>)>);

    Turnstile*  getTurnstile()  { return turnstile; }
    Connection* getConnection() { return conn; }

private:
    void refreshBalances();

    void refreshTransactions();    
    void refreshSentZTrans();
    void refreshReceivedZTrans(QList<QString> zaddresses);

    bool processUnspent     (const json& reply, QMap<QString, double>* newBalances, QList<UnspentOutput>* newUtxos);
    void updateUI           (bool anyUnconfirmed);

    void getInfoThenRefresh(bool force);

    // RUNTIME STUB AUTO-HEAL: evaluate (conservatively) whether the live node is a
    // healthy-but-stuck STUB — it started fine, loaded a tiny non-genesis chain from an
    // aborted P2P sync, and now finds 0 peers — and, if so, route into the SAME
    // redownloadChain() ladder the manual Help -> Repair uses (via
    // MainWindow::autoHealStubChain). Called from the getblockchaininfo sync poll only
    // when (isSyncing && connections==0 && ezNoPeerPolls>=3). Fires ONLY when ALL hold:
    //   (1) connections == 0 sustained well past the banner (ezNoPeerPolls >= 12, ~60s
    //       at the 5s syncing cadence);
    //   (2) the on-disk blocks/ store is unambiguously a STUB — its total size is below
    //       a hard 1 GiB floor (a synced chain is ~10 GiB; a stub is tens of MB),
    //       measured via ConnectionLoader::blocksDirSizeBytes from the active datadir.
    //       A large blocks/ (a fully-synced node that is merely OFFLINE) is NEVER
    //       wiped — that is the dangerous lookalike;
    //   (3) it has NOT already auto-redownloaded this run (ezStubAutoHealTried) AND the
    //       persisted per-install cooldown (rpc/stubHeal.count, capped) is not exhausted,
    //       so a genuinely peerless environment can't loop wipe->rebootstrap->wipe.
    // Never fires for an external daemon we don't own, or when blocks/ can't be measured.
    // Returns true ONLY if it actually launched a heal (the caller then suppresses the
    // peerless banner for this tick).
    bool maybeAutoHealStubChain(int connections);

    // Once-per-process latch so the runtime stub auto-heal launches at most one
    // re-download per app run; combined with the persisted cooldown below.
    bool                        ezStubAutoHealTried         = false;
    // Once-per-session latch for clearing the persisted per-install stub-heal cap on
    // the first peered poll (so we don't rewrite QSettings every tick).
    bool                        ezStubHealCooldownCleared   = false;

    void getBalance(const std::function<void(json)>& cb);

    void getTransparentUnspent  (const std::function<void(json)>& cb);
    void getZUnspent            (const std::function<void(json)>& cb);
    void getTransactions        (const std::function<void(json)>& cb);
    void getZAddresses          (const std::function<void(json)>& cb);
    void getTAddresses          (const std::function<void(json)>& cb);

    // Runtime daemon-crash recovery: catch the embedded node dying at runtime
    // (OOM/crash) and offer to restart it instead of stranding the user in a
    // permanent "No Connection". ezExpectedShutdown gates the normal-shutdown
    // path; ezRestartCount caps automatic restarts.
    void handleEZClassicdCrash(int exitCode, QProcess::ExitStatus status);
    void restartEmbeddedZClassicd();   // relaunch the dead node, stripping repair flags
    bool                        ezExpectedShutdown          = false;
    bool                        ezCrashDialogOpen           = false;
    int                         ezRestartCount              = 0;
    QMetaObject::Connection     ezCrashConn;

    // Re-entrancy guard for shutdownZClassicd(): the shutdown wait spins a nested
    // event loop (and the "please wait" dialog is non-modal for the first 700ms),
    // so the main window stays live. This latch makes a second Quit / window-close
    // during that window a no-op instead of stacking a second nested loop. It
    // serializes re-entrancy WITHIN one shutdown only: it is cleared at every exit
    // of shutdownZClassicd() and when setEZClassicd() adopts a fresh live node, so
    // it never latches for the process lifetime (a later restart can shut down too).
    bool                        ezShuttingDown              = false;

    // C2/F5: once we've legitimately confirmed the chain tip (blocks>=headers with
    // peers) this latch keeps us "synced" through brief peer drops so an established
    // wallet doesn't flicker back to "syncing".
    bool                        ezEverSynced                = false;

    // C9/F6: debounce the waiting-for-peers banner; a single connections==0 sample
    // shouldn't flip the banner. Counts consecutive peerless polls; reset on a peer.
    int                         ezNoPeerPolls               = 0;

    // Edit #6 (bob-fix): once-per-run guard for MainWindow::showForeignNodeStuck(). The
    // poller fires that actionable dialog EXACTLY ONCE when an ATTACHED FOREIGN node
    // (ezclassicd == nullptr — NOT one we launched) has been peerless/stuck for a
    // sustained window. An OWNED embedded node (it self-heals + has bootstrap peers) and a
    // FOREIGN node that has peers / is syncing/synced NEVER trigger it. This bool keeps it
    // from re-showing every poll.
    bool                        ezForeignStuckShown         = false;

    // G6: the warmup-wedge cap (heal/attempts.warmupRestart) must only be cleared by
    // SUSTAINED health, never by one getinfo — otherwise a node that answers a single
    // getinfo and then re-wedges would reset the counter forever and never escalate
    // to NEEDS_MANUAL. clearHealLedger() (one getinfo) intentionally leaves
    // warmupRestart alone; we clear it here only after several consecutive healthy
    // sync polls OR after the chain tip has demonstrably advanced past warmup.
    int                         ezHealthyPolls              = 0;
    bool                        ezWarmupWedgeCleared        = false;

    Connection*                 conn                        = nullptr;
    QProcess*                   ezclassicd                     = nullptr;

    QList<UnspentOutput>*       utxos                       = nullptr;
    QMap<QString, double>*      allBalances                 = nullptr;
    QMap<QString, bool>*        usedAddresses               = nullptr;
    QList<QString>*             zaddresses                  = nullptr;
    QList<QString>*             taddresses                  = nullptr;
    
    QMap<QString, WatchedTx>    watchingOps;

    TxTableModel*               transactionsTableModel      = nullptr;
    BalancesTableModel*         balancesTableModel          = nullptr;

    QTimer*                     timer;
    QTimer*                     txTimer;
    QTimer*                     priceTimer;

    Ui::MainWindow*             ui;
    MainWindow*                 main;
    Turnstile*                  turnstile;

    // Current balance in the UI. If this number updates, then refresh the UI
    QString                     currentBalance;
};

#endif // RPCCLIENT_H
