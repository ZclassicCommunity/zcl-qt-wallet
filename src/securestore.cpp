#include "securestore.h"
#include "settings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileDialog>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QMovie>
#include <QApplication>
#include <QScreen>
#include <QEventLoop>
#include <QThread>
#include <functional>
#include <thread>
#include <atomic>

// The 8-byte (crypto_kdf_CONTEXTBYTES) domain-separation context for subkey derivation.
static const char* KDF_CTX = "zclstore";
static const QByteArray kCanary = QByteArrayLiteral("zcl-secure-store-canary-v1");
static const QByteArray kBlobMagic = QByteArrayLiteral("ZCS");
static const QByteArray kVerMagic  = QByteArrayLiteral("ZCK1");

// Atomic, owner-only (0600) write.
static bool atomicWrite(const QString& path, const QByteArray& bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(bytes) != bytes.size()) { f.cancelWriting(); return false; }
    if (!f.commit()) return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

SecureStore::SecureStore() {
    sodium_memzero(masterKey, sizeof masterKey);
}

SecureStore::~SecureStore() {
    lock();
}

SecureStore* SecureStore::getInstance() {
    static SecureStore inst;
    return &inst;
}

QString SecureStore::dataDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    // Owner-only directory (0700) so siblings can't enumerate our encrypted files.
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return dir;
}

QString SecureStore::verifierPath() const {
    return QDir(dataDir()).filePath("master.bin");
}

bool SecureStore::verifierExists() const {
    return QFile::exists(verifierPath());
}

void SecureStore::lock() {
    sodium_memzero(masterKey, sizeof masterKey);
    if (keyLocked) { sodium_munlock(masterKey, sizeof masterKey); keyLocked = false; }
    unlocked = false;
}

// Hash a keyfile's full contents to a fixed 32-byte value (BLAKE2b). Empty if unreadable.
static QByteArray hashKeyfile(const QString& path) {
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) return QByteArray();
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, crypto_generichash_BYTES);
    char buf[65536];
    qint64 n;
    while ((n = f.read(buf, sizeof buf)) > 0)
        crypto_generichash_update(&st, (const unsigned char*) buf, (unsigned long long) n);
    f.close();
    QByteArray out(crypto_generichash_BYTES, 0);
    crypto_generichash_final(&st, (unsigned char*) out.data(), out.size());
    return out;
}

QByteArray SecureStore::buildSecret(SecretType type, const QString& password,
                                    const QString& keyfilePath) const {
    QByteArray pw = password.toUtf8();
    QByteArray kf = (type == SecretPassword) ? QByteArray() : hashKeyfile(keyfilePath);

    if (type == SecretKeyfile)  return kf;                 // keyfile-only
    if (type == SecretPassword) return pw;                 // password-only
    // Both: bind them together so neither alone suffices — BLAKE2b(password || keyfileHash).
    if (kf.isEmpty()) return QByteArray();
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, crypto_generichash_BYTES);
    crypto_generichash_update(&st, (const unsigned char*) pw.constData(), pw.size());
    crypto_generichash_update(&st, (const unsigned char*) kf.constData(), kf.size());
    QByteArray out(crypto_generichash_BYTES, 0);
    crypto_generichash_final(&st, (unsigned char*) out.data(), out.size());
    sodium_memzero(pw.data(), pw.size());
    return out;
}

bool SecureStore::deriveMaster(const QByteArray& secret, const unsigned char* salt,
                               quint64 ops, quint64 mem) {
    if (secret.isEmpty()) return false;
    if (!keyLocked) { sodium_mlock(masterKey, sizeof masterKey); keyLocked = true; }
    int rc = crypto_pwhash(masterKey, sizeof masterKey, secret.constData(), secret.size(), salt,
                           ops, (size_t) mem, crypto_pwhash_ALG_ARGON2ID13);
    return rc == 0;
}

