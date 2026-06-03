#include "mainwindow.h"
#include "addressbook.h"
#include "ui_mainwindow.h"
#include <QProgressBar>
#include <QElapsedTimer>
#include "ui_addressbook.h"
#include "ui_privkey.h"
#include "ui_about.h"
#include "ui_settings.h"
#include "ui_turnstile.h"
#include "ui_turnstileprogress.h"
#include "rpc.h"
#include "balancestablemodel.h"
#include "privacybadgedelegate.h"
#include "settings.h"
#include "version.h"
#include "turnstile.h"
#include "senttxstore.h"
#include "connection.h"

#include <QSystemTrayIcon>
#include <QCloseEvent>
#include <QtNetwork/QTcpSocket>
#include <QFrame>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabBar>
#include <QSignalBlocker>

using json = nlohmann::json;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    logger = new Logger(this, QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("zcl-qt-wallet.log"));

    // SELF-HEAL: a heal/inProgress flag seen at PROCESS STARTUP is necessarily stale —
    // it's a persisted QSettings flag that a prior session set just before a deferred
    // relaunch, and that session (and its live ConnectionLoader) is gone. Left set, it
    // would make every handleStartupFailure() in this run bail at the in-progress
    // guard and strand the modal connect dialog. Clear it ONCE here, before the first
    // ConnectionLoader is created, so a fresh run can classify+heal normally. This
    // runs before rpc = new RPC(this) below (whose loadConnection() is deferred).
    QSettings().setValue("heal/inProgress", false);

    // Status Bar
    setupStatusBar();
    
    // Settings editor 
    setupSettingsModal();

    // Set up exit action. Use quitApp() (not close()) so that in tray-resident
    // mode File -> Exit really quits and cleanly stops the node, rather than
    // just hiding the window to the tray.
    QObject::connect(ui->actionExit, &QAction::triggered, this, &MainWindow::quitApp);

    // Set up check for updates action
    QObject::connect(ui->actionCheck_for_Updates, &QAction::triggered, [=] () {
        // Silent is false, so show notification even if no update was found
        rpc->checkForUpdate(false);
    });

    // Pay ZClassic URI
    QObject::connect(ui->actionPay_URI, &QAction::triggered, [=] () {
        payZClassicURI();
    });

    // Import Private Key
    QObject::connect(ui->actionImport_Private_Key, &QAction::triggered, this, &MainWindow::importPrivKey);

    // Export All Private Keys
    QObject::connect(ui->actionExport_All_Private_Keys, &QAction::triggered, this, &MainWindow::exportAllKeys);

    // Backup wallet.dat
    QObject::connect(ui->actionBackup_wallet_dat, &QAction::triggered, this, &MainWindow::backupWalletDat);

    // Export transactions
    QObject::connect(ui->actionExport_transactions, &QAction::triggered, this, &MainWindow::exportTransactions);

    // Address Book
    QObject::connect(ui->action_Address_Book, &QAction::triggered, this, &MainWindow::addressBook);

    // Set up about action
    QObject::connect(ui->actionAbout, &QAction::triggered, [=] () {
        QDialog aboutDialog(this);
        Ui_about about;
        about.setupUi(&aboutDialog);
        Settings::saveRestore(&aboutDialog);

        QString version    = QString("Version ") % QString(APP_VERSION) % " (" % QString(__DATE__) % ")";
        about.versionLabel->setText(version);

        aboutDialog.exec();
    });

    // SELF-HEAL: always-reachable manual override. The .ui file is checked in and not
    // regenerated, so the action is created in code and added to the existing Help
    // menu. It is present even after auto-heal latches NEEDS_MANUAL, so the user is
    // never permanently stuck. It cleanly stops the running node first, then spins up
    // a fresh ConnectionLoader and invokes the SAME staged repair ladder used at
    // startup (offerCorruptionRepair) — there is no second ladder.
    {
        auto* repairAction = new QAction(tr("Repair / Re-download blockchain…"), this);
        ui->menuHelp->addSeparator();
        ui->menuHelp->addAction(repairAction);
        QObject::connect(repairAction, &QAction::triggered, [=]() {
            auto confirm = QMessageBox::question(this, tr("Repair blockchain data"),
                tr("This will stop your ZClassic node so its blockchain files can be repaired "
                   "or re-downloaded.\n\n"
                   "Your wallet and coins are safe — a backup copy of your wallet is made first, "
                   "and none of these options touch your wallet or private keys.\n\nContinue?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (confirm != QMessageBox::Yes)
                return;

            // Drive the SAME stop-node + fresh-loader + staged-ladder flow the runtime
            // stub auto-heal uses; there is exactly one launch path (launchBlockchainRepair).
            launchBlockchainRepair();
        });
    }

    // Initialize to the balances tab
    ui->tabWidget->setCurrentIndex(0);

    // The zclassicd tab is hidden by default, and only later added in if the embedded zclassicd is started
    zclassicdtab = ui->tabWidget->widget(4);
    ui->tabWidget->removeTab(4);

    setupSendTab();
    setupTransactionsTab();
    setupRecieveTab();
    setupBalancesTab();
    setupTurnstileDialog();
    setupZClassicdTab();

    // Phase-3a redesign (Quiet+): replace the top tab bar with a modern left
    // nav rail. Built here, AFTER the tab pages are set up and the zclassicd tab
    // has been removed (above), so the rail's primary destinations map to the
    // live tab indices (Home/Balance 0, Send 1, Receive 2, Activity 3).
    setupNavRail();

    rpc = new RPC(this);

    restoreSavedStates();

    // Opt-in tray-resident mode: set up the tray icon + app-exit semantics to
    // match the saved preference (default OFF -> no tray, quit-on-close as before).
    applyTraySetting(Settings::getInstance()->getKeepInTray());
}

void MainWindow::restoreSavedStates() {
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());

    ui->balancesTable->horizontalHeader()->restoreState(s.value("baltablegeometry").toByteArray());
    ui->transactionsTable->horizontalHeader()->restoreState(s.value("tratablegeometry").toByteArray());

    // Paint the last-known balances immediately so the first ~1-minute node
    // warmup shows a usable view instead of a blank "0.00". The live RPC
    // overwrites these the moment it connects. Gated on its own default-ON
    // preference (independent of saved-tx-history) so every user gets the instant
    // paint; the live values are authoritative, this is only a convenience cache.
    if (Settings::getInstance()->getShowCachedBalance() && s.contains("cache/balTotal")) {
        double balT   = s.value("cache/balTransparent", 0.0).toDouble();
        double balZ   = s.value("cache/balShielded",    0.0).toDouble();
        double balTot = s.value("cache/balTotal",       0.0).toDouble();

        ui->balTransparent->setText(Settings::getZCLDisplayFormat(balT));
        ui->balSheilded   ->setText(Settings::getZCLDisplayFormat(balZ));
        ui->balTotal      ->setText(Settings::getZCLDisplayFormat(balTot));

        qint64 epoch = s.value("cache/lastSyncEpoch", (qint64) 0).toLongLong();
        if (epoch > 0) {
            QString when = QDateTime::fromSecsSinceEpoch(epoch).toString("MMM d, h:mm AP");
            QString tip  = tr("Last known balance — last updated %1 (refreshing…)").arg(when);
            ui->balTransparent->setToolTip(tip);
            ui->balSheilded   ->setToolTip(tip);
            ui->balTotal      ->setToolTip(tip);
        }
    }
}

void MainWindow::doClose() {
    // doClose() is the "really quit" path (e.g. an OS signal handler), so make
    // sure closeEvent performs the real shutdown rather than hiding to the tray.
    quitting = true;
    closeEvent(nullptr);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s;

    s.setValue("geometry", saveGeometry());
    s.setValue("baltablegeometry", ui->balancesTable->horizontalHeader()->saveState());
    s.setValue("tratablegeometry", ui->transactionsTable->horizontalHeader()->saveState());

    s.sync();

    // Tray-resident mode: closing the window just hides it (the node keeps
    // running so the next open is instant). Only an explicit Quit / File->Exit
    // (which sets `quitting`) tears the node down. Never silently hide if the
    // system tray is unavailable -- fall through to a real shutdown so the node
    // is never orphaned with no way to bring the window back.
    if (!quitting
            && Settings::getInstance()->getKeepInTray()
            && trayIcon != nullptr
            && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        if (event)
            event->ignore();

        if (!trayHintShown) {
            trayHintShown = true;
            trayIcon->showMessage(tr("ZclWallet is still running"),
                tr("ZclWallet keeps running in the background so it opens instantly next time. "
                   "Right-click the tray icon (or use File > Exit) to quit completely."),
                QSystemTrayIcon::Information, 5000);
        }
        return;
    }

    // Real shutdown. Let the RPC know to shut down any running service.
    rpc->shutdownZClassicd();

    // Bubble up
    if (event)
        QMainWindow::closeEvent(event);

    // quit-on-last-window-closed is now disabled for the WHOLE GUI lifetime (set
    // in main(); never re-enabled by applyTraySetting), so closing the window no
    // longer ends the app by itself. We only reach here on a REAL shutdown (the
    // tray-hide branch returned early above), i.e. the user/OS is intentionally
    // closing the window after rpc->shutdownZClassicd() -- quitting is the correct
    // outcome. Quit explicitly and UNCONDITIONALLY (not just in tray mode) so the
    // window's X button, File->Exit, the reindex/rescan close, and every
    // quitApp()-driven path still quit cleanly with the flag permanently false.
    qApp->quit();
}

void MainWindow::showFromTray() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitApp() {
    // The real, clean exit: mark quitting so closeEvent stops the node instead
    // of hiding to the tray, then close (which triggers the shutdown sequence).
    quitting = true;
    close();
}

void MainWindow::setupTrayIcon() {
    if (trayIcon != nullptr)
        return;

    trayIcon = new QSystemTrayIcon(QIcon(":/icons/res/icon.ico"), this);
    trayIcon->setToolTip("ZclWallet");

    auto trayMenu = new QMenu(this);
    auto showAction = trayMenu->addAction(tr("Show ZclWallet"));
    QObject::connect(showAction, &QAction::triggered, this, &MainWindow::showFromTray);
    trayMenu->addSeparator();
    auto quitAction = trayMenu->addAction(tr("Quit"));
    QObject::connect(quitAction, &QAction::triggered, this, &MainWindow::quitApp);
    trayIcon->setContextMenu(trayMenu);

    // A left-click / double-click on the tray icon brings the window back.
    QObject::connect(trayIcon, &QSystemTrayIcon::activated, this,
        [=](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                showFromTray();
        });
}

void MainWindow::applyTraySetting(bool enabled) {
    if (enabled && QSystemTrayIcon::isSystemTrayAvailable()) {
        setupTrayIcon();
        trayIcon->show();
        // The window-close should hide to tray, not end the app.
        qApp->setQuitOnLastWindowClosed(false);
    } else {
        if (trayIcon != nullptr)
            trayIcon->hide();
        // NEVER-STRAND invariant: do NOT re-enable quit-on-last-window-closed here.
        // main() disables it for the whole GUI lifetime so that no orphaned modal
        // dialog teardown (connect dialog, connect-error box, node-crash box,
        // foreign-stuck box) can silently end the process before the main window
        // maps -- bob's exact bug. Previously this branch flipped the flag back to
        // true whenever keep-in-tray was OFF (the default), which RE-ARMED that
        // trap the first time the user clicked Settings->OK. The legitimate quit on
        // a real window close is now provided unconditionally by
        // MainWindow::closeEvent() (qApp->quit()), so leaving the flag false costs
        // nothing and closes the regression.
        //
        // Intentionally a no-op on the flag in both tray-off and headless cases.
    }
}

