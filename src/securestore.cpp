#include "securestore.h"
#include "settings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringBuilder>
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

// The 8-byte (crypto_kdf_CONTEXTBYTES) domain-separation context for subkey derivation.
static const char* KDF_CTX = "zclstore";
static const QByteArray kCanary    = QByteArrayLiteral("zcl-secure-store-canary-v1");
static const QByteArray kBlobMagic = QByteArrayLiteral("ZCS");
static const QByteArray kVerMagic  = QByteArrayLiteral("ZCK1");

// The logical stores SecureStore manages (used for reset and to keep the testnet/extension
// rule in ONE place, so a future 4th sensitive file can't silently escape a privacy reset).
static const char* kStores[] = { "senttxstore", "addresslabels", "turnstilemigrationplan" };

// Atomic, owner-only (0600) write.
static bool atomicWrite(const QString& path, const QByteArray& bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(bytes) != bytes.size()) { f.cancelWriting(); return false; }
    if (!f.commit()) return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

// Best-effort secure delete of a plaintext file: overwrite its bytes with zeros, flush, then
// unlink. HONEST CAVEAT: on copy-on-write / log-structured / SSD-wear-levelled filesystems
// this does NOT guarantee the old bytes are gone from the physical medium — only full-disk
// encryption does. We still do it because it raises the bar on simple filesystems and a plain
// unlink leaves the cleartext fully recoverable. (See docs/OPSEC_ENCRYPTION.md.)
static void secureRemove(const QString& path) {
    QFile f(path);
    if (!f.exists()) return;
    if (f.open(QIODevice::ReadWrite)) {
        qint64 n = f.size();
        f.seek(0);
        QByteArray zeros(qMin<qint64>(n, 65536), '\0');
        while (n > 0) {
            qint64 chunk = qMin<qint64>(n, zeros.size());
            if (f.write(zeros.constData(), chunk) != chunk) break;
            n -= chunk;
        }
        f.flush();
        f.close();
    }
    QFile::remove(path);
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
    // Owner-only directory (0700) so siblings can't enumerate our files.
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return dir;
}

QString SecureStore::storePath(const QString& base, bool encrypted) const {
    const QString name = (Settings::getInstance()->isTestnet() ? QStringLiteral("testnet-") : QString())
                       % base % (encrypted ? QStringLiteral(".enc") : QStringLiteral(".dat"));
    return QDir(dataDir()).filePath(name);
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
    // Both: bind them so neither alone suffices — BLAKE2b(password || keyfileHash). kf is always
    // exactly crypto_generichash_BYTES, so the concatenation boundary is unambiguous.
    if (kf.isEmpty()) { sodium_memzero(pw.data(), pw.size()); return QByteArray(); }
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, crypto_generichash_BYTES);
    crypto_generichash_update(&st, (const unsigned char*) pw.constData(), pw.size());
    crypto_generichash_update(&st, (const unsigned char*) kf.constData(), kf.size());
    QByteArray out(crypto_generichash_BYTES, 0);
    crypto_generichash_final(&st, (unsigned char*) out.data(), out.size());
    sodium_memzero(pw.data(), pw.size());
    sodium_memzero(kf.data(), kf.size());
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
// Single-layer AEAD: XChaCha20-Poly1305 over a crypto_kdf subkey, header authenticated as AAD.
// Blob: 'Z''C''S' | ver(1) | nonce(24) | ciphertext(+16B tag)
// ---------------------------------------------------------------------------
QByteArray SecureStore::encrypt(const QByteArray& plain) {
    if (!unlocked) return QByteArray();

    unsigned char k[32];
    crypto_kdf_derive_from_key(k, sizeof k, 1, KDF_CTX, masterKey);

    QByteArray hdr;
    hdr.append(kBlobMagic);        // 'Z''C''S'
    hdr.append((char) 1);          // version

    const int NP = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(nonce, sizeof nonce);

    QByteArray c(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    unsigned long long clen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        (unsigned char*) c.data(), &clen,
        (const unsigned char*) plain.constData(), plain.size(),
        (const unsigned char*) hdr.constData(), hdr.size(),   // header authenticated (AAD)
        nullptr, nonce, k);
    sodium_memzero(k, sizeof k);
    if (rc != 0) return QByteArray();
    c.resize((int) clen);

    QByteArray out;
    out.append(hdr);
    out.append((const char*) nonce, NP);
    out.append(c);
    return out;
}

