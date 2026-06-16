// Copyright 2026 Rhett Creighton - Apache License 2.0
//
// ZNAM (ZCL Names) shared GUI helpers. See namescommon.h.

#include "namescommon.h"

QString znamTargetLabel(int code)
{
    switch (code) {
    case (int)ZNAMTarget::Onion:   return QStringLiteral("Tor .onion");
    case (int)ZNAMTarget::ZAddr:   return QStringLiteral("ZCL shielded (z-addr)");
    case (int)ZNAMTarget::TAddr:   return QStringLiteral("ZCL transparent (t-addr)");
    case (int)ZNAMTarget::BTC:     return QStringLiteral("Bitcoin address");
    case (int)ZNAMTarget::LTC:     return QStringLiteral("Litecoin address");
    case (int)ZNAMTarget::DOGE:    return QStringLiteral("Dogecoin address");
    case (int)ZNAMTarget::Content: return QStringLiteral("Content hash");
    default:                       return QStringLiteral("unknown");
    }
}

QVector<ZNAMTargetChoice> znamTargetChoices()
{
    QVector<ZNAMTargetChoice> v;
    for (int c = (int)ZNAMTarget::Onion; c <= (int)ZNAMTarget::Content; ++c)
        v.push_back({ c, znamTargetLabel(c) });
    return v;
}

bool znamIsValidName(const QString& name, QString* reason)
{
    auto bad = [&](const QString& why) { if (reason) *reason = why; return false; };

    if (name.isEmpty())
        return bad(QStringLiteral("Enter a name."));
    if (name.size() > 63)
        return bad(QStringLiteral("Names are at most 63 characters."));
    if (name.startsWith('-') || name.endsWith('-'))
        return bad(QStringLiteral("Names cannot start or end with a hyphen."));

    for (int i = 0; i < name.size(); ++i) {
        const QChar ch = name.at(i);
        const ushort u = ch.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-';
        if (!ok)
            return bad(QStringLiteral("Use lowercase letters, digits, and hyphens only."));
    }
    if (reason) reason->clear();
    return true;
}