void MainWindow::turnstileProgress() {
    Ui_TurnstileProgress progress;
    QDialog d(this);
    progress.setupUi(&d);
    Settings::saveRestore(&d);

    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    progress.msgIcon->setPixmap(icon.pixmap(64, 64));

    bool migrationFinished = false;
    auto fnUpdateProgressUI = [=, &migrationFinished] () mutable {
        // Get the plan progress
        if (rpc->getTurnstile()->isMigrationPresent()) {
            auto curProgress = rpc->getTurnstile()->getPlanProgress();
            
            progress.progressTxt->setText(QString::number(curProgress.step) % QString(" / ") % QString::number(curProgress.totalSteps));
            progress.progressBar->setValue(100 * curProgress.step / curProgress.totalSteps);
            
            auto nextTxBlock = curProgress.nextBlock - Settings::getInstance()->getBlockNumber();
            
            progress.fromAddr->setText(curProgress.from);
            progress.toAddr->setText(curProgress.to);

            if (curProgress.step == curProgress.totalSteps) {
                migrationFinished = true;
                auto txt = QString("Turnstile migration finished");
                if (curProgress.hasErrors) {
                    txt = txt + ". There were some errors.\n\nYour funds are all in your wallet, so you should be able to finish moving them manually.";
                }
                progress.nextTx->setText(txt);
            } else {
                progress.nextTx->setText(QString("Next transaction in ") 
                                    % QString::number(nextTxBlock < 0 ? 0 : nextTxBlock)
                                    % " blocks via " % curProgress.via % "\n" 
                                    % (nextTxBlock <= 0 ? "(waiting for confirmations)" : ""));
            }
            
        } else {
            progress.progressTxt->setText("");
            progress.progressBar->setValue(0);
            progress.nextTx->setText("No turnstile migration is in progress");
        }
    };

    QTimer progressTimer(this);        
    QObject::connect(&progressTimer, &QTimer::timeout, fnUpdateProgressUI);
    progressTimer.start(Settings::updateSpeed);
    fnUpdateProgressUI();
    
    auto curProgress = rpc->getTurnstile()->getPlanProgress();

    // Abort button
    if (curProgress.step != curProgress.totalSteps)
        progress.buttonBox->button(QDialogButtonBox::Discard)->setText("Abort");
    else
        progress.buttonBox->button(QDialogButtonBox::Discard)->setVisible(false);

    // Abort button clicked
    QObject::connect(progress.buttonBox->button(QDialogButtonBox::Discard), &QPushButton::clicked, [&] () {
        if (curProgress.step != curProgress.totalSteps) {
            auto abort = QMessageBox::warning(this, "Are you sure you want to Abort?",
                                    "Are you sure you want to abort the migration?\nAll further transactions will be cancelled.\nAll your funds are still in your wallet.",
                                    QMessageBox::Yes, QMessageBox::No);
            if (abort == QMessageBox::Yes) {
                rpc->getTurnstile()->removeFile();
                d.close();
                ui->statusBar->showMessage("Automatic Sapling turnstile migration aborted.");
            }
        }
    });

    d.exec();    
    if (migrationFinished || curProgress.step == curProgress.totalSteps) {
        // Finished, so delete the file
        rpc->getTurnstile()->removeFile();
    }    
}

void MainWindow::turnstileDoMigration(QString fromAddr) {
    // Return if there is no connection
    if (rpc->getAllZAddresses() == nullptr)
        return;

    // If a migration is already in progress, show the progress dialog instead
    if (rpc->getTurnstile()->isMigrationPresent()) {
        turnstileProgress();
        return;
    }

    Ui_Turnstile turnstile;
    QDialog d(this);
    turnstile.setupUi(&d);
    Settings::saveRestore(&d);

    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
    turnstile.msgIcon->setPixmap(icon.pixmap(64, 64));

    auto fnGetAllSproutBalance = [=] () {
        double bal = 0;
        for (auto addr : *rpc->getAllZAddresses()) {
            if (Settings::getInstance()->isSproutAddress(addr)) {
                bal += rpc->getAllBalances()->value(addr);
            }
        }

        return bal;
    };

    turnstile.fromBalance->setText(Settings::getZCLUSDDisplayFormat(fnGetAllSproutBalance()));
    for (auto addr : *rpc->getAllZAddresses()) {
        auto bal = rpc->getAllBalances()->value(addr);
        if (Settings::getInstance()->isSaplingAddress(addr)) {
            turnstile.migrateTo->addItem(addr, bal);
        } else {
            turnstile.migrateZaddList->addItem(addr, bal);
        }
    }

    auto fnUpdateSproutBalance = [=] (QString addr) {
        double bal = 0;

        // The currentText contains the balance as well, so strip that.
        if (addr.contains("(")) {
            addr = addr.left(addr.indexOf("("));
        }

        if (addr.startsWith("All")) {
            bal = fnGetAllSproutBalance();
        } else {
            bal = rpc->getAllBalances()->value(addr);
        }
        
        auto balTxt = Settings::getZCLUSDDisplayFormat(bal);
        
        if (bal < Turnstile::minMigrationAmount) {
            turnstile.fromBalance->setStyleSheet("color: red;");
            turnstile.fromBalance->setText(balTxt % " [You need at least " 
                        % Settings::getZCLDisplayFormat(Turnstile::minMigrationAmount)
                        % " for automatic migration]");
            turnstile.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        } else {
            turnstile.fromBalance->setStyleSheet("");
            turnstile.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
            turnstile.fromBalance->setText(balTxt);
        }
    };

    if (!fromAddr.isEmpty())
        turnstile.migrateZaddList->setCurrentText(fromAddr);

    fnUpdateSproutBalance(turnstile.migrateZaddList->currentText());    

    // Combo box selection event
    QObject::connect(turnstile.migrateZaddList, &QComboBox::currentTextChanged, fnUpdateSproutBalance);
        
    // Privacy level combobox
    // Num tx over num blocks
    QList<std::tuple<int, int>> privOptions; 
    privOptions.push_back(std::make_tuple<int, int>(3, 576));
    privOptions.push_back(std::make_tuple<int, int>(5, 1152));
    privOptions.push_back(std::make_tuple<int, int>(10, 2304));

    QObject::connect(turnstile.privLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), [=] (auto idx) {
        // Update the fees
        turnstile.minerFee->setText(
            Settings::getZCLUSDDisplayFormat(std::get<0>(privOptions[idx]) * Settings::getMinerFee()));
    });

    for (auto i : privOptions) {
        turnstile.privLevel->addItem(QString::number((int)(std::get<1>(i) / 24 / 24)) % " days (" % // 24 blks/hr * 24 hrs per day
                                     QString::number(std::get<1>(i)) % " blocks, ~" %
                                     QString::number(std::get<0>(i)) % " txns)"
        );
    }
    
    turnstile.buttonBox->button(QDialogButtonBox::Ok)->setText("Start");

    if (d.exec() == QDialog::Accepted) {
        auto privLevel = privOptions[turnstile.privLevel->currentIndex()];
        rpc->getTurnstile()->planMigration(
            turnstile.migrateZaddList->currentText(), 
            turnstile.migrateTo->currentText(),
            std::get<0>(privLevel), std::get<1>(privLevel));

        QMessageBox::information(this, "Backup your wallet.dat",
                                    "The migration will now start. You can check progress in the File -> Sapling Turnstile menu.\n\nYOU MUST BACKUP YOUR wallet.dat NOW!\n\nNew Addresses have been added to your wallet which will be used for the migration.",
                                    QMessageBox::Ok);
    }
}

void MainWindow::setupTurnstileDialog() {
    // Turnstile migration
    QObject::connect(ui->actionTurnstile_Migration, &QAction::triggered, [=] () {
        // If there is current migration that is present, show the progress button
        if (rpc->getTurnstile()->isMigrationPresent())
            turnstileProgress();
        else
            turnstileDoMigration();
    });
}

void MainWindow::setupStatusBar() {
    // Status Bar
    loadingLabel = new QLabel();
    loadingMovie = new QMovie(":/icons/res/loading.gif");
    loadingMovie->setScaledSize(QSize(32, 16));
    loadingMovie->start();
    loadingLabel->setAttribute(Qt::WA_NoSystemBackground);
    loadingLabel->setMovie(loadingMovie);

    ui->statusBar->addPermanentWidget(loadingLabel);
    loadingLabel->setVisible(false);

    // Custom status bar menu
    ui->statusBar->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(ui->statusBar, &QStatusBar::customContextMenuRequested, [=](QPoint pos) {
        auto msg = ui->statusBar->currentMessage();
        QMenu menu(this);

        if (!msg.isEmpty() && msg.startsWith(Settings::txidStatusMessage)) {
            auto txid = msg.split(":")[1].trimmed();
            menu.addAction("Copy txid", [=]() {
                QGuiApplication::clipboard()->setText(txid);
            });
            menu.addAction("View tx on block explorer", [=]() {
                QString url = Settings::getExplorerTxURL(txid);
                if (!url.isEmpty())
                    QDesktopServices::openUrl(QUrl(url));
            });
        }

        menu.addAction("Refresh", [=]() {
            rpc->refresh(true);
        });
        QPoint gpos(mapToGlobal(pos).x(), mapToGlobal(pos).y() + this->height() - ui->statusBar->height());
        menu.exec(gpos);
    });

    statusLabel = new QLabel();
    ui->statusBar->addPermanentWidget(statusLabel);

    statusIcon = new QLabel();
    ui->statusBar->addPermanentWidget(statusIcon);
}

// P0-6: prominent, non-technical-friendly sync banner on the Balance/main tab.
// Built in C++ (rather than hand-editing the .ui XML) so the insertion is
// mechanically safe: gridLayout_2 holds exactly one item (horizontalLayout_5),
// which we move to row 1 while the banner takes row 0.
void MainWindow::setupSyncBanner() {
    syncBanner = new QWidget(ui->tab);
    auto* bannerLayout = new QHBoxLayout(syncBanner);
    bannerLayout->setContentsMargins(8, 6, 8, 6);
    bannerLayout->setSpacing(10);

    syncStatusLabel = new QLabel(syncBanner);
    syncStatusLabel->setWordWrap(true);
    syncStatusLabel->setText(tr("Connecting to your ZClassic node…"));
    QFont f = syncStatusLabel->font();
    f.setBold(true);
    syncStatusLabel->setFont(f);

    syncProgressBar = new QProgressBar(syncBanner);
    syncProgressBar->setRange(0, 100);
    syncProgressBar->setValue(0);
    syncProgressBar->setTextVisible(true);
    syncProgressBar->setMinimumWidth(180);
    syncProgressBar->setMaximumWidth(300);
    syncProgressBar->setVisible(false);

    bannerLayout->addWidget(syncStatusLabel, 1);
    bannerLayout->addWidget(syncProgressBar, 0);

    // Move the existing balances content (horizontalLayout_5) down to row 1 and
    // place the banner on row 0. takeAt(0) is safe: gridLayout_2 has one item.
    auto* grid = ui->gridLayout_2;
    QLayoutItem* existing = grid->takeAt(0);
    grid->addWidget(syncBanner, 0, 0, 1, 1);
    if (existing && existing->layout()) {
        grid->addItem(existing, 1, 0, 1, 1);
    } else if (existing) {
        // Fallback (shouldn't happen): re-add whatever we took.
        grid->addItem(existing, 1, 0, 1, 1);
    }
    grid->setRowStretch(0, 0);
    grid->setRowStretch(1, 1);

    // Start in the neutral "connecting" state.
    setSyncStatusConnecting();
}

