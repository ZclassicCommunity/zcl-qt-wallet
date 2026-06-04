#include "txtablemodel.h"
#include "settings.h"
#include "rpc.h"

TxTableModel::TxTableModel(QObject *parent)
     : QAbstractTableModel(parent) {
    headers << QObject::tr("Type") << QObject::tr("Address") << QObject::tr("Date/Time") << QObject::tr("Amount");
}

// PERF (Phase 4 Win 2): fold the render-affecting fields of a transaction list into
// a compact fingerprint. We hash a deterministic textual encoding of every item, in
// order. `confirmations` IS included, so a pending(0)->confirmed transition flips the
// fingerprint and forces a repaint. A leading sentinel byte makes the fingerprint of
// an EMPTY list distinct from a never-yet-set (null) cached fingerprint, so the very
// first (possibly empty) apply still runs once.
QByteArray TxTableModel::fingerprint(const QList<TransactionItem>& data) {
    QByteArray buf;
    buf.append('v');                       // sentinel: non-null even for an empty list
    buf.append(QByteArray::number(data.size()));
    buf.append('|');
    for (const auto& t : data) {
        buf.append(t.type.toUtf8());                              buf.append('\x1f');
        buf.append(QByteArray::number(t.datetime));              buf.append('\x1f');
        buf.append(t.address.toUtf8());                           buf.append('\x1f');
        buf.append(t.txid.toUtf8());                              buf.append('\x1f');
        buf.append(QByteArray::number(t.amount, 'f', 8));        buf.append('\x1f');
        buf.append(QByteArray::number((qulonglong)t.confirmations)); buf.append('\x1f');
        buf.append(t.fromAddr.toUtf8());                          buf.append('\x1f');
        buf.append(t.memo.toUtf8());                              buf.append('\x1e');
    }
    return QCryptographicHash::hash(buf, QCryptographicHash::Sha1);
}

TxTableModel::~TxTableModel() {
    delete modeldata;
    delete tTrans;
    delete zsTrans;
    delete zrTrans;
}

void TxTableModel::addZSentData(const QList<TransactionItem>& data) {
    // PERF (Phase 4 Win 2): if this source is byte-identical to what we last applied,
    // skip the whole rebuild (no model signals -> no flicker, no per-poll CPU).
    QByteArray fp = fingerprint(data);
    if (fp == lastZsFingerprint)
        return;
    lastZsFingerprint = fp;

    delete zsTrans;
    zsTrans = new QList<TransactionItem>();
    std::copy(data.begin(), data.end(), std::back_inserter(*zsTrans));

    updateAllData();
}

void TxTableModel::addZRecvData(const QList<TransactionItem>& data) {
    QByteArray fp = fingerprint(data);
    if (fp == lastZrFingerprint)
        return;
    lastZrFingerprint = fp;

    delete zrTrans;
    zrTrans = new QList<TransactionItem>();
    std::copy(data.begin(), data.end(), std::back_inserter(*zrTrans));

    updateAllData();
}


void TxTableModel::addTData(const QList<TransactionItem>& data) {
    QByteArray fp = fingerprint(data);
    if (fp == lastTFingerprint)
        return;
    lastTFingerprint = fp;

    delete tTrans;
    tTrans = new QList<TransactionItem>();
    std::copy(data.begin(), data.end(), std::back_inserter(*tTrans));

    updateAllData();
}

bool TxTableModel::exportToCsv(QString fileName) const {
    if (!modeldata)
        return false;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate))
        return false;

    QTextStream out(&file);   // we will serialize the data into the file

    // Write headers
    for (int i = 0; i < headers.length(); i++) {
        out << "\"" << headers[i] << "\",";
    }
    out << "\"Memo\"";
    out << endl;
    
    // Write out each row
    for (int row = 0; row < modeldata->length(); row++) {
        for (int col = 0; col < headers.length(); col++) {
            out << "\"" << data(index(row, col), Qt::DisplayRole).toString() << "\",";
        }
        // Memo
        out << "\"" << modeldata->at(row).memo << "\"";
        out << endl;
    }

    file.close();
    return true;
}

