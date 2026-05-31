#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "precompiled.h"
#include "logger.h"

#include <QProgressBar>
#include <QElapsedTimer>

// Forward declare to break circular dependency.
class RPC;
class Settings;
class WSServer;
class WormholeClient;
class QSystemTrayIcon;

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

    QString doSendTxValidations(Tx tx);
    void setDefaultPayFrom();

    void replaceWormholeClient(WormholeClient* newClient);
    bool isWebsocketListening();
    void createWebsocket(QString wormholecode);
    void stopWebsocket();

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

    void setupTurnstileDialog();
    void setupSettingsModal();
    void setupStatusBar();
    void setupSyncBanner();

    // P0-6: state used to estimate a sync ETA from observed block rate.
    QElapsedTimer       syncEtaTimer;
    bool                syncEtaStarted = false;
    int                 syncEtaStartBlock = 0;

    void removeExtraAddresses();

    Tx   createTxFromSendPage();
    bool confirmTx(Tx tx);

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
    
    void donate();
    void addressBook();
    void postToZBoard();
    void importPrivKey();
    void exportAllKeys();
    void exportKeys(QString addr = "");
    void backupWalletDat();
    void exportTransactions();

    // P0-2: first-run fund-safety prompt. Nags (once per launch) to back up
    // wallet.dat until the user has backed up, then never nags again.
    void promptWalletBackup();

    void doImport(QList<QString>* keys);

    void restoreSavedStates();
    bool eventFilter(QObject *object, QEvent *event);

    bool            uiPaymentsReady    = false;
    QString         pendingURIPayment;

    WSServer*       wsserver = nullptr;
    WormholeClient* wormhole = nullptr;

    RPC*         rpc  = nullptr;
    QCompleter*  labelCompleter = nullptr;

    QMovie*      loadingMovie;
};

#endif // MAINWINDOW_H
