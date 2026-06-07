#include "senttxstore.h"
#include "settings.h"
#include "securestore.h"

// Helper: build a path in the app data dir, with the testnet- prefix when needed.
static QString sentTxPath(const QString& filename) {
    auto dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!dir.exists())
        QDir().mkpath(dir.absolutePath());
    return Settings::getInstance()->isTestnet() ? dir.filePath("testnet-" % filename)
                                                : dir.filePath(filename);
}

/// The encrypted store (current format). Sent z-tx history is OPSEC-sensitive (recipient
/// addresses + amounts for shielded sends the daemon can't even see), so it is encrypted at rest
/// via SecureStore — written atomically, owner-only (0600).
QString SentTxStore::writeableFile() {
    return sentTxPath(QStringLiteral("senttxstore.enc"));
}

// The legacy plaintext file, migrated to .enc on first read.
static QString legacyFile() {
    return sentTxPath(QStringLiteral("senttxstore.dat"));
}

// delete the sent history.
void SentTxStore::deleteHistory() {
    QFile::remove(writeableFile());
    QFile::remove(legacyFile());
}

QList<TransactionItem> SentTxStore::readSentTxFile() {
    // Reads the encrypted store; transparently migrates a legacy plaintext senttxstore.dat.
    QByteArray bytes = SecureStore::getInstance()->migrateIfNeeded(legacyFile(), writeableFile());
    if (bytes.isEmpty())
        return QList<TransactionItem>();

    QJsonDocument jsonDoc = QJsonDocument::fromJson(bytes);

    QList<TransactionItem> items;

    for (auto i : jsonDoc.array()) {
        auto sentTx = i.toObject();
        TransactionItem t{"send", (qint64)sentTx["datetime"].toVariant().toLongLong(),
                          sentTx["address"].toString(),
                          sentTx["txid"].toString(),
                          sentTx["amount"].toDouble() + sentTx["fee"].toDouble(),
                          0, sentTx["from"].toString(), ""};
        items.push_back(t);
    }

    return items;
}

void SentTxStore::addToSentTx(Tx tx, QString txid) {
    // Save transactions only if the settings are allowed
    if (!Settings::getInstance()->getSaveZtxs())
        return;

    // Also, only store outgoing txs where the from address is a z-Addr. Else, regular zclassicd
    // stores it just fine
    if (!tx.fromAddr.startsWith("z"))
        return;

    // Load the existing (encrypted) history, migrating any legacy plaintext first.
    QByteArray existing = SecureStore::getInstance()->migrateIfNeeded(legacyFile(), writeableFile());
    QJsonDocument jsonDoc = existing.isEmpty() ? QJsonDocument(QJsonArray())
                                               : QJsonDocument::fromJson(existing);

    // Calculate total amount in this tx
    double totalAmount = 0;
    for (auto i : tx.toAddrs) {
        totalAmount += i.amount;
    }

    QString toAddresses;
    if (tx.toAddrs.length() == 1) {
        toAddresses = tx.toAddrs[0].addr;
    } else {
        // Concatenate all the toAddresses
        for (auto a : tx.toAddrs) {
            toAddresses += a.addr % "(" % Settings::getZCLDisplayFormat(a.amount) % ")  ";
        }
    }

    auto list = jsonDoc.array();
    QJsonObject txItem;
    txItem["type"]      = "sent";
    txItem["from"]      = tx.fromAddr;
    txItem["datetime"]  = QDateTime::currentMSecsSinceEpoch() / (qint64)1000;
    txItem["address"]   = toAddresses;
    txItem["txid"]      = txid;
    txItem["amount"]    = -totalAmount;
    txItem["fee"]       = -tx.fee;
    list.append(txItem);

    jsonDoc.setArray(list);

    // Encrypted, atomic, owner-only write via SecureStore.
    SecureStore::getInstance()->writeFile(writeableFile(), jsonDoc.toJson());
}