void TxTableModel::updateAllData() {    
    auto newmodeldata = new QList<TransactionItem>();

    if (tTrans  != nullptr) std::copy( tTrans->begin(),  tTrans->end(), std::back_inserter(*newmodeldata));
    if (zsTrans != nullptr) std::copy(zsTrans->begin(), zsTrans->end(), std::back_inserter(*newmodeldata));
    if (zrTrans != nullptr) std::copy(zrTrans->begin(), zrTrans->end(), std::back_inserter(*newmodeldata));

    // Sort by reverse time
    std::sort(newmodeldata->begin(), newmodeldata->end(), [=] (auto a, auto b) {
        return a.datetime > b.datetime; // reverse sort
    });

    // And then swap out the modeldata with the new one.
    delete modeldata;
    modeldata = newmodeldata;

    dataChanged(index(0, 0), index(modeldata->size()-1, columnCount(index(0,0))-1));
    layoutChanged();
}

 int TxTableModel::rowCount(const QModelIndex&) const
 {
    if (modeldata == nullptr) return 0;
    return modeldata->size();
 }

 int TxTableModel::columnCount(const QModelIndex&) const
 {
    return headers.size();
 }


 QVariant TxTableModel::data(const QModelIndex &index, int role) const
 {
     // Align column 4 (amount) right
    if (role == Qt::TextAlignmentRole && index.column() == 3) return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    
    if (role == Qt::ForegroundRole) {
        QBrush b;
        // Pending (0-conf): amber = not-yet-final, matching the PUBLIC/amber token.
        // Confirmed: normal dark-theme text. NEVER Qt::black here — it is invisible
        // on the dark Type/Date columns (the default-delegate columns honor this).
        if (modeldata->at(index.row()).confirmations == 0)
            b.setColor(QColor("#d9822b"));
        else
            b.setColor(QColor("#e6e6e6"));
        return b;
    }

    auto dat = modeldata->at(index.row());
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return dat.type;
        case 1: { 
                    auto addr = modeldata->at(index.row()).address;
                    if (addr.trimmed().isEmpty()) 
                        return "(Shielded)";
                    else 
                        return addr;
                }
        case 2: return QDateTime::fromMSecsSinceEpoch(modeldata->at(index.row()).datetime * (qint64)1000)
                        .toLocalTime().toString("yyyy-MM-dd hh:mm");   // compact, scannable, sorts right; full ts in tooltip
        case 3: return Settings::getZCLDisplayFormat(modeldata->at(index.row()).amount);
        }
    } 

    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case 0: return modeldata->at(index.row()).type + 
                    (dat.memo.isEmpty() ? "" : " tx memo: \"" + dat.memo + "\"");
        case 1: { 
                    auto addr = modeldata->at(index.row()).address;
                    if (addr.trimmed().isEmpty()) 
                        return "(Shielded)";
                    else 
                        return addr;
                }
        case 2: return QDateTime::fromMSecsSinceEpoch(modeldata->at(index.row()).datetime * (qint64)1000).toLocalTime().toString();
        case 3: return Settings::getInstance()->getUSDFormat(modeldata->at(index.row()).amount);
        }    
    }

    if (role == Qt::DecorationRole && index.column() == 0) {
        if (!dat.memo.isEmpty()) {
            // Return the info pixmap to indicate memo
            QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);            
            return QVariant(icon.pixmap(16, 16));
        } else {
            // Empty pixmap to make it align
            QPixmap p(16, 16);
            p.fill(Qt::white);
            return QVariant(p);
        }
    }

    return QVariant();
 }


 QVariant TxTableModel::headerData(int section, Qt::Orientation orientation, int role) const
 {
     if (role == Qt::TextAlignmentRole && section == 3) return QVariant(Qt::AlignRight | Qt::AlignVCenter);

     if (role == Qt::FontRole) {
         QFont f;
         f.setBold(true);
         return f;
     }

     if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
         return headers.at(section);
     }

     return QVariant();
 }

QString TxTableModel::getTxId(int row) const {
    return modeldata->at(row).txid;
}

QString TxTableModel::getMemo(int row) const {
    return modeldata->at(row).memo;
}

qint64 TxTableModel::getConfirmations(int row) const {
    return modeldata->at(row).confirmations;
}

QString TxTableModel::getAddr(int row) const {
    return modeldata->at(row).address.trimmed();
}

qint64 TxTableModel::getDate(int row) const {
    return modeldata->at(row).datetime;
}

QString TxTableModel::getType(int row) const {
    return modeldata->at(row).type;
}

QString TxTableModel::getAmt(int row) const {
    return Settings::getDecimalString(modeldata->at(row).amount);
}