#ifndef SECURESTORE_H
#define SECURESTORE_H

#include "precompiled.h"

// SecureStore — the wallet's master-password-derived encryption for every sensitive file the GUI
// writes to disk (transaction history, address labels, turnstile plan, …).
//
// Threat model: a local adversary who can read the user's disk/AppData (stolen laptop, backup
// leak, forensic image) must learn NOTHING about the user's shielded activity from our files.
//
// Cryptography (see docs/OPSEC_ENCRYPTION.md):
//   • KDF:      Argon2id (crypto_pwhash), SENSITIVE params (~1 GiB, multi-second) → 256-bit master
//               key. Memory-hard, the real front line against password brute-force.
//   • Cipher:   a TWO-LAYER AEAD CASCADE with independent subkeys (crypto_kdf / BLAKE2b):
//                 inner = XChaCha20-Poly1305  (stream family, 192-bit random nonce)
//                 outer = AES-256-GCM         (block family; cipher-family diversity)
//               If AES-NI is unavailable (e.g. some ARM) the outer layer falls back to a second,
//               independent-key XChaCha20-Poly1305 — recorded in the blob's mode byte so files
//               stay self-describing. A cryptanalytic break of either cipher still leaves the data
//               sealed by the other. 256-bit keys ⇒ ~128-bit post-quantum (Grover) — quantum-ample
//               for symmetric, password-based encryption.
//
// The master key lives only in mlock'd memory for the session and is zeroed on lock()/exit.

class SecureStore {
public:
    static SecureStore* getInstance();

    // How the master key's secret is supplied. A keyfile is any file the user keeps elsewhere
    // (USB stick, etc.); its contents are hashed (BLAKE2b) and stretched by Argon2id just like a
    // password, so a stolen disk alone — without the keyfile — can't derive the key.
    enum SecretType { SecretPassword = 0, SecretKeyfile = 1, SecretBoth = 2 };

    // --- lifecycle -------------------------------------------------------
    bool verifierExists() const;                 // has a master secret ever been set?
    SecretType storedSecretType() const;         // what kind of secret unlocks this wallet?
    bool isUnlocked() const { return unlocked; }
    bool createMaster(SecretType type, const QString& password, const QString& keyfilePath);
    bool unlock(SecretType type, const QString& password, const QString& keyfilePath);
    void lock();                                 // zero the in-memory key

    // Interactive startup: first-run notice + set-password, or unlock with a
    // retry/reset/quit choice on a wrong password. Returns false if the user declined
    // (the caller should then refuse to start, so nothing is ever written in the clear).
    bool bootstrap(QWidget* parent);

    // --- encrypted file I/O (atomic, 0600) -------------------------------
    bool writeFile(const QString& path, const QByteArray& plain);
    bool readFile(const QString& path, QByteArray& plainOut);   // false if missing/corrupt/locked

    // Read a legacy plaintext file and transparently migrate it to an encrypted one
    // (write `encPath`, then delete `legacyPath`). Returns the plaintext bytes either way.
    // Used by the stores so old wallets upgrade in place on first unlock.
    QByteArray migrateIfNeeded(const QString& legacyPath, const QString& encPath);

    // --- raw blob crypto (used by the above + the verifier/canary) -------
    QByteArray encrypt(const QByteArray& plain);
    bool       decrypt(const QByteArray& blob, QByteArray& plainOut);

    // --- reset -----------------------------------------------------------
    static QString dataDir();
    void purgeAllData();          // delete every managed file + the verifier, then lock

private:
    SecureStore();
    ~SecureStore();

    // Build the raw secret bytes fed to Argon2id from a password and/or a keyfile.
    QByteArray buildSecret(SecretType type, const QString& password, const QString& keyfilePath) const;
    bool   deriveMaster(const QByteArray& secret, const unsigned char* salt,
                        quint64 ops, quint64 mem);
    QString verifierPath() const;

    bool          unlocked = false;
    unsigned char masterKey[32];  // crypto_kdf_KEYBYTES; mlock'd while unlocked
    bool          keyLocked = false;
};

#endif // SECURESTORE_H
