// Copyright 2026 Rhett Creighton - Apache License 2.0
//
// ZNAM (ZCL Names) shared GUI helpers: name validation, target-type <-> label
// mapping, and small POD row types used by the Names tab + dialogs. Mirrors
// nftcommon.h. C++14 (no std::optional/std::string_view) — empty-QString is the
// "not set" sentinel; types in this header are self-contained (Qt only) so any
// header that takes them by value/ref pulls only this include.

#ifndef NAMESCOMMON_H
#define NAMESCOMMON_H

#include <QString>
#include <QVector>
#include <QPair>

// The ZNAM target/record types (the permanent on-chain codes, mirror
// znam/znammsg.h ZNAM_TARGET_*). The GUI never hard-codes the integers
// elsewhere — it goes through these helpers so a label change is one place.
enum class ZNAMTarget {
    Onion   = 1,
    ZAddr   = 2,
    TAddr   = 3,
    BTC     = 4,
    LTC     = 5,
    DOGE    = 6,
    Content = 7,
};

// Human label for a target_type code (1..7), or "unknown".
QString znamTargetLabel(int code);

// All selectable target types, in display order, as (code, label) pairs — for
// populating a combo box.
struct ZNAMTargetChoice { int code; QString label; };
QVector<ZNAMTargetChoice> znamTargetChoices();

// Validate a ZNAM name exactly as the daemon does (znam_validate_name):
// lowercase [a-z0-9-], no leading/trailing hyphen, 1..63 bytes. Returns true if
// acceptable; on false, `reason` (if non-null) is set to a user-facing message.
bool znamIsValidName(const QString& name, QString* reason = nullptr);

// One owned-name row for the My-Names list model.
struct NameRow {
    QString name;
    QString owner;
    QString status;        // "active" | "grace" | "free"
    qint64  expiryHeight = 0;
};

// The fully-resolved record set for one name (name_resolve result). Qt-only,
// self-contained POD — carried by VALUE across the synchronous doRPC callback
// boundary (same pattern as NFTOfferRow), so no metatype registration is needed.
// A null resolve (name not registered) is conveyed by the wrapper's found=false
// flag, NOT by an empty ResolvedName. records[] = (type, value); text[] =
// (key, value). C++14: empty-QString sentinels.
struct ResolvedName {
    QString name, owner, status;
    int     primaryType = 0;
    QString primaryTypeName, primaryValue;
    QVector<QPair<int, QString>>     records;
    QVector<QPair<QString, QString>> text;
};

#endif // NAMESCOMMON_H
