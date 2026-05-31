#ifndef CONNECTION_H
#define CONNECTION_H

#include "mainwindow.h"
#include "ui_connection.h"
#include "precompiled.h"

using json = nlohmann::json;

class RPC;
class QProgressBar;

enum ConnectionType {
    DetectedConfExternalZClassicD = 1,
    UISettingsZClassicD,
    InternalZClassicD
};

struct ConnectionConfig {
    QString host;
    QString port;
    QString rpcuser;
    QString rpcpassword;
    bool    usingZClassicConf;
    bool    zclassicDaemon;
    QString zclassicDir;
    QString proxy;

    ConnectionType connType;
};

class Connection;

class ConnectionLoader {

public:
    ConnectionLoader(MainWindow* main, RPC* rpc);
    ~ConnectionLoader();

    void loadConnection();

private:
    std::shared_ptr<ConnectionConfig> autoDetectZClassicConf();
    std::shared_ptr<ConnectionConfig> loadFromSettings();

    Connection* makeConnection(std::shared_ptr<ConnectionConfig> config);

    void doAutoConnect(bool tryEzclassicdStart = true);
    void doManualConnect();

    void createZClassicConf();
    QString locateZClassicConfFile();
    QString zclassicConfWritableLocation();
    QString zcashParamsDir();

    bool verifyParams();
    void downloadParams(std::function<void(void)> cb);
    void doNextDownload(std::function<void(void)> cb);
    // P2-1: optional one-shot extra args (e.g. "-reindex-chainstate") are passed
    // ONLY to the next launch and are NOT persisted into zclassic.conf, so a
    // repair can never loop forever.
    bool startEmbeddedZClassicd(const QStringList& extraArgs = QStringList());

    void refreshZClassicdState(Connection* connection, std::function<void(void)> refused);

    void showError(QString explanation);
    void showInformation(QString info, QString detail = "");

    void doRPCSetConnection(Connection* conn);

    // P1-2 / P1-7 idiotproof-onboarding helpers
    bool ensureEnoughDiskSpace(const QString& path);
    void setBarPercent(int pct);
    void offerRetry(QString explanation, std::function<void(void)> cb);

    // P2-1: corruption failsafe. When the embedded node dies DURING warmup
    // (before the RPC connection is established) we classify the failure from
    // its stderr + debug.log tail and, if it looks like a corrupt/partway block
    // or chainstate database, offer a safe, staged repair ladder. NONE of these
    // paths ever touch wallet.dat except to read+copy it as a backup.
    bool        ezNodeQuitDuringStartup = false;  // node exited before RPC came up
    bool        ezRepairOffered         = false;  // repair dialog already shown this run
    QString     ezStdErr;                          // accumulated node stderr this run
    QString     ezDataDir;                         // datadir holding blocks/chainstate/debug.log/wallet.dat

    void    handleStartupFailure();                       // detect + classify + route
    bool    looksLikeDbCorruption(const QString& text);   // marker scan
    QString readDebugLogTail(int maxBytes = 16384);        // read-only tail of debug.log
    void    offerCorruptionRepair();                       // staged repair ladder dialog
    void    relaunchForRepair(const QStringList& extraArgs);  // backup wallet, then relaunch once
    bool    backupWalletForRepair();                       // read+copy wallet.dat; never writes it
    QDir    resolveDataSubdir();                            // datadir root, or testnet3/ when it holds the chain

    // Single-file release support: when no sibling daemon ships next to the GUI,
    // the node is appended to our OWN executable (GUI | daemon | sha256 | len | magic).
    // Extract+verify it to a per-user cache and return its absolute path; returns
    // an empty string (-> caller falls back) when there is no embedded payload,
    // on a hash mismatch, or on macOS (notarization forbids exec of an extracted file).
    QString ensureDaemonExtracted();

    QProcess*               ezclassicd  = nullptr;
    QElapsedTimer           ezWarmupTimer;   // measures embedded-node startup/warmup time

    QLabel*                 ezCard      = nullptr;   // one-time onboarding explainer card
    QProgressBar*           ezProgress  = nullptr;   // onboarding progress indicator
    int                     ezRetryCount = 0;        // consecutive param-download retries

    QDialog*                d;
    Ui_ConnectionDialog*    connD;

    MainWindow*             main;
    RPC*                    rpc;

    QNetworkReply* currentDownload = nullptr;
    QFile*         currentOutput   = nullptr;
    QQueue<QUrl>*  downloadQueue   = nullptr;

    QNetworkAccessManager* client  = nullptr; 
    QTime downloadTime;
};

/**
 * Represents a connection to a zclassicd. It may even start a new zclassicd if needed.
 * This is also a UI class, so it may show a dialog waiting for the connection.
*/
class Connection {
public:
    Connection(MainWindow* m, QNetworkAccessManager* c, QNetworkRequest* r, std::shared_ptr<ConnectionConfig> conf);
    ~Connection();

    QNetworkAccessManager*              restclient;
    QNetworkRequest*                    request;
    std::shared_ptr<ConnectionConfig>   config;
    MainWindow*                         main;

    void shutdown();

    void doRPC(const json& payload, const std::function<void(json)>& cb, 
               const std::function<void(QNetworkReply*, const json&)>& ne);
    void doRPCWithDefaultErrorHandling(const json& payload, const std::function<void(json)>& cb);
    void doRPCIgnoreError(const json& payload, const std::function<void(json)>& cb) ;

    void showTxError(const QString& error);

    // Batch method. Note: Because of the template, it has to be in the header file. 
    template<class T>
    void doBatchRPC(const QList<T>& payloads,
                     std::function<json(T)> payloadGenerator,
                     std::function<void(QMap<T, json>*)> cb) {    
        auto responses = new QMap<T, json>(); // zAddr -> list of responses for each call. 
        int totalSize = payloads.size();
        if (totalSize == 0)
            return;

        // Keep track of all pending method calls, so as to prevent 
        // any overlapping calls
        static QMap<QString, bool> inProgress;

        QString method = QString::fromStdString(payloadGenerator(payloads[0])["method"]);
        //if (inProgress.value(method, false)) {
        //    qDebug() << "In progress batch, skipping";
        //    return;
        //}

        for (auto item: payloads) {
            json payload = payloadGenerator(item);
            inProgress[method] = true;
            
            QNetworkReply *reply = restclient->post(*request, QByteArray::fromStdString(payload.dump()));

            QObject::connect(reply, &QNetworkReply::finished, [=] {
                reply->deleteLater();
                if (shutdownInProgress) {
                    // Ignoring callback because shutdown in progress
                    return;
                }
                
                auto all = reply->readAll();            
                auto parsed = json::parse(all.toStdString(), nullptr, false);

                if (reply->error() != QNetworkReply::NoError) {            
                    qDebug() << QString::fromStdString(parsed.dump());
                    qDebug() << reply->errorString();

                    (*responses)[item] = json::object();    // Empty object
                } else {
                    if (parsed.is_discarded()) {
                        (*responses)[item] = json::object();    // Empty object
                    } else {
                        (*responses)[item] = parsed["result"];
                    }
                }
            });
        }

        auto waitTimer = new QTimer(main);
        QObject::connect(waitTimer, &QTimer::timeout, [=]() {
            if (shutdownInProgress) {
                waitTimer->stop();
                waitTimer->deleteLater();  
                return;
            }

            // If all responses have arrived, return
            if (responses->size() == totalSize) {
                waitTimer->stop();
                
                cb(responses);
                inProgress[method] = false;

                waitTimer->deleteLater();            
            }
        });
        waitTimer->start(100);    
    }

private:
    bool shutdownInProgress = false;    
};

#endif