// Phase-3a redesign (Quiet+): the modern left vertical NAV RAIL.
//
// LOW-RISK, PROGRAMMATIC build — no .ui structural change. We DO NOT remove,
// rename, or recreate any widget, and we DO NOT swap the QTabWidget for a
// QStackedWidget. Instead:
//   * The QTabWidget keeps all of its pages/objectNames; we only HIDE its
//     tabBar(). Pages remain index-selectable (setCurrentIndex still works), so
//     the L0/L1 harness — which drives Receive radios on tab_3 directly and
//     never touches the now-hidden tabBar — stays green.
//   * We build a QFrame#navRail of flat, checkable QPushButtons in an EXCLUSIVE
//     QButtonGroup, each wired to ui->tabWidget->setCurrentIndex(idx).
//   * We re-parent the central content into a horizontal layout
//     [ navRail | tabWidget ] by inserting the rail at column 0 of the existing
//     centralWidget grid (gridLayout_3) and moving the tabWidget to column 1.
//   * syncBanner is a child of ui->tab (the Balance page) and sits ABOVE that
//     page's content via gridLayout_2; it is unaffected and stays above the
//     content, spanning the content column (not the rail), exactly as required.
void MainWindow::setupNavRail() {
    // 1) Hide the tab bar — pages stay; index selection still works.
    ui->tabWidget->tabBar()->hide();
    // The pane border/top-padding was sized for the (now hidden) tab strip; with
    // the rail driving navigation, drop the document-mode chrome so page content
    // sits flush against the rail.
    ui->tabWidget->setDocumentMode(true);

    // 2) Build the rail frame + its vertical layout.
    auto* navRail = new QFrame(ui->centralWidget);
    navRail->setObjectName("navRail");
    navRail->setFrameShape(QFrame::NoFrame);
    navRail->setFixedWidth(168);   // ~150-190px, on the 8px grid (21*8)

    auto* railLayout = new QVBoxLayout(navRail);
    railLayout->setContentsMargins(8, 16, 8, 16);
    railLayout->setSpacing(6);

    // 3) Exclusive group so exactly one destination is highlighted at a time.
    navRailGroup = new QButtonGroup(this);
    navRailGroup->setExclusive(true);

    // Helper: a large, flat, checkable rail button bound to a tab index.
    auto makeRailButton = [&](const QString& label, int tabIndex,
                              const QString& extraObjName = QString()) -> QPushButton* {
        auto* btn = new QPushButton(label, navRail);
        btn->setObjectName(extraObjName.isEmpty() ? "navRailButton" : extraObjName);
        btn->setCheckable(true);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("navrail", true);   // qss hook: QPushButton[navrail="true"]
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        navRailGroup->addButton(btn, tabIndex);
        railLayout->addWidget(btn);
        QObject::connect(btn, &QPushButton::clicked, [this, tabIndex]() {
            ui->tabWidget->setCurrentIndex(tabIndex);
        });
        return btn;
    };

    // 4) Primary destinations, in human order. Indices are the LIVE tabWidget
    //    indices at this point (zclassicd already removed): Balance 0, Send 1,
    //    Receive 2, Transactions 3.
    makeRailButton(tr("Home"),     0);   // Balance
    makeRailButton(tr("Send"),     1);
    makeRailButton(tr("Receive"),  2);
    makeRailButton(tr("Activity"), 3);   // Transactions

    // Spacer pushes the demoted "Advanced" gear to the BOTTOM of the rail.
    railLayout->addStretch(1);

    // 5) Demoted node-stats page ("zclassicd") as a small gear at the bottom.
    //    That page is added to the tabWidget LATER (rpc.cpp, when the embedded
    //    node starts) and lives at whatever index it then occupies, so resolve
    //    its index dynamically on click. It is NOT in the exclusive primary
    //    group's check-sync set above; we give it its own checkable button and
    //    add it to the same group so selecting it clears the others.
    auto* advBtn = new QPushButton(tr("⚙  Advanced"), navRail);
    advBtn->setObjectName("navRailAdvanced");
    advBtn->setCheckable(true);
    advBtn->setFlat(true);
    advBtn->setCursor(Qt::PointingHandCursor);
    advBtn->setProperty("navrail", true);
    advBtn->setProperty("navrailadv", true);   // qss hook for the smaller gear
    advBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Use a high group id so it never collides with a primary tab index.
    navRailGroup->addButton(advBtn, 1000);
    railLayout->addWidget(advBtn);
    QObject::connect(advBtn, &QPushButton::clicked, [this]() {
        // Find the zclassicd page in the live tabWidget; switch to it if present.
        int idx = ui->tabWidget->indexOf(zclassicdtab);
        if (idx >= 0)
            ui->tabWidget->setCurrentIndex(idx);
    });

    // 6) Keep the rail's checked state synced to the CURRENT tab index, from any
    //    source (rail click, code-driven setCurrentIndex in sendButton/receive
    //    flows, or the zclassicd tab being added/removed at runtime).
    QObject::connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int idx) {
        if (!navRailGroup) return;
        int advIdx = ui->tabWidget->indexOf(zclassicdtab);
        QAbstractButton* match = nullptr;
        if (advIdx >= 0 && idx == advIdx) {
            match = navRailGroup->button(1000);          // Advanced
        } else {
            match = navRailGroup->button(idx);            // a primary destination
        }
        if (match && !match->isChecked()) {
            // Toggle without re-emitting a navigation click.
            QSignalBlocker block(navRailGroup);
            match->setChecked(true);
        }
    });

    // 7) Re-parent: make the central layout horizontal — [ navRail | tabWidget ].
    //    gridLayout_3 currently holds tabWidget at (0,0). Move it to (0,1) and
    //    drop the rail in at (0,0). We move the EXISTING tabWidget, not a copy.
    auto* central = qobject_cast<QGridLayout*>(ui->centralWidget->layout());
    if (central) {
        central->removeWidget(ui->tabWidget);
        central->setContentsMargins(0, 0, 0, 0);
        central->setSpacing(0);
        central->addWidget(navRail,       0, 0);
        central->addWidget(ui->tabWidget, 0, 1);
        central->setColumnStretch(0, 0);   // rail: fixed width
        central->setColumnStretch(1, 1);   // content: takes the rest
    } else {
        // Fallback (shouldn't happen given the .ui): wrap in a fresh HBox.
        auto* hbox = new QHBoxLayout(ui->centralWidget);
        hbox->setContentsMargins(0, 0, 0, 0);
        hbox->setSpacing(0);
        hbox->addWidget(navRail);
        hbox->addWidget(ui->tabWidget, 1);
    }

    // 8) Open on Home (Balance, index 0) — not Receive. Check the Home button to
    //    match. (The .ui's currentIndex=2 default is overridden here and by the
    //    ctor's setCurrentIndex(0).)
    ui->tabWidget->setCurrentIndex(0);
    if (auto* home = navRailGroup->button(0))
        home->setChecked(true);
}

// P0-6: called from rpc.cpp's getblockchaininfo poll on every refresh.
void MainWindow::setSyncStatus(bool isSyncing, int blockNumber, int estimatedHeight, double progress) {
    if (syncBanner == nullptr) return;

    if (!isSyncing) {
        // Fully caught up: unmistakable green "ready" state.
        syncEtaStarted = false;
        syncProgressBar->setVisible(false);
        syncProgressBar->setRange(0, 100);   // reset in case it was indeterminate
        syncStatusLabel->setText(tr("✓ Synced — Ready to use"));
        syncBanner->setStyleSheet("QWidget { background-color: #1f7a1f; } QLabel { color: white; }");
        return;
    }

    // Indeterminate: we are syncing but have no usable target yet (e.g. the node's
    // header height isn't known on the first poll). Show a busy bar and a friendly
    // "starting" message rather than a misleading 0%/height/ETA.
    bool indeterminate = isSyncing && (estimatedHeight <= 0 || progress < 0.0);
    if (indeterminate) {
        syncEtaStarted = false;
        syncProgressBar->setVisible(true);
        syncProgressBar->setRange(0, 0);     // busy/indeterminate animation
        // Include the live block height so the text VISIBLY changes as blocks arrive,
        // rather than a frozen "Starting your node…" that reads as stuck on a slow link.
        // When the node already has a tip (blockNumber > 0) it is finding peers / confirming
        // the chain; before that it is genuinely still starting.
        if (blockNumber > 0)
            syncStatusLabel->setText(tr("Starting your node… (block %1, connecting to peers)")
                .arg(blockNumber));
        else
            syncStatusLabel->setText(tr("Starting your node…"));
        syncBanner->setStyleSheet("QWidget { background-color: #d9822b; } QLabel { color: white; }");
        return;
    }

    // Syncing: show a progress bar and an estimated time remaining. qRound (not a
    // truncating cast) so e.g. 99.6% reads 100, not 99.
    int pct = qRound(progress * 100.0);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    syncProgressBar->setVisible(true);
    syncProgressBar->setRange(0, 100);       // reset in case it was indeterminate
    syncProgressBar->setValue(pct);

    QString etaText;
    if (estimatedHeight > 0 && blockNumber > 0) {
        if (!syncEtaStarted) {
            syncEtaStarted = true;
            syncEtaStartBlock = blockNumber;
            syncEtaTimer.restart();
        } else {
            qint64 elapsedMs = syncEtaTimer.elapsed();
            int    blocksDone = blockNumber - syncEtaStartBlock;
            if (elapsedMs > 2000 && blocksDone > 0) {
                double blocksPerSec = (double) blocksDone / (elapsedMs / 1000.0);
                int    remaining    = estimatedHeight - blockNumber;
                if (blocksPerSec > 0.0 && remaining > 0) {
                    qint64 secsLeft = (qint64) (remaining / blocksPerSec);
                    QString human;
                    if (secsLeft >= 3600)
                        human = tr("about %1h %2m").arg(secsLeft / 3600).arg((secsLeft % 3600) / 60);
                    else if (secsLeft >= 60)
                        human = tr("about %1m").arg(secsLeft / 60);
                    else
                        human = tr("less than a minute");
                    etaText = QString(" • ") % tr("ETA %1").arg(human);
                }
            }
        }
    }

    QString heightText;
    if (estimatedHeight > 0) {
        heightText = QString(" • ") % tr("block %1 / ~%2")
            .arg(QString::number(blockNumber), QString::number(estimatedHeight));
    }

    syncStatusLabel->setText(tr("Syncing blockchain… %1%").arg(pct) % heightText % etaText);
    syncBanner->setStyleSheet("QWidget { background-color: #d9822b; } QLabel { color: white; }");
}