bool SecureStore::decrypt(const QByteArray& blob, QByteArray& plainOut) {
    if (!unlocked) return false;
    const int HDR = 4;   // magic(3) + ver(1)
    const int NP  = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const int AB  = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (blob.size() < HDR + NP + AB) return false;
    if (blob.left(3) != kBlobMagic)  return false;
    // The version byte (blob[3]) is not parsed here: the whole 4-byte header is authenticated as
    // AEAD associated data, so any version other than the one we encrypt with fails the Poly1305
    // tag below (returns false) — a future format bump can't be silently misread as this one.

    const unsigned char* p     = (const unsigned char*) blob.constData();
    const unsigned char* nonce = p + HDR;
    const unsigned char* c     = nonce + NP;
    int clen = blob.size() - (HDR + NP);

    unsigned char k[32];
    crypto_kdf_derive_from_key(k, sizeof k, 1, KDF_CTX, masterKey);

    QByteArray plain(clen, 0);   // >= plaintext length
    unsigned long long plen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        (unsigned char*) plain.data(), &plen, nullptr,
        c, clen,
        p, HDR,                  // header must match the AAD used at encrypt
        nonce, k);
    sodium_memzero(k, sizeof k);
    if (rc != 0) return false;
    plain.resize((int) plen);
    plainOut = plain;
    return true;
}

// ---------------------------------------------------------------------------
// Master password / verifier
// Verifier: "ZCK1" | ver(1) | secretType(1) | salt(16) | ops(8) | mem(8) | canaryBlob
// ---------------------------------------------------------------------------
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

    // MODERATE (~256 MiB, sub-second): memory-hard, but fast enough never to freeze startup and
    // light enough never to lock a usable machine out of its own history. The exact ops/mem are
    // recorded below so unlock reproduces the key and a future stronger tier can coexist.
    quint64 ops = crypto_pwhash_OPSLIMIT_MODERATE;
    quint64 mem = crypto_pwhash_MEMLIMIT_MODERATE;
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
// Store-facing API (the only file I/O the stores use). Plaintext when not unlocked, encrypted
// (with transparent legacy migration) when unlocked — the caller never knows which.
// ---------------------------------------------------------------------------
QByteArray SecureStore::loadStore(const QString& base, bool& ok) {
    ok = true;
    const QString datPath = storePath(base, /*encrypted*/ false);
    const QString encPath = storePath(base, /*encrypted*/ true);

    if (!unlocked) {
        // Encryption off this session: read the plaintext form, exactly as before. (If only an
        // .enc exists — the user turned encryption off — we can't read it; report empty, not a
        // hard error, so writes aren't blocked. The history reappears if they re-enable.)
        QFile f(datPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            QByteArray b = f.readAll(); f.close(); return b;
        }
        return QByteArray();
    }

    const bool haveEnc = QFile::exists(encPath);
    const bool haveDat = QFile::exists(datPath);

    // A plaintext .dat that is NEWER than the .enc was written during an opt-out interval
    // (encryption turned OFF, edits made, then turned back ON) and is the authoritative copy —
    // migrate it FORWARD rather than silently reverting to the stale .enc and losing those edits.
    // (After any normal unlocked save the .dat is wiped, so a newer .dat can only come from such
    // an interval; on a first opt-in there is no .enc yet, so this is also the legacy-migration path.)
    if (haveDat && (!haveEnc ||
                    QFileInfo(datPath).lastModified() > QFileInfo(encPath).lastModified())) {
        QFile lf(datPath);
        if (lf.open(QIODevice::ReadOnly)) {
            QByteArray plain = lf.readAll(); lf.close();
            saveStore(base, plain);   // re-encrypts to .enc and best-effort wipes the cleartext .dat
            return plain;
        }
    }

    // Otherwise the .enc is authoritative.
    if (haveEnc) {
        if (haveDat) secureRemove(datPath);   // drop any stale (older) cleartext leftover
        QByteArray blob; QFile f(encPath);
        if (f.open(QIODevice::ReadOnly)) { blob = f.readAll(); f.close(); }
        QByteArray plain;
        if (decrypt(blob, plain)) return plain;
        // Present but UNDECRYPTABLE (corruption — the key is right since we're unlocked). Do NOT
        // return empty: that would let the caller overwrite irreplaceable history. Signal a hard
        // error; the ciphertext is left intact (the user can recover or reset from Settings).
        ok = false;
        return QByteArray();
    }
    return QByteArray();   // nothing stored yet
}

