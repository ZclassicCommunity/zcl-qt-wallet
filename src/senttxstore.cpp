#include "senttxstore.h"
#include "settings.h"
#include "securestore.h"

// The shielded send history (recipient z-addresses + amounts for sends the daemon can't even
// see) is the most OPSEC-sensitive file the GUI keeps. SecureStore owns the path, the testnet-
// prefix, the atomic owner-only (0600) write, and — when the user has opted into encryption —
// the encryption + transparent migration. The logical store name is all the caller passes.
static const QString kStore = QStringLiteral("senttxstore");

// delete the sent history (both plaintext and encrypted forms).
void SentTxStore::deleteHistory() {
    SecureStore::getInstance()->removeStore(kStore);
}

QList<TransactionItem> SentTxStore::readSentTxFile() {
    bool ok = true;
    QByteArray bytes = SecureStore::getInstance()->loadStore(kStore, ok);
    if (!ok || bytes.isEmpty())
        return QList<TransactionItem>();   // unreadable (corrupt) or absent -> show nothing

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

    // Load the existing history. If it exists but can't be read (corrupt encrypted file), DO NOT
    // overwrite it — that would permanently destroy the only record of past shielded sends.
    bool ok = true;
    QByteArray existing = SecureStore::getInstance()->loadStore(kStore, ok);
    if (!ok) {
        qDebug() << "SentTxStore: existing history is unreadable; refusing to overwrite it.";
        return;
    }
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

    // Atomic, owner-only (0600) write — encrypted iff the user opted in. A failed write must not
    // pass silently: this is the only record of the send.
    if (!SecureStore::getInstance()->saveStore(kStore, jsonDoc.toJson()))
        qDebug() << "SentTxStore: write FAILED — this send was not recorded to local history.";
}