// P0-6: called from rpc.cpp's noConnection() so the user sees a clear,
// non-alarming "connecting" state instead of a blank/stuck UI.
void MainWindow::setSyncStatusConnecting() {
    if (syncBanner == nullptr) return;
    syncEtaStarted = false;
    syncProgressBar->setVisible(false);
    syncStatusLabel->setText(tr("Connecting to your ZClassic node…"));
    syncBanner->setStyleSheet("QWidget { background-color: #555555; } QLabel { color: white; }");
}

// C9: called from rpc.cpp when the node is "syncing" but has 0 peers. A fresh
// install with no peers would otherwise sit on a stuck "Syncing 0%" -- this tells
// the user the real problem (no network) instead.
void MainWindow::setSyncStatusWaitingForPeers(bool longStretch) {
    if (syncBanner == nullptr) return;
    syncEtaStarted = false;
    syncProgressBar->setVisible(false);
    syncProgressBar->setRange(0, 100);   // reset in case it was indeterminate
    if (longStretch)
        syncStatusLabel->setText(tr("Still waiting for peers… please check your internet "
            "connection and try again later"));
    else
        syncStatusLabel->setText(tr("Waiting for peers… check your internet connection"));
    syncBanner->setStyleSheet("QWidget { background-color: #d9822b; } QLabel { color: white; }");
}

// Bootstrap snapshot download in progress: show real determinate progress instead of
// the misleading "waiting for peers". The node reports 0 normal P2P peers while it
// downloads the snapshot, but it is actively working -- not stuck.
void MainWindow::setSyncStatusBootstrapSnapshot(int pct, qint64 received, qint64 total, double mbps) {
    if (syncBanner == nullptr) return;
    syncEtaStarted = false;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    syncProgressBar->setVisible(true);
    syncProgressBar->setRange(0, 100);
    syncProgressBar->setValue(pct);

    QString detail;
    if (total > 0) {
        const double gb = 1024.0 * 1024.0 * 1024.0;
        detail = QString(" • ") % tr("%1 / %2 GB")
            .arg(received / gb, 0, 'f', 1).arg(total / gb, 0, 'f', 1);
    }
    if (mbps > 0.0)
        detail = detail % QString(" • ") % tr("%1 MB/s").arg(mbps, 0, 'f', 1);

    syncStatusLabel->setText(tr("Downloading blockchain snapshot… %1%").arg(pct) % detail);
    syncBanner->setStyleSheet("QWidget { background-color: #d9822b; } QLabel { color: white; }");
}

// SELF-HEAL (B-side): non-blocking validation banner. An empty message clears it
// (the next sync poll repaints the normal state). A non-empty message overlays a
// neutral-blue informational banner reusing the same syncBanner widget. It is
// called AFTER setSyncStatus on each poll, so an active validation message wins for
// that tick without disturbing the headers-vs-progress sync logic.
void MainWindow::setBootstrapValidationBanner(const QString& msg) {
    if (syncBanner == nullptr) return;
    if (msg.isEmpty())
        return;   // nothing to show; leave whatever setSyncStatus painted this tick
    syncProgressBar->setVisible(false);
    syncStatusLabel->setText(msg);
    syncBanner->setStyleSheet("QWidget { background-color: #2c5d8f; } QLabel { color: white; }");
}

// Non-modal notification. Prefer a system-tray balloon (visible even when the main
// window is hidden in tray-resident mode); fall back to a status-bar message when
// there is no tray. Used by the background node-crash recovery so it never pops a
// modal over a hidden window.
void MainWindow::notify(const QString& title, const QString& body) {
    if (trayIcon != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon->showMessage(title, body, QSystemTrayIcon::Information, 6000);
    } else {
        ui->statusBar->showMessage(title % ": " % body, 10 * 1000);
    }
    logger->write("notify: " % title % " — " % body);
}

// SINGLE launch path for the "stop the node, then drive the staged re-download ladder"
// flow. Both the always-reachable Help -> Repair action and the runtime stub auto-heal
// (autoHealStubChain) call THIS — there is no second destructive path. It stops the
// running embedded node (a no-op + bounded wait for an external daemon we don't own),
// then spins up a fresh ConnectionLoader (the old one is deleted post-handoff) and
// invokes startManualRepair(), which runs offerCorruptionRepair()/redownloadChain()
// with all their existing guards (confirmDaemonDeadForRepair, wallet.dat backup, the
// ~15 GB disk floor, reversible set-aside rename).
void MainWindow::launchBlockchainRepair() {
    // Stop the running embedded node so there are no open LevelDB handles before any
    // set-aside/reindex. shutdownZClassicd() is a no-op for an external daemon (we
    // don't own its lifetime) and waits (bounded) for exit.
    rpc->shutdownZClassicd();

    // Fresh loader: ConnectionLoader is deleted post-handoff, so we create a new one
    // exactly like the settings-change path does, then drive the ladder.
    auto* cl = new ConnectionLoader(this, rpc);
    cl->startManualRepair();
}

// RUNTIME STUB AUTO-HEAL entry. Called from the post-connection sync poller (rpc.cpp)
// ONLY when ALL of its conservative triggers hold (sustained 0 peers AND a blocks/ dir
// far below a real chain AND not already auto-healed this run/cooldown). It is the
// runtime analogue of the Help -> Repair action: it reuses the EXACT same launch path
// (launchBlockchainRepair -> startManualRepair -> redownloadChain), so every existing
// safety guard applies. It is intentionally NON-interactive (no confirm dialog) but it
// surfaces a clear status first; the destructive work still goes through
// redownloadChain()'s daemon-dead/backup/disk/set-aside guards. The poller owns the
// trigger arithmetic and the per-process / per-install-cooldown guards; this method
// just performs the heal. Returns immediately (no-op) if a heal is already in flight.
void MainWindow::autoHealStubChain() {
    // Serialize with the heal ledger so this can't race a startup/watchdog heal that is
    // already mid-flight (mainwindow.cpp clears a stale heal/inProgress once at process
    // start, so a value seen here is a LIVE in-progress heal this run).
    if (QSettings().value("heal/inProgress", false).toBool()) {
        logger->write("autoHealStubChain: a heal is already in progress; skipping");
        return;
    }

    logger->write("autoHealStubChain: stub chain + sustained peerless -> re-downloading blockchain");

    // Clear, visible status (never a silent wipe). The connect dialog spun up by the
    // fresh ConnectionLoader / redownloadChain's own "Re-downloading…" information box
    // takes over from here, but paint the banner immediately so the moment is explained.
    if (syncStatusLabel != nullptr) {
        syncProgressBar->setVisible(false);
        syncStatusLabel->setText(tr("Re-downloading blockchain…"));
        syncBanner->setStyleSheet("QWidget { background-color: #2c5d8f; } QLabel { color: white; }");
    }
    notify(tr("Re-downloading blockchain"),
        tr("ZClassic couldn't find any peers and the local copy is incomplete, so it is "
           "downloading a fresh copy. Your wallet and coins are safe."));

    launchBlockchainRepair();
}

// Local double-launch / "is anything still on the node ports?" probe for the runtime
// dialogs below (the connect-time ConnectionLoader::portsFree is private and there is no
// live loader at runtime). True iff BOTH the RPC port and the matching P2P port on
// 127.0.0.1 are currently UNBOUND. Mainnet 8023/8033, testnet 18023/18033; testnet is read
// from the authoritative Settings signal (matching edit #7).
static bool mwNodePortsFree() {
    bool testnet  = Settings::getInstance()->isTestnet();
    quint16 rpcPort = testnet ? 18023 : 8023;
    quint16 p2pPort = testnet ? 18033 : 8033;
    auto portInUse = [](quint16 port) -> bool {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), port);
        bool connected = probe.waitForConnected(150);   // refused/timeout => free
        probe.abort();
        return connected;
    };
    return !portInUse(rpcPort) && !portInUse(p2pPort);
}

// Relaunch this application as a fresh process, then quit the current one. Used as the
// primary "fix it" action of the runtime dialogs: after the user stops the foreign node,
// the relaunched wallet finds the ports free and starts/heals its OWN embedded node.
static void mwRelaunchSelf() {
    QStringList args = QApplication::arguments();
    if (!args.isEmpty())
        args.removeFirst();   // drop argv[0]
    QProcess::startDetached(QApplication::applicationFilePath(), args);
}

// RUNTIME actionable dialog for a FOREIGN node that is genuinely STUCK (attached, but
// peerless/not syncing for a sustained window). See the header. Persistent (looping/re-
// shown until resolved), actionable, retryable. Centred on STUCK, NOT "too old". NEVER
// kills/mutates the foreign node; NEVER a silent hang. The caller (rpc.cpp poller) guards
// it with a once-per-run bool so it cannot spam.
void MainWindow::showForeignNodeStuck() {
    logger->write("Showing foreign-node-stuck actionable dialog (attached node is peerless/not syncing)");

    for (;;) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Another ZClassic node is running"));
        box.setIcon(QMessageBox::Warning);
        box.setTextFormat(Qt::RichText);
        box.setText(tr("This wallet is connected to a ZClassic node it did not start, "
                       "and that node is not syncing."));
        box.setInformativeText(tr(
            "The other node cannot find any peers, so the blockchain is not progressing. "
            "This wallet can download and repair the blockchain automatically — but only "
            "for a node it starts itself, so it can't fix the other one.<br><br>"
            "<b>Your wallet file and your coins are safe.</b> Nothing has been changed.<br><br>"
            "<b>To fix this:</b> stop the other node, then reopen this wallet so it can "
            "manage its own node.<br>"
            "&nbsp;&nbsp;• On Linux, run: <code>systemctl --user stop zclassicd</code><br>"
            "&nbsp;&nbsp;• Or simply close the other copy of ZClassic that is open."));

        // Offer "Use the bundled node" ONLY when the ports are ACTUALLY free right now
        // (the foreign node was already stopped) — re-checked on click below.
        bool portsAreFree = mwNodePortsFree();

        QPushButton* reopenBtn  = box.addButton(tr("Stop the other node, then reopen"),
                                                QMessageBox::AcceptRole);
        QPushButton* bundledBtn = portsAreFree
            ? box.addButton(tr("Use the bundled node"), QMessageBox::ActionRole)
            : nullptr;
        QPushButton* quitBtn    = box.addButton(tr("Quit"), QMessageBox::RejectRole);
        box.setDefaultButton(reopenBtn);

        box.exec();
        QAbstractButton* clicked = box.clickedButton();

        if (clicked == bundledBtn) {
            // RE-CHECK at click time: never double-launch onto a held port.
            if (mwNodePortsFree()) {
                logger->write("Foreign-node-stuck: ports free; relaunching to start the bundled node");
                mwRelaunchSelf();
                quitApp();
                return;
            }
            logger->write("Foreign-node-stuck: ports still in use at click; re-showing");
            continue;   // still held — re-explain
        } else if (clicked == quitBtn) {
            logger->write("Foreign-node-stuck: Quit pressed");
            quitApp();
            return;
        } else {   // reopenBtn or window-chrome close: relaunch so we can manage our own node
            logger->write("Foreign-node-stuck: relaunching app so this wallet manages its own node");
            mwRelaunchSelf();
            quitApp();
            return;
        }
    }
}