bool SecureStore::saveStore(const QString& base, const QByteArray& plain) {
    if (unlocked) {
        const QString encPath = storePath(base, /*encrypted*/ true);
        // NEVER overwrite an existing encrypted store we can't read back (corruption) — doing so
        // would destroy recoverable ciphertext (the only record of past shielded sends). Refuse;
        // the caller keeps the prior file intact. This single chokepoint protects EVERY store
        // (present and future callers), not just the ones that pre-check loadStore()'s ok flag.
        if (QFile::exists(encPath)) {
            QFile f(encPath);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray cur = f.readAll(); f.close();
                QByteArray tmp;
                if (!decrypt(cur, tmp)) return false;
            }
        }
        QByteArray blob = encrypt(plain);
        if (blob.isEmpty() && !plain.isEmpty()) return false;
        bool wrote = atomicWrite(encPath, blob);
        if (wrote) secureRemove(storePath(base, /*encrypted*/ false));   // drop any stale cleartext
        return wrote;
    }
    // Encryption off: write the plaintext form, atomic + owner-only (0600), as before.
    return atomicWrite(storePath(base, /*encrypted*/ false), plain);
}

void SecureStore::removeStore(const QString& base) {
    secureRemove(storePath(base, /*encrypted*/ false));
    QFile::remove(storePath(base, /*encrypted*/ true));
}

// ---------------------------------------------------------------------------
// Reset (used when the user forgot the password and chooses to start fresh)
// ---------------------------------------------------------------------------
void SecureStore::purgeAllData() {
    // Remove BOTH networks' forms (mainnet + testnet-) of every managed store, without mutating
    // the global testnet setting. Plaintext forms are best-effort wiped before unlink.
    QDir d(dataDir());
    for (const char* b : kStores) {
        for (const QString& pfx : { QString(), QStringLiteral("testnet-") }) {
            secureRemove(d.filePath(pfx % QString(b) % QStringLiteral(".dat")));
            QFile::remove(d.filePath(pfx % QString(b) % QStringLiteral(".enc")));
        }
    }
    QFile::remove(verifierPath());
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
                QObject::tr("Your keyfile is now part of your wallet's lock. Back it up and keep "
                            "it private — lose it and the encrypted data can't be read."));
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

// A small frameless splash shown while the Argon2id derivation runs on a worker thread. The
// logo + indeterminate bar animate because the main thread keeps a QEventLoop running (the
// crypto is off-thread), so the user never stares at a frozen window.
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

// Run `work` (the key derivation) on a worker thread while the splash animates on a nested
// event loop. `work` touches no GUI objects (it doesn't); nothing else touches SecureStore
// during bootstrap (it runs before MainWindow exists), so there is no data race on the key.
static bool runWithSplash(QWidget* parent, const QString& msg, std::function<bool()> work) {
    DeriveSplash splash(msg, parent);
    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        QRect g = scr->geometry();
        splash.move(g.center() - QPoint(splash.width() / 2, splash.height() / 2));
    }
    splash.show();
    splash.raise();

    QEventLoop loop;
    bool result = false;
    std::thread worker([&]() {
        result = work();
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();
    splash.close();
    return result;
}

bool SecureStore::bootstrap(QWidget* parent) {
    if (!verifierExists()) {
        QMessageBox::information(parent, QObject::tr("Protect your wallet data"),
            QObject::tr(
            "Your shielded transaction history and address labels will be encrypted on this "
            "computer with a password you set now.<br><br>"
            "<b>There is no recovery</b> — if you lose the password you can only erase that data "
            "and start fresh. <b>Your coins are safe either way</b> (they live in wallet.dat, "
            "which this never touches)."));

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
                auto again = QMessageBox::question(parent, QObject::tr("Set up encryption"),
                    QObject::tr("Encryption setup wasn't completed. Try again?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (again == QMessageBox::Yes) continue;
                return false;
            }
            bool created = runWithSplash(parent, QObject::tr("Encrypting your wallet data…"),
                [&]() { return createMaster(type, pw, kf); });
            pw.fill(QChar('\0')); pw.clear();
            if (!created) {
                QMessageBox::critical(parent, QObject::tr("Encryption setup failed"),
                    QObject::tr("Could not set up the encrypted store. The keyfile may be "
                                "unreadable, or this machine couldn't allocate the memory the "
                                "key derivation needs. Aborting for safety."));
                return false;
            }
            return true;
        }
    }

    // Returning user — unlock with the stored factor(s); retry / reset / quit on failure.
    SecretType type = storedSecretType();
    for (;;) {
        QString pw, kf;
        bool entered = promptSecret(parent, type, /*create*/ false, pw, kf);
        bool unlockedOk = entered && runWithSplash(parent, QObject::tr("Unlocking your wallet data…"),
                                                   [&]() { return unlock(type, pw, kf); });
        pw.fill(QChar('\0')); pw.clear();
        if (unlockedOk) return true;

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