// ---------------------------------------------------------------------------
// Two-layer AEAD cascade with independent subkeys (see securestore.h).
// Blob: 'Z''C''S' | ver(1) | mode(1) | inner_nonce(24) | outer_nonce(12|24) | ciphertext
//   mode 0 = XChaCha20-Poly1305 (inner) then AES-256-GCM (outer)
//   mode 1 = XChaCha20-Poly1305 (inner) then XChaCha20-Poly1305 (outer, independent key)
// ---------------------------------------------------------------------------
QByteArray SecureStore::encrypt(const QByteArray& plain) {
    if (!unlocked) return QByteArray();

    unsigned char kin[32], kout[32];
    crypto_kdf_derive_from_key(kin,  sizeof kin,  1, KDF_CTX, masterKey);
    crypto_kdf_derive_from_key(kout, sizeof kout, 2, KDF_CTX, masterKey);

    // Inner layer — XChaCha20-Poly1305 (192-bit random nonce, misuse-resistant).
    const int NIN = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    unsigned char nin[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(nin, sizeof nin);
    QByteArray c1(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    unsigned long long c1len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        (unsigned char*) c1.data(), &c1len,
        (const unsigned char*) plain.constData(), plain.size(),
        nullptr, 0, nullptr, nin, kin);
    c1.resize((int) c1len);

    // Outer layer — AES-256-GCM where AES-NI exists, else a second independent XChaCha20.
    bool aes = crypto_aead_aes256gcm_is_available() != 0;
    char mode = aes ? 0 : 1;
    QByteArray nout, c2;
    unsigned long long c2len = 0;
    if (aes) {
        nout.resize(crypto_aead_aes256gcm_NPUBBYTES);
        randombytes_buf((unsigned char*) nout.data(), nout.size());
        c2.resize(c1.size() + crypto_aead_aes256gcm_ABYTES);
        crypto_aead_aes256gcm_encrypt(
            (unsigned char*) c2.data(), &c2len,
            (const unsigned char*) c1.constData(), c1.size(),
            nullptr, 0, nullptr, (const unsigned char*) nout.constData(), kout);
    } else {
        nout.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
        randombytes_buf((unsigned char*) nout.data(), nout.size());
        c2.resize(c1.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
        crypto_aead_xchacha20poly1305_ietf_encrypt(
            (unsigned char*) c2.data(), &c2len,
            (const unsigned char*) c1.constData(), c1.size(),
            nullptr, 0, nullptr, (const unsigned char*) nout.constData(), kout);
    }
    c2.resize((int) c2len);
    sodium_memzero(kin, sizeof kin);
    sodium_memzero(kout, sizeof kout);

    QByteArray out;
    out.append(kBlobMagic);          // 'Z''C''S'
    out.append((char) 1);            // version
    out.append(mode);
    out.append((const char*) nin, NIN);
    out.append(nout);
    out.append(c2);
    return out;
}

bool SecureStore::decrypt(const QByteArray& blob, QByteArray& plainOut) {
    if (!unlocked) return false;
    const int HDR = 5;   // magic(3) + ver(1) + mode(1)
    const int NIN = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (blob.size() < HDR + NIN) return false;
    if (blob.left(3) != kBlobMagic) return false;
    char mode = blob.at(4);
    int noutLen = (mode == 0) ? crypto_aead_aes256gcm_NPUBBYTES
                              : crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (blob.size() <= HDR + NIN + noutLen) return false;

    const unsigned char* p    = (const unsigned char*) blob.constData();
    const unsigned char* nin  = p + HDR;
    const unsigned char* nout = nin + NIN;
    const unsigned char* c2   = nout + noutLen;
    int c2len = blob.size() - (HDR + NIN + noutLen);

    unsigned char kin[32], kout[32];
    crypto_kdf_derive_from_key(kin,  sizeof kin,  1, KDF_CTX, masterKey);
    crypto_kdf_derive_from_key(kout, sizeof kout, 2, KDF_CTX, masterKey);

    // Outer decrypt -> c1
    QByteArray c1(c2len, 0);
    unsigned long long c1len = 0;
    int rc;
    if (mode == 0) {
        if (crypto_aead_aes256gcm_is_available() == 0) {
            sodium_memzero(kin, sizeof kin); sodium_memzero(kout, sizeof kout);
            return false;   // file needs AES-NI to read; this machine lacks it
        }
        rc = crypto_aead_aes256gcm_decrypt((unsigned char*) c1.data(), &c1len, nullptr,
                 c2, c2len, nullptr, 0, nout, kout);
    } else {
        rc = crypto_aead_xchacha20poly1305_ietf_decrypt((unsigned char*) c1.data(), &c1len, nullptr,
                 c2, c2len, nullptr, 0, nout, kout);
    }
    if (rc != 0) { sodium_memzero(kin, sizeof kin); sodium_memzero(kout, sizeof kout); return false; }
    c1.resize((int) c1len);

    // Inner decrypt -> plaintext
    QByteArray plain(c1.size(), 0);
    unsigned long long plen = 0;
    rc = crypto_aead_xchacha20poly1305_ietf_decrypt((unsigned char*) plain.data(), &plen, nullptr,
             (const unsigned char*) c1.constData(), c1.size(), nullptr, 0, nin, kin);
    sodium_memzero(kin, sizeof kin);
    sodium_memzero(kout, sizeof kout);
    if (rc != 0) return false;
    plain.resize((int) plen);
    plainOut = plain;
    return true;
}

// ---------------------------------------------------------------------------
// Master password / verifier
// ---------------------------------------------------------------------------
// Verifier: "ZCK1" | ver(1) | secretType(1) | salt(16) | ops(8) | mem(8) | canaryBlob
static const int kVerHead = 4 + 1 + 1 + crypto_pwhash_SALTBYTES + 8 + 8;

SecureStore::SecretType SecureStore::storedSecretType() const {
    QFile f(verifierPath());
    if (!f.open(QIODevice::ReadOnly)) return SecretPassword;
    QByteArray v = f.read(6);
    f.close();
    if (v.size() < 6 || v.left(4) != kVerMagic) return SecretPassword;
    int t = (unsigned char) v.at(5);
    return (t == SecretKeyfile || t == SecretBoth) ? (SecretType) t : SecretPassword;
}

bool SecureStore::createMaster(SecretType type, const QString& password, const QString& keyfilePath) {
    if (verifierExists()) return false;
    QByteArray secret = buildSecret(type, password, keyfilePath);
    if (secret.isEmpty()) return false;

    unsigned char salt[crypto_pwhash_SALTBYTES];
    randombytes_buf(salt, sizeof salt);

    // Strict: ALWAYS Argon2id SENSITIVE (~1 GiB, multi-second). No weaker fallback — we never
    // silently downgrade the brute-force cost. The exact parameters are recorded in the verifier
    // so unlock reproduces the key. If this machine can't allocate SENSITIVE, setup fails loudly.
    quint64 ops = crypto_pwhash_OPSLIMIT_SENSITIVE;
    quint64 mem = crypto_pwhash_MEMLIMIT_SENSITIVE;
    if (!deriveMaster(secret, salt, ops, mem)) {
        sodium_memzero(secret.data(), secret.size());
        lock();
        return false;
    }
    sodium_memzero(secret.data(), secret.size());
    unlocked = true;

    QByteArray cblob = encrypt(kCanary);
    if (cblob.isEmpty()) { lock(); return false; }

    QByteArray v;
    v.append(kVerMagic);               // "ZCK1"
    v.append((char) 1);                // version
    v.append((char) type);             // secret type
    v.append((const char*) salt, (int) sizeof salt);
    for (int i = 0; i < 8; i++) v.append((char) ((ops >> (8 * i)) & 0xff));
    for (int i = 0; i < 8; i++) v.append((char) ((mem >> (8 * i)) & 0xff));
    v.append(cblob);
    if (!atomicWrite(verifierPath(), v)) { lock(); return false; }
    return true;
}

bool SecureStore::unlock(SecretType type, const QString& password, const QString& keyfilePath) {
    QFile f(verifierPath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray v = f.readAll();
    f.close();

    if (v.size() < kVerHead) return false;
    if (v.left(4) != kVerMagic) return false;

    const unsigned char* salt = (const unsigned char*) v.constData() + 6;
    const unsigned char* q    = salt + crypto_pwhash_SALTBYTES;
    quint64 ops = 0, mem = 0;
    for (int i = 0; i < 8; i++) ops |= ((quint64) (unsigned char) q[i])     << (8 * i);
    for (int i = 0; i < 8; i++) mem |= ((quint64) (unsigned char) q[8 + i]) << (8 * i);
    QByteArray cblob = v.mid(kVerHead);

    QByteArray secret = buildSecret(type, password, keyfilePath);
    if (!deriveMaster(secret, salt, ops, mem)) { sodium_memzero(secret.data(), secret.size()); lock(); return false; }
    sodium_memzero(secret.data(), secret.size());
    unlocked = true;

    QByteArray got;
    if (!decrypt(cblob, got) || got != kCanary) { lock(); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// File helpers + migration
// ---------------------------------------------------------------------------
bool SecureStore::writeFile(const QString& path, const QByteArray& plain) {
    if (!unlocked) return false;
    QByteArray blob = encrypt(plain);
    if (blob.isEmpty() && !plain.isEmpty()) return false;
    return atomicWrite(path, blob);
}

bool SecureStore::readFile(const QString& path, QByteArray& plainOut) {
    if (!unlocked) return false;
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
    QByteArray blob = f.readAll();
    f.close();
    return decrypt(blob, plainOut);
}

QByteArray SecureStore::migrateIfNeeded(const QString& legacyPath, const QString& encPath) {
    QByteArray plain;
    if (QFile::exists(encPath)) {
        readFile(encPath, plain);
        return plain;
    }
    QFile lf(legacyPath);
    if (lf.exists() && lf.open(QIODevice::ReadOnly)) {
        plain = lf.readAll();
        lf.close();
        if (writeFile(encPath, plain))
            lf.remove();   // upgraded in place — drop the cleartext original
    }
    return plain;
}

// ---------------------------------------------------------------------------
// Reset (used when the user forgot the password and chooses to start fresh)
// ---------------------------------------------------------------------------
void SecureStore::purgeAllData() {
    QDir d(dataDir());
    static const char* bases[] = {
        "senttxstore", "addresslabels", "turnstilemigrationplan"
    };
    for (const char* b : bases) {
        for (const QString& pfx : { QStringLiteral(""), QStringLiteral("testnet-") }) {
            d.remove(pfx % QString(b) % ".enc");
            d.remove(pfx % QString(b) % ".dat");
        }
    }
    d.remove("master.bin");
    lock();
}

// ---------------------------------------------------------------------------
// Interactive bootstrap at startup
// ---------------------------------------------------------------------------
// Prompt for the secret(s) of a given type. For create==true a password is asked twice and
// confirmed. Returns false if the user cancelled. On success, `password`/`keyfile` are filled.
static bool promptSecret(QWidget* parent, SecureStore::SecretType type, bool create,
                         QString& password, QString& keyfile) {
    password.clear(); keyfile.clear();

    if (type == SecureStore::SecretKeyfile || type == SecureStore::SecretBoth) {
        keyfile = QFileDialog::getOpenFileName(parent,
            QObject::tr("Choose your keyfile"), QString());
        if (keyfile.isEmpty()) return false;
        if (create)
            QMessageBox::information(parent, QObject::tr("Keep your keyfile safe"),
                QObject::tr("Your keyfile is now part of your wallet's lock. <b>Back it up</b> "
                            "(e.g. a USB stick) and keep it private — if you lose it, the encrypted "
                            "data can't be read; if someone copies it%1, they hold part of the key.")
                .arg(type == SecureStore::SecretKeyfile ? QObject::tr(" and it's your only factor")
                                                        : QString()));
    }

    if (type == SecureStore::SecretPassword || type == SecureStore::SecretBoth) {
        bool ok = false;
        QString p1 = QInputDialog::getText(parent,
            create ? QObject::tr("Set password") : QObject::tr("Enter password"),
            QObject::tr("Wallet data password:"), QLineEdit::Password, "", &ok);
        if (!ok) return false;
        if (create) {
            if (p1.isEmpty()) {
                QMessageBox::warning(parent, QObject::tr("Set password"),
                    QObject::tr("The password can't be empty."));
                return false;
            }
            QString p2 = QInputDialog::getText(parent, QObject::tr("Confirm password"),
                QObject::tr("Re-enter the password to confirm:"), QLineEdit::Password, "", &ok);
            if (!ok) return false;
            if (p1 != p2) {
                QMessageBox::warning(parent, QObject::tr("Confirm password"),
                    QObject::tr("The passwords didn't match. Try again."));
                return false;
            }
        }
        password = p1;
    }
    return true;
}

// A small frameless splash shown while the multi-second Argon2id derivation runs on a worker
// thread. The ZCL logo and the indeterminate progress bar animate because the main thread keeps
// pumping the event loop (the heavy crypto is off-thread), so the user is never left staring at
// a frozen, blank screen for a few seconds after typing the password.
class DeriveSplash : public QWidget {
public:
    DeriveSplash(const QString& msg, QWidget* parent)
        : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint) {
        setObjectName("DeriveSplash");
        setFixedSize(380, 320);
        setStyleSheet("#DeriveSplash { background:#26292d; border:1px solid #ce742f; }");

        auto v = new QVBoxLayout(this);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(16);
        v->setAlignment(Qt::AlignCenter);

        auto logo = new QLabel(this);
        logo->setAlignment(Qt::AlignCenter);
        logoMovie = new QMovie(":/img/res/logobig.gif", QByteArray(), this);
        if (logoMovie->isValid()) {
            logoMovie->setScaledSize(QSize(180, 180));
            logo->setMovie(logoMovie);
            logoMovie->start();
        } else {
            QPixmap pm(":/img/res/logobig.gif");
            if (!pm.isNull())
                logo->setPixmap(pm.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        v->addWidget(logo);

        auto title = new QLabel(QObject::tr("ZclWallet"), this);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("color:#ce742f; font-size:18px; font-weight:bold;");
        v->addWidget(title);

        auto text = new QLabel(msg, this);
        text->setAlignment(Qt::AlignCenter);
        text->setStyleSheet("color:#e0e0e0;");
        v->addWidget(text);

        auto bar = new QProgressBar(this);
        bar->setRange(0, 0);                 // indeterminate / busy animation
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        v->addWidget(bar);
    }
    QMovie* logoMovie = nullptr;
};

// Run `work` (the heavy key derivation) on a worker thread while the splash animates on the main
// thread. Returns work()'s result. The work lambda must touch no GUI objects (it doesn't).
static bool runWithSplash(QWidget* parent, const QString& msg, std::function<bool()> work) {
    DeriveSplash splash(msg, parent);
    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        QRect g = scr->geometry();
        splash.move(g.center() - QPoint(splash.width() / 2, splash.height() / 2));
    }
    splash.show();
    splash.raise();
    QApplication::processEvents();

    std::atomic<bool> finished(false);
    bool result = false;
    std::thread worker([&]() { result = work(); finished.store(true); });
    while (!finished.load()) {
        QApplication::processEvents(QEventLoop::AllEvents, 30);
        QThread::msleep(15);
    }
    worker.join();
    splash.close();
    QApplication::processEvents();
    return result;
}

bool SecureStore::bootstrap(QWidget* parent) {
    if (!verifierExists()) {
        QMessageBox::information(parent, QObject::tr("Protect your wallet data"),
            QObject::tr(
            "<b>This wallet now encrypts the sensitive files it stores on your computer.</b><br><br>"
            "Your shielded <b>transaction history</b>, <b>address labels</b> and turnstile plan "
            "are written to disk. Until now some of these were in plain text — "
            "readable by anyone with access to this machine or a backup of it.<br><br>"
            "You'll set a <b>password and/or a keyfile</b> now. It encrypts and decrypts those files "
            "with a strong cipher cascade (XChaCha20-Poly1305 + AES-256), and in future it will also "
            "unlock the wallet's own multi-cipher key encryption.<br><br>"
            "<b>There is no recovery.</b> If you lose your password/keyfile, the encrypted "
            "history/labels cannot be read — you can only purge them and start fresh (your coins are "
            "safe in wallet.dat regardless)."));

        // Choose the unlock factor(s).
        QMessageBox choose(parent);
        choose.setIcon(QMessageBox::Question);
        choose.setWindowTitle(QObject::tr("Choose how to unlock"));
        choose.setText(QObject::tr("How do you want to protect your wallet data?"));
        choose.setInformativeText(QObject::tr(
            "<b>Password</b> — something you remember.<br>"
            "<b>Keyfile</b> — a file you keep (e.g. on a USB stick); nothing to memorise.<br>"
            "<b>Both</b> — strongest: a stolen disk needs your keyfile <i>and</i> your password."));
        QPushButton* bPw   = choose.addButton(QObject::tr("Password"), QMessageBox::AcceptRole);
        QPushButton* bKf   = choose.addButton(QObject::tr("Keyfile"),  QMessageBox::AcceptRole);
        QPushButton* bBoth = choose.addButton(QObject::tr("Both"),     QMessageBox::AcceptRole);
        QPushButton* bQuit = choose.addButton(QObject::tr("Quit"),     QMessageBox::RejectRole);
        choose.setDefaultButton(bPw);
        choose.exec();
        if (choose.clickedButton() == bQuit) return false;
        SecretType type = (choose.clickedButton() == bKf)   ? SecretKeyfile
                        : (choose.clickedButton() == bBoth) ? SecretBoth
                                                            : SecretPassword;

        for (;;) {
            QString pw, kf;
            if (!promptSecret(parent, type, /*create*/ true, pw, kf)) {
                // Let them re-pick the factor or quit rather than dead-ending.
                auto again = QMessageBox::question(parent, QObject::tr("Set up encryption"),
                    QObject::tr("Encryption setup wasn't completed. Try again?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (again == QMessageBox::Yes) continue;
                return false;
            }
            bool created = runWithSplash(parent, QObject::tr("Encrypting your wallet data…"),
                [&]() { return createMaster(type, pw, kf); });
            if (!created) {
                QMessageBox::critical(parent, QObject::tr("Encryption setup failed"),
                    QObject::tr("Could not initialise the encrypted store. This can happen if the "
                                "keyfile is unreadable, or if this machine couldn't allocate the "
                                "~1 GiB needed for the strong key derivation. Aborting for safety."));
                return false;
            }
            return true;
        }
    }

    // Returning user — unlock with the stored factor(s); retry / reset / quit on failure.
    SecretType type = storedSecretType();
    for (;;) {
        QString pw, kf;
        if (!promptSecret(parent, type, /*create*/ false, pw, kf)) {
            // Treat a cancel as "give me the wrong-secret choices" rather than a silent quit.
        } else if (runWithSplash(parent, QObject::tr("Decrypting your wallet data…"),
                                 [&]() { return unlock(type, pw, kf); })) {
            return true;
        }

        QMessageBox box(parent);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QObject::tr("Couldn't unlock"));
        box.setText(QObject::tr("That password/keyfile didn't unlock your wallet data."));
        box.setInformativeText(QObject::tr(
            "Try again, or <b>reset</b> — which permanently deletes the encrypted transaction "
            "history, address labels and turnstile plan so you can start fresh. "
            "Your coins are NOT affected (they live in wallet.dat)."));
        QPushButton* retry = box.addButton(QObject::tr("Try again"), QMessageBox::AcceptRole);
        QPushButton* reset = box.addButton(QObject::tr("Reset (delete data)"), QMessageBox::DestructiveRole);
        QPushButton* quit  = box.addButton(QObject::tr("Quit"), QMessageBox::RejectRole);
        box.setDefaultButton(retry);
        box.exec();

        if (box.clickedButton() == quit) return false;
        if (box.clickedButton() == reset) {
            auto sure = QMessageBox::warning(parent, QObject::tr("Confirm reset"),
                QObject::tr("Permanently delete the encrypted wallet data and start fresh? "
                            "This cannot be undone."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (sure == QMessageBox::Yes) {
                purgeAllData();
                return bootstrap(parent);   // fall back into first-run setup flow
            }
        }
        // else: retry loop
    }
}