// Sibling of the above for the connect-time CATCH-ALL (edit #2). The node has not answered
// cleanly nor with a recognized warmup state for too long. Actionable + retryable; Retry
// relaunches the app (a clean reconnect) instead of leaving the connect dialog frozen.
void MainWindow::showNodeNotRespondingRetry() {
    logger->write("Showing node-not-responding actionable retry dialog");

    QMessageBox box(this);
    box.setWindowTitle(tr("ZClassic is taking longer than expected"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("The ZClassic node hasn't finished starting up."));
    box.setInformativeText(tr(
        "It hasn't responded normally for a while. Your wallet and coins are safe — "
        "nothing has been changed.\n\n"
        "Press Retry to start over, or Quit to close ZClassic."));
    QPushButton* retryBtn = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
    QPushButton* quitBtn  = box.addButton(tr("Quit"), QMessageBox::RejectRole);
    box.setDefaultButton(retryBtn);
    box.exec();

    if (box.clickedButton() == quitBtn) {
        logger->write("Node-not-responding: Quit pressed");
        quitApp();
        return;
    }
    // Retry / closed: relaunch the app to re-run the whole connect flow cleanly.
    logger->write("Node-not-responding: Retry pressed; relaunching app");
    mwRelaunchSelf();
    quitApp();
}

void MainWindow::setupSettingsModal() {    
    // Set up File -> Settings action
    QObject::connect(ui->actionSettings, &QAction::triggered, [=]() {
        QDialog settingsDialog(this);
        Ui_Settings settings;
        settings.setupUi(&settingsDialog);
        Settings::saveRestore(&settingsDialog);

        // Setup save sent check box
        QObject::connect(settings.chkSaveTxs, &QCheckBox::stateChanged, [=](auto checked) {
            Settings::getInstance()->setSaveZtxs(checked);
        });

        // Setup clear button
        QObject::connect(settings.btnClearSaved, &QCheckBox::clicked, [=]() {
            if (QMessageBox::warning(this, "Clear saved history?",
                "Shielded z-Address transactions are stored locally in your wallet, outside zclassicd. You may delete this saved information safely any time for your privacy.\nDo you want to delete the saved shielded transactions now?",
                QMessageBox::Yes, QMessageBox::Cancel)) {
                    SentTxStore::deleteHistory();
                    // Also drop the cached last-known balances (same local data).
                    QSettings().remove("cache");
                    // Reload after the clear button so existing txs disappear
                    rpc->refresh(true);
            }
        });

        // Save sent transactions
        settings.chkSaveTxs->setChecked(Settings::getInstance()->getSaveZtxs());

        // Keep running in the background (tray-resident). Disable + explain when
        // there is no system tray available so the user isn't offered a no-op.
        settings.chkKeepInTray->setChecked(Settings::getInstance()->getKeepInTray());
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            settings.chkKeepInTray->setEnabled(false);
            settings.chkKeepInTray->setToolTip(tr("Your desktop doesn't provide a system tray, so this option isn't available."));
        }

        // Custom fees
        settings.chkCustomFees->setChecked(Settings::getInstance()->getAllowCustomFees());

        // Auto shielding
        settings.chkAutoShield->setChecked(Settings::getInstance()->getAutoShield());

        // Use Tor
        bool isUsingTor = false;
        if (rpc->getConnection() != nullptr) {
            isUsingTor = !rpc->getConnection()->config->proxy.isEmpty();
        }
        settings.chkTor->setChecked(isUsingTor);
        if (rpc->getEZClassicD() == nullptr) {
            settings.chkTor->setEnabled(false);
            settings.lblTor->setEnabled(false);
            QString tooltip = tr("Tor configuration is available only when running an embedded zclassicd.");
            settings.chkTor->setToolTip(tooltip);
            settings.lblTor->setToolTip(tooltip);
        }

        // Connection Settings
        QIntValidator validator(0, 65535);
        settings.port->setValidator(&validator);

        // If values are coming from zclassic.conf, then disable all the fields
        auto zclassicConfLocation = Settings::getInstance()->getZClassicdConfLocation();
        if (!zclassicConfLocation.isEmpty()) {
            settings.confMsg->setText("Settings are being read from \n" + zclassicConfLocation);
            settings.hostname->setEnabled(false);
            settings.port->setEnabled(false);
            settings.rpcuser->setEnabled(false);
            settings.rpcpassword->setEnabled(false);
        }
        else {
            settings.confMsg->setText("No local zclassic.conf found. Please configure connection manually.");
            settings.hostname->setEnabled(true);
            settings.port->setEnabled(true);
            settings.rpcuser->setEnabled(true);
            settings.rpcpassword->setEnabled(true);
        }

        // Load current values into the dialog        
        auto conf = Settings::getInstance()->getSettings();
        settings.hostname->setText(conf.host);
        settings.port->setText(conf.port);
        settings.rpcuser->setText(conf.rpcuser);
        settings.rpcpassword->setText(conf.rpcpassword);

        // Connection tab by default
        settings.tabWidget->setCurrentIndex(0);

        // Enable the troubleshooting options only if using embedded zclassicd
        if (!rpc->isEmbedded()) {
            settings.chkRescan->setEnabled(false);
            settings.chkRescan->setToolTip(tr("You're using an external zclassicd. Please restart zclassicd with -rescan"));

            settings.chkReindex->setEnabled(false);
            settings.chkReindex->setToolTip(tr("You're using an external zclassicd. Please restart zclassicd with -reindex"));
        }

        if (settingsDialog.exec() == QDialog::Accepted) {
            // Custom fees
            bool customFees = settings.chkCustomFees->isChecked();
            Settings::getInstance()->setAllowCustomFees(customFees);
            ui->minerFeeAmt->setReadOnly(!customFees);
            if (!customFees)
                ui->minerFeeAmt->setText(Settings::getDecimalString(Settings::getMinerFee()));

            // Auto shield
            Settings::getInstance()->setAutoShield(settings.chkAutoShield->isChecked());

            // Keep running in the background (tray-resident). Apply immediately
            // so the change takes effect this session, no restart needed.
            bool keepInTray = settings.chkKeepInTray->isChecked();
            Settings::getInstance()->setKeepInTray(keepInTray);
            applyTraySetting(keepInTray);

            if (!isUsingTor && settings.chkTor->isChecked()) {
                // If "use tor" was previously unchecked and now checked
                Settings::addToZClassicConf(zclassicConfLocation, "proxy=127.0.0.1:9050");
                rpc->getConnection()->config->proxy = "proxy=127.0.0.1:9050";

                QMessageBox::information(this, tr("Enable Tor"), 
                    tr("Connection over Tor has been enabled. To use this feature, you need to restart ZclWallet."), 
                    QMessageBox::Ok);
            }

            if (isUsingTor && !settings.chkTor->isChecked()) {
                // If "use tor" was previously checked and now is unchecked
                Settings::removeFromZClassicConf(zclassicConfLocation, "proxy");
                rpc->getConnection()->config->proxy.clear();

                QMessageBox::information(this, tr("Disable Tor"),
                    tr("Connection over Tor has been disabled. To fully disconnect from Tor, you need to restart ZclWallet."),
                    QMessageBox::Ok);
            }

            if (zclassicConfLocation.isEmpty()) {
                // Save settings
                Settings::getInstance()->saveSettings(
                    settings.hostname->text(),
                    settings.port->text(),
                    settings.rpcuser->text(),
                    settings.rpcpassword->text());
                
                auto cl = new ConnectionLoader(this, rpc);
                cl->loadConnection();
            }

            // Check to see if rescan or reindex have been enabled
            bool showRestartInfo = false;
            if (settings.chkRescan->isChecked()) {
                Settings::addToZClassicConf(zclassicConfLocation, "rescan=1");
                showRestartInfo = true;
            }

            if (settings.chkReindex->isChecked()) {
                Settings::addToZClassicConf(zclassicConfLocation, "reindex=1");
                showRestartInfo = true;
            }

            if (showRestartInfo) {
                auto desc = tr("ZclWallet needs to restart to rescan/reindex. ZclWallet will now close, please restart ZclWallet to continue");
                
                QMessageBox::information(this, tr("Restart ZclWallet"), desc, QMessageBox::Ok);
                QTimer::singleShot(1, [=]() { this->close(); });
            }
        }
    });
}

void MainWindow::addressBook() {
    // Check to see if there is a target.
    QRegExp re("Address[0-9]+", Qt::CaseInsensitive);
    for (auto target: ui->sendToWidgets->findChildren<QLineEdit *>(re)) {
        if (target->hasFocus()) {
            AddressBook::open(this, target);
            return;
        }
    };

    // If there was no target, then just run with no target.
    AddressBook::open(this);
}



void MainWindow::doImport(QList<QString>* keys) {
    if (rpc->getConnection() == nullptr) {
        // No connection, just return
        return;
    }

    if (keys->isEmpty()) {
        delete keys;
        ui->statusBar->showMessage(tr("Private key import rescan finished"));
        return;
    }

    // Pop the first key
    QString key = keys->first();
    keys->pop_front();
    bool rescan = keys->isEmpty();

    if (key.startsWith("S") ||
        key.startsWith("secret")) { // Z key
        rpc->importZPrivKey(key, rescan, [=] (auto) { this->doImport(keys); });                   
    } else {
        rpc->importTPrivKey(key, rescan, [=] (auto) { this->doImport(keys); });
    }
}


// Callback invoked when the RPC has finished loading all the balances, and the UI 
// is now ready to send transactions.
void MainWindow::balancesReady() {
    // First-time check
    if (uiPaymentsReady)
        return;

    uiPaymentsReady = true;
    qDebug() << "Payment UI now ready!";

    // There is a pending URI payment (from the command line, or from a secondary instance),
    // process it.
    if (!pendingURIPayment.isEmpty()) {
        qDebug() << "Paying zclassic URI";
        payZClassicURI(pendingURIPayment);
        pendingURIPayment = "";
    }
}

