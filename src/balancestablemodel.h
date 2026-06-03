#ifndef BALANCESTABLEMODEL_H
#define BALANCESTABLEMODEL_H

#include "precompiled.h"

struct UnspentOutput {
    QString address;
    QString txid;
    QString amount;    
    int     confirmations;
    bool    spendable;
};

class BalancesTableModel : public QAbstractTableModel
{
public:
    BalancesTableModel(QObject* parent);
    ~BalancesTableModel();

    void setNewData(const QMap<QString, double>* balances, const QList<UnspentOutput>* outputs);

    int rowCount(const QModelIndex &parent) const;
    int columnCount(const QModelIndex &parent) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;

private:
    // PERF (Phase 4 Win 2): content fingerprint of the incoming balances + utxos, so
    // an unchanged poll early-outs without re-emitting model signals (no flicker, no
    // CPU). Folds in per-address balances AND each utxo's address+confirmations
    // (confirmations==0 paints a row red, so it must affect the fingerprint).
    static QByteArray fingerprint(const QMap<QString, double>* balances,
                                  const QList<UnspentOutput>* outputs);
    QByteArray lastFingerprint;

    QList<std::tuple<QString, double>>*    modeldata   = nullptr;
    QList<UnspentOutput>*                  utxos       = nullptr;

    bool loading = true;
};

#endif // BALANCESTABLEMODEL_H