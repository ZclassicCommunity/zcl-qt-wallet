#include "connection.h"
#include "mainwindow.h"
#include "settings.h"
#include "ui_connection.h"
#include "ui_createzclassicconfdialog.h"
#include "rpc.h"

#include "precompiled.h"

#include <QStorageInfo>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QStringList>
#include <QCryptographicHash>

using json = nlohmann::json;

ConnectionLoader::ConnectionLoader(MainWindow* main, RPC* rpc) {
    this->main = main;
    this->rpc  = rpc;

    d = new QDialog(main);
    connD = new Ui_ConnectionDialog();
    connD->setupUi(d);
    QPixmap logo(":/img/res/logobig.gif");
    connD->topIcon->setBasePixmap(logo.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // ---- P1-1: friendly, non-scary staged-onboarding card (built in C++ so the
    // checked-in connection.ui / ui_connection.h stay untouched). It is shown on
    // first run only and stays visible through the initial sync. ----
    ezCard = new QLabel(d);
    ezCard->setWordWrap(true);
    ezCard->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ezCard->setTextFormat(Qt::RichText);
    ezCard->setContentsMargins(16, 8, 16, 8);
    ezCard->setText(QObject::tr(
        "<b>Setting up ZClassic</b><br>"
        "The first run takes about <b>10–20 minutes</b> while your wallet "
        "prepares itself. You can leave it running — it is safe to come back later.<br><br>"
        "&nbsp;&nbsp;<b>Step 1.</b> Getting the security files ready<br>"
        "&nbsp;&nbsp;<b>Step 2.</b> Starting your wallet<br>"
        "&nbsp;&nbsp;<b>Step 3.</b> Catching up with the network"));

    ezProgress = new QProgressBar(d);
    ezProgress->setTextVisible(false);
    // Busy / indeterminate by default; switched to a real 0-100 bar once we have
    // an actual percentage (param download % or block-sync %).
    ezProgress->setRange(0, 0);

    // Insert the card above, and the progress bar below, the status labels.
    // verticalLayout order today: topIcon, line_2, status, statusDetail, line.
    // We want: topIcon, line_2, [ezCard], status, statusDetail, [ezProgress], line.
    int statusIdx = connD->verticalLayout->indexOf(connD->status);
    if (statusIdx < 0) statusIdx = 2;
    connD->verticalLayout->insertWidget(statusIdx, ezCard);
    int lineIdx = connD->verticalLayout->indexOf(connD->line);
    if (lineIdx < 0) lineIdx = connD->verticalLayout->count();
    connD->verticalLayout->insertWidget(lineIdx, ezProgress);

    // Only show the explanatory card the very first time the user ever launches.
    bool firstRun = !QSettings().value("ez/onboardingShown", false).toBool();
    ezCard->setVisible(firstRun);
    if (firstRun)
        QSettings().setValue("ez/onboardingShown", true);

    d->setMinimumWidth(540);
}

ConnectionLoader::~ConnectionLoader() {    
    delete d;
    delete connD;
}

void ConnectionLoader::loadConnection() {
    QTimer::singleShot(1, [=]() { this->doAutoConnect(); });
    if (!Settings::getInstance()->isHeadless())
        d->exec();
}

void ConnectionLoader::doAutoConnect(bool tryEzclassicdStart) {
    // Priority 1: Ensure all params are present.
    if (!verifyParams()) {
        downloadParams([=]() { this->doAutoConnect(); });
        return;
    }

    // Priority 2: Try to connect to detect zclassic.conf and connect to it.
    auto config = autoDetectZClassicConf();
    main->logger->write(QObject::tr("Attempting autoconnect"));

    if (config.get() != nullptr) {
        auto connection = makeConnection(config);

        // P2-1: remember the datadir (holds blocks/, chainstate/, debug.log and
        // wallet.dat) so the corruption failsafe can read debug.log and back up
        // wallet.dat if the node dies before the RPC connection comes up.
        if (ezDataDir.isEmpty() && !config->zclassicDir.isEmpty())
            ezDataDir = config->zclassicDir;

        refreshZClassicdState(connection, [=] () {
            // Connection refused: the embedded node either has not started yet, or
            // it is alive but still warming up -- binding its RPC port can take ~1
            // minute while it loads the block index. Be patient and reassuring;
            // NEVER show a scary error while the node is alive or within a warmup
            // window. (Previously a transient refused during normal warmup could
            // fire a false "Couldn't start the embedded zclassicd" on first launch.)
            if (!Settings::getInstance()->useEmbedded()) {
                main->logger->write("Not using embedded and couldn't connect to zclassicd");
                this->showError(QObject::tr("ZClassic couldn't connect.\n\n"
                    "It is set to use an outside connection that isn't responding. "
                    "Please check your internet connection and try opening ZClassic again."));
                return;
            }

            // Never started yet -> start it once.
            if (ezclassicd == nullptr) {
                this->showInformation(QObject::tr("Step 2 of 3: Starting your wallet…"));
                if (!this->startEmbeddedZClassicd()) {
                    main->logger->write("Could not launch the embedded zclassicd binary");
                    this->showError(QObject::tr("ZClassic couldn't start its node.\n\n"
                        "Please reinstall the app. If the problem continues, make sure your "
                        "security software isn't blocking it."));
                    return;
                }
                ezWarmupTimer.start();
                int probeMs = (ezWarmupTimer.isValid() && ezWarmupTimer.elapsed() < 4000) ? 250 : 1000;
                QTimer::singleShot(probeMs, [=]() { doAutoConnect(); });
                return;
            }

            // Started, RPC not answering yet. While the process is alive -- or within
            // a generous warmup window after it forked into the background -- keep
            // polling with a friendly message instead of ever showing an error.
            if (ezclassicd->state() != QProcess::NotRunning ||
                (ezWarmupTimer.isValid() && ezWarmupTimer.elapsed() < 120000)) {
                this->showInformation(QObject::tr("Step 2 of 3: Starting your wallet…"),
                    QObject::tr("Almost ready — getting things going (this can take a minute)…"));
                // Probe quickly during the first few seconds of warmup so warm restarts
                // reconnect without waiting a full extra second, then settle to 1s.
                int probeMs = (ezWarmupTimer.isValid() && ezWarmupTimer.elapsed() < 4000) ? 250 : 1000;
                QTimer::singleShot(probeMs, [=]() { doAutoConnect(); });
                return;
            }

            // Process is genuinely dead and the warmup window elapsed. Before
            // showing a generic error, look at WHY it died: a corrupt or
            // partway block/chainstate database has a safe, staged repair path.
            main->logger->write("Embedded zclassicd exited and did not come online");
            this->handleStartupFailure();
        });
    } else {
        if (Settings::getInstance()->useEmbedded()) {
            // zclassic.conf was not found, so create one
            createZClassicConf();
        } else {
            // Fall back to manual connect
            doManualConnect();
        }
    } 
}

QString randomPassword() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    const int passwordLength = 10;
    char* s = new char[passwordLength + 1];

    for (int i = 0; i < passwordLength; ++i) {
        s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    }

    s[passwordLength] = 0;
    return QString::fromStdString(s);
}

/**
 * This will create a new zclassic.conf, download ZClassic parameters.
 */ 