// P0-2: First-run / until-backed-up fund-safety prompt.
//
// The ZClassic wallet is wallet.dat FILE-based: there is NO BIP39 seed phrase.
// If the user loses wallet.dat (disk failure, reinstall, lost laptop) and has
// no backup, the coins are gone forever. This prompt nags on every launch
// until the user backs up (file copy) or exports their private keys, then it
// is silenced permanently via the persisted "options/walletbackedup" flag.
//
// It is modal/blocking but ALWAYS dismissible ("Maybe later") so it can never
// become an un-closable trap.
void MainWindow::promptWalletBackup() {
    // Already backed up at some point? Never nag again.
    if (QSettings().value("options/walletbackedup", false).toBool())
        return;

    // We need a live connection to know where wallet.dat lives / to copy it.
    if (!rpc || !rpc->getConnection())
        return;

    QMessageBox box(this);
    box.setWindowTitle(tr("Back up your wallet"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("Protect your coins — back up your wallet now."));
    box.setInformativeText(tr(
        "Your ZClassic is stored in a single file on this computer called "
        "<b>wallet.dat</b>. There is <b>no seed phrase / recovery phrase</b> for this wallet.<br><br>"
        "If this computer is lost, reset, or its disk fails and you have no backup, "
        "<b>your coins are gone forever and cannot be recovered by anyone</b>.<br><br>"
        "Make a backup now and keep a copy somewhere safe (a USB stick or another "
        "computer). You can:<br>"
        "&nbsp;&nbsp;• Save a copy of the wallet.dat file, or<br>"
        "&nbsp;&nbsp;• Export your private keys to a text file.<br><br>"
        "Keep your backup private — anyone who has it can spend your coins."));

    QPushButton* backupBtn = box.addButton(tr("Back Up Now"), QMessageBox::AcceptRole);
    QPushButton* exportBtn = box.addButton(tr("Export Private Keys"), QMessageBox::ActionRole);
    QPushButton* laterBtn  = box.addButton(tr("Maybe later"), QMessageBox::RejectRole);
    box.setDefaultButton(backupBtn);

    box.exec();
    QAbstractButton* clicked = box.clickedButton();

    if (clicked == backupBtn) {
        // Copy wallet.dat to a user-chosen location. We mirror backupWalletDat()
        // here (rather than calling it) so we can detect a SUCCESSFUL copy and
        // only then mark the wallet as backed up.
        QDir zclassicdir(rpc->getConnection()->config->zclassicDir);
        QString backupDefaultName = "zclassic-wallet-backup-" +
            QDateTime::currentDateTime().toString("yyyyMMdd") + ".dat";

        if (Settings::getInstance()->isTestnet()) {
            zclassicdir.cd("testnet3");
            backupDefaultName = "testnet-" + backupDefaultName;
        }

        QFile wallet(zclassicdir.filePath("wallet.dat"));
        if (!wallet.exists()) {
            QMessageBox::critical(this, tr("No wallet.dat"),
                tr("Couldn't find the wallet.dat on this computer") + "\n" +
                tr("You need to back it up from the machine zclassicd is running on"),
                QMessageBox::Ok);
            return;
        }

        QUrl backupName = QFileDialog::getSaveFileUrl(this, tr("Backup wallet.dat"),
            backupDefaultName, "Data file (*.dat)");
        if (backupName.isEmpty())
            return;  // User cancelled the save dialog: leave the flag unset so we nag again next launch.

        if (wallet.copy(backupName.toLocalFile())) {
            // Success: remember that the wallet has been backed up.
            QSettings().setValue("options/walletbackedup", true);
            QMessageBox::information(this, tr("Backup complete"),
                tr("Your wallet backup was saved.\n\n"
                   "Keep this file private and in a safe place. You can restore your "
                   "coins later by copying it back into the wallet's data folder."),
                QMessageBox::Ok);
        } else {
            QMessageBox::critical(this, tr("Couldn't backup"),
                tr("Couldn't backup the wallet.dat file.") +
                tr("You need to back it up manually."), QMessageBox::Ok);
        }
    } else if (clicked == exportBtn) {
        // Reuse the existing private-key export dialog. The user saves the keys
        // to a file from inside that dialog. Because that dialog's Save is
        // optional and reports no status, we explicitly confirm the user
        // actually saved before marking the wallet backed up -- otherwise we'd
        // give a false sense of safety and never remind them again.
        exportAllKeys();
        auto saved = QMessageBox::question(this, tr("Did you save your keys?"),
            tr("Did you save the file with your private keys somewhere safe?\n\n"
               "Only choose Yes if you actually saved it — otherwise we'll remind "
               "you to back up again next time."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (saved == QMessageBox::Yes)
            QSettings().setValue("options/walletbackedup", true);
    } else {
        // "Maybe later" (or the dialog was closed): make the consequence explicit,
        // but never block. The flag stays unset, so we nag again next launch.
        QMessageBox::warning(this, tr("No backup yet"),
            tr("You have not backed up your wallet.\n\n"
               "Until you do, you risk losing all your coins if this computer is "
               "lost or breaks. We'll remind you again next time you open the wallet."),
            QMessageBox::Ok);
    }
}

// Event filter for MacOS specific handling of payment URIs
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::FileOpen) {
        QFileOpenEvent *fileEvent = static_cast<QFileOpenEvent*>(event);
        if (!fileEvent->url().isEmpty())
            payZClassicURI(fileEvent->url().toString());

        return true;
    }

    return QObject::eventFilter(object, event);
}


// Pay the ZClassic URI by showing a confirmation window. If the URI parameter is empty, the UI
// will prompt for one.
void MainWindow::payZClassicURI(QString uri) {
    // If the Payments UI is not ready (i.e, all balances have not loaded), defer the payment URI
    if (!uiPaymentsReady) {
        qDebug() << "Payment UI not ready, waiting for UI to pay URI";
        pendingURIPayment = uri;
        return;
    }

    // Error to display if something goes wrong.
    auto payZClassicURIError = [=] (QString errorDetail = "") {
        QMessageBox::critical(this, tr("Error paying zclassic URI"), 
                tr("URI should be of the form 'zclassic:<addr>?amt=x&memo=y") + "\n" + errorDetail);
    };

    // If there was no URI passed, ask the user for one.
    if (uri.isEmpty()) {
        uri = QInputDialog::getText(this, tr("Paste ZClassic URI"),
            "ZClassic URI" + QString(" ").repeated(180));
    }

    // If there's no URI, just exit
    if (uri.isEmpty())
        return;

    // URI should be of the form zclassic://address?amt=x&memo=y
    if (!uri.startsWith("zclassic:")) {
        payZClassicURIError();
        return;
    }

    // Extract the address
    qDebug() << "Recieved URI " << uri;
    uri = uri.right(uri.length() - QString("zclassic:").length());

    QRegExp re("([a-zA-Z0-9]+)");
    int pos;
    if ( (pos = re.indexIn(uri)) == -1 ) {
        payZClassicURIError();
        return;
    }

    QString addr = re.cap(1);
    if (!Settings::isValidAddress(addr)) {
        payZClassicURIError(tr("Could not understand address"));
        return;
    }
    uri = uri.right(uri.length() - addr.length());

    double amount = 0.0;
    QString memo  = "";

    if (!uri.isEmpty()) {
        uri = uri.right(uri.length() - 1); // Eat the "?"

        QStringList args = uri.split("&");
        for (QString arg: args) {
            QStringList kv = arg.split("=");
            if (kv.length() != 2) {
                payZClassicURIError();
                return;
            }

            if (kv[0].toLower() == "amt" || kv[0].toLower() == "amount") {
                amount = kv[1].toDouble(); 
            } else if (kv[0].toLower() == "memo" || kv[0].toLower() == "message" || kv[0].toLower() == "msg") {
                memo = kv[1];
                // Test if this is hex

                QRegularExpression hexMatcher("^[0-9A-F]+$",
                                            QRegularExpression::CaseInsensitiveOption);
                QRegularExpressionMatch match = hexMatcher.match(memo);
                if (match.hasMatch()) {
                    // Encoded as hex, convert to string
                    memo = QByteArray::fromHex(memo.toUtf8());
                }
            } else {
                payZClassicURIError(tr("Unknown field in URI:") + kv[0]);
                return;
            }
        }
    }

    // Now, set the fields on the send tab
    removeExtraAddresses();
    ui->Address1->setText(addr);
    ui->Address1->setCursorPosition(0);
    ui->Amount1->setText(QString::number(amount));
    ui->MemoTxt1->setText(memo);

    // And switch to the send tab.
    ui->tabWidget->setCurrentIndex(1);
    raise();

    // And click the send button if the amount is > 0, to validate everything. If everything is OK, it will show the confirm box
    // else, show the error message;
    if (amount > 0) {
        sendButton();
    }
}


void MainWindow::importPrivKey() {
    QDialog d(this);
    Ui_PrivKey pui;
    pui.setupUi(&d);
    Settings::saveRestore(&d);

    pui.buttonBox->button(QDialogButtonBox::Save)->setVisible(false);
    pui.helpLbl->setText(QString() %
                        tr("Please paste your private keys (z-Addr or t-Addr) here, one per line") % ".\n" %
                        tr("The keys will be imported into your connected zclassicd node"));  

    if (d.exec() == QDialog::Accepted && !pui.privKeyTxt->toPlainText().trimmed().isEmpty()) {
        auto rawkeys = pui.privKeyTxt->toPlainText().trimmed().split("\n");

        QList<QString> keysTmp;
        // Filter out all the empty keys.
        std::copy_if(rawkeys.begin(), rawkeys.end(), std::back_inserter(keysTmp), [=] (auto key) {
            return !key.startsWith("#") && !key.trimmed().isEmpty();
        });

        auto keys = new QList<QString>();
        std::transform(keysTmp.begin(), keysTmp.end(), std::back_inserter(*keys), [=](auto key) {
            return key.trimmed().split(" ")[0];
        });

        // Start the import. The function takes ownership of keys
        QTimer::singleShot(1, [=]() {doImport(keys);});

        // Show the dialog that keys will be imported. 
        QMessageBox::information(this,
            tr("Importing keys"),
            tr("Your keys are being imported and the wallet will now rescan the "
               "blockchain. This can take several minutes — your balance will "
               "update when it finishes. Please leave ZClassic open."),
            QMessageBox::Ok);
    }
}

/** 
 * Export transaction history into a CSV file
 */
void MainWindow::exportTransactions() {
    // First, get the export file name
    QString exportName = "zclassic-transactions-" + QDateTime::currentDateTime().toString("yyyyMMdd") + ".csv";

    QUrl csvName = QFileDialog::getSaveFileUrl(this, 
            tr("Export transactions"), exportName, "CSV file (*.csv)");

    if (csvName.isEmpty())
        return;

    if (!rpc->getTransactionsModel()->exportToCsv(csvName.toLocalFile())) {
        QMessageBox::critical(this, tr("Error"), 
            tr("Error exporting transactions, file was not saved"), QMessageBox::Ok);
    }
} 

/**
 * Backup the wallet.dat file. This is kind of a hack, since it has to read from the filesystem rather than an RPC call
 * This might fail for various reasons - Remote zclassicd, non-standard locations, custom params passed to zclassicd, many others
*/
void MainWindow::backupWalletDat() {
    if (!rpc->getConnection())
        return;

    QDir zclassicdir(rpc->getConnection()->config->zclassicDir);
    QString backupDefaultName = "zclassic-wallet-backup-" + QDateTime::currentDateTime().toString("yyyyMMdd") + ".dat";

    if (Settings::getInstance()->isTestnet()) {
        zclassicdir.cd("testnet3");
        backupDefaultName = "testnet-" + backupDefaultName;
    }
    
    QFile wallet(zclassicdir.filePath("wallet.dat"));
    if (!wallet.exists()) {
        QMessageBox::critical(this, tr("No wallet.dat"), tr("Couldn't find the wallet.dat on this computer") + "\n" +
            tr("You need to back it up from the machine zclassicd is running on"), QMessageBox::Ok);
        return;
    }
    
    QUrl backupName = QFileDialog::getSaveFileUrl(this, tr("Backup wallet.dat"), backupDefaultName, "Data file (*.dat)");
    if (backupName.isEmpty())
        return;

    if (!wallet.copy(backupName.toLocalFile())) {
        QMessageBox::critical(this, tr("Couldn't backup"), tr("Couldn't backup the wallet.dat file.") + 
            tr("You need to back it up manually."), QMessageBox::Ok);
    }
}

void MainWindow::exportAllKeys() {
    exportKeys("");
}

void MainWindow::exportKeys(QString addr) {
    bool allKeys = addr.isEmpty() ? true : false;

    // P1-6: private keys ARE the money — anyone who sees this text can spend your funds, so confirm intent first.
    if (QMessageBox::warning(this, tr("Export Private Keys"),
            tr("Anyone who has these private keys can spend all the funds in these addresses. Only export them if you are sure no one else can see your screen or the saved file."),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }

    QDialog d(this);
    Ui_PrivKey pui;
    pui.setupUi(&d);
    
    // Make the window big by default
    auto ps = this->geometry();
    QMargins margin = QMargins() + 50;
    d.setGeometry(ps.marginsRemoved(margin));

    Settings::saveRestore(&d);

    pui.privKeyTxt->setPlainText(tr("Loading..."));
    pui.privKeyTxt->setReadOnly(true);
    pui.privKeyTxt->setLineWrapMode(QPlainTextEdit::LineWrapMode::NoWrap);

    if (allKeys)
        pui.helpLbl->setText(tr("These are all the private keys for all the addresses in your wallet"));
    else
        pui.helpLbl->setText(tr("Private key for ") + addr);

    // Disable the save button until it finishes loading
    pui.buttonBox->button(QDialogButtonBox::Save)->setEnabled(false);
    pui.buttonBox->button(QDialogButtonBox::Ok)->setVisible(false);

    // Wire up save button
    QObject::connect(pui.buttonBox->button(QDialogButtonBox::Save), &QPushButton::clicked, [=] () {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"),
                           allKeys ? "zclassic-all-privatekeys.txt" : "zclassic-privatekey.txt");
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::information(this, tr("Unable to open file"), file.errorString());
            return;
        }        
        QTextStream out(&file);
        out << pui.privKeyTxt->toPlainText();
    });

    // Call the API
    auto isDialogAlive = std::make_shared<bool>(true);

    auto fnUpdateUIWithKeys = [=](QList<QPair<QString, QString>> privKeys) {
        // Check to see if we are still showing.
        if (! *(isDialogAlive.get()) ) return;

        QString allKeysTxt;
        for (auto keypair : privKeys) {
            allKeysTxt = allKeysTxt % keypair.second % " # addr=" % keypair.first % "\n";
        }

        pui.privKeyTxt->setPlainText(allKeysTxt);
        pui.buttonBox->button(QDialogButtonBox::Save)->setEnabled(true);
    };

    if (allKeys) {
        rpc->getAllPrivKeys(fnUpdateUIWithKeys);
    }
    else {        
        auto fnAddKey = [=](json key) {
            QList<QPair<QString, QString>> singleAddrKey;
            singleAddrKey.push_back(QPair<QString, QString>(addr, QString::fromStdString(key.get<json::string_t>())));
            fnUpdateUIWithKeys(singleAddrKey);
        };

        if (Settings::getInstance()->isZAddress(addr)) {
            rpc->getZPrivKey(addr, fnAddKey);
        }
        else {
            rpc->getTPrivKey(addr, fnAddKey);
        }        
    }
    
    d.exec();
    *isDialogAlive = false;
}

