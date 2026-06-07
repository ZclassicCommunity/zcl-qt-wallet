#ifndef SECURESTORE_H
#define SECURESTORE_H

#include "precompiled.h"

// SecureStore — OPT-IN (default OFF) master-password encryption-at-rest for the sensitive
// files the GUI writes to disk: the shielded send history (the ONLY record of z→z sends —
// the daemon can't see them), address labels, and the turnstile migration plan.
//
// Threat model: a local adversary who can read the user's disk/AppData (stolen laptop,
// backup leak, forensic image) must learn NOTHING about the user's shielded activity from
// OUR files. Out of scope: wallet.dat (the node's own `encryptwallet` protects spend keys);
// the RPC password in zclassic.conf (the node must read it). See docs/OPSEC_ENCRYPTION.md.
//
// Design (corrected from the original cascade proposal for fit + safety):
//   • OPT-IN. Default OFF. When off, the stores write owner-only (0600) plaintext exactly as
//     before — no password, no startup cost, no lock-out risk. The user turns it on in
//     Settings; it takes effect on the next launch (a one-time set-up, then unlock per run).
//   • KDF: Argon2id (crypto_pwhash), MODERATE params (~256 MiB, sub-second) → 256-bit key.
//     Memory-hard, but sub-second so it never freezes startup, and 256 MiB never locks a
//     usable machine out of its own history. The exact ops/mem are recorded in the verifier
//     so a later unlock always reproduces the key (and a future "max security" tier can
//     coexist file-by-file without breaking existing data).
//   • Cipher: a SINGLE XChaCha20-Poly1305 AEAD (192-bit random nonce, misuse-resistant) over
//     a crypto_kdf (BLAKE2b) subkey of the master key, with the blob header authenticated as
//     associated data. One strong modern AEAD — no AES-NI-dependent cross-machine lock-out
//     (the prior AES-256-GCM outer layer made a file written on an AES-NI box unreadable on
//     one without it, for ~0 real-world security gain). 256-bit ⇒ ~128-bit post-quantum.
//   • Key hygiene: master key in sodium_mlock'd memory, zeroed on lock()/exit; secret buffers
//     zeroed after use; files written atomically (QSaveFile) owner-only (0600); data dir 0700.
//
// Callers never build paths or choose plaintext-vs-encrypted: they pass a logical store name
// and SecureStore owns the directory, the testnet- prefix, the extension, legacy migration,
// and the encrypt/plaintext decision.

class SecureStore {
public:
    static SecureStore* getInstance();

    // How the master key's secret is supplied. A keyfile is any file the user keeps elsewhere
    // (USB stick, etc.); its contents are hashed (BLAKE2b) and stretched by Argon2id like a
    // password, so a stolen disk alone — without the keyfile — can't derive the key.
    enum SecretType { SecretPassword = 0, SecretKeyfile = 1, SecretBoth = 2 };

    // --- lifecycle -------------------------------------------------------
    bool verifierExists() const;                 // has a master secret ever been set?
    SecretType storedSecretType() const;         // what kind of secret unlocks this wallet?
    bool isUnlocked() const { return unlocked; } // encryption is in effect this session
    bool createMaster(SecretType type, const QString& password, const QString& keyfilePath);
    bool unlock(SecretType type, const QString& password, const QString& keyfilePath);
    void lock();                                 // zero the in-memory key

    // Interactive startup gate. ONLY call when the user has opted in (Settings) and we are not
    // headless / under the test seam. First-run notice + set-secret, or unlock with a
    // retry/reset/quit choice. Returns false if the user declined (the caller should then
    // refuse to start, so nothing is ever written in the clear).
    bool bootstrap(QWidget* parent);

    // --- store-facing API (the stores use ONLY these) --------------------
    // loadStore: the store's plaintext bytes. ok=false ONLY on a HARD error — an encrypted
    //   file exists but cannot be decrypted — so a caller never mistakes corruption for
    //   "empty" and overwrites the only record of past shielded sends. Missing => empty, ok=true.
    QByteArray loadStore(const QString& base, bool& ok);
    bool       saveStore(const QString& base, const QByteArray& plain);  // atomic, 0600
    void       removeStore(const QString& base);                         // both .enc and .dat

    // --- reset -----------------------------------------------------------
    static QString dataDir();
    void purgeAllData();          // delete every managed store (both forms) + the verifier, then lock

private:
    SecureStore();
    ~SecureStore();

    // Build the raw secret bytes fed to Argon2id from a password and/or a keyfile.
    QByteArray buildSecret(SecretType type, const QString& password, const QString& keyfilePath) const;
    bool    deriveMaster(const QByteArray& secret, const unsigned char* salt,
                         quint64 ops, quint64 mem);
    QString verifierPath() const;

    // Path for a managed store: dir + (testnet-) + base + (.enc | .dat). One place owns it.
    QString storePath(const QString& base, bool encrypted) const;

    // Raw blob crypto (single-layer XChaCha20-Poly1305 + authenticated header) + verifier canary.
    QByteArray encrypt(const QByteArray& plain);
    bool       decrypt(const QByteArray& blob, QByteArray& plainOut);

    bool          unlocked = false;
    unsigned char masterKey[32];  // crypto_kdf_KEYBYTES; mlock'd while unlocked
    bool          keyLocked = false;
};

#endif // SECURESTORE_H