void ConnectionLoader::createZClassicConf() {
    main->logger->write("createZClassicConf");

    auto confLocation = zclassicConfWritableLocation();
    QFileInfo fi(confLocation);

    QDialog d(main);
    Ui_createZClassicConf ui;
    ui.setupUi(&d);

    QPixmap logo(":/img/res/zclassicdlogo.gif");
    ui.lblTopIcon->setBasePixmap(logo.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui.btnPickDir->setEnabled(false);

    ui.grpAdvanced->setVisible(false);
    QObject::connect(ui.btnAdvancedConfig, &QPushButton::toggled, [=](bool isVisible) {
        ui.grpAdvanced->setVisible(isVisible);
        ui.btnAdvancedConfig->setText(isVisible ? QObject::tr("Hide Advanced Config") : QObject::tr("Show Advanced Config"));
    });

    QObject::connect(ui.chkCustomDatadir, &QCheckBox::stateChanged, [=](int chked) {
        if (chked == Qt::Checked) {
            ui.btnPickDir->setEnabled(true);
        }
        else {
            ui.btnPickDir->setEnabled(false);
        }
    });

    QObject::connect(ui.btnPickDir, &QPushButton::clicked, [=]() {
        auto datadir = QFileDialog::getExistingDirectory(main, QObject::tr("Choose data directory"), ui.lblDirName->text(), QFileDialog::ShowDirsOnly);
        if (!datadir.isEmpty()) {
            ui.lblDirName->setText(QDir::toNativeSeparators(datadir));
        }
    });

    // Show the dialog
    QString datadir = "";
    bool useTor = false;
    if (d.exec() == QDialog::Accepted) {
        datadir = ui.lblDirName->text();
        useTor = ui.chkUseTor->isChecked();
    }

    main->logger->write("Creating file " + confLocation);
    QDir().mkpath(fi.dir().absolutePath());

    QFile file(confLocation);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        main->logger->write("Could not create zclassic.conf, returning");
        // Don't silently strand the user on the connecting screen forever — tell
        // them what went wrong so they can fix permissions / free disk space.
        this->showError(QObject::tr("ZClassic couldn't save its configuration file.\n\n"
            "Please make sure you have permission to write to your home folder and "
            "enough free disk space, then open ZClassic again."));
        return;
    }
        
    QTextStream out(&file); 
    
    out << "server=1\n";
    // No addnode here: mainnet.z.cash is a Zcash (ZEC) node, not a valid ZClassic
    // peer, and the zclassicd we drive already pins its own compiled bootstrap
    // peers / seeds as -addnode on every start. Baking a wrong peer into the
    // user's permanent conf only causes repeated failed connections.
    out << "rpcuser=zcl-qt-wallet\n";
    out << "rpcpassword=" % randomPassword() << "\n";
    if (!datadir.isEmpty()) {
        out << "datadir=" % datadir % "\n";
    }
    if (useTor) {
        out << "proxy=127.0.0.1:9050\n";
    }

    file.close();

    // Now that zclassic.conf exists, try to autoconnect again
    this->doAutoConnect();
}


void ConnectionLoader::downloadParams(std::function<void(void)> cb) {    
    main->logger->write("Adding params to download queue");

    // P1-7: make sure there is enough room before pulling ~1.7GB of security files.
    if (!ensureEnoughDiskSpace(zcashParamsDir()))
        return;

    // P1-1: this is Step 1 of onboarding.
    this->showInformation(QObject::tr("Step 1 of 3: Getting the security files ready…"),
                          QObject::tr("One-time download of about 1.7 GB — on a typical home connection this takes 10–20 minutes. You only do this once."));

    // Add all the files to the download queue
    downloadQueue = new QQueue<QUrl>();
    client = new QNetworkAccessManager(main);   
    
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sapling-output.params"));
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sapling-spend.params"));    
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sprout-proving.key"));
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sprout-verifying.key"));
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sprout-groth16.params"));

    doNextDownload(cb);    
}

void ConnectionLoader::doNextDownload(std::function<void(void)> cb) {
    auto fnSaveFileName = [&] (QUrl url) {
        QString path = url.path();
        QString basename = QFileInfo(path).fileName();

        return basename;
    };

    if (downloadQueue->isEmpty()) {
        delete downloadQueue;
        downloadQueue = nullptr;
        client->deleteLater();
        client = nullptr;

        main->logger->write("All Downloads done");
        ezRetryCount = 0;
        // Back to a busy bar; the node-start / sync phase has no per-file %.
        if (ezProgress) ezProgress->setRange(0, 0);
        this->showInformation(QObject::tr("Step 1 of 3: Security files ready."),
                              QObject::tr("Starting your wallet…"));
        cb();
        return;
    }

    QUrl url = downloadQueue->head();   // peek; only dequeue on success so retries can re-pull
    int totalFiles = 5;                 // we enqueue 5 param files
    int fileNumber = totalFiles - downloadQueue->size() + 1;

    QString filename = fnSaveFileName(url);
    QString paramsDir = zcashParamsDir();

    if (QFile(QDir(paramsDir).filePath(filename)).exists()) {
        main->logger->write(filename + " already exists, skipping");
        downloadQueue->dequeue();
        doNextDownload(cb);

        return;
    }

    // The downloaded file is written to a new name, and then renamed when the operation completes.
    currentOutput = new QFile(QDir(paramsDir).filePath(filename + ".part"));   

    if (!currentOutput->open(QIODevice::WriteOnly)) {
        main->logger->write("Couldn't open " + currentOutput->fileName() + " for writing");
        delete currentOutput;
        currentOutput = nullptr;
        // P1-2 fix: must NOT fall through after a failed open.
        this->showError(QObject::tr("ZClassic couldn't save the security files.\n\n"
            "Please make sure you have enough free disk space, then open ZClassic again."));
        return;
    }
    main->logger->write("Downloading to " + filename);
    qDebug() << "Downloading " << url << " to " << filename;
    
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    // P1-5: a stalled socket should fail fast and retry instead of hanging forever.
    // NOTE: in Qt 5.15 setTransferTimeout lives on QNetworkRequest / QNetworkAccessManager,
    // NOT on QNetworkReply, so it is set here on the request before get().
    request.setTransferTimeout(30000);
    currentDownload = client->get(request);
    downloadTime.start();
    
    // Download Progress
    QObject::connect(currentDownload, &QNetworkReply::downloadProgress, [=] (auto done, auto total) {
        // calculate the download speed
        double speed = done * 1000.0 / (downloadTime.elapsed() > 0 ? downloadTime.elapsed() : 1);
        QString unit;
        double dispSpeed = speed;
        if (dispSpeed < 1024) {
            unit = " bytes/sec";
        } else if (dispSpeed < 1024*1024) {
            dispSpeed /= 1024;
            unit = " kB/s";
        } else {
            dispSpeed /= 1024*1024;
            unit = " MB/s";
        }

        // P1-2: aggregate, friendly progress. Drive the real 0-100 bar off this
        // file's percentage, and show file X of N plus an ETA for the current file.
        QString eta;
        if (total > 0 && speed > 1) {
            int secsLeft = (int)((total - done) / speed);
            if (secsLeft > 90)
                eta = QObject::tr(", about ") % QString::number(secsLeft / 60) % QObject::tr(" min left");
            else if (secsLeft > 0)
                eta = QObject::tr(", about ") % QString::number(secsLeft) % QObject::tr(" sec left");
        }
        if (total > 0)
            this->setBarPercent((int)(done * 100 / total));

        this->showInformation(
            QObject::tr("Step 1 of 3: Getting the security files ready…"),
            QObject::tr("File ") % QString::number(fileNumber) % QObject::tr(" of ") % QString::number(totalFiles)
                % " — "
                % QString::number(done/1024.0/1024.0, 'f', 0) % QObject::tr(" MB of ")
                % (total > 0 ? QString::number(total/1024.0/1024.0, 'f', 0) : QObject::tr("?")) % QObject::tr(" MB at ")
                % QString::number(dispSpeed, 'f', 1) % unit % eta);
    });
    
    // Download new data available. 
    QObject::connect(currentDownload, &QNetworkReply::readyRead, [=] () {
        if (currentOutput)
            currentOutput->write(currentDownload->readAll());
    });    

    // Download Finished
    QObject::connect(currentDownload, &QNetworkReply::finished, [=] () {
        bool failed = (currentDownload->error() != QNetworkReply::NoError);

        if (currentOutput) {
            currentOutput->close();
        }
        currentDownload->deleteLater();

        // C10: a "successful" transfer that is implausibly small is almost certainly
        // a captive-portal/redirect HTML page or a truncated body, not a real param
        // (all 5 real params are far larger than 1 MB). Treat it as a failed download
        // so it falls into the same auto-retry / offerRetry path below.
        if (!failed && currentOutput && currentOutput->size() < 1000000) {
            main->logger->write(QString("Downloaded %1 is too small (%2 bytes); treating as failed")
                                    .arg(filename).arg(currentOutput->size()));
            failed = true;
        }

        if (failed) {
            main->logger->write("Downloading " + filename + " failed: " + currentDownload->errorString());
            // Drop the partial file so a retry starts clean.
            if (currentOutput) {
                currentOutput->remove();
                currentOutput->deleteLater();
                currentOutput = nullptr;
            }

            // P1-2/P1-5: auto-retry the same file a few times, then offer a Retry button
            // instead of a dead end.
            if (ezRetryCount < 3) {
                ezRetryCount++;
                main->logger->write(QString("Auto-retrying download (attempt %1)").arg(ezRetryCount));
                this->showInformation(QObject::tr("Step 1 of 3: Getting the security files ready…"),
                                      QObject::tr("Connection hiccup — retrying…"));
                QTimer::singleShot(1500, [=]() { this->doNextDownload(cb); });
            } else {
                this->offerRetry(QObject::tr("The setup files couldn't finish downloading.\n\n"
                    "This is almost always a temporary internet problem. Check your connection "
                    "(Wi-Fi / ethernet / phone hotspot), then tap Retry. Your progress so far is kept."), cb);
            }
            return;
        }

        // Success: rename the .part into place and move on.
        main->logger->write("Finished downloading " + filename);
        if (currentOutput) {
            currentOutput->rename(QDir(paramsDir).filePath(filename));
            currentOutput->deleteLater();
            currentOutput = nullptr;
        }
        ezRetryCount = 0;
        downloadQueue->dequeue();
        doNextDownload(cb);
    });
}