void MainWindow::setupBalancesTab() {
    ui->unconfirmedWarning->setVisible(false);

    // P0-6: build the prominent sync banner that sits above the balances.
    setupSyncBanner();

    // Phase-2 redesign (privacy badges): install the PrivacyBadgeDelegate on the
    // balances table WITHOUT any .ui change. BalancesTableModel column layout
    // (balancestablemodel.cpp headerData/data): col 0 = Address, col 1 = Amount.
    // The address column gets the privacy pill; the amount column gets the
    // monospace/tabular/sign-tinted render. (setItemDelegateForColumn works
    // before or after the model is set in RPC::RPC.)
    ui->balancesTable->setItemDelegateForColumn(
        0, new PrivacyBadgeDelegate(PrivacyBadgeDelegate::Mode::Address, ui->balancesTable));
    ui->balancesTable->setItemDelegateForColumn(
        1, new PrivacyBadgeDelegate(PrivacyBadgeDelegate::Mode::Amount, ui->balancesTable));

    // Double click on balances table
    auto fnDoSendFrom = [=](const QString& addr, const QString& to = QString(), bool sendMax = false) {
        // Find the inputs combo
        for (int i = 0; i < ui->inputsCombo->count(); i++) {
            auto inputComboAddress = ui->inputsCombo->itemText(i);
            if (inputComboAddress.startsWith(addr)) {
                ui->inputsCombo->setCurrentIndex(i);
                break;
            }
        }

        // If there's a to address, add that as well
        if (!to.isEmpty()) {
            // Remember to clear any existing address fields, because we are creating a new transaction.
            this->removeExtraAddresses();
            ui->Address1->setText(to);
        }

        // See if max button has to be checked
        if (sendMax) {
            ui->Max1->setChecked(true);
        }

        // And switch to the send tab.
        ui->tabWidget->setCurrentIndex(1);
    };

    // Double click opens up memo if one exists
    QObject::connect(ui->balancesTable, &QTableView::doubleClicked, [=](auto index) {
        index = index.sibling(index.row(), 0);
        auto addr = AddressBook::addressFromAddressLabel(ui->balancesTable->model()->data(index).toString());
        
        fnDoSendFrom(addr);
    });

    // Setup context menu on balances tab
    ui->balancesTable->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(ui->balancesTable, &QTableView::customContextMenuRequested, [=] (QPoint pos) {
        QModelIndex index = ui->balancesTable->indexAt(pos);
        if (index.row() < 0) return;

        index = index.sibling(index.row(), 0);
        auto addr = AddressBook::addressFromAddressLabel(
                            ui->balancesTable->model()->data(index).toString());

        QMenu menu(this);

        menu.addAction(tr("Copy address"), [=] () {
            QClipboard *clipboard = QGuiApplication::clipboard();
            clipboard->setText(addr);            
            ui->statusBar->showMessage(tr("Copied to clipboard"), 3 * 1000);
        });

        menu.addAction(tr("Get private key"), [=] () {
            this->exportKeys(addr);
        });

        menu.addAction("Send from " % addr.left(40) % (addr.size() > 40 ? "..." : ""), [=]() {
            fnDoSendFrom(addr);
        });

        if (addr.startsWith("t")) {
            auto defaultSapling = rpc->getDefaultSaplingAddress();
            if (!defaultSapling.isEmpty()) {
                menu.addAction(tr("Shield balance to Sapling"), [=] () {
                    fnDoSendFrom(addr, defaultSapling, true);
                });
            }

            menu.addAction(tr("View on block explorer"), [=] () {
                QString url = Settings::getExplorerAddressURL(addr);
                if (!url.isEmpty())
                    QDesktopServices::openUrl(QUrl(url));
            });
        }

        if (Settings::getInstance()->isSproutAddress(addr)) {
            menu.addAction(tr("Migrate to Sapling"), [=] () {
                this->turnstileDoMigration(addr);
            });
        }

        menu.exec(ui->balancesTable->viewport()->mapToGlobal(pos));            
    });
}

void MainWindow::setupZClassicdTab() {    
    ui->zclassicdlogo->setBasePixmap(QPixmap(":/img/res/zclassicdlogo.gif"));
}

