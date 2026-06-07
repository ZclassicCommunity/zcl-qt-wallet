// ============================================================================
// TEST SHIM securestore_stub.cpp  (L0 unit suite — tst_logic)
//
// Plaintext, libsodium-free definitions of the SecureStore methods that L0-linked
// product code (senttxstore.cpp) actually references. Compiled against the REAL
// src/securestore.h, so the class layout/signatures match the product exactly.
//
// It mirrors the DEFAULT (encryption OFF) path: store bytes verbatim as owner-only
// (0600) plaintext .dat under the same path/testnet rule as production — keeping the
// senttxstore G1 (write gating) and G4 (0600 / testnet-prefixed name) tests
// meaningful. The libsodium-backed methods (encrypt/unlock/bootstrap/migrate/…) are
// intentionally NOT defined here: the L0 suite never references them, and leaving them
// out keeps L0 free of libsodium. The AEAD round-trip AND the corrupt-.enc no-clobber
// contract (loadStore ok=false; saveStore refuses to overwrite) are validated against the
// REAL SecureStore at L1 (tst_widget: opsec_noClobberAndRoundTrip).
// ============================================================================
#include "securestore.h"
#include "settings.h"

#include <QDir>
#include <QStandardPaths>
#include <QSaveFile>
#include <QFile>
#include <QFileDevice>
#include <QStringBuilder>

SecureStore::SecureStore()  {}    // no libsodium key zeroing in the stub
SecureStore::~SecureStore() {}    // no lock() in the stub

SecureStore* SecureStore::getInstance() { static SecureStore inst; return &inst; }

QString SecureStore::dataDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

QString SecureStore::storePath(const QString& base, bool encrypted) const {
    const QString name = (Settings::getInstance()->isTestnet() ? QStringLiteral("testnet-") : QString())
                       % base % (encrypted ? QStringLiteral(".enc") : QStringLiteral(".dat"));
    return QDir(dataDir()).filePath(name);
}

QByteArray SecureStore::loadStore(const QString& base, bool& ok) {
    ok = true;
    QFile f(storePath(base, /*encrypted*/ false));
    if (f.exists() && f.open(QIODevice::ReadOnly)) { QByteArray b = f.readAll(); f.close(); return b; }
    return QByteArray();
}

bool SecureStore::saveStore(const QString& base, const QByteArray& plain) {
    const QString path = storePath(base, /*encrypted*/ false);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(plain) != plain.size()) { f.cancelWriting(); return false; }
    if (!f.commit()) return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void SecureStore::removeStore(const QString& base) {
    QFile::remove(storePath(base, /*encrypted*/ false));
    QFile::remove(storePath(base, /*encrypted*/ true));
}