bool ConnectionLoader::startEmbeddedZClassicd(const QStringList& extraArgs) {
    if (!Settings::getInstance()->useEmbedded())
        return false;

    main->logger->write("Trying to start embedded zclassicd");

    if (ezclassicd != nullptr) {
        if (ezclassicd->state() == QProcess::NotRunning) {
            // The node object already exists but the process is dead. This is the
            // failsafe's job now: handleStartupFailure() inspects stderr/debug.log
            // and offers a staged repair. We do NOT show a generic dialog here, and
            // we do NOT relaunch the same dead QProcess. Caller handles the result.
            if (!ezStdErr.isEmpty())
                main->logger->write("node stderr: " + ezStdErr);
            return false;
        } else {
            return true;
        }
    }

    // Finally, start zclassicd. Resolution order is SIBLING-FIRST so that the
    // dev build, the .deb and the macOS .app (all of which ship a real daemon
    // file next to / inside the bundle) keep working exactly as before. Only the
    // single-file release -- where no sibling exists because the daemon is
    // appended to our own executable -- falls back to extracting it.
    QDir appPath(QCoreApplication::applicationDirPath());
    QString zclassicdProgram;
#ifdef Q_OS_LINUX
    zclassicdProgram = appPath.absoluteFilePath("zqw-zclassicd");
    if (!QFile(zclassicdProgram).exists())
        zclassicdProgram = appPath.absoluteFilePath("zclassicd");
    if (!QFile(zclassicdProgram).exists())
        zclassicdProgram = ensureDaemonExtracted();
#elif defined(Q_OS_DARWIN)
    // No runtime extraction on macOS: notarization / hardened runtime forbid
    // executing a binary written at runtime, so the signed daemon ships as a
    // sibling inside Contents/MacOS and ensureDaemonExtracted() returns empty.
    zclassicdProgram = appPath.absoluteFilePath("zclassicd");
#else
    zclassicdProgram = appPath.absoluteFilePath("zclassicd.exe");
    if (!QFile(zclassicdProgram).exists())
        zclassicdProgram = ensureDaemonExtracted();   // absolute path under %LOCALAPPDATA%
#endif

    if (zclassicdProgram.isEmpty() || !QFile(zclassicdProgram).exists()) {
        qDebug() << "Can't find zclassicd at " << zclassicdProgram;
        main->logger->write("Can't find zclassicd at " + zclassicdProgram);
        return false;
    }

    // Reset per-launch failsafe state so each (re)launch is judged on its own output.
    ezNodeQuitDuringStartup = false;
    ezStdErr.clear();

    ezclassicd = new QProcess(main);
    QObject::connect(ezclassicd, &QProcess::started, [=] () {
        //qDebug() << "zclassicd started";
    });

    // A. DETECT: if the node EXITS before the RPC connection is established (i.e.
    // while ezWarmupTimer is still inside its window), record that it quit during
    // startup. doRPCSetConnection() clears ezWarmupTimer when RPC succeeds, so a
    // 'finished' after that is a normal shutdown and is ignored here.
    QObject::connect(ezclassicd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [=](int exitCode, QProcess::ExitStatus exitStatus) {
        main->logger->write(QString("zclassicd finished: code=%1 status=%2")
                            .arg(exitCode).arg((int)exitStatus));
        // Drain any final stderr the readyRead signal hasn't delivered yet.
        if (ezclassicd) ezStdErr.append(ezclassicd->readAllStandardError());
        if (ezWarmupTimer.isValid())
            ezNodeQuitDuringStartup = true;   // died before we ever connected
    });

    // A. DETECT: failure to even launch (missing exec permission, AV block, etc.).
    QObject::connect(ezclassicd, &QProcess::errorOccurred, [=] (QProcess::ProcessError error) {
        main->logger->write(QString("zclassicd errorOccurred: %1 (%2)")
                            .arg((int)error)
                            .arg(ezclassicd ? ezclassicd->errorString() : QString()));
        if (ezWarmupTimer.isValid())
            ezNodeQuitDuringStartup = true;
    });

    QObject::connect(ezclassicd, &QProcess::readyReadStandardError, [=]() {
        auto output = ezclassicd->readAllStandardError();
        main->logger->write("zclassicd stderr:" + output);
        ezStdErr.append(output);
    });

    // Startup-speed defaults. -dbcache/-par/-maxsigcachesize apply on every launch.
    // -checkblocks is added only on a NORMAL launch (gated below): the default
    // 288-block startup re-verification (~12h of 2.5-min blocks, re-read every launch)
    // is the bulk of the visible warmup; the chain was already validated when first
    // connected and the corruption failsafe (handleStartupFailure) covers a damaged db
    // separately, so capping it is safe -- but during a repair relaunch the caller's
    // -reindex must run full validation, so we skip -checkblocks then.
    QStringList launchArgs;
    launchArgs << "-dbcache=450"          // bigger chainstate cache
               << "-par=0"                // auto-detect cores: parallel script
                                          // verification (startup verify + IBD;
                                          // daemon default is single-threaded)
               << "-maxsigcachesize=128"; // fewer redundant signature re-verifies
    // -checkblocks=12 ONLY on a normal launch. During a repair relaunch the caller
    // passes -reindex / -reindex-chainstate in extraArgs, where capping startup
    // verification would defeat the point of a full re-validation.
    if (extraArgs.isEmpty())
        launchArgs << "-checkblocks=12";
    launchArgs.append(extraArgs);
    // NOTE: ezWarmupTimer is started by the caller (doAutoConnect / relaunchForRepair)
    // right after this returns; do NOT restart it here or the warmup-window check and
    // the startup-timing log would measure from the wrong instant.

#ifdef Q_OS_LINUX
    ezclassicd->start(zclassicdProgram, launchArgs);
#elif defined(Q_OS_DARWIN)
    ezclassicd->start(zclassicdProgram, launchArgs);
#else
    // Spawn by ABSOLUTE path: in the single-file release the daemon is extracted
    // outside appPath (under %LOCALAPPDATA%), so the old bare-name "zclassicd.exe"
    // + working-directory spawn would not find it.
    ezclassicd->setWorkingDirectory(appPath.absolutePath());
    ezclassicd->start(zclassicdProgram, launchArgs);
#endif // Q_OS_LINUX


    return true;
}