void MainWindow::setupTransactionsTab() {
    // Phase-2 redesign (privacy badges): install the PrivacyBadgeDelegate on the
    // transactions table WITHOUT any .ui change. TxTableModel column layout
    // (txtablemodel.cpp ctor/data): col 0 = Type, col 1 = Address, col 2 =
    // Date/Time, col 3 = Amount. The address column gets the privacy pill (and,
    // for a send to a transparent recipient, the red De-shield variant — the
    // delegate reads the sibling Type cell); the amount column gets the
    // monospace/tabular/sign-tinted render.
    ui->transactionsTable->setItemDelegateForColumn(
        1, new PrivacyBadgeDelegate(PrivacyBadgeDelegate::Mode::Address, ui->transactionsTable));
    ui->transactionsTable->setItemDelegateForColumn(
        3, new PrivacyBadgeDelegate(PrivacyBadgeDelegate::Mode::Amount, ui->transactionsTable));

    // Double click opens up memo if one exists
    QObject::connect(ui->transactionsTable, &QTableView::doubleClicked, [=] (auto index) {
        auto txModel = dynamic_cast<TxTableModel *>(ui->transactionsTable->model());
        QString memo = txModel->getMemo(index.row());

        if (!memo.isEmpty()) {
            QMessageBox mb(QMessageBox::Information, tr("Memo"), memo, QMessageBox::Ok, this);
            mb.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            mb.exec();
        }
    });

    // Set up context menu on transactions tab
    ui->transactionsTable->setContextMenuPolicy(Qt::CustomContextMenu);

    // Table right click
    QObject::connect(ui->transactionsTable, &QTableView::customContextMenuRequested, [=] (QPoint pos) {
        QModelIndex index = ui->transactionsTable->indexAt(pos);
        if (index.row() < 0) return;

        QMenu menu(this);

        auto txModel = dynamic_cast<TxTableModel *>(ui->transactionsTable->model());

        QString txid = txModel->getTxId(index.row());
        QString memo = txModel->getMemo(index.row());
        QString addr = txModel->getAddr(index.row());

        menu.addAction(tr("Copy txid"), [=] () {            
            QGuiApplication::clipboard()->setText(txid);
            ui->statusBar->showMessage(tr("Copied to clipboard"), 3 * 1000);
        });

        if (!addr.isEmpty()) {
            menu.addAction(tr("Copy address"), [=] () {
                QGuiApplication::clipboard()->setText(addr);
                ui->statusBar->showMessage(tr("Copied to clipboard"), 3 * 1000);
            });
        }

        menu.addAction(tr("View on block explorer"), [=] () {
            QString url = Settings::getExplorerTxURL(txid);
            if (!url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        });

        if (!memo.isEmpty()) {
            menu.addAction(tr("View Memo"), [=] () {
                QMessageBox mb(QMessageBox::Information, tr("Memo"), memo, QMessageBox::Ok, this);
                mb.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                mb.exec();
            });
        }

        // If memo contains a reply to address, add a "Reply to" menu item
        if (!memo.isEmpty()) {
            int lastPost     = memo.trimmed().lastIndexOf(QRegExp("[\r\n]+"));
            QString lastWord = memo.right(memo.length() - lastPost - 1);
            
            if (Settings::getInstance()->isSaplingAddress(lastWord) || 
                Settings::getInstance()->isSproutAddress(lastWord)) {
                menu.addAction(tr("Reply to ") + lastWord.left(25) + "...", [=]() {
                    // First, cancel any pending stuff in the send tab by pretending to click
                    // the cancel button
                    cancelButton();

                    // Then set up the fields in the send tab
                    ui->Address1->setText(lastWord);
                    ui->Address1->setCursorPosition(0);
                    ui->Amount1->setText("0.0001");

                    // And switch to the send tab.
                    ui->tabWidget->setCurrentIndex(1);

                    qApp->processEvents();

                    // Click the memo button
                    this->memoButtonClicked(1, true);
                });
            }
        }

        menu.exec(ui->transactionsTable->viewport()->mapToGlobal(pos));        
    });
}

void MainWindow::addNewZaddr(bool sapling) {

    rpc->newZaddr(sapling, [=] (json reply) {
        QString addr = QString::fromStdString(reply.get<json::string_t>());
        // Make sure the RPC class reloads the z-addrs for future use
        rpc->refreshAddresses();

        // Just double make sure the z-address is still checked
        if (( sapling && ui->rdioZSAddr->isChecked()) ||
            (!sapling && ui->rdioZAddr->isChecked())) {
            ui->listRecieveAddresses->insertItem(0, addr); 
            ui->listRecieveAddresses->setCurrentIndex(0);

            ui->statusBar->showMessage(QString::fromStdString("Created new zAddr") %
                                       (sapling ? "(Sapling)" : "(Sprout)"), 
                                       10 * 1000);
        }
    });
}


// Adds sapling or sprout z-addresses to the combo box. Technically, returns a
// lambda, which can be connected to the appropriate signal
std::function<void(bool)> MainWindow::addZAddrsToComboList(bool sapling) {
    return [=] (bool checked) { 
        if (checked && this->rpc->getAllZAddresses() != nullptr) { 
            auto addrs = this->rpc->getAllZAddresses();
            ui->listRecieveAddresses->clear();

            std::for_each(addrs->begin(), addrs->end(), [=] (auto addr) {
                if ( (sapling &&  Settings::getInstance()->isSaplingAddress(addr)) ||
                    (!sapling && !Settings::getInstance()->isSaplingAddress(addr))) {
                        if (rpc->getAllBalances()) {
                            auto bal = rpc->getAllBalances()->value(addr);
                            ui->listRecieveAddresses->addItem(addr, bal);
                        }
                }
            }); 

            // If z-addrs are empty, then create a new one.
            if (addrs->isEmpty()) {
                addNewZaddr(sapling);
            }
        } 
    };
}

void MainWindow::setupRecieveTab() {
    auto addNewTAddr = [=] () {
        rpc->newTaddr([=] (json reply) {
            QString addr = QString::fromStdString(reply.get<json::string_t>());

            // Just double make sure the t-address is still checked
            if (ui->rdioTAddr->isChecked()) {
                ui->listRecieveAddresses->insertItem(0, addr);
                ui->listRecieveAddresses->setCurrentIndex(0);

                ui->statusBar->showMessage(tr("Created new t-Addr"), 10 * 1000);
            }
        });
    };

    // Connect t-addr radio button
    QObject::connect(ui->rdioTAddr, &QRadioButton::toggled, [=] (bool checked) {
        // Whenever the t-address is selected, we generate a new address, because we don't
        // want to reuse t-addrs
        if (checked && this->rpc->getUTXOs() != nullptr) {
            updateTAddrCombo(checked);
            addNewTAddr();
        }
        if (checked) {
            // Transparent addresses are PUBLIC -- make that impossible to miss.
            // Reuses the existing red warning label (the Sapling/Sprout branches set
            // their own text/visibility), so no new widget is needed.
            ui->lblSproutWarning->setText(tr(
                "<html><head/><body><p><b>Transparent address.</b> This address and any "
                "balance it holds are <b>PUBLIC</b> and permanently visible to everyone "
                "on the blockchain. For privacy, receive to a shielded (z) address "
                "instead.</p></body></html>"));
            ui->lblSproutWarning->setVisible(true);
        }
    });

    // Sprout Warning is hidden by default
    ui->lblSproutWarning->setVisible(false);

    // zAddr toggle button, one for sprout and one for sapling
    QObject::connect(ui->rdioZAddr, &QRadioButton::toggled, [=](bool checked) {
        ui->btnRecieveNewAddr->setEnabled(!checked);
        if (checked) {
            ui->btnRecieveNewAddr->setToolTip(tr("Creation of new Sprout addresses is deprecated"));
        }
        else {
            ui->btnRecieveNewAddr->setToolTip("");
        }
        
        addZAddrsToComboList(false)(checked);

        // Legacy Sprout deprecation warning (only on the pre-2.0.4 daemon). Set the
        // text explicitly here because the t-Addr branch reuses this same label for
        // its PUBLIC warning, so we must restore the Sprout message when re-showing.
        bool showWarning = checked && Settings::getInstance()->getZClassicdVersion() < 2000425;
        if (showWarning) {
            ui->lblSproutWarning->setText(tr(
                "<html><head/><body><p>You should suspend trust in the receipt of funds "
                "to Sprout z-addresses until you upgrade to zclassicd v2.0.4. See "
                "<a href=\"https://z.cash/support/security/announcements/security-announcement-2019-03-19/\">"
                "<span style=\" text-decoration: underline; color:#0000ff;\">Security Announcement</span></a>."
                "</p></body></html>"));
        }
        ui->lblSproutWarning->setVisible(showWarning);
    });

    QObject::connect(ui->rdioZSAddr, &QRadioButton::toggled, [=] (bool checked) {
        addZAddrsToComboList(true)(checked);
        // Sapling is shielded/private: clear any transparent-or-Sprout warning text
        // the other radios may have left visible.
        if (checked)
            ui->lblSproutWarning->setVisible(false);
    });

    // Explicitly get new address button.
    QObject::connect(ui->btnRecieveNewAddr, &QPushButton::clicked, [=] () {
        if (!rpc->getConnection())
            return;

        // New-address creation is shielded Sapling or transparent only. Creating NEW
        // legacy Sprout addresses is removed (Sprout is deprecated); rdioZAddr remains
        // purely a read/select view of any EXISTING Sprout funds -- spending them is
        // unaffected. Under rdioZAddr the button is already disabled (toggle handler),
        // so dropping this branch is also a belt-and-suspenders guard against a
        // user ever minting a fresh deprecated Sprout address.
        if (ui->rdioZSAddr->isChecked()) {
            addNewZaddr(true);
        } else if (ui->rdioTAddr->isChecked()) {
            addNewTAddr();
        }
    });

    // Focus enter for the Receive Tab
    QObject::connect(ui->tabWidget, &QTabWidget::currentChanged, [=] (int tab) {
        if (tab == 2) {
            // Switched to receive tab, so update everything. 

            // Hide Sapling radio button if Sapling is not active
            if (Settings::getInstance()->isSaplingActive()) {
                ui->rdioZSAddr->setVisible(true);    
                ui->rdioZSAddr->setChecked(true);
                ui->rdioZAddr->setText("z-Addr(Legacy Sprout)");
            } else {
                ui->rdioZSAddr->setVisible(false);    
                ui->rdioZAddr->setChecked(true);
                ui->rdioZAddr->setText("z-Addr");   // Don't use the "Sprout" label if there's no Sapling
            }
            
            // And then select the first one
            ui->listRecieveAddresses->setCurrentIndex(0);
        }
    });

    // Validator for label
    QRegExpValidator* v = new QRegExpValidator(QRegExp(Settings::labelRegExp), ui->rcvLabel);
    ui->rcvLabel->setValidator(v);

    // Select item in address list
    QObject::connect(ui->listRecieveAddresses, 
        QOverload<int>::of(&QComboBox::currentIndexChanged), [=] (int index) {
        QString addr = ui->listRecieveAddresses->itemText(index);
        if (addr.isEmpty()) {
            // Draw empty stuff

            ui->rcvLabel->clear();
            ui->rcvBal->clear();
            ui->txtRecieve->clear();
            ui->qrcodeDisplay->clear();
            return;
        }

        auto label = AddressBook::getInstance()->getLabelForAddress(addr);
        if (label.isEmpty()) {
            ui->rcvUpdateLabel->setText("Add Label");
        }
        else {
            ui->rcvUpdateLabel->setText("Update Label");
        }
        
        ui->rcvLabel->setText(label);
        ui->rcvBal->setText(Settings::getZCLUSDDisplayFormat(rpc->getAllBalances()->value(addr)));
        ui->txtRecieve->setPlainText(addr);       
        ui->qrcodeDisplay->setQrcodeString(addr);
        if (rpc->getUsedAddresses()->value(addr, false)) {
            ui->rcvBal->setToolTip(tr("Address has been previously used"));
        } else {
            ui->rcvBal->setToolTip(tr("Address is unused"));
        }
        
    });    

    // Receive tab add/update label
    QObject::connect(ui->rcvUpdateLabel, &QPushButton::clicked, [=]() {
        QString addr = ui->listRecieveAddresses->currentText();
        if (addr.isEmpty())
            return;

        auto curLabel = AddressBook::getInstance()->getLabelForAddress(addr);
        auto label = ui->rcvLabel->text().trimmed();

        if (curLabel == label)  // Nothing to update
            return;

        QString info;

        if (!curLabel.isEmpty() && label.isEmpty()) {
            info = "Removed Label '" % curLabel % "'";
            AddressBook::getInstance()->removeAddressLabel(curLabel, addr);
        }
        else if (!curLabel.isEmpty() && !label.isEmpty()) {
            info = "Updated Label '" % curLabel % "' to '" % label % "'";
            AddressBook::getInstance()->updateLabel(curLabel, addr, label);
        }
        else if (curLabel.isEmpty() && !label.isEmpty()) {
            info = "Added Label '" % label % "'";
            AddressBook::getInstance()->addAddressLabel(label, addr);
        }

        // Update labels everywhere on the UI
        updateLabels();

        // Show the user feedback
        if (!info.isEmpty()) {
            QMessageBox::information(this, "Label", info, QMessageBox::Ok);
        }
    });

    // Recieve Export Key
    QObject::connect(ui->exportKey, &QPushButton::clicked, [=]() {
        QString addr = ui->listRecieveAddresses->currentText();
        if (addr.isEmpty())
            return;

        this->exportKeys(addr);
    });
}

void MainWindow::updateTAddrCombo(bool checked) {
    if (checked) {
        auto utxos = this->rpc->getUTXOs();
        ui->listRecieveAddresses->clear();

        std::for_each(utxos->begin(), utxos->end(), [=](auto& utxo) {
            auto addr = utxo.address;
            if (addr.startsWith("t") && ui->listRecieveAddresses->findText(addr) < 0) {
                auto bal = rpc->getAllBalances()->value(addr);
                ui->listRecieveAddresses->addItem(addr, bal);
            }
        });
    }
};

// Updates the labels everywhere on the UI. Call this after the labels have been updated
void MainWindow::updateLabels() {
    // Update the Receive tab
    if (ui->rdioTAddr->isChecked()) {
        updateTAddrCombo(true);
    }
    else {
        addZAddrsToComboList(ui->rdioZSAddr->isChecked())(true);
    }

    // Update the Send Tab
    updateFromCombo();

    // Update the autocomplete
    updateLabelsAutoComplete();
}

MainWindow::~MainWindow()
{
    delete ui;
    delete rpc;
    delete labelCompleter;

    delete loadingMovie;
    delete logger;
}

#ifdef ZCL_WIDGET_TEST
// TEST-ONLY SEAM (L1 widget tests). Compiled in ONLY under ZCL_WIDGET_TEST;
// NEVER present in the shipped app. Installs an empty balances map on the RPC so
// confirmTx()'s rpc->getAllBalances()->value(...) deref is safe in-process
// without a live daemon (the value() of an absent key is 0, which is fine for a
// pure UX-warning assertion).
void MainWindow::testSeedBalances() {
    rpc->testSetBalances(new QMap<QString, double>());
}
#endif
