// ============================================================================
// nftcommon implementation — see nftcommon.h. The exact strings here were
// previously duplicated across nftsenddialog/nftselldialog/nftbuydialog; keeping
// them in one TU means a wording change (or a translation) happens once.
// ============================================================================
#include "nftcommon.h"
#include "settings.h"

#include <QLabel>
#include <QObject>

bool nftValidateTAddrInto(QLabel* status, const QString& addrIn,
                          const QString& notSupportedHint) {
    if (status == nullptr)
        return false;
    const QString addr = addrIn.trimmed();
    if (addr.isEmpty()) {
        status->clear();
        return false;
    }
    if (!Settings::isValidAddress(addr)) {
        status->setText(QObject::tr("That doesn't look like a ZClassic address."));
        status->setStyleSheet("color:#c0392b;");
        return false;
    }
    if (Settings::isTAddress(addr)) {
        status->setText(QObject::tr("Looks good — a public (transparent) address."));
        status->setStyleSheet("color:#d9822b;");
        return true;
    }
    // A valid shielded (z-) address: the public ZSLP path can't deliver to it.
    status->setText(notSupportedHint);
    status->setStyleSheet("color:#c0392b;");
    return false;
}

QString nftPublicTradeNote() {
    return QObject::tr(
        "This trade settles publicly on-chain — the price and both addresses are "
        "visible. Only the negotiation can be private.");
}
