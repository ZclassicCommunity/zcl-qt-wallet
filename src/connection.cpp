#include "connection.h"
#include "mainwindow.h"
#include "settings.h"
#include "ui_connection.h"
#include "ui_createzclassicconfdialog.h"
#include "rpc.h"

#include "precompiled.h"

using json = nlohmann::json;

ConnectionLoader::ConnectionLoader(MainWindow* main, RPC* rpc) {
    this->main = main;
    this->rpc  = rpc;

    d = new QDialog(main);
    connD = new Ui_ConnectionDialog();
    connD->setupUi(d);
    QPixmap logo(":/img/res/logobig.gif");
    connD->topIcon->setBasePixmap(logo.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

ConnectionLoader::~ConnectionLoader() {
    delete d;
    delete connD;
}

void ConnectionLoader::loadConnection() {
    QTimer::singleShot(1, [=]() { this->doAutoConnect(); });
    d->exec();
}

void ConnectionLoader::doAutoConnect(bool tryEzclassicdStart) {
    // Priority 1: Ensure all params are present.
    if (!verifyParams()) {
        downloadParams([=]() { this->doAutoConnect(); });
        return;
    }

    // Priority 2: Try to connect to detect zclassic.conf and connect to it.
    auto config = autoDetectZclassicConf();
    main->logger->write(QObject::tr("Attempting autoconnect"));

    if (config.get() != nullptr) {
        auto connection = makeConnection(config);

        refreshZclassicdState(connection, [=] () {
            // Refused connection. So try and start embedded zclassicd
            if (Settings::getInstance()->useEmbedded()) {
                if (tryEzclassicdStart) {
                    this->showInformation(QObject::tr("Starting embedded zclassicd"));
                    if (this->startEmbeddedZclassicd()) {
                        // Embedded zclassicd started up. Wait a second and then refresh the connection
                        main->logger->write("Embedded zclassicd started up, trying autoconnect in 1 sec");
                        QTimer::singleShot(1000, [=]() { doAutoConnect(); } );
                    } else {
                        if (config->zclassicDaemon) {
                            // zclassicd is configured to run as a daemon, so we must wait for a few seconds
                            // to let it start up.
                            main->logger->write("zclassicd is daemon=1. Waiting for it to start up");
                            this->showInformation(QObject::tr("zclassicd is set to run as daemon"), QObject::tr("Waiting for zclassicd"));
                            QTimer::singleShot(5000, [=]() { doAutoConnect(/* don't attempt to start ezclassicd */ false); });
                        } else {
                            // Something is wrong.
                            // We're going to attempt to connect to the one in the background one last time
                            // and see if that works, else throw an error
                            main->logger->write("Unknown problem while trying to start zclassicd");
                            QTimer::singleShot(2000, [=]() { doAutoConnect(/* don't attempt to start ezclassicd */ false); });
                        }
                    }
                } else {
                    // We tried to start ezclassicd previously, and it didn't work. So, show the error.
                    main->logger->write("Couldn't start embedded zclassicd for unknown reason");
                    QString explanation;
                    if (config->zclassicDaemon) {
                        explanation = QString() % QObject::tr("You have zclassicd set to start as a daemon, which can cause problems "
                            "with zcl-qt-wallet\n\n."
                            "Please remove the following line from your zclassic.conf and restart zcl-qt-wallet\n"
                            "daemon=1");
                    } else {
                        explanation = QString() % QObject::tr("Couldn't start the embedded zclassicd.\n\n"
                            "Please try restarting.\n\nIf you previously started zclassicd with custom arguments, you might need to reset zclassic.conf.\n\n"
                            "If all else fails, please run zclassicd manually.") %
                            (ezclassicd ? QObject::tr("The process returned") + ":\n\n" % ezclassicd->errorString() : QString(""));
                    }

                    this->showError(explanation);
                }
            } else {
                // zclassic.conf exists, there's no connection, and the user asked us not to start zclassicd. Error!
                main->logger->write("Not using embedded and couldn't connect to zclassicd");
                QString explanation = QString() % QObject::tr("Couldn't connect to zclassicd configured in zclassic.conf.\n\n"
                                      "Not starting embedded zclassicd because --no-embedded was passed");
                this->showError(explanation);
            }
        });
    } else {
        if (Settings::getInstance()->useEmbedded()) {
            // zclassic.conf was not found, so create one
            createZclassicConf();
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
 * This will create a new zclassic.conf, download Zclassic parameters.
 */
void ConnectionLoader::createZclassicConf() {
    main->logger->write("createZclassicConf");

    auto confLocation = zclassicConfWritableLocation();
    QFileInfo fi(confLocation);

    QDialog d(main);
    Ui_createZclassicConf ui;
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
        client->deleteLater();

        main->logger->write("All Downloads done");
        this->showInformation(QObject::tr("All Downloads Finished Successfully!"));
        cb();
        return;
    }

    QUrl url = downloadQueue->dequeue();
    int filesRemaining = downloadQueue->size();

    QString filename = fnSaveFileName(url);
    QString paramsDir = zclassicParamsDir();

    if (QFile(QDir(paramsDir).filePath(filename)).exists()) {
        main->logger->write(filename + " already exists, skipping");
        doNextDownload(cb);

        return;
    }

    // The downloaded file is written to a new name, and then renamed when the operation completes.
    currentOutput = new QFile(QDir(paramsDir).filePath(filename + ".part"));

    if (!currentOutput->open(QIODevice::WriteOnly)) {
        main->logger->write("Couldn't open " + currentOutput->fileName() + " for writing");
        this->showError(QObject::tr("Couldn't download params. Please check the help site for more info."));
    }
    main->logger->write("Downloading to " + filename);
    qDebug() << "Downloading " << url << " to " << filename;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    currentDownload = client->get(request);
    downloadTime.start();

    // Download Progress
    QObject::connect(currentDownload, &QNetworkReply::downloadProgress, [=] (auto done, auto total) {
        // calculate the download speed
        double speed = done * 1000.0 / downloadTime.elapsed();
        QString unit;
        if (speed < 1024) {
            unit = "bytes/sec";
        } else if (speed < 1024*1024) {
            speed /= 1024;
            unit = "kB/s";
        } else {
            speed /= 1024*1024;
            unit = "MB/s";
        }

        this->showInformation(
            QObject::tr("Downloading ") % filename % (filesRemaining > 1 ? " ( +" % QString::number(filesRemaining)  % QObject::tr(" more remaining )") : QString("")),
            QString::number(done/1024/1024, 'f', 0) % QObject::tr("MB of ") % QString::number(total/1024/1024, 'f', 0) + QObject::tr("MB at ") % QString::number(speed, 'f', 2) % unit);
    });

    // Download Finished
    QObject::connect(currentDownload, &QNetworkReply::finished, [=] () {
        // Rename file
        main->logger->write("Finished downloading " + filename);
        currentOutput->rename(QDir(paramsDir).filePath(filename));

        currentOutput->close();
        currentDownload->deleteLater();
        currentOutput->deleteLater();

        if (currentDownload->error()) {
            main->logger->write("Downloading " + filename + " failed");
            this->showError(QObject::tr("Downloading ") + filename + QObject::tr(" failed. Please check the help site for more info"));
        } else {
            doNextDownload(cb);
        }
    });

    // Download new data available.
    QObject::connect(currentDownload, &QNetworkReply::readyRead, [=] () {
        currentOutput->write(currentDownload->readAll());
    });
}

bool ConnectionLoader::startEmbeddedZclassicd() {
    if (!Settings::getInstance()->useEmbedded())
        return false;

    main->logger->write("Trying to start embedded zclassicd");

    // Static because it needs to survive even after this method returns.
    static QString processStdErrOutput;

    if (ezclassicd != nullptr) {
        if (ezclassicd->state() == QProcess::NotRunning) {
            if (!processStdErrOutput.isEmpty()) {
                QMessageBox::critical(main, QObject::tr("zclassicd error"), "zclassicd said: " + processStdErrOutput,
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
    refreshZclassicdState(connection, [=] () {
        QString explanation = QString()
                % QObject::tr("Could not connect to zclassicd configured in settings.\n\n"
                "Please set the host/port and user/password in the Edit->Settings menu.");

        showError(explanation);
        doRPCSetConnection(nullptr);

        return;
    });
}

void ConnectionLoader::doRPCSetConnection(Connection* conn) {
    rpc->setEZclassicd(ezclassicd);
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

void ConnectionLoader::refreshZclassicdState(Connection* connection, std::function<void(void)> refused) {
    json payload = {
        {"jsonrpc", "1.0"},
        {"id", "someid"},
        {"method", "getinfo"}
    };
    connection->doRPC(payload,
        [=] (auto) {
            // Success, hide the dialog if it was shown.
            d->hide();
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
                        QObject::tr("Authentication failed. The username / password you specified was "
                        "not accepted by zclassicd. Try changing it in the Edit->Settings menu");

                this->showError(explanation);
            } else if (err == QNetworkReply::NetworkError::InternalServerError &&
                    !res.is_discarded()) {
                // The server is loading, so just poll until it succeeds
                QString status    = QString::fromStdString(res["error"]["message"]);
                {
                    static int dots = 0;
                    status = status.left(status.length() - 3) + QString(".").repeated(dots);
                    dots++;
                    if (dots > 3)
                        dots = 0;
                }
                this->showInformation(QObject::tr("Your zclassicd is starting up. Please wait."), status);
                main->logger->write("Waiting for zclassicd to come online.");
                // Refresh after one second
                QTimer::singleShot(1000, [=]() { this->refreshZclassicdState(connection, refused); });
            }
        }
    );
}

void ConnectionLoader::showInformation(QString info, QString detail) {
    connD->status->setText(info);
    connD->statusDetail->setText(detail);
}

/**
 * Show error will close the loading dialog and show an error.
*/
void ConnectionLoader::showError(QString explanation) {
    rpc->setEZclassicd(nullptr);
    rpc->noConnection();

    QMessageBox::critical(main, QObject::tr("Connection Error"), explanation, QMessageBox::Ok);
    d->close();
}

QString ConnectionLoader::locateZclassicConfFile() {
#ifdef Q_OS_LINUX
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, ".zclassic/zclassic.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, "Library/Application Support/Zclassic/zclassic.conf");
#else
    auto confLocation = QStandardPaths::locate(QStandardPaths::AppDataLocation, "../../Zclassic/zclassic.conf");
#endif

    main->logger->write("Found zclassicconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::zclassicConfWritableLocation() {
#ifdef Q_OS_LINUX
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".zclassic/zclassic.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/Zclassic/zclassic.conf");
#else
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../Zclassic/zclassic.conf");
#endif

    main->logger->write("Found zclassicconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::zclassicParamsDir() {
    #ifdef Q_OS_LINUX
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".zclassic-params"));
#elif defined(Q_OS_DARWIN)
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/ZclassicParams"));
#else
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../ZclassicParams"));
#endif

    if (!paramsLocation.exists()) {
        main->logger->write("Creating params location at " + paramsLocation.absolutePath());
        QDir().mkpath(paramsLocation.absolutePath());
    }

    main->logger->write("Found Zclassic params directory at " + paramsLocation.absolutePath());
    return paramsLocation.absolutePath();
}

bool ConnectionLoader::verifyParams() {
    QDir paramsDir(zclassicParamsDir());

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
std::shared_ptr<ConnectionConfig> ConnectionLoader::autoDetectZclassicConf() {
    auto confLocation = locateZclassicConfFile();

    if (confLocation.isNull()) {
        // No Zclassic file, just return with nothing
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
    zclassicconf->connType = ConnectionType::DetectedConfExternalZclassicD;
    zclassicconf->usingZclassicConf = true;
    zclassicconf->zclassicDir = QFileInfo(confLocation).absoluteDir().absolutePath();
    zclassicconf->zclassicDaemon = false;

    Settings::getInstance()->setUsingZclassicConf(confLocation);

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
                zclassicconf->port = "18232";
        }
    }

    // If rpcport is not in the file, and it was not set by the testnet=1 flag, then go to default
    if (zclassicconf->port.isEmpty()) zclassicconf->port = "8232";
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

    auto uiConfig = new ConnectionConfig{ host, port, username, password, false, false, "", "", ConnectionType::UISettingsZclassicD};

    return std::shared_ptr<ConnectionConfig>(uiConfig);
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