// Single-file release: locate the daemon payload appended to our own executable,
// verify it, and extract it to a per-user cache. Layout at the END of the file:
//   [ ...GUI ELF... ][ daemon bytes ][ sha256(daemon) :32 ][ daemon len :8 LE ][ magic :8 ]
// Returns the absolute path of the verified, executable daemon, or an empty
// string to make the caller fall back (no payload / mismatch / macOS).
QString ConnectionLoader::ensureDaemonExtracted() {
#ifdef Q_OS_DARWIN
    return QString();   // never extract+exec on macOS (notarization)
#else
    static const QByteArray MAGIC = QByteArrayLiteral("ZQWDMON1");   // 8 bytes
    const qint64 LEN_BYTES   = 8;
    const qint64 HASH_BYTES  = 32;
    const qint64 FOOTER      = MAGIC.size() + LEN_BYTES + HASH_BYTES;   // 48

    QFile self(QCoreApplication::applicationFilePath());
    if (!self.open(QIODevice::ReadOnly)) {
        main->logger->write("ensureDaemonExtracted: cannot open self for reading: " + self.errorString());
        return QString();
    }
    const qint64 selfSize = self.size();
    if (selfSize < FOOTER)
        return QString();

    // Check the trailing magic; absent on a plain (un-bundled) GUI build.
    self.seek(selfSize - MAGIC.size());
    if (self.read(MAGIC.size()) != MAGIC)
        return QString();

    // Read the little-endian length and the expected hash.
    self.seek(selfSize - MAGIC.size() - LEN_BYTES);
    QByteArray lenBytes = self.read(LEN_BYTES);
    quint64 len = 0;
    for (int i = 0; i < LEN_BYTES; i++)
        len |= (quint64)(uchar)lenBytes.at(i) << (8 * i);

    self.seek(selfSize - MAGIC.size() - LEN_BYTES - HASH_BYTES);
    QByteArray wantHash = self.read(HASH_BYTES);

    const qint64 payloadOffset = selfSize - FOOTER - (qint64)len;
    // Reject a zero/absurd/negative length: a corrupt or tampered 8-byte footer must
    // not drive a multi-hour read that looks like a first-run hang. Real daemon ~13MB.
    if (len == 0 || len > 100ull * 1024 * 1024 || payloadOffset < 0) {
        main->logger->write("ensureDaemonExtracted: invalid embedded payload size=" + QString::number(len));
        return QString();
    }

    // Content-addressed cache dir: name = first 16 hex chars of the daemon hash,
    // so an app upgrade (new daemon) lands in a fresh dir and is re-extracted,
    // while an unchanged daemon is reused. Other stamp dirs are pruned.
    const QString stamp = QString::fromLatin1(wantHash.toHex().left(16));
    QDir cacheRoot(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    cacheRoot.mkpath("zclassic-node");
    QDir nodeDir(cacheRoot.filePath("zclassic-node"));
    for (const QString& d : nodeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (d != stamp)
            QDir(nodeDir.filePath(d)).removeRecursively();
    }
    nodeDir.mkpath(stamp);

#ifdef Q_OS_WIN
    const QString outName = "zqw-zclassicd.exe";
#else
    const QString outName = "zqw-zclassicd";
#endif
    const QString outPath  = QDir(nodeDir.filePath(stamp)).filePath(outName);
    const QString partPath = outPath + ".part";

    auto makeExecutable = [](const QString& p) {
#ifndef Q_OS_WIN
        QFile::setPermissions(p, QFile::permissions(p)
            | QFileDevice::ExeOwner | QFileDevice::ExeUser
            | QFileDevice::ExeGroup | QFileDevice::ExeOther);
#else
        Q_UNUSED(p);
#endif
    };

    // Fast path: already extracted (content-addressed dir + size match). Avoids
    // re-hashing ~14MB on every launch.
    if (QFile::exists(outPath) && QFileInfo(outPath).size() == (qint64)len) {
        makeExecutable(outPath);
        return outPath;
    }
    QFile::remove(outPath);

    // Extract the payload to a .part file, hashing as we go.
    if (!self.seek(payloadOffset)) {
        main->logger->write("ensureDaemonExtracted: seek to payload offset failed");
        return QString();
    }
    // Refuse to start a partial extract on a near-full disk (leaves a junk .part).
    const qint64 avail = QStorageInfo(nodeDir.filePath(stamp)).bytesAvailable();
    if (avail >= 0 && avail < (qint64)len + (16ll << 20)) {
        main->logger->write(QString("ensureDaemonExtracted: insufficient disk: need %1 have %2")
                                .arg((qint64)len + (16ll << 20)).arg(avail));
        return QString();
    }
    QFile out(partPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        main->logger->write("ensureDaemonExtracted: cannot open .part for writing: " + out.errorString());
        return QString();
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    qint64 remaining = (qint64)len;
    const qint64 CHUNK = 1 << 20;
    bool ok = true;
    while (remaining > 0) {
        QByteArray buf = self.read(qMin(CHUNK, remaining));
        if (buf.isEmpty()) { ok = false; break; }
        hasher.addData(buf);
        if (out.write(buf) != buf.size()) { ok = false; break; }
        remaining -= buf.size();
    }
    out.close();

    if (!ok || remaining != 0 || hasher.result() != wantHash) {
        main->logger->write("Embedded daemon payload failed verification; falling back");
        QFile::remove(partPath);
        return QString();
    }

    // Atomically publish, then set the exec bit.
    QFile::remove(outPath);
    if (!QFile::rename(partPath, outPath)) {
        main->logger->write("ensureDaemonExtracted: rename .part -> final failed");
        QFile::remove(partPath);
        return QString();
    }
    makeExecutable(outPath);
    main->logger->write("Extracted embedded daemon to " + outPath);
    return outPath;
#endif // Q_OS_DARWIN
}

// A./B. Detect-and-classify entry point. Called from doAutoConnect once the node
// is confirmed dead and the warmup window has elapsed. Reads the captured stderr
// plus the tail of <datadir>/debug.log, scans for corruption markers, and either
// offers the staged repair ladder (corruption) or shows the existing generic
// friendly error (everything else).
void ConnectionLoader::handleStartupFailure() {
    // Pull the last of the daemon's diagnostics from disk too: stderr may be empty
    // if the daemon logged the corruption only to debug.log before aborting.
    QString diag = ezStdErr;
    QString logTail = readDebugLogTail();
    if (!logTail.isEmpty())
        diag += "\n" + logTail;

    main->logger->write(QString("Startup failure classification; quitDuringStartup=%1, diag bytes=%2")
                        .arg(ezNodeQuitDuringStartup).arg(diag.size()));

    // Disk full: a repair (reindex/re-download) needs MORE space, so it would only
    // fail again or make things worse. Never offer a repair here — say so plainly.
    qint64 freeBytes = ezDataDir.isEmpty() ? -1 : QStorageInfo(resolveDataSubdir()).bytesAvailable();
    if (diag.contains("Disk space is low", Qt::CaseInsensitive) ||
        (freeBytes >= 0 && freeBytes < (qint64)2 * 1024 * 1024 * 1024)) {
        this->showError(QObject::tr("ZClassic is running low on disk space.\n\n"
            "Please free up several gigabytes on this drive, then open ZClassic again. "
            "Your wallet and coins are safe."));
        return;
    }

    // Another copy is already using this data folder (a second wallet window, or a
    // manually started zclassicd). A repair is the wrong action — point at that.
    if (diag.contains("probably already running", Qt::CaseInsensitive) ||
        diag.contains("Cannot obtain a lock", Qt::CaseInsensitive)) {
        this->showError(QObject::tr("ZClassic may already be running.\n\n"
            "Please close any other ZClassic windows (or wait a moment and try again), "
            "then reopen it. Your wallet and coins are safe."));
        return;
    }

    // C. If it looks like a corrupt / partway database, offer the staged repair.
    // Only offer once per run so a user who declines isn't trapped in a loop, and
    // cap repeated attempts across runs so a failing disk can't loop a heavy rebuild.
    if (!ezRepairOffered && looksLikeDbCorruption(diag)) {
        if (QSettings().value("repair/attempts", 0).toInt() >= 3) {
            this->showError(QObject::tr("ZClassic tried to repair its blockchain data several "
                "times but couldn't fix it, which can mean a failing disk.\n\n"
                "Your wallet and coins are safe (a backup copy of your wallet was saved next "
                "to it). You may want to copy your wallet file somewhere safe and have this "
                "computer's disk checked."));
            return;
        }
        ezRepairOffered = true;
        this->offerCorruptionRepair();
        return;
    }

    // Otherwise: the original generic, reassuring message.
    QString detail = ezclassicd ? ezclassicd->errorString() : QString();
    this->showError(QObject::tr("ZClassic couldn't finish starting up.\n\n"
        "This usually clears up on its own — please quit and open ZClassic again. "
        "If it keeps happening, make sure you have enough free disk space and a working "
        "internet connection.")
        % (detail.isEmpty() ? QString("") : QString("\n\n(") % detail % QString(")")));
}

// B. CLASSIFY: scan captured stderr + debug.log tail for the daemon's known
// block/chainstate corruption markers (see src/init.cpp). Case-insensitive.
bool ConnectionLoader::looksLikeDbCorruption(const QString& text) {
    if (text.isEmpty())
        return false;

    static const char* markers[] = {
        "Corrupted block database detected",
        "Error opening block database",
        "Error loading block database",
        "Aborted block database rebuild",
        "Failed to read block",
        "System error while flushing",
        "Do you want to rebuild the block database now"
    };

    for (auto m : markers) {
        if (text.contains(QString::fromLatin1(m), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// The network data subdirectory: the datadir root for mainnet, or testnet3/ when
// that subdir actually holds the chain/wallet. We probe the filesystem rather than
// Settings::isTestnet() because isTestnet() is only set from the getinfo RPC reply,
// which never arrives when the node dies during startup.
QDir ConnectionLoader::resolveDataSubdir() {
    QDir root(ezDataDir);
    QDir testnet(QDir(ezDataDir).filePath("testnet3"));
    if (testnet.exists() &&
        (QFile::exists(testnet.filePath("wallet.dat")) ||
         QFile::exists(testnet.filePath("debug.log")) ||
         QDir(testnet.filePath("blocks")).exists()))
        return testnet;
    return root;
}

// Read the last maxBytes of <datadir>/debug.log, READ-ONLY. Returns "" if the
// datadir/log is unknown or unreadable. Never opens any file for writing.
QString ConnectionLoader::readDebugLogTail(int maxBytes) {
    if (ezDataDir.isEmpty())
        return QString();

    QString logPath = resolveDataSubdir().filePath("debug.log");

    QFile log(logPath);
    if (!log.exists() || !log.open(QIODevice::ReadOnly))
        return QString();

    qint64 sz = log.size();
    if (sz > maxBytes)
        log.seek(sz - maxBytes);
    QString tail = QString::fromUtf8(log.readAll());
    log.close();
    return tail;
}

// C. OFFER A SAFE, STAGED REPAIR LADDER (least to most invasive). The dialog is
// plain-language and reassures the user that their wallet/keys are untouched.
// Step 0 (wallet.dat backup) always runs first inside relaunchForRepair() before
// any repair flag is applied; if that backup fails, the repair is aborted.
void ConnectionLoader::offerCorruptionRepair() {
    main->logger->write("Offering staged corruption repair");

    QMessageBox box(main);
    box.setWindowTitle(QObject::tr("ZClassic needs a quick repair"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QObject::tr("ZClassic couldn't open its blockchain files."));
    box.setTextFormat(Qt::RichText);
    box.setInformativeText(QObject::tr(
        "It looks like the downloaded blockchain data on this computer was left in a "
        "damaged or half-finished state (this can happen if the app or the computer "
        "shut down while it was still working).<br><br>"
        "<b>Your wallet and your coins are safe.</b> The blockchain data is just a local "
        "copy of the public network and can always be rebuilt. None of these options "
        "touch your wallet file or private keys — and ZClassic will make a backup copy "
        "of your wallet first, just to be sure.<br><br>"
        "How would you like to fix it? Start with the fast option; you can try the next "
        "one if it doesn't work.<br><br>") +
        QObject::tr("• Re-download — sets aside the local blockchain and downloads a fresh "
            "copy (largest, slowest)."));

    // Least-invasive first. AcceptRole/ActionRole keep them in a predictable order.
    QPushButton* fastBtn   = box.addButton(QObject::tr("Repair (fast)"),            QMessageBox::AcceptRole);
    QPushButton* rebuildBtn= box.addButton(QObject::tr("Rebuild index (slower)"),   QMessageBox::ActionRole);
    QPushButton* redownBtn = box.addButton(QObject::tr("Re-download blockchain"),   QMessageBox::ActionRole);
    box.addButton(QObject::tr("Not now"),                                           QMessageBox::RejectRole);
    box.setDefaultButton(fastBtn);

    box.exec();
    QAbstractButton* clicked = box.clickedButton();

    if (clicked == fastBtn) {
        // Step 1: rebuild ONLY the chainstate/UTXO set from the existing block files.
        this->relaunchForRepair(QStringList() << "-reindex-chainstate");
    } else if (clicked == rebuildBtn) {
        // Step 2: rebuild the block index too (from the blk files). Slower.
        this->relaunchForRepair(QStringList() << "-reindex");
    } else if (clicked == redownBtn) {
        // Step 3 (last resort): genuinely re-fetch. redownloadChain() sets the local
        // blocks/ and chainstate/ aside and starts the node normally, so it re-runs
        // its automatic bootstrap-snapshot flow. It NEVER touches wallet.dat.
        auto confirm = QMessageBox::question(main, QObject::tr("Re-download the blockchain?"),
            QObject::tr("This downloads a fresh copy of the blockchain and can take a while. "
                "It still does not touch your wallet or private keys.\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (confirm == QMessageBox::Yes)
            this->redownloadChain();
    } else { // "Not now" or dialog closed
        this->showError(QObject::tr("ZClassic couldn't finish starting up.\n\n"
            "You can open ZClassic again whenever you're ready, and choose a repair "
            "option then. Your wallet and coins are safe."));
    }
}

// Step 0 + relaunch. ALWAYS back up wallet.dat (read+copy) first; only if that
// succeeds do we apply the one-shot repair flag and relaunch the node ONCE.
// The flag is passed only to this next start and is never written to zclassic.conf.
void ConnectionLoader::relaunchForRepair(const QStringList& extraArgs) {
    // GUARD RAIL: never proceed to a repair without a fresh wallet.dat backup.
    if (!backupWalletForRepair()) {
        this->showError(QObject::tr("ZClassic couldn't make a safety backup of your wallet, "
            "so the repair was stopped.\n\n"
            "Your wallet and coins have not been changed. Please make sure you have free "
            "disk space and try again."));
        return;
    }

    // Count this repair attempt (persisted per install) so handleStartupFailure
    // can stop escalating after a few failures instead of looping a heavy rebuild.
    QSettings().setValue("repair/attempts", QSettings().value("repair/attempts", 0).toInt() + 1);

    main->logger->write("Relaunching embedded node for repair with args: " + extraArgs.join(" "));

    // Drop the dead QProcess so startEmbeddedZClassicd() creates a fresh one with
    // the one-shot repair flag. We never reuse or relaunch the old process object.
    if (ezclassicd) {
        ezclassicd->deleteLater();
        ezclassicd = nullptr;
    }
    ezNodeQuitDuringStartup = false;
    ezStdErr.clear();

    this->showInformation(QObject::tr("Repairing your blockchain data…"),
        QObject::tr("This can take several minutes. Your wallet is safe."));

    if (!this->startEmbeddedZClassicd(extraArgs)) {
        this->showError(QObject::tr("ZClassic couldn't start the repair.\n\n"
            "Please open ZClassic again. Your wallet and coins are safe."));
        return;
    }

    // Re-arm the warmup window and resume the normal patient polling loop, which
    // will now connect once the reindex completes (or, if it fails again, fall
    // back through handleStartupFailure() — but ezRepairOffered is already set, so
    // the user gets the plain error instead of an endless repair loop).
    ezWarmupTimer.start();
    QTimer::singleShot(1000, [=]() { doAutoConnect(); });
}

// A GENUINE re-download (unlike -reindex, which rebuilds from the same possibly
// corrupt local blk files): set the local blocks/ and chainstate/ aside into ONE
// timestamped parent dir, then start the node normally so that — seeing an empty
// datadir — it re-runs its automatic bootstrap-snapshot flow and fetches a fresh
// chain. (Verified: the shipped daemon auto-bootstraps on an empty/eligible datadir
// with NO flag — -bootstrap defaults to true and -bootstrapmode defaults to
// "anchor" — so a plain start is correct here.) The move is rollback-safe: if any
// directory fails to move, every prior move is undone and the datadir is left
// exactly as found. Disk growth from the set-aside copy is bounded to ONE
// generation: a prior set-aside dir is swept before this one is created, so a
// repeated re-download never accumulates. The single "chain-set-aside-*" parent
// stays in the datadir (renaming within it is instant and recoverable); the user
// can delete it later to reclaim disk space, and the next re-download sweeps it.
//
// This only ever runs from offerCorruptionRepair <- handleStartupFailure, i.e. the
// node process is ALREADY DEAD, so there are no open file handles on these dirs.
void ConnectionLoader::redownloadChain() {
    main->logger->write("redownloadChain: wiping local chain data for fresh re-download");

    // Step 0: always make a fresh wallet.dat safety backup first.
    if (!this->backupWalletForRepair()) {
        this->showError(QObject::tr("ZClassic couldn't make a safety backup of your wallet, "
            "so the re-download was stopped.\n\n"
            "Your wallet and coins have not been changed. Please make sure you have free "
            "disk space and try again."));
        return;
    }

    QDir dd = resolveDataSubdir();
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");

    // Bound disk growth to one generation: sweep any set-aside dirs left over from a
    // previous re-download before creating this one. (The node is dead, so nothing
    // holds these.) Doing this first also reclaims that space ahead of the disk check.
    for (const QString& entry : dd.entryList(QStringList() << "chain-set-aside-*",
                                             QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir(dd.filePath(entry)).removeRecursively();
    }

    // A re-download fetches a FRESH full chain that must briefly coexist on disk with
    // the set-aside old copy (the set-aside is just a rename, so it keeps occupying
    // its original space). On the near-full disk that often CAUSED the corruption,
    // this would fail partway and strand the user with no chain. Require a generous
    // floor up front and bail with a clear message rather than half-finishing. The
    // generic ensureEnoughDiskSpace() (~3 GB) is far too low for a full chain.
    {
        QStorageInfo storage(dd.absolutePath());
        const qint64 needed = (qint64)15 * 1024 * 1024 * 1024;   // ~15 GB headroom for a fresh chain
        if (storage.isValid() && storage.isReady() && storage.bytesAvailable() < needed) {
            QString human = QString::number(storage.bytesAvailable() / 1024.0 / 1024.0 / 1024.0, 'f', 1);
            main->logger->write(QString("redownloadChain: insufficient disk for re-download (%1 GB free)").arg(human));
            this->showError(QObject::tr("There isn't enough free disk space to download a fresh "
                "copy of the blockchain.\n\n"
                "About 15 GB free is needed, but only %1 GB is available. Please free up some "
                "space and open ZClassic again. Your wallet and coins are safe, and your existing "
                "blockchain data has not been changed.").arg(human));
            return;
        }
    }

    // Set aside blocks/ and chainstate/ into ONE parent rather than deleting them, so
    // the move is instant and fully recoverable. If any rename fails, roll back every
    // move already done and remove the parent, leaving the datadir exactly as found.
    QString aside = "chain-set-aside-" + stamp;
    dd.mkpath(aside);

    QStringList moved;   // names successfully moved INTO 'aside', for rollback
    bool moveFailed = false;
    for (const QString& name : { QStringLiteral("blocks"), QStringLiteral("chainstate") }) {
        if (dd.exists(name)) {
            if (!dd.rename(name, aside + "/" + name)) {
                moveFailed = true;
                break;
            }
            moved << name;
        }
    }
    if (moveFailed) {
        main->logger->write("redownloadChain: a set-aside move failed; rolling back");
        // Undo every move already done, then drop the (now-partial) parent.
        for (const QString& done : moved)
            dd.rename(aside + "/" + done, done);
        QDir(dd.filePath(aside)).removeRecursively();
        this->showError(QObject::tr("ZClassic couldn't move the old blockchain data aside.\n\n"
            "Please make sure you have free disk space and that no other copy of "
            "ZClassic is running, then try again. Your wallet and coins are safe."));
        return;
    }

    // Count this repair attempt (persisted per install), matching relaunchForRepair.
    QSettings().setValue("repair/attempts", QSettings().value("repair/attempts", 0).toInt() + 1);

    // Drop the dead QProcess so a fresh one is created with no repair flags.
    if (ezclassicd) {
        ezclassicd->deleteLater();
        ezclassicd = nullptr;
    }
    ezNodeQuitDuringStartup = false;
    ezStdErr.clear();

    this->showInformation(QObject::tr("Re-downloading your blockchain data…"),
        QObject::tr("This downloads a fresh copy and can take a while. Your wallet is safe."));

    // Normal start (no extra args): the empty datadir triggers the daemon's own
    // bootstrap-snapshot flow. If the relaunch fails the old chain data is still
    // intact under the set-aside folder, so tell the user where it is.
    if (!this->startEmbeddedZClassicd()) {
        this->showError(QObject::tr("ZClassic couldn't start the re-download.\n\n"
            "Please open ZClassic again — it will download a fresh copy then. "
            "Your wallet and coins are safe, and your previous blockchain data was "
            "kept in a folder named \"%1\" inside the data directory.").arg(aside));
        return;
    }

    ezWarmupTimer.start();
    QTimer::singleShot(1000, [=]() { doAutoConnect(); });
}

// HARD INVARIANT enforcement: this is the ONLY code in the failsafe that touches
// wallet.dat, and it only ever READS it (QFile::copy opens the source read-only)
// to a timestamped backup beside it. It never deletes, renames, or opens
// wallet.dat for writing. Returns true on a verified successful copy.
bool ConnectionLoader::backupWalletForRepair() {
    if (ezDataDir.isEmpty()) {
        main->logger->write("Repair backup: datadir unknown, cannot locate wallet.dat");
        return false;
    }

    QDir dir = resolveDataSubdir();

    QFile wallet(dir.filePath("wallet.dat"));
    if (!wallet.exists()) {
        // No wallet.dat means there is nothing to protect (e.g. a brand-new datadir
        // that never finished its first run). Safe to proceed with the repair.
        main->logger->write("Repair backup: no wallet.dat present; nothing to back up");
        return true;
    }

    // Make sure we have room for the copy before attempting it.
    if (!ensureEnoughDiskSpace(dir.absolutePath()))
        return false;

    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QString backupPath = dir.filePath("wallet.dat.backup-" + stamp);

    // Don't clobber an existing backup of the same second (extremely unlikely).
    if (QFile::exists(backupPath)) {
        main->logger->write("Repair backup: target already exists, aborting to avoid overwrite");
        return false;
    }

    if (!wallet.copy(backupPath)) {
        main->logger->write("Repair backup: copy failed: " + wallet.errorString());
        return false;
    }

    main->logger->write("Repair backup: wallet.dat copied to " + backupPath);
    return true;
}

void ConnectionLoader::doManualConnect() {
    auto config = loadFromSettings();

    if (!config) {
        // Nothing configured, show an error
        QString explanation = QString()
                % QObject::tr("A manual connection was requested, but the settings are not configured.\n\n"
                "Please set the host/port and user/password in the Edit->Settings menu.");

        showError(explanation);
        doRPCSetConnection(nullptr);

        return;
    }

    auto connection = makeConnection(config);
    refreshZClassicdState(connection, [=] () {
        QString explanation = QString()
                % QObject::tr("ZClassic couldn't connect to the node in your settings.\n\n" 
                "Please check the address and sign-in details under Edit → Settings.");

        showError(explanation);
        doRPCSetConnection(nullptr);

        return;
    });
}

void ConnectionLoader::doRPCSetConnection(Connection* conn) {
    // The RPC connection is now live: the node is genuinely up, so end the warmup
    // window. After this, a QProcess 'finished' signal is a normal shutdown, not a
    // startup quit, and must NOT trigger the corruption failsafe.
    // Startup profiling: log how long from node launch to first live RPC, so the
    // warmup cost (block-index + chainstate load + checkblocks verify) is visible.
    if (ezWarmupTimer.isValid())
        main->logger->write(QString("Startup: node RPC online %1 ms after launch")
                                .arg(ezWarmupTimer.elapsed()));
    ezWarmupTimer.invalidate();
    ezNodeQuitDuringStartup = false;

    // This ConnectionLoader is about to be deleted (delete this, below), but its
    // lambdas are still connected to ezclassicd's finished/errorOccurred/readyRead
    // signals. Disconnect them so a later signal (a normal shutdown or a runtime
    // crash) can't call into freed memory. rpc, which takes ownership next, only
    // polls the process and connects no signals of its own.
    if (ezclassicd)
        QObject::disconnect(ezclassicd, nullptr, nullptr, nullptr);

    // Successful startup: clear the persistent repair-attempt counter.
    QSettings().setValue("repair/attempts", 0);

    rpc->setEZClassicd(ezclassicd);
    rpc->setConnection(conn);

    d->accept();

    delete this;
}

Connection* ConnectionLoader::makeConnection(std::shared_ptr<ConnectionConfig> config) {
    QNetworkAccessManager* client = new QNetworkAccessManager(main);
         
    QUrl myurl;
    myurl.setScheme("http");
    myurl.setHost(config.get()->host);
    myurl.setPort(config.get()->port.toInt());

    QNetworkRequest* request = new QNetworkRequest();
    request->setUrl(myurl);
    request->setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    
    QString userpass = config.get()->rpcuser % ":" % config.get()->rpcpassword;
    QString headerData = "Basic " + userpass.toLocal8Bit().toBase64();
    request->setRawHeader("Authorization", headerData.toLocal8Bit());    

    return new Connection(main, client, request, config);
}

void ConnectionLoader::refreshZClassicdState(Connection* connection, std::function<void(void)> refused) {
    json payload = {
        {"jsonrpc", "1.0"},
        {"id", "someid"},
        {"method", "getinfo"}
    };
    connection->doRPC(payload,
        [=] (auto) {
            // Success, hide the dialog if it was shown. 
            d->hide();
            main->logger->write("zclassicd is online.");
            this->doRPCSetConnection(connection);
        },
        [=] (auto reply, auto res) {            
            // Failed, see what it is. 
            auto err = reply->error();
            //qDebug() << err << ":" << QString::fromStdString(res.dump());

            if (err == QNetworkReply::NetworkError::ConnectionRefusedError) {   
                refused();
            } else if (err == QNetworkReply::NetworkError::AuthenticationRequiredError) {
                main->logger->write("Authentication failed");
                QString explanation = QString() % 
                        QObject::tr("ZClassic couldn't sign in to its node.\n\n"
                        "The saved username or password wasn't accepted. "
                        "You can correct it under Edit → Settings.");

                this->showError(explanation);
            } else if (err == QNetworkReply::NetworkError::InternalServerError && 
                    !res.is_discarded()) {
                // The server is loading, so just poll until it succeeds
                QString status      = QString::fromStdString(res["error"]["message"]);
                {
                    static int dots = 0;
                    status = status.left(status.length() - 3) + QString(".").repeated(dots);
                    dots++;
                    if (dots > 3)
                        dots = 0;
                }
                this->showInformation(QObject::tr("Step 2 of 3: Starting your wallet…"), status);
                main->logger->write("Waiting for zclassicd to come online.");
                // Refresh quickly during the first few seconds of warmup, then every second.
                int probeMs = (ezWarmupTimer.isValid() && ezWarmupTimer.elapsed() < 4000) ? 250 : 1000;
                QTimer::singleShot(probeMs, [=]() { this->refreshZClassicdState(connection, refused); });
            }
        }
    );
}

// Update the UI with the status
void ConnectionLoader::showInformation(QString info, QString detail) {
    // Once we reach the syncing phase, drop the now-stale onboarding card so it
    // doesn't contradict the live status; the progress bar carries on.
    if (info.contains(QObject::tr("Step 3")) && ezCard && ezCard->isVisible())
        ezCard->setVisible(false);

    static int rescanCount = 0;
    if (detail.toLower().startsWith("rescan")) {
        rescanCount++;
    }
    
    if (rescanCount > 10) {
        detail = detail + "\n" + QObject::tr("This may take several hours");
    }

    connD->status->setText(info);
    connD->statusDetail->setText(detail);

    if (rescanCount < 10)
        main->logger->write(info + ":" + detail);
}

/**
 * Show error will close the loading dialog and show an error. 
*/
void ConnectionLoader::showError(QString explanation) {    
    rpc->setEZClassicd(nullptr);
    rpc->noConnection();

    QMessageBox::critical(main, QObject::tr("Connection Error"), explanation, QMessageBox::Ok);
    d->close();
}

QString ConnectionLoader::locateZClassicConfFile() {
#ifdef Q_OS_LINUX
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, ".zclassic/zclassic.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, "Library/Application Support/ZClassic/zclassic.conf");
#else
    auto confLocation = QStandardPaths::locate(QStandardPaths::AppDataLocation, "../../ZClassic/zclassic.conf");
#endif

    main->logger->write("Found zclassicconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::zclassicConfWritableLocation() {
#ifdef Q_OS_LINUX
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".zclassic/zclassic.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/ZClassic/zclassic.conf");
#else
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../ZClassic/zclassic.conf");
#endif

    main->logger->write("Found zclassicconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::zcashParamsDir() {
    #ifdef Q_OS_LINUX
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".zcash-params"));
#elif defined(Q_OS_DARWIN)
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/ZcashParams"));
#else
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../ZcashParams"));
#endif

    if (!paramsLocation.exists()) {
        main->logger->write("Creating params location at " + paramsLocation.absolutePath());
        QDir().mkpath(paramsLocation.absolutePath());
    }

    main->logger->write("Found ZClassic params directory at " + paramsLocation.absolutePath());
    return paramsLocation.absolutePath();
}

bool ConnectionLoader::verifyParams() {
    QDir paramsDir(zcashParamsDir());

    if (!QFile(paramsDir.filePath("sapling-output.params")).exists()) return false;
    if (!QFile(paramsDir.filePath("sapling-spend.params")).exists()) return false;
    if (!QFile(paramsDir.filePath("sprout-proving.key")).exists()) return false;
    if (!QFile(paramsDir.filePath("sprout-verifying.key")).exists()) return false;
    if (!QFile(paramsDir.filePath("sprout-groth16.params")).exists()) return false;

    return true;
}

/**
 * Try to automatically detect a zclassic.conf file in the correct location and load parameters
 */ 
std::shared_ptr<ConnectionConfig> ConnectionLoader::autoDetectZClassicConf() {    
    auto confLocation = locateZClassicConfFile();

    if (confLocation.isNull()) {
        // No ZClassic file, just return with nothing
        return nullptr;
    }

    QFile file(confLocation);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << file.errorString();
        return nullptr;
    }

    QTextStream in(&file);

    auto zclassicconf = new ConnectionConfig();
    zclassicconf->host     = "127.0.0.1";
    zclassicconf->connType = ConnectionType::DetectedConfExternalZClassicD;
    zclassicconf->usingZClassicConf = true;
    zclassicconf->zclassicDir = QFileInfo(confLocation).absoluteDir().absolutePath();
    zclassicconf->zclassicDaemon = false;

    Settings::getInstance()->setUsingZClassicConf(confLocation);

    while (!in.atEnd()) {
        QString line = in.readLine();
        auto s = line.indexOf("=");
        QString name  = line.left(s).trimmed().toLower();
        QString value = line.right(line.length() - s - 1).trimmed();

        if (name == "rpcuser") {
            zclassicconf->rpcuser = value;
        }
        if (name == "rpcpassword") {
            zclassicconf->rpcpassword = value;
        }
        if (name == "rpcport") {
            zclassicconf->port = value;
        }
        if (name == "daemon" && value == "1") {
            zclassicconf->zclassicDaemon = true;
        }
        if (name == "proxy") {
            zclassicconf->proxy = value;
        }
        if (name == "testnet" &&
            value == "1"  &&
            zclassicconf->port.isEmpty()) {
                zclassicconf->port = "18023";
        }
    }

    // If rpcport is not in the file, and it was not set by the testnet=1 flag, then go to default
    if (zclassicconf->port.isEmpty()) zclassicconf->port = "8023";
    file.close();

    // In addition to the zclassic.conf file, also double check the params. 

    return std::shared_ptr<ConnectionConfig>(zclassicconf);
}

/**
 * Load connection settings from the UI, which indicates an unknown, external zclassicd
 */ 
std::shared_ptr<ConnectionConfig> ConnectionLoader::loadFromSettings() {
    // Load from the QT Settings. 
    QSettings s;
    
    auto host        = s.value("connection/host").toString();
    auto port        = s.value("connection/port").toString();
    auto username    = s.value("connection/rpcuser").toString();
    auto password    = s.value("connection/rpcpassword").toString();    

    if (username.isEmpty() || password.isEmpty())
        return nullptr;

    auto uiConfig = new ConnectionConfig{ host, port, username, password, false, false, "", "", ConnectionType::UISettingsZClassicD};

    return std::shared_ptr<ConnectionConfig>(uiConfig);
}





// P1-7: refuse to start a ~1.7GB download / heavy sync when the disk is nearly full.
bool ConnectionLoader::ensureEnoughDiskSpace(const QString& path) {
    QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady()) {
        // Can't tell -- don't block the user on an unknown.
        main->logger->write("Disk space check skipped (storage not ready) for " + path);
        return true;
    }

    const qint64 needed = (qint64)3 * 1024 * 1024 * 1024;   // ~3 GB
    qint64 avail = storage.bytesAvailable();
    main->logger->write(QString("Free space at %1: %2 MB").arg(path).arg(avail / 1024 / 1024));

    if (avail < needed) {
        QString human = QString::number(avail / 1024.0 / 1024.0 / 1024.0, 'f', 1);
        this->showError(QObject::tr("Not enough free disk space.\n\n"
            "ZClassic needs about 3 GB free to finish setting up, but only %1 GB is available. "
            "Please free up some space and open ZClassic again.").arg(human));
        return false;
    }
    return true;
}

// P1-2: switch the onboarding bar from busy/indeterminate to a real 0-100 reading.
void ConnectionLoader::setBarPercent(int pct) {
    if (!ezProgress) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (ezProgress->maximum() == 0)
        ezProgress->setRange(0, 100);
    ezProgress->setValue(pct);
}

// P1-2: instead of a dead-end error on a failed download, let the user try again.
void ConnectionLoader::offerRetry(QString explanation, std::function<void(void)> cb) {
    auto choice = QMessageBox::warning(main, QObject::tr("ZClassic"),
        explanation % "\n\n" % QObject::tr("Would you like to try again?"),
        QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);

    if (choice == QMessageBox::Retry) {
        ezRetryCount = 0;
        main->logger->write("User chose to retry param download");
        QTimer::singleShot(100, [=]() { this->doNextDownload(cb); });
    } else {
        this->showError(QObject::tr("Setup was stopped.\n\n"
            "You can open ZClassic again whenever you're ready to finish setting up."));
    }
}

/***********************************************************************************
 *  Connection Class
 ************************************************************************************/ 
Connection::Connection(MainWindow* m, QNetworkAccessManager* c, QNetworkRequest* r, 
                        std::shared_ptr<ConnectionConfig> conf) {
    this->restclient  = c;
    this->request     = r;
    this->config      = conf;
    this->main        = m;
}

Connection::~Connection() {
    delete restclient;
    delete request;
}

void Connection::doRPC(const json& payload, const std::function<void(json)>& cb, 
                       const std::function<void(QNetworkReply*, const json&)>& ne) {
    if (shutdownInProgress) {
        // Ignoring RPC because shutdown in progress
        return;
    }

    QNetworkReply *reply = restclient->post(*request, QByteArray::fromStdString(payload.dump()));

    QObject::connect(reply, &QNetworkReply::finished, [=] {
        reply->deleteLater();
        if (shutdownInProgress) {
            // Ignoring callback because shutdown in progress
            return;
        }
        
        if (reply->error() != QNetworkReply::NoError) {
            auto parsed = json::parse(reply->readAll(), nullptr, false);
            ne(reply, parsed);
            
            return;
        } 
        
        auto parsed = json::parse(reply->readAll(), nullptr, false);
        if (parsed.is_discarded()) {
            ne(reply, "Unknown error");
        }
        
        cb(parsed["result"]);        
    });
}

void Connection::doRPCWithDefaultErrorHandling(const json& payload, const std::function<void(json)>& cb) {
    doRPC(payload, cb, [=] (auto reply, auto parsed) {
        if (!parsed.is_discarded() && !parsed["error"]["message"].is_null()) {
            this->showTxError(QString::fromStdString(parsed["error"]["message"]));    
        } else {
            this->showTxError(reply->errorString());
        }
    });
}

void Connection::doRPCIgnoreError(const json& payload, const std::function<void(json)>& cb) {
    doRPC(payload, cb, [=] (auto, auto) {
        // Ignored error handling
    });
}

void Connection::showTxError(const QString& error) {
    if (error.isNull()) return;

    // Prevent multiple dialog boxes from showing, because they're all called async
    static bool shown = false;
    if (shown)
        return;

    shown = true;
    QMessageBox::critical(main, QObject::tr("Transaction Error"), QObject::tr("There was an error sending the transaction. The error was:") + "\n\n"
        + error, QMessageBox::StandardButton::Ok);
    shown = false;
}

/**
 * Prevent all future calls from going through
 */ 
void Connection::shutdown() {
    shutdownInProgress = true;
}
