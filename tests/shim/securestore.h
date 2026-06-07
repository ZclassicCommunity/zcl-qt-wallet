#ifndef SECURESTORE_H
#define SECURESTORE_H
// ============================================================================
// TEST SHIM securestore.h  (L0 unit suite — tst_logic)
//
// A DELIBERATELY transparent, no-encryption stand-in for src/securestore.h.
// The real SecureStore pulls in libsodium (Argon2id + the AEAD cascade); the L0
// suite links NO libsodium, so this shim shadows it via INCLUDEPATH ordering
// (shim/ ahead of ../src in tests.pro) and stores bytes verbatim instead.
//
// It preserves the on-disk *contract* the senttxstore tests assert — files are
// written atomically, owner-only (0600), under the same path senttxstore.cpp
// asks for — so G1 (write gating) and G4 (0600 / testnet-prefixed name) stay
// meaningful. The cipher itself is validated separately against real libsodium;
// here we only need the store plumbing to compile and round-trip.
//
// NOTE: no src/ file is modified — substitution happens purely via tests.pro.
// ============================================================================
#include <QString>
#include <QByteArray>
#include <QFile>
#include <QSaveFile>
#include <QFileDevice>
#include <QIODevice>

class SecureStore {
public:
    static SecureStore* getInstance() { static SecureStore inst; return &inst; }

    bool isUnlocked() const { return true; }

    // Atomic, owner-only (0600) write of the bytes verbatim (no encryption at L0).
    bool writeFile(const QString& path, const QByteArray& plain) {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        if (f.write(plain) != plain.size()) { f.cancelWriting(); return false; }
        if (!f.commit()) return false;
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return true;
    }

    bool readFile(const QString& path, QByteArray& plainOut) {
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
        plainOut = f.readAll();
        f.close();
        return true;
    }

    // Same migration shape as the real store: prefer the .enc, else read the legacy
    // file and rewrite it as .enc (dropping the original).
    QByteArray migrateIfNeeded(const QString& legacyPath, const QString& encPath) {
        QByteArray plain;
        if (QFile::exists(encPath)) { readFile(encPath, plain); return plain; }
        QFile lf(legacyPath);
        if (lf.exists() && lf.open(QIODevice::ReadOnly)) {
            plain = lf.readAll();
            lf.close();
            if (writeFile(encPath, plain)) lf.remove();
        }
        return plain;
    }

private:
    SecureStore() {}
};

#endif // SECURESTORE_H
