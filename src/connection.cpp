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
                QTimer::singleShot(1000, [=]() { doAutoConnect(); });
                return;
            }

            // Started, RPC not answering yet. While the process is alive -- or within
            // a generous warmup window after it forked into the background -- keep
            // polling with a friendly message instead of ever showing an error.
            if (ezclassicd->state() != QProcess::NotRunning ||
                (ezWarmupTimer.isValid() && ezWarmupTimer.elapsed() < 120000)) {
                this->showInformation(QObject::tr("Step 2 of 3: Starting your wallet…"),
                    QObject::tr("Almost ready — getting things going (this can take a minute)…"));
                QTimer::singleShot(1000, [=]() { doAutoConnect(); });
                return;
            }

            // Process is genuinely dead and the warmup window elapsed -> friendly error.
            main->logger->write("Embedded zclassicd exited and did not come online");
            QString detail = ezclassicd ? ezclassicd->errorString() : QString();
            this->showError(QObject::tr("ZClassic couldn't finish starting up.\n\n"
                "This usually clears up on its own — please quit and open ZClassic again. "
                "If it keeps happening, make sure you have enough free disk space and a working "
                "internet connection.")
                % (detail.isEmpty() ? QString("") : QString("\n\n(") % detail % QString(")")));
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
    QDir().mkdir(fi.dir().absolutePath());

    QFile file(confLocation);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        main->logger->write("Could not create zclassic.conf, returning");
        return;
    }
        
    QTextStream out(&file); 
    
    out << "server=1\n";
    out << "addnode=mainnet.z.cash\n";
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
                          QObject::tr("This is a one-time download of about 1.7 GB."));

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
                    "This is almost always a temporary internet problem."), cb);
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

bool ConnectionLoader::startEmbeddedZClassicd() {
    if (!Settings::getInstance()->useEmbedded()) 
        return false;
    
    main->logger->write("Trying to start embedded zclassicd");

    // Static because it needs to survive even after this method returns.
    static QString processStdErrOutput;

    if (ezclassicd != nullptr) {
        if (ezclassicd->state() == QProcess::NotRunning) {
            if (!processStdErrOutput.isEmpty()) {
                main->logger->write("node stderr: " + processStdErrOutput);
                QMessageBox::critical(main, QObject::tr("ZClassic"),
                                      QObject::tr("Your wallet had trouble starting up.\n\n"
                                      "Please open ZClassic again. If it keeps happening, make sure your "
                                      "security software isn't blocking it and that you have free disk space."),
                                      QMessageBox::Ok);
            }
            return false;
        } else {
            return true;
        }        
    }

    // Finally, start zclassicd    
    QDir appPath(QCoreApplication::applicationDirPath());
#ifdef Q_OS_LINUX
    auto zclassicdProgram = appPath.absoluteFilePath("zqw-zclassicd");
    if (!QFile(zclassicdProgram).exists()) {
        zclassicdProgram = appPath.absoluteFilePath("zclassicd");
    }
#elif defined(Q_OS_DARWIN)
    auto zclassicdProgram = appPath.absoluteFilePath("zclassicd");
#else
    auto zclassicdProgram = appPath.absoluteFilePath("zclassicd.exe");
#endif
    
    if (!QFile(zclassicdProgram).exists()) {
        qDebug() << "Can't find zclassicd at " << zclassicdProgram;
        main->logger->write("Can't find zclassicd at " + zclassicdProgram); 
        return false;
    }

    ezclassicd = new QProcess(main);    
    QObject::connect(ezclassicd, &QProcess::started, [=] () {
        //qDebug() << "zclassicd started";
    });

    QObject::connect(ezclassicd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [=](int, QProcess::ExitStatus) {
        //qDebug() << "zclassicd finished with code " << exitCode << "," << exitStatus;    
    });

    QObject::connect(ezclassicd, &QProcess::errorOccurred, [&] (auto) {
        //qDebug() << "Couldn't start zclassicd: " << error;
    });

    QObject::connect(ezclassicd, &QProcess::readyReadStandardError, [=]() {
        auto output = ezclassicd->readAllStandardError();
       main->logger->write("zclassicd stderr:" + output);
        processStdErrOutput.append(output);
    });

#ifdef Q_OS_LINUX
    ezclassicd->start(zclassicdProgram);
#elif defined(Q_OS_DARWIN)
    ezclassicd->start(zclassicdProgram);
#else
    ezclassicd->setWorkingDirectory(appPath.absolutePath());
    ezclassicd->start("zclassicd.exe");
#endif // Q_OS_LINUX


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
                // Refresh after one second
                QTimer::singleShot(1000, [=]() { this->refreshZClassicdState(connection, refused); });
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
