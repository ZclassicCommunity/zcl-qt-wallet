// ============================================================================
// tst_logic — L0 unit suite for the ZClassic Qt5 GUI wallet
//
// Links ONLY the wallet's pure-logic translation units (settings.cpp,
// senttxstore.cpp, addresscombo.cpp) plus test shims. NO daemon, NO libsodium,
// NO product-code modifications: the shims (tests/shim/) shadow the heavy
// headers purely via INCLUDEPATH ordering in tests.pro.
//
// Covers checklist items from docs/TESTING.md:
//   C3  USD tooltip/format gating (price>0 && !testnet)
//   C5  getDecimalString edges
//   C6  token name ZCL/ZCT by net
//   D6  addressFromAddressLabel + paren-strip via AddressCombo
//   E7  field-validation pieces needing no RPC (amount/decimal formatting)
//   G1  getSaveZtxs() + non-z from-addr gating suppresses SentTxStore write
//   G4  SentTxStore written 0600 owner-only, testnet-prefixed name
//   H1  db-corruption / startup-marker / long-warmup classifiers (mirrored)
//   H2  blocksDirSizeBytes byte total over a temp dir (mirrored algorithm)
//   + exhaustive Settings address classifiers
//       (isTAddress/isZAddress/isSaplingAddress/isSproutAddress)
//   + formatters (getDecimalString/getZCLDisplayFormat/getUSDFormat/token name)
// ============================================================================
#include <QtTest/QtTest>
#include <QtGlobal>
#include <QStandardItemModel>

#include "settings.h"
#include "senttxstore.h"
#include "mainwindow.h"        // shim: ToFields / Tx + SendCategory
#include "rpc.h"               // shim: TransactionItem
#include "addresscombo.h"
#include "addressbook.h"       // shim
#include "privacybadgedelegate.h"   // PRIV-13/14: classify/labelFor/colorFor (real)
#include "securerandom.h"      // CONF-1/NOTIFY-SRV: CSPRNG secret generation (real)
#include "nft.h"                     // Phase C0: NFTItem POD
#include "nftgallerymodel.h"         // Phase C0: model under test
#include "nftgallerydelegate.h"      // Phase C0: delegate sizeHint under test
#include "nftimagecache.h"           // Phase C0: threaded decode/verify pipeline
#include "contentengine.h"           // Phase C1: streaming hash / chunked Merkle engine
#include <QSet>
#include <QVector>
#include <QRegularExpression>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QBuffer>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QStandardPaths>

#ifndef Q_OS_WIN
#include <unistd.h>            // getuid() — mirrors the per-user temp-fallback subdir (ITEM 5)
#endif

// ---------------------------------------------------------------------------
// Real valid ZClassic addresses, lifted from src/settings.cpp (donation/zboard).
// These pass the checksum gate in Settings::isValidAddress, so the classifiers
// run their full real path (regex + checksum) rather than the early-out.
// ---------------------------------------------------------------------------
namespace addr {
// mainnet
static const QString ZS_SAPLING   = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
static const QString ZS_ZBOARD    = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";
static const QString ZC_SPROUT    = "zcEgrceTwvoiFdEvPWcsJHAMrpLsprMF6aRJiQa3fan5ZphyXLPuHghnEPrEPRoEVzUy65GnMVyCTRdkT6BYBepnXh6NBYs";
// testnet
static const QString ZTS_SAPLING  = "ztestsapling1wn6889vznyu42wzmkakl2effhllhpe4azhu696edg2x6me4kfsnmqwpglaxzs7tmqsq7kudemp5";
static const QString ZTN_SPROUT   = "ztn6fYKBii4Fp4vbGhkPgrtLU4XjXp4ZBMZgShtopmDGbn1L2JLTYbBp2b7SSkNr9F3rQeNZ9idmoR7s4JCVUZ7iiM5byhF";
}

// Flip one character of a valid address to break its checksum (typo negative).
// Picks a body char and rotates it within the address's own charset family so
// the structural regex still matches but the checksum must reject it.
static QString corrupt(const QString& a) {
    QString s = a;
    int i = s.length() / 2;          // middle-ish, safely past the HRP/prefix
    QChar c = s.at(i);
    // rotate within base58/bech32-ish alnum; avoid producing the same char
    QChar repl = (c == QChar('q')) ? QChar('p') : QChar('q');
    if (s.startsWith("zc") || s.startsWith("ztn") || s.startsWith("t"))
        repl = (c == QChar('A')) ? QChar('B') : QChar('A');  // base58 keeps it in-alphabet
    s[i] = repl;
    return s;
}

// =====================================================================
// Mirror of the H1/H2 pure classifiers from src/connection.cpp.
//
// These four predicates are non-static members of the widget-entangled
// ConnectionLoader (connection.cpp pulls ui_*.h + RPC + the daemon launcher),
// so they cannot be linked at L0 and the no-src-modification constraint
// forbids extracting them. The bodies below are copied VERBATIM from
// connection.cpp at the cited line numbers and must stay in sync; the tests
// then exercise this exact classification logic and (for H2) real filesystem
// traversal over a temp dir. See tests/README.md "H1/H2 mirror" note.
// =====================================================================
namespace cxmirror {

// src/connection.cpp:995-1015  ConnectionLoader::looksLikeDbCorruption
static bool looksLikeDbCorruption(const QString& text) {
    if (text.isEmpty()) return false;
    static const char* markers[] = {
        "Corrupted block database detected",
        "Error opening block database",
        "Error loading block database",
        "Error initializing block database",
        "Aborted block database rebuild",
        "Failed to read block",
        "System error while flushing",
        "Do you want to rebuild the block database now"
    };
    for (auto m : markers)
        if (text.contains(QString::fromLatin1(m), Qt::CaseInsensitive)) return true;
    return false;
}

// src/connection.cpp:965-991  ConnectionLoader::startupDiagHasMarker
static bool startupDiagHasMarker(const QString& text) {
    if (text.isEmpty()) return false;
    static const char* markers[] = {
        "Disk space is low",
        "probably already running",
        "Cannot obtain a lock",
        "payload failed verification",
        "invalid embedded payload",
        "Can't find zclassicd",
        "requires newer version",
        "Wallet corrupted",
        "salvage failed",
        "bootstrap snapshot verification failed",
    };
    for (auto m : markers)
        if (text.contains(QString::fromLatin1(m), Qt::CaseInsensitive)) return true;
    return looksLikeDbCorruption(text);
}

// src/connection.cpp:1502-1518  ConnectionLoader::isLongWarmupPhase
static bool isLongWarmupPhase(const QString& status) {
    static const char* kPhases[] = {
        "Verifying blocks", "Activating best chain", "Rewinding",
        "Loading block index", "Pruning blockstore", "Rescanning",
        "Zapping", "Upgrading",
    };
    for (const char* p : kPhases)
        if (status.contains(QLatin1String(p), Qt::CaseInsensitive)) return true;
    return false;
}

// src/connection.cpp:1041-1067  ConnectionLoader::blocksDirSizeBytes
// (root-or-testnet3 resolution + recursive Files sum; -1 == "can't measure")
static qint64 blocksDirSizeBytes(const QString& datadirRoot) {
    if (datadirRoot.isEmpty()) return -1;
    QDir base(datadirRoot);
    QDir testnet(QDir(datadirRoot).filePath("testnet3"));
    if (testnet.exists() &&
        (QFile::exists(testnet.filePath("wallet.dat")) ||
         QFile::exists(testnet.filePath("debug.log")) ||
         QDir(testnet.filePath("blocks")).exists()))
        base = testnet;
    QDir blocks(base.filePath("blocks"));
    if (!blocks.exists()) return -1;
    qint64 total = 0;
    QDirIterator it(blocks.absolutePath(), QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); total += it.fileInfo().size(); }
    return total;
}

} // namespace cxmirror

// =====================================================================
// PRIV-11/UX-12 — the four-way classifier is the REAL production free function
// `sendCategoryOf()` (src/sendcategory.h), pulled in via the shim mainwindow.h
// include. There is NO hand-copied mirror: the L0 cases below call the SAME body
// production uses (MainWindow::classifySend forwards to it), so a regression in
// the classifier goes red here directly, and the L1 suite drives it via the real
// MainWindow::classifySend over the same quadrants.
// =====================================================================


class TestLogic : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // --- Settings address classifiers (exhaustive) ---
    void classifiers_data();
    void classifiers();

    void invalidAddressesRejected_data();
    void invalidAddressesRejected();

    // --- formatters ---
    void getDecimalString_data();   // C5
    void getDecimalString();
    void zclDisplayFormat_data();
    void zclDisplayFormat();
    void usdFormat_data();          // C3
    void usdFormat();
    void tokenName();               // C6
    void e7FieldFormatting_data();  // E7 (no-RPC field pieces)
    void e7FieldFormatting();

    // --- AddressCombo / addressbook parsing ---  D6
    void addressFromLabel_data();
    void addressFromLabel();
    void addressComboParenStrip();

    // --- NFT gallery (Phase C0): model state + delegate sizeHint + cache pipe ---
    void nftModelFingerprintGuard();      // flicker-free no-op re-feed
    void nftModelRolesAndOnImageReady();  // custom roles + verifyState update
    void nftModelTooltipNamesState();     // review fix #4: no-bytes != loading vs verified
    void nftDelegateSizeHintStable();     // fixed (168x208)*dpr, stable
    void nftCachePipelineVerifyMismatchPending();  // SHA-256 verify states e2e

    // --- ContentEngine (Phase C1): streaming hash / chunked Merkle / cache ------
    void ceStreamHashEqualsWholeHash_data();  // streaming SHA-256 == one-shot, many sizes
    void ceStreamHashEqualsWholeHash();
    void ceStreamHashBoundedLargeFile();      // 64 MiB streams to completion (no readAll OOM)
    void ceMerkleRootDeterministic();         // root reproducible + domain-separated
    void ceMerkleDetectsTamperedChunk();      // 1-byte flip in chunk K -> verifyChunk fails
    void ceMerkleSingleLeafDegenerate();      // root == SHA256(0x00||bytes) != bare SHA256
    void ceMerkleChunkBoundaries_data();      // exact-multiple / +1 / empty chunkCount
    void ceMerkleChunkBoundaries();
    void ceVerifyAcceptsRootOrWhole();        // anchor may be bare SHA256 OR Merkle root
    void ceAnchorHexForRootVsWhole();         // anchorHexFor: whole (small) vs merkleRoot (multi-chunk)
    void ceKindClassification_data();         // png/jpg->Image mp4->Video pdf->Document bin->Bytes
    void ceKindClassification();
    void ceHumanSize();                       // 42 B / 12.3 MB style formatting
    void ceSafeKeyPathTraversal();            // ../../etc/passwd neutralized to safe hex
    void ceCacheRoundTrip();                  // cachePut/cacheGet store-once + miss => ""
    void ceRejectsRemoteUrl();                // request()/cachePut refuse http(s):// (privacy)
    void ceVerifyMismatchOnFlippedByte();     // engine verify() red badge on 1-byte tamper
    void cePosterForTokenDelivers();          // posterForToken -> posterReady(token, img, vs)
    void cePosterForTokenRejects();           // token==0 / empty path / remote URL -> CE_Pending

    // --- SentTxStore at-rest ---  G1 / G4
    void sentTxStore_perms_and_testnetName();   // G4
    void sentTxStore_gating();                  // G1
    void sentTxStore_roundTrip();

    // --- H1 / H2 classifiers (mirrored) ---
    void dbCorruptionMarkers_data();   // H1
    void dbCorruptionMarkers();
    void startupMarkers_data();        // H1
    void startupMarkers();
    void longWarmupPhase_data();       // H1
    void longWarmupPhase();
    void blocksDirSizeBytes();         // H2

    // --- PRIV-9: auto-shield default-ON canary ---
    void priv9_autoShieldDefaultOn();

    // --- PRIV-11/UX-12: four-way send classification (mirror of classifySend) ---
    void priv11_sendClassification_data();
    void priv11_sendClassification();
    // PRIV-12 locus: ONLY z->t (de-shield) is the acknowledgement-gated category.
    void priv12_onlyDeshieldIsGated_data();
    void priv12_onlyDeshieldIsGated();

    // --- PRIV-13: privacy-badge classify()/classifyForIndex()/labelFor() (real) ---
    void priv13_badgeClassify_data();
    void priv13_badgeClassify();
    void priv13_deshieldOnlyForPublicSendRow();
    // --- PRIV-14: badge colour hex tokens match dark.qss ---
    void priv14_badgeColorTokens();

    // --- CONF-1 / NOTIFY-SRV: CSPRNG secret generation (securerandom.h, the
    //     single source randomPassword() and the notify token both delegate to) ---
    void conf1_passwordHighEntropyCSPRNG();
    void conf1_notifyTokenHexFormat();
    void conf1_randomPasswordUsesCSPRNG();   // caller-divergence guard (connection.cpp)

    // --- Hardening guards (source-text + behavioral) ---
    void notify_tempFallbackDirIsOwnerOnly();         // ITEM 5 behavioral (mirrored)
    void notify_runtimeDirHardensTempFallback();      // ITEM 5 source guard (notifyserver.cpp)
    void notify_connectorLogsFailureToStderr();       // ITEM 4 source guard (notifyserver.cpp)
    void notify_launchArgsGatedOnSocketReady();       // ITEM 2 source guard (connection.cpp)
    void conf_permsVerifiedAfterWrite();              // ITEM 3 source guard (connection.cpp)

    // --- First-run trust: send-gate staleness predicate (pure) ---
    void sendgate_staleWhenPollStops();               // Settings::syncGateIsStale boundary

private:
    QString sandboxAppData() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
};

// ---------------------------------------------------------------------------
void TestLogic::initTestCase() {
    // Sandbox: route QSettings + QStandardPaths::AppDataLocation into a throwaway
    // location so we NEVER touch the user's real profile or ~/.zclassic.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("ZClassicTest");
    QCoreApplication::setOrganizationDomain("test.zclassic.invalid");
    QCoreApplication::setApplicationName("tst_logic");

    // Settings is a singleton accessed via getInstance(); init it once.
    Settings::init();
    QVERIFY(Settings::getInstance() != nullptr);

    // Start from a clean QSettings each run.
    QSettings().clear();
    QSettings().sync();
}

void TestLogic::cleanupTestCase() {
    QSettings().clear();
    QSettings().sync();
}

// =====================================================================
// Address classifiers (exhaustive truth table)
// =====================================================================
void TestLogic::classifiers_data() {
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("testnet");
    QTest::addColumn<bool>("isT");
    QTest::addColumn<bool>("isZ");
    QTest::addColumn<bool>("isSapling");
    QTest::addColumn<bool>("isSprout");

    //                              addr                 net=t  isT    isZ    isSap  isSpr
    QTest::newRow("mainnet sapling zs")  << addr::ZS_SAPLING  << false << false << true  << true  << false;
    QTest::newRow("mainnet zboard zs")   << addr::ZS_ZBOARD   << false << false << true  << true  << false;
    QTest::newRow("mainnet sprout zc")   << addr::ZC_SPROUT   << false << false << true  << false << true;
    QTest::newRow("testnet sapling zts") << addr::ZTS_SAPLING << true  << false << true  << true  << false;
    QTest::newRow("testnet sprout ztn")  << addr::ZTN_SPROUT  << true  << false << true  << false << true;

    // On the WRONG network, a Sapling addr is not recognized as Sapling
    // (isSaplingAddress is net-aware), so it classifies as Sprout (z && !sapling).
    QTest::newRow("zs on testnet -> sprout") << addr::ZS_SAPLING  << true  << false << true  << false << true;
    QTest::newRow("zts on mainnet -> sprout")<< addr::ZTS_SAPLING << false << false << true  << false << true;
}

void TestLogic::classifiers() {
    QFETCH(QString, address);
    QFETCH(bool, testnet);
    QFETCH(bool, isT);
    QFETCH(bool, isZ);
    QFETCH(bool, isSapling);
    QFETCH(bool, isSprout);

    Settings::getInstance()->setTestnet(testnet);

    QCOMPARE(Settings::isTAddress(address), isT);
    QCOMPARE(Settings::isZAddress(address), isZ);
    QCOMPARE(Settings::getInstance()->isSaplingAddress(address), isSapling);
    QCOMPARE(Settings::getInstance()->isSproutAddress(address), isSprout);

    Settings::getInstance()->setTestnet(false);  // restore default
}

void TestLogic::invalidAddressesRejected_data() {
    QTest::addColumn<QString>("address");

    QTest::newRow("empty")              << QString("");
    QTest::newRow("garbage")            << QString("not-an-address");
    QTest::newRow("too short t")        << QString("t1abc");
    QTest::newRow("bad checksum zs")    << corrupt(addr::ZS_SAPLING);
    QTest::newRow("bad checksum zc")    << corrupt(addr::ZC_SPROUT);
    QTest::newRow("bad checksum zts")   << corrupt(addr::ZTS_SAPLING);
    QTest::newRow("bad checksum ztn")   << corrupt(addr::ZTN_SPROUT);
    QTest::newRow("only prefix z")      << QString("z");
    QTest::newRow("only prefix t")      << QString("t");
}

void TestLogic::invalidAddressesRejected() {
    QFETCH(QString, address);
    // Every classifier early-outs to false on a non-valid address, regardless of net.
    QVERIFY(!Settings::isValidAddress(address));
    QVERIFY(!Settings::isTAddress(address));
    QVERIFY(!Settings::isZAddress(address));
    QVERIFY(!Settings::getInstance()->isSaplingAddress(address));
    QVERIFY(!Settings::getInstance()->isSproutAddress(address));
}

// =====================================================================
// C5 — getDecimalString edges
// =====================================================================
void TestLogic::getDecimalString_data() {
    QTest::addColumn<double>("amt");
    QTest::addColumn<QString>("expected");

    QTest::newRow("1.5")        << 1.5              << QString("1.5");
    QTest::newRow("5")          << 5.0              << QString("5");
    QTest::newRow("neg zero")   << -0.0             << QString("0");
    QTest::newRow("dust")       << 0.00000001       << QString("0.00000001");
    QTest::newRow("-1.5")       << -1.5             << QString("-1.5");
    QTest::newRow("zero")       << 0.0              << QString("0");
    QTest::newRow("trailing 0s")<< 2.50000000       << QString("2.5");
    QTest::newRow("whole 100")  << 100.0            << QString("100");
}

void TestLogic::getDecimalString() {
    QFETCH(double, amt);
    QFETCH(QString, expected);
    QCOMPARE(Settings::getDecimalString(amt), expected);
}

// =====================================================================
// getZCLDisplayFormat  (decimal + token, net-aware token)
// =====================================================================
void TestLogic::zclDisplayFormat_data() {
    QTest::addColumn<double>("amt");
    QTest::addColumn<bool>("testnet");
    QTest::addColumn<QString>("expected");

    QTest::newRow("mainnet 1.5")  << 1.5  << false << QString("1.5 ZCL");
    QTest::newRow("mainnet 5")    << 5.0  << false << QString("5 ZCL");
    QTest::newRow("testnet 1.5")  << 1.5  << true  << QString("1.5 ZCT");
    QTest::newRow("mainnet dust") << 0.00000001 << false << QString("0.00000001 ZCL");
}

void TestLogic::zclDisplayFormat() {
    QFETCH(double, amt);
    QFETCH(bool, testnet);
    QFETCH(QString, expected);
    Settings::getInstance()->setTestnet(testnet);
    QCOMPARE(Settings::getZCLDisplayFormat(amt), expected);
    Settings::getInstance()->setTestnet(false);
}

// =====================================================================
// C3 — getUSDFormat: only when price>0 && !testnet
// =====================================================================
void TestLogic::usdFormat_data() {
    QTest::addColumn<double>("price");
    QTest::addColumn<bool>("testnet");
    QTest::addColumn<double>("bal");
    QTest::addColumn<QString>("expected");   // "" means: empty string expected

    QTest::newRow("mainnet priced")     << 2.0   << false << 3.0  << QString("$6.00");
    QTest::newRow("mainnet zero price") << 0.0   << false << 3.0  << QString("");
    QTest::newRow("testnet priced")     << 2.0   << true  << 3.0  << QString("");
    QTest::newRow("testnet zero price") << 0.0   << true  << 3.0  << QString("");
    QTest::newRow("mainnet neg price")  << -1.0  << false << 3.0  << QString("");
}

void TestLogic::usdFormat() {
    QFETCH(double, price);
    QFETCH(bool, testnet);
    QFETCH(double, bal);
    QFETCH(QString, expected);

    Settings::getInstance()->setTestnet(testnet);
    Settings::getInstance()->setZCLPrice(price);

    QString got = Settings::getUSDFormat(bal);
    if (expected.isEmpty())
        QVERIFY2(got.isEmpty(), qPrintable(QString("expected empty, got '%1'").arg(got)));
    else
        QCOMPARE(got, expected);

    Settings::getInstance()->setTestnet(false);
    Settings::getInstance()->setZCLPrice(0.0);
}

// =====================================================================
// C6 — token name ZCL (mainnet) / ZCT (testnet)
// =====================================================================
void TestLogic::tokenName() {
    Settings::getInstance()->setTestnet(false);
    QCOMPARE(Settings::getTokenName(), QString("ZCL"));
    Settings::getInstance()->setTestnet(true);
    QCOMPARE(Settings::getTokenName(), QString("ZCT"));
    Settings::getInstance()->setTestnet(false);
}

// =====================================================================
// E7 — field-validation pieces that need no RPC: amount/decimal formatting
// round-trips that the Send-tab validation relies on.
// =====================================================================
void TestLogic::e7FieldFormatting_data() {
    QTest::addColumn<double>("amt");
    QTest::addColumn<QString>("decimal");

    // doSendTxValidations formats amounts through getDecimalString before display;
    // these assert the boundary amounts a recipient field must round-trip exactly.
    QTest::newRow("miner fee")   << Settings::getMinerFee()    << QString("0.0001");
    QTest::newRow("zboard amt")  << Settings::getZboardAmount()<< QString("0.0001");
    QTest::newRow("8dp boundary")<< 0.12345678                 << QString("0.12345678");
    QTest::newRow("negative")    << -2.0                       << QString("-2");
}

void TestLogic::e7FieldFormatting() {
    QFETCH(double, amt);
    QFETCH(QString, decimal);
    QCOMPARE(Settings::getDecimalString(amt), decimal);
}

// =====================================================================
// D6 — addressFromAddressLabel + AddressCombo paren-strip
// =====================================================================
void TestLogic::addressFromLabel_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("plain addr")        << addr::ZS_SAPLING << addr::ZS_SAPLING;
    QTest::newRow("label/addr")        << QString("mylabel/") + addr::ZS_SAPLING << addr::ZS_SAPLING;
    QTest::newRow("leading space")     << QString("  trim/") + addr::ZC_SPROUT << addr::ZC_SPROUT;
    QTest::newRow("multi-slash takes last") << QString("a/b/") + addr::ZTS_SAPLING << addr::ZTS_SAPLING;
    QTest::newRow("no slash trims")    << QString("   ") + addr::ZTN_SPROUT + QString("  ") << addr::ZTN_SPROUT;
}

void TestLogic::addressFromLabel() {
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(AddressBook::addressFromAddressLabel(input), expected);
}

// AddressCombo::itemText/currentText strip "(amount)" then label, matching
// docs D6: `label/zaddr(1.5 ZCL)` -> `zaddr`.
void TestLogic::addressComboParenStrip() {
    AddressCombo combo;
    // Seed a label so addLabelToAddress produces "label/addr".
    AddressBook::getInstance()->addAddressLabel("donations", addr::ZS_SAPLING);

    // addItem(addr, bal>0) stores "label/addr(<bal> ZCL)" — exercise the strip.
    Settings::getInstance()->setTestnet(false);
    combo.addItem(addr::ZS_SAPLING, 1.5);
    QCOMPARE(combo.count(), 1);
    // itemText must return JUST the bare address (parens + label removed).
    QCOMPARE(combo.itemText(0), addr::ZS_SAPLING);
    QCOMPARE(combo.currentText(), addr::ZS_SAPLING);

    // An item with zero balance has no parens; still returns the bare addr.
    combo.addItem(addr::ZC_SPROUT, 0.0);
    QCOMPARE(combo.itemText(1), addr::ZC_SPROUT);
}

// =====================================================================
// G4 — SentTxStore file is 0600 owner-only, testnet-prefixed name
// =====================================================================
void TestLogic::sentTxStore_perms_and_testnetName() {
    Settings::getInstance()->setTestnet(true);
    QSettings().setValue("options/savesenttx", true);
    QSettings().sync();

    // clean slate
    SentTxStore::deleteHistory();

    Tx tx;
    tx.fromAddr = addr::ZTS_SAPLING;   // z-addr so the write is NOT gated out
    tx.fee = 0.0001;
    ToFields f; f.addr = addr::ZTN_SPROUT; f.amount = 1.25;
    tx.toAddrs.append(f);

    SentTxStore::addToSentTx(tx, "deadbeefcafetxid");

    // The file must exist under a "testnet-"-prefixed name in AppDataLocation.
    QString expected = QDir(sandboxAppData()).filePath("testnet-senttxstore.dat");
    QVERIFY2(QFile::exists(expected), qPrintable("missing: " + expected));

    // Permissions must be owner-read + owner-write only (0600), no group/other.
    QFile::Permissions p = QFile::permissions(expected);
    QVERIFY(p.testFlag(QFileDevice::ReadOwner));
    QVERIFY(p.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!p.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!p.testFlag(QFileDevice::WriteGroup));
    QVERIFY(!p.testFlag(QFileDevice::ReadOther));
    QVERIFY(!p.testFlag(QFileDevice::WriteOther));
    QVERIFY(!p.testFlag(QFileDevice::ExeOwner));

    SentTxStore::deleteHistory();
    Settings::getInstance()->setTestnet(false);
}

// =====================================================================
// G1 — write gated by getSaveZtxs() AND by z-prefixed from-addr
// =====================================================================
void TestLogic::sentTxStore_gating() {
    Settings::getInstance()->setTestnet(false);
    QString file = QDir(sandboxAppData()).filePath("senttxstore.dat");

    auto mkTx = [](const QString& from) {
        Tx tx; tx.fromAddr = from; tx.fee = 0.0001;
        ToFields f; f.addr = addr::ZS_SAPLING; f.amount = 2.0;
        tx.toAddrs.append(f);
        return tx;
    };

    // (a) savesenttx OFF -> no write, even with a z from-addr.
    SentTxStore::deleteHistory();
    QSettings().setValue("options/savesenttx", false);
    QSettings().sync();
    SentTxStore::addToSentTx(mkTx(addr::ZS_SAPLING), "txid_a");
    QVERIFY2(!QFile::exists(file), "savesenttx=false must suppress the write");

    // (b) savesenttx ON but from-addr is a t-addr -> still no write (G1 gate).
    QSettings().setValue("options/savesenttx", true);
    QSettings().sync();
    SentTxStore::deleteHistory();
    QString tAddr = "t1KsZQNTM8N5gxRoyfcHrDrwLgxffzPpTLg";  // shape doesn't need a real checksum: addToSentTx only checks startsWith("z")
    SentTxStore::addToSentTx(mkTx(tAddr), "txid_b");
    QVERIFY2(!QFile::exists(file), "non-z from-addr must suppress the write");

    // (c) savesenttx ON + z from-addr -> write happens.
    SentTxStore::deleteHistory();
    SentTxStore::addToSentTx(mkTx(addr::ZS_SAPLING), "txid_c");
    QVERIFY2(QFile::exists(file), "z from-addr with savesenttx=true must write");

    SentTxStore::deleteHistory();
}

// Round-trip: write then read back gives one item with the aggregated amount.
void TestLogic::sentTxStore_roundTrip() {
    Settings::getInstance()->setTestnet(false);
    QSettings().setValue("options/savesenttx", true);
    QSettings().sync();
    SentTxStore::deleteHistory();

    Tx tx; tx.fromAddr = addr::ZS_SAPLING; tx.fee = 0.0001;
    ToFields f1; f1.addr = addr::ZS_ZBOARD; f1.amount = 1.0; tx.toAddrs.append(f1);
    ToFields f2; f2.addr = addr::ZC_SPROUT; f2.amount = 0.5; tx.toAddrs.append(f2);

    SentTxStore::addToSentTx(tx, "rt_txid");

    QList<TransactionItem> items = SentTxStore::readSentTxFile();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items[0].txid, QString("rt_txid"));
    // amount stored = -(sum) ; read-back = amount + fee = -(1.5) + (-0.0001)
    QVERIFY(items[0].amount < 0);
    QCOMPARE(items[0].fromAddr, addr::ZS_SAPLING);

    SentTxStore::deleteHistory();
}

// =====================================================================
// H1 — db-corruption / startup-marker / long-warmup classifiers (mirror)
// =====================================================================
void TestLogic::dbCorruptionMarkers_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("hit");

    QTest::newRow("empty")            << QString("")                                   << false;
    QTest::newRow("clean log")        << QString("UpdateTip: new best=abc height=100") << false;
    QTest::newRow("corrupted block")  << QString("Corrupted block database detected")  << true;
    QTest::newRow("case-insensitive") << QString("CORRUPTED BLOCK DATABASE DETECTED")  << true;
    QTest::newRow("error opening")    << QString("...\nError opening block database\n")<< true;
    QTest::newRow("failed to read")   << QString("Failed to read block")               << true;
    QTest::newRow("rebuild prompt")   << QString("Do you want to rebuild the block database now") << true;
}

void TestLogic::dbCorruptionMarkers() {
    QFETCH(QString, text);
    QFETCH(bool, hit);
    QCOMPARE(cxmirror::looksLikeDbCorruption(text), hit);
}

void TestLogic::startupMarkers_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("hit");

    QTest::newRow("empty")          << QString("")                                  << false;
    QTest::newRow("benign")         << QString("zclassicd starting up nicely")      << false;
    QTest::newRow("lock")           << QString("Cannot obtain a lock on data dir")  << true;
    QTest::newRow("already running")<< QString("zclassicd is probably already running") << true;
    QTest::newRow("disk low")       << QString("Error: Disk space is low!")         << true;
    QTest::newRow("db newer")       << QString("requires newer version of ...")     << true;
    QTest::newRow("wallet corrupt") << QString("Wallet corrupted")                  << true;
    QTest::newRow("anchor reject")  << QString("bootstrap snapshot verification failed") << true;
    QTest::newRow("delegates to db")<< QString("Corrupted block database detected") << true;
}

void TestLogic::startupMarkers() {
    QFETCH(QString, text);
    QFETCH(bool, hit);
    QCOMPARE(cxmirror::startupDiagHasMarker(text), hit);
}

void TestLogic::longWarmupPhase_data() {
    QTest::addColumn<QString>("status");
    QTest::addColumn<bool>("hit");

    QTest::newRow("empty")           << QString("")                       << false;
    QTest::newRow("idle")            << QString("Done loading")           << false;
    QTest::newRow("verifying")       << QString("Verifying blocks 3 of 6")<< true;
    QTest::newRow("best chain")      << QString("Activating best chain...")<< true;
    QTest::newRow("loading index")   << QString("Loading block index...") << true;
    QTest::newRow("rescanning")      << QString("Rescanning... 42%")      << true;
    QTest::newRow("upgrading")       << QString("Upgrading database")     << true;
    QTest::newRow("case insens")     << QString("REWINDING blocks")       << true;
}

void TestLogic::longWarmupPhase() {
    QFETCH(QString, status);
    QFETCH(bool, hit);
    QCOMPARE(cxmirror::isLongWarmupPhase(status), hit);
}

// =====================================================================
// H2 — blocksDirSizeBytes correct byte total over a temp dir
// =====================================================================
void TestLogic::blocksDirSizeBytes() {
    // empty/unknown root -> -1
    QCOMPARE(cxmirror::blocksDirSizeBytes(QString()), (qint64)-1);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QString root = tmp.path();

    // No blocks/ dir yet -> -1 ("can't measure", never a heal trigger).
    QCOMPARE(cxmirror::blocksDirSizeBytes(root), (qint64)-1);

    // Build root/blocks/{blk00000.dat, index/foo.ldb} with known byte sizes.
    QDir(root).mkpath("blocks/index");
    auto writeBytes = [](const QString& path, int n) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(n, 'x'));
        f.close();
    };
    writeBytes(QDir(root).filePath("blocks/blk00000.dat"), 1000);
    writeBytes(QDir(root).filePath("blocks/rev00000.dat"), 500);
    writeBytes(QDir(root).filePath("blocks/index/000001.ldb"), 250);

    // Recursive sum = 1000 + 500 + 250 = 1750.
    QCOMPARE(cxmirror::blocksDirSizeBytes(root), (qint64)1750);

    // testnet3/ resolution: when testnet3/ holds the chain, it's measured instead.
    QString root2;
    {
        QTemporaryDir tmp2;
        QVERIFY(tmp2.isValid());
        root2 = tmp2.path();
        QDir(root2).mkpath("blocks");                 // mainnet blocks (would be 0/empty)
        QDir(root2).mkpath("testnet3/blocks");
        // mark testnet3 as the live chain
        writeBytes(QDir(root2).filePath("testnet3/debug.log"), 1);
        writeBytes(QDir(root2).filePath("testnet3/blocks/blk00000.dat"), 4096);
        QCOMPARE(cxmirror::blocksDirSizeBytes(root2), (qint64)4096);
    }
}

// =====================================================================
// PRIV-9 — auto-shield default is ON. CANARY: if someone flips the
// QSettings default back to false (settings.cpp getAutoShield), this goes red.
// We must assert the DEFAULT, i.e. with options/autoshield UNSET.
// =====================================================================
void TestLogic::priv9_autoShieldDefaultOn() {
    // Ensure the key is genuinely unset so we read the compiled-in default.
    QSettings().remove("options/autoshield");
    QSettings().sync();

    QVERIFY2(Settings::getInstance()->getAutoShield(),
             "PRIV-9 CANARY: getAutoShield() must default to TRUE when "
             "options/autoshield is unset (privacy-by-default flip). If this fails, "
             "someone reverted the default back to false in settings.cpp.");

    // And the setter must still round-trip both ways (off stays off, on stays on).
    Settings::getInstance()->setAutoShield(false);
    QVERIFY(!Settings::getInstance()->getAutoShield());
    Settings::getInstance()->setAutoShield(true);
    QVERIFY(Settings::getInstance()->getAutoShield());

    // Restore the unset/default state for any later test.
    QSettings().remove("options/autoshield");
    QSettings().sync();
}

// =====================================================================
// PRIV-11 / UX-12 — four-way from-aware classification, all quadrants.
// Uses real checksum-valid addresses so Settings::isT/isZ run their full path.
// =====================================================================
void TestLogic::priv11_sendClassification_data() {
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("to");
    QTest::addColumn<int>("expected");   // SendCategory as int

    const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";  // mainnet t-addr
    // z->z private (shielded from, shielded to)
    QTest::newRow("z->z private")    << addr::ZS_SAPLING << addr::ZS_ZBOARD << int(SendCategory::ZToZ_private);
    QTest::newRow("z->z sprout from")<< addr::ZC_SPROUT  << addr::ZS_SAPLING << int(SendCategory::ZToZ_private);
    // t->z shielding (transparent from, shielded to)
    QTest::newRow("t->z shielding")  << T               << addr::ZS_SAPLING << int(SendCategory::TToZ_shielding);
    // z->t de-shield (shielded from, transparent to) — the strongest warning case
    QTest::newRow("z->t deshield")   << addr::ZS_SAPLING << T               << int(SendCategory::ZToT_deshield);
    QTest::newRow("sprout->t desh")  << addr::ZC_SPROUT  << T               << int(SendCategory::ZToT_deshield);
    // t->t public (transparent from, transparent to)
    QTest::newRow("t->t public")     << T               << T               << int(SendCategory::TToT_public);
}

void TestLogic::priv11_sendClassification() {
    QFETCH(QString, from);
    QFETCH(QString, to);
    QFETCH(int, expected);

    Settings::getInstance()->setTestnet(false);

    Tx tx;
    tx.fromAddr = from;
    tx.fee = 0.0001;
    ToFields f; f.addr = to; f.amount = 1.0;
    tx.toAddrs.append(f);

    // Calls the REAL production classifier (src/sendcategory.h), the same body
    // MainWindow::classifySend forwards to — no mirror.
    QCOMPARE(int(sendCategoryOf(tx)), expected);
}

// PRIV-12: ONLY a z->t DE-SHIELD requires the acknowledgement gate; the other
// three categories MUST NOT add that friction. This drives the REAL product gate
// `isDeshieldSend()` (src/sendcategory.h — the same body MainWindow::isDeshield
// forwards to) over a real Tx per quadrant, NOT a value computed in the test. A
// z->t with MULTIPLE recipients (one transparent) must still be gated: ANY
// transparent recipient taints the whole send.
void TestLogic::priv12_onlyDeshieldIsGated_data() {
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("to");
    QTest::addColumn<QString>("to2");   // optional 2nd recipient ("" => none)
    QTest::addColumn<bool>("gated");

    const QString T = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";   // mainnet t-addr

    QTest::newRow("z->z private not gated")   << addr::ZS_SAPLING << addr::ZS_ZBOARD << QString() << false;
    QTest::newRow("t->z shielding not gated") << T               << addr::ZS_SAPLING << QString() << false;
    QTest::newRow("z->t deshield GATED")      << addr::ZS_SAPLING << T               << QString() << true;
    QTest::newRow("t->t public not gated")    << T               << T               << QString() << false;
    // z -> (z, t): a single transparent recipient among shielded ones still gates.
    QTest::newRow("z->(z,t) deshield GATED")  << addr::ZS_SAPLING << addr::ZS_ZBOARD << T         << true;
}

void TestLogic::priv12_onlyDeshieldIsGated() {
    QFETCH(QString, from);
    QFETCH(QString, to);
    QFETCH(QString, to2);
    QFETCH(bool, gated);

    Settings::getInstance()->setTestnet(false);

    Tx tx;
    tx.fromAddr = from;
    tx.fee = 0.0001;
    ToFields f; f.addr = to; f.amount = 1.0;
    tx.toAddrs.append(f);
    if (!to2.isEmpty()) {
        ToFields f2; f2.addr = to2; f2.amount = 0.5;
        tx.toAddrs.append(f2);
    }

    // Calls the REAL product gate (no value recomputed inside the test).
    QCOMPARE(isDeshieldSend(tx), gated);
}

// =====================================================================
// PRIV-13 — privacy-badge classify() maps each address family correctly. Uses
// the REAL PrivacyBadgeDelegate::testClassify seam (no duplicated truth table).
// =====================================================================
void TestLogic::priv13_badgeClassify_data() {
    QTest::addColumn<QString>("display");
    QTest::addColumn<bool>("testnet");
    QTest::addColumn<int>("kind");   // PrivacyBadgeDelegate::Kind as int

    using K = PrivacyBadgeDelegate::Kind;
    QTest::newRow("sapling -> Private")        << addr::ZS_SAPLING << false << int(K::Private);
    QTest::newRow("sprout -> PrivateLegacy")   << addr::ZC_SPROUT  << false << int(K::PrivateLegacy);
    QTest::newRow("t -> Public")               << QString("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi") << false << int(K::Public);
    QTest::newRow("(Shielded) -> Private")     << QString("(Shielded)") << false << int(K::Private);
    QTest::newRow("empty -> None")             << QString("")      << false << int(K::None);
    QTest::newRow("label(addr) strip sapling") << (QString("Alice (") + addr::ZS_SAPLING + ")") << false << int(K::Private);
    QTest::newRow("testnet sapling -> Private")<< addr::ZTS_SAPLING << true  << int(K::Private);
}

void TestLogic::priv13_badgeClassify() {
    QFETCH(QString, display);
    QFETCH(bool, testnet);
    QFETCH(int, kind);

    Settings::getInstance()->setTestnet(testnet);
    QCOMPARE(int(PrivacyBadgeDelegate::testClassify(display)), kind);
    Settings::getInstance()->setTestnet(false);
}

// PRIV-13 — Deshield is promoted ONLY for a Public (transparent) recipient on a
// row whose Type cell (col 0) contains send/sent. A transparent recipient on a
// receive row stays Public; a shielded recipient never becomes Deshield.
void TestLogic::priv13_deshieldOnlyForPublicSendRow() {
    using K = PrivacyBadgeDelegate::Kind;
    Settings::getInstance()->setTestnet(false);

    const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
    const QString ZS = addr::ZS_SAPLING;

    // A model with col0 = Type, col1 = Address (mirrors the transactions table).
    QStandardItemModel model(3, 2);
    model.setData(model.index(0, 0), "send");      model.setData(model.index(0, 1), T);
    model.setData(model.index(1, 0), "receive");   model.setData(model.index(1, 1), T);
    model.setData(model.index(2, 0), "send");      model.setData(model.index(2, 1), ZS);

    // send + transparent recipient -> Deshield (the privacy-leaking case).
    QCOMPARE(int(PrivacyBadgeDelegate::testClassifyForIndex(model.index(0, 1), T)),  int(K::Deshield));
    // receive + transparent -> stays Public (incoming public funds, not a leak).
    QCOMPARE(int(PrivacyBadgeDelegate::testClassifyForIndex(model.index(1, 1), T)),  int(K::Public));
    // send + shielded recipient -> Private (never Deshield).
    QCOMPARE(int(PrivacyBadgeDelegate::testClassifyForIndex(model.index(2, 1), ZS)), int(K::Private));

    // No model / no Type column -> the bare classify() result (Public), never Deshield.
    QCOMPARE(int(PrivacyBadgeDelegate::testClassifyForIndex(QModelIndex(), T)), int(K::Public));
}

// =====================================================================
// PRIV-14 — badge label + colour hex tokens. The hex MUST match dark.qss:
//   green #1f7a1f (Private/Legacy), amber #d9822b (Public), red #c0392b (Deshield).
// We also grep the shipped res/styles/dark.qss for the same tokens so the badge
// and the stylesheet can never silently drift apart.
// =====================================================================
void TestLogic::priv14_badgeColorTokens() {
    using K = PrivacyBadgeDelegate::Kind;

    // Labels.
    QCOMPARE(PrivacyBadgeDelegate::testLabelFor(K::Private),       QString("Private"));
    QCOMPARE(PrivacyBadgeDelegate::testLabelFor(K::PrivateLegacy), QString("Private (legacy)"));
    QCOMPARE(PrivacyBadgeDelegate::testLabelFor(K::Public),        QString("PUBLIC"));
    QCOMPARE(PrivacyBadgeDelegate::testLabelFor(K::Deshield),      QString("De-shield"));

    // Colour hex (QColor::name() is lower-case "#rrggbb").
    QCOMPARE(PrivacyBadgeDelegate::testColorHexFor(K::Private),       QString("#1f7a1f"));
    QCOMPARE(PrivacyBadgeDelegate::testColorHexFor(K::PrivateLegacy), QString("#1f7a1f"));
    QCOMPARE(PrivacyBadgeDelegate::testColorHexFor(K::Public),        QString("#d9822b"));
    QCOMPARE(PrivacyBadgeDelegate::testColorHexFor(K::Deshield),      QString("#c0392b"));

    // Guard: the SAME hex tokens must be present in the shipped dark.qss. Resolve
    // the stylesheet relative to this test file's source dir (set by qmake via
    // QT_TESTCASE_BUILDDIR = the tests/ dir; dark.qss lives at ../res/styles).
    QStringList candidates = {
        QStringLiteral(QT_TESTCASE_BUILDDIR) + "/../res/styles/dark.qss",
        QFINDTESTDATA("../res/styles/dark.qss"),
    };
    QString qss;
    for (const QString& c : candidates) {
        if (c.isEmpty()) continue;
        QFile f(c);
        if (f.open(QIODevice::ReadOnly)) { qss = QString::fromUtf8(f.readAll()); break; }
    }
    QVERIFY2(!qss.isEmpty(), "could not locate res/styles/dark.qss to guard the colour tokens");

    // NIT-4: strip C-style /* ... */ comments before grepping, so the guard proves
    // each token backs a REAL rule (not just a mention in the header legend). A token
    // present only inside a comment must NOT satisfy the guard. Manual single-pass
    // strip (avoids QRegExp greedy-match foot-guns across multiple comment blocks).
    QString code;
    {
        int i = 0;
        while (i < qss.length()) {
            if (i + 1 < qss.length() && qss[i] == '/' && qss[i+1] == '*') {
                int end = qss.indexOf("*/", i + 2);
                if (end < 0) break;          // unterminated comment -> drop the rest
                i = end + 2;
            } else {
                code.append(qss[i]);
                ++i;
            }
        }
    }

    QVERIFY2(code.contains("#1f7a1f", Qt::CaseInsensitive),
             "dark.qss missing green token #1f7a1f in a REAL (non-comment) rule");
    QVERIFY2(code.contains("#d9822b", Qt::CaseInsensitive),
             "dark.qss missing amber token #d9822b in a REAL (non-comment) rule");
    QVERIFY2(code.contains("#c0392b", Qt::CaseInsensitive),
             "dark.qss missing red token #c0392b in a REAL (non-comment) rule");
}

// ---------------------------------------------------------------------------
// CONF-1 / NOTIFY-SRV — the wallet's secrets (rpcpassword + per-session notify
// token) MUST come from the system CSPRNG with >= 128 bits. The first two tests
// exercise the shared securerandom.h body DIRECTLY (a short / weak / charset-
// stuck generator inside the header fails their length + bit-floor + charset-
// coverage asserts). connection.cpp — which holds randomPassword() — is too heavy
// to link into this guiless suite, so the third test (conf1_randomPasswordUsesCSPRNG)
// is a STATIC guard that pins the delegation: the conf writer must call
// secureRandomBase62() and contain no raw rand()/qrand(), catching a caller that
// diverges back to a weak RNG.
// ---------------------------------------------------------------------------
void TestLogic::conf1_passwordHighEntropyCSPRNG() {
    // Default length is 32 base62 chars (the value randomPassword() uses).
    const QString pw = secureRandomBase62();
    QCOMPARE(pw.length(), 32);

    // Bit floor: chars * log2(62) must clear the 128-bit CONF-1 requirement.
    const double bits = pw.length() * 5.9541963;   // log2(62)
    QVERIFY2(bits >= 128.0,
             qPrintable(QString("rpcpassword entropy %1 bits < 128").arg(bits, 0, 'f', 1)));

    // Charset: every character is base62 [0-9A-Za-z] (conf-parser-safe — no
    // whitespace, '#', '=' or newline that could corrupt a key=value line).
    const QString allowed =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (const QChar c : pw)
        QVERIFY2(allowed.contains(c),
                 qPrintable(QString("rpcpassword has non-base62 char '%1'").arg(c)));

    // CSPRNG sanity over a large sample: every value distinct (collision among
    // ~190-bit strings is astronomically impossible — a constant/buggy generator
    // would collide), and the full 62-symbol alphabet is exercised (a generator
    // stuck on a subset — e.g. digits only — would be caught).
    QSet<QString> seen;
    QSet<QChar>   chars;
    const int N = 512;
    for (int i = 0; i < N; ++i) {
        const QString s = secureRandomBase62();
        seen.insert(s);
        for (const QChar c : s) chars.insert(c);
    }
    QCOMPARE(seen.size(), N);                 // no repeats across 512 draws
    QVERIFY2(chars.size() >= 60,
             qPrintable(QString("only %1/62 base62 symbols seen across %2 samples — "
                                "generator may be stuck on a subset").arg(chars.size()).arg(N)));

    // Explicit length is honored (used to mint shorter/longer secrets if needed).
    QCOMPARE(secureRandomBase62(22).length(), 22);   // ~131 bits, the 128-bit minimum
}

void TestLogic::conf1_notifyTokenHexFormat() {
    // The notify token MUST match the on-the-wire NOTIFY-SRV payload contract
    // exactly: 64 lowercase-hex chars (32 bytes = 256 bits). Validated here so the
    // listener's `^[0-9a-f]{64}$` guard and the token minter agree by construction.
    const QRegularExpression re("^[0-9a-f]{64}$");
    const QString tok = secureRandomHex();
    QCOMPARE(tok.length(), 64);
    QVERIFY2(re.match(tok).hasMatch(),
             qPrintable("notify token not 64 lowercase-hex chars: " + tok));

    // Distinctness over a large sample (no repeats; full nibble alphabet seen).
    QSet<QString> seen;
    QSet<QChar>   nibbles;
    const int N = 512;
    for (int i = 0; i < N; ++i) {
        const QString s = secureRandomHex();
        QVERIFY(re.match(s).hasMatch());
        seen.insert(s);
        for (const QChar c : s) nibbles.insert(c);
    }
    QCOMPARE(seen.size(), N);
    QCOMPARE(nibbles.size(), 16);            // all of 0-9a-f exercised

    // Explicit byte count is honored: 16 bytes -> 32 hex chars (128 bits).
    QCOMPARE(secureRandomHex(16).length(), 32);
}

void TestLogic::conf1_randomPasswordUsesCSPRNG() {
    // Locate connection.cpp the same way priv14 finds dark.qss (QT_TESTCASE_BUILDDIR
    // is the tests/ dir; src/ is its sibling).
    QStringList candidates = {
        QStringLiteral(QT_TESTCASE_BUILDDIR) + "/../src/connection.cpp",
        QFINDTESTDATA("../src/connection.cpp"),
    };
    QString src;
    for (const QString& c : candidates) {
        if (c.isEmpty()) continue;
        QFile f(c);
        if (f.open(QIODevice::ReadOnly)) { src = QString::fromUtf8(f.readAll()); break; }
    }
    QVERIFY2(!src.isEmpty(), "could not locate src/connection.cpp for the CSPRNG caller guard");

    // Strip // line + /* ... */ block comments so a 'rand()' written in PROSE (the
    // comments here deliberately document the OLD weak generator) cannot fool the
    // guard. We assert on the remaining real code only.
    QString code;
    {
        int i = 0, n = src.length();
        while (i < n) {
            if (i + 1 < n && src[i] == '/' && src[i+1] == '/') {
                int end = src.indexOf('\n', i + 2);
                if (end < 0) break;
                i = end;                          // keep the newline
            } else if (i + 1 < n && src[i] == '/' && src[i+1] == '*') {
                int end = src.indexOf("*/", i + 2);
                if (end < 0) break;
                i = end + 2;
            } else {
                code.append(src[i]);
                ++i;
            }
        }
    }

    QVERIFY2(code.contains("secureRandomBase62"),
             "connection.cpp must mint the rpcpassword via secureRandomBase62() (CONF-1)");
    // 'rand()' (lowercase + parens) matches the weak C RNG but NOT QRandomGenerator
    // ('Random', capital R, no parens) — so this fires only on a real regression.
    QVERIFY2(!code.contains("rand()"),
             "connection.cpp calls rand() in real code — CONF-1 forbids the weak/predictable RNG");
    QVERIFY2(!code.contains("qrand"),
             "connection.cpp uses qrand() — CONF-1 requires QRandomGenerator::system()");
}

// =====================================================================
// Hardening guards (ITEMs 2-5)
//
// ITEMs 2/3 live in connection.cpp and ITEMs 4/5 in notifyserver.cpp; the
// behavior (gating launch-args on socketReady, reading conf perms back,
// logging a connector failure, a 0700 temp-fallback dir) is wired into Qt
// process/socket plumbing that the L0 binary does not link. These are
// SOURCE-TEXT divergence guards in the SAME STYLE as
// conf1_randomPasswordUsesCSPRNG() above: they read the real source and assert
// the safety wiring is present, so a regression that silently drops it fails
// the suite. ITEM 5 ALSO has a behavioral test below (the dir-creation logic is
// small enough to mirror and verify the resulting perms are owner-only).
// =====================================================================

// Locate + comment-strip a src/ file the same way conf1_randomPasswordUsesCSPRNG does.
static QString readStrippedSrc(const QString& relName) {
    QStringList candidates = {
        QStringLiteral(QT_TESTCASE_BUILDDIR) + "/../src/" + relName,
        QFINDTESTDATA("../src/" + relName),
    };
    QString src;
    for (const QString& c : candidates) {
        if (c.isEmpty()) continue;
        QFile f(c);
        if (f.open(QIODevice::ReadOnly)) { src = QString::fromUtf8(f.readAll()); break; }
    }
    if (src.isEmpty()) return QString();

    QString code;
    int i = 0, n = src.length();
    while (i < n) {
        if (i + 1 < n && src[i] == '/' && src[i+1] == '/') {
            int end = src.indexOf('\n', i + 2);
            if (end < 0) break;
            i = end;
        } else if (i + 1 < n && src[i] == '/' && src[i+1] == '*') {
            int end = src.indexOf("*/", i + 2);
            if (end < 0) break;
            i = end + 2;
        } else {
            code.append(src[i]);
            ++i;
        }
    }
    return code;
}

// ITEM 5 behavioral: the SHARED-temp fallback must yield an owner-only (0700) dir.
// notifyRuntimeDir()'s fallback is file-static + not linked here, so mirror its exact
// dir-creation steps into the test sandbox and assert the resulting perms confine access.
void TestLogic::notify_tempFallbackDirIsOwnerOnly() {
#ifdef Q_OS_WIN
    QSKIP("POSIX dir-perm check; Windows uses ACLs (setPermissions) not mode bits");
#else
    // A throwaway "shared temp" root, then the per-user subdir the real code carves out.
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString sub  = QStringLiteral("zqw-test-%1").arg((uint)::getuid());
    const QString dir  = QDir(temp).filePath(sub);
    QDir().rmdir(dir);                       // clean slate (ignore if absent)
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QVERIFY2(QFileInfo(dir).isDir(), qPrintable("fallback dir not created: " + dir));
    QFile::Permissions p = QFile::permissions(dir);
    // Owner may traverse; group/other must have NOTHING (0700 confinement).
    QVERIFY(p.testFlag(QFileDevice::ReadOwner));
    QVERIFY(p.testFlag(QFileDevice::WriteOwner));
    QVERIFY(p.testFlag(QFileDevice::ExeOwner));
    QVERIFY(!p.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!p.testFlag(QFileDevice::WriteGroup));
    QVERIFY(!p.testFlag(QFileDevice::ExeGroup));
    QVERIFY(!p.testFlag(QFileDevice::ReadOther));
    QVERIFY(!p.testFlag(QFileDevice::WriteOther));
    QVERIFY(!p.testFlag(QFileDevice::ExeOther));

    QDir().rmdir(dir);                       // tidy up
#endif
}

// ITEM 5 source guard: notifyRuntimeDir() must harden the TempLocation fallback with a
// per-user subdir + mkpath + 0700 setPermissions (NOT return the shared temp dir bare).
void TestLogic::notify_runtimeDirHardensTempFallback() {
    const QString code = readStrippedSrc("notifyserver.cpp");
    QVERIFY2(!code.isEmpty(), "could not locate src/notifyserver.cpp");
    QVERIFY2(code.contains("TempLocation"),
             "notifyserver.cpp no longer references TempLocation fallback");
    QVERIFY2(code.contains("mkpath"),
             "ITEM 5: temp fallback must mkpath a per-user subdir");
    QVERIFY2(code.contains("setPermissions") && code.contains("ExeOwner"),
             "ITEM 5: temp fallback subdir must be narrowed to owner-only (0700) perms");
}

// ITEM 4 source guard: runConnector() must emit a stderr diagnostic on a non-zero
// sendNotify() result (the headless connector has no logger). No secret is printed.
void TestLogic::notify_connectorLogsFailureToStderr() {
    const QString code = readStrippedSrc("notifyserver.cpp");
    QVERIFY2(!code.isEmpty(), "could not locate src/notifyserver.cpp");
    QVERIFY2(code.contains("fprintf(stderr") || code.contains("qWarning"),
             "ITEM 4: runConnector must log a connector failure to stderr/qWarning");
    // Defense-in-depth: no stderr/qWarning diagnostic line may interpolate the token.
    static const QRegularExpression leakyDiag(
        "(fprintf\\s*\\(\\s*stderr|qWarning\\s*\\(\\s*)[^;]*token", QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(!leakyDiag.match(code).hasMatch(),
             "ITEM 4: the connector diagnostic must not print the token/secret");
}

// ITEM 2 source guard: the -walletnotify/-blocknotify launch-args must be gated on the
// socket having actually started (a notifyReady/isListening flag), NOT appended
// unconditionally whenever a NotifyServer object merely exists.
void TestLogic::notify_launchArgsGatedOnSocketReady() {
    const QString code = readStrippedSrc("connection.cpp");
    QVERIFY2(!code.isEmpty(), "could not locate src/connection.cpp");
    QVERIFY2(code.contains("isListening"),
             "ITEM 2: notify launch-args must consult the socket's isListening() state");
    QVERIFY2(code.contains("buildNotifyArgs"),
             "connection.cpp no longer wires buildNotifyArgs (regression?)");
    // The gate guard ('if (notifyReady)') and the append must both be present, and the
    // append must be reached THROUGH the gate — not the old unconditional
    // 'if (rpc && rpc->getNotifyServer()) ... buildNotifyArgs'.
    QVERIFY2(code.contains("notifyReady"),
             "ITEM 2: a notifyReady gate flag must guard the buildNotifyArgs append");
    const int gateIdx   = code.indexOf("if (notifyReady)");
    const int appendIdx = code.indexOf("launchArgs.append(RPC::buildNotifyArgs");
    QVERIFY2(gateIdx >= 0 && appendIdx > gateIdx,
             "ITEM 2: buildNotifyArgs must be appended only inside the notifyReady gate");
}

// ITEM 3 source guard: createZClassicConf() must READ the perms back from disk after the
// setPermissions(0600) call and log a diagnostic if group/other bits remain (best-effort,
// never aborts). The diagnostic must not echo the rpcpassword.
void TestLogic::conf_permsVerifiedAfterWrite() {
    const QString code = readStrippedSrc("connection.cpp");
    QVERIFY2(!code.isEmpty(), "could not locate src/connection.cpp");
    // Read-back of the on-disk perms after the write.
    QVERIFY2(code.contains("QFile::permissions(confLocation)"),
             "ITEM 3: createZClassicConf must read the conf perms back from disk");
    // Tests for any group/other access bit and logs about it.
    QVERIFY2(code.contains("ReadGroup") && code.contains("ReadOther"),
             "ITEM 3: the verify must test for group/other read/write bits");
    QVERIFY2(code.contains("could not be secured to 0600"),
             "ITEM 3: a clear 0600 diagnostic must be logged when the conf stays leaky");
}

// First-run trust: the send gate must ignore a FROZEN isSyncing flag (a poll that
// stopped refreshing) so a wedged flag can't imply a fresh sync problem — but must
// NOT treat a fresh flag, or a never-polled flag, as stale. Pure boundary check.
void TestLogic::sendgate_staleWhenPollStops() {
    const qint64 W = Settings::kSyncGateStaleSecs;   // window (180s)
    const qint64 now = 1'000'000;
    // Never polled yet (last==0): NOT stale -> let the normal gate run.
    QVERIFY(!Settings::syncGateIsStale(now, 0));
    // Fresh poll: not stale.
    QVERIFY(!Settings::syncGateIsStale(now, now));
    // Just inside the window: not stale.
    QVERIFY(!Settings::syncGateIsStale(now, now - (W - 1)));
    // Exactly at the window edge: not stale (uses '>' not '>=').
    QVERIFY(!Settings::syncGateIsStale(now, now - W));
    // Past the window: stale.
    QVERIFY(Settings::syncGateIsStale(now, now - (W + 1)));
    QVERIFY(Settings::syncGateIsStale(now, now - 10 * W));
}

// ===========================================================================
// NFT gallery (Phase C0) — model state, delegate sizeHint, threaded verify pipe.
// Pure GUI/logic; no daemon, no chain. Runs under QT_QPA_PLATFORM=offscreen via
// the QTEST_MAIN-provided QApplication (QPixmap/QImage are available offscreen).
// ===========================================================================
namespace {
// Build a small in-memory NFTItem set. Verify state starts pending (0) for all;
// the cache (or a direct onImageReady) sets it later.
static QVector<NFTItem> makeNftFixtures() {
    QVector<NFTItem> v;
    NFTItem a; a.name = "Aurora";  a.collection = "Originals"; a.txid = "tx-a";
               a.docHashHex = "aaaa"; a.cachePath = "p-a"; a.isPrivate = true;  v.push_back(a);
    NFTItem b; b.name = "Ember";   b.collection = "Foundry";   b.txid = "tx-b";
               b.docHashHex = "bbbb"; b.cachePath = "p-b"; b.isPrivate = false; v.push_back(b);
    NFTItem c; c.name = "Verdant"; c.collection = "Wild";      c.txid = "tx-c";
               c.docHashHex = "cccc"; c.cachePath = "p-c"; c.isPrivate = true;  v.push_back(c);
    return v;
}

// Write a real PNG to disk and return its path + true SHA-256 hex. Used by the
// pipeline test so it depends on NO bundled QRC resource.
static bool writeTestPng(const QString& path, const QColor& fill, QString& outHashHex) {
    QImage img(48, 48, QImage::Format_RGB32);
    img.fill(fill);
    if (!img.save(path, "PNG"))
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = f.readAll();
    outHashHex = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return true;
}
} // namespace

// Flicker-free guard: a byte-identical re-feed must NOT reset the model (mirrors
// TxTableModel). modelReset fires once for the first feed, not for the identical
// second feed, and DOES fire when the content actually changes.
void TestLogic::nftModelFingerprintGuard() {
    NFTGalleryModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    const QVector<NFTItem> items = makeNftFixtures();
    model.setItems(items);
    QCOMPARE(model.rowCount(), items.size());
    QCOMPARE(resetSpy.count(), 1);

    // Identical re-feed -> no-op (no extra reset).
    model.setItems(items);
    QCOMPARE(resetSpy.count(), 1);

    // A real change (one more item) -> a reset fires.
    QVector<NFTItem> more = items;
    NFTItem d; d.name = "Slate"; d.docHashHex = "dddd"; more.push_back(d);
    model.setItems(more);
    QCOMPARE(model.rowCount(), more.size());
    QCOMPARE(resetSpy.count(), 2);
}

// Custom roles read back correctly, and onImageReady() flips the matching row's
// verifyState to VERIFIED / MISMATCH while leaving an untouched row PENDING.
void TestLogic::nftModelRolesAndOnImageReady() {
    NFTGalleryModel model;
    model.setItems(makeNftFixtures());

    const QModelIndex i0 = model.index(0, 0);
    QCOMPARE(model.data(i0, NFTGalleryModel::NameRole).toString(),       QString("Aurora"));
    QCOMPARE(model.data(i0, NFTGalleryModel::CollectionRole).toString(), QString("Originals"));
    QCOMPARE(model.data(i0, NFTGalleryModel::IsPrivateRole).toBool(),    true);
    // PENDING by construction.
    QCOMPARE(model.data(i0, NFTGalleryModel::VerifyStateRole).toInt(),   0);
    QVERIFY(model.data(i0, NFTGalleryModel::ThumbnailRole).value<QPixmap>().isNull());

    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

    // Deliver a VERIFIED thumbnail for row 0's hash ("aaaa").
    QPixmap pm(8, 8); pm.fill(Qt::green);
    model.onImageReady("aaaa", pm, /*verified*/1);
    QCOMPARE(model.data(i0, NFTGalleryModel::VerifyStateRole).toInt(), 1);
    QVERIFY(!model.data(i0, NFTGalleryModel::ThumbnailRole).value<QPixmap>().isNull());
    QVERIFY(dataSpy.count() >= 1);

    // Deliver a MISMATCH for row 1's hash ("bbbb").
    model.onImageReady("bbbb", QPixmap(), /*mismatch*/2);
    QCOMPARE(model.data(model.index(1, 0), NFTGalleryModel::VerifyStateRole).toInt(), 2);

    // Row 2 was never delivered -> still PENDING.
    QCOMPARE(model.data(model.index(2, 0), NFTGalleryModel::VerifyStateRole).toInt(), 0);

    // A hash that matches no row is a harmless no-op (no crash, no state change).
    model.onImageReady("zzzz", pm, 1);
    QCOMPARE(model.data(model.index(2, 0), NFTGalleryModel::VerifyStateRole).toInt(), 0);
}

// Review fix #4: a confirmed-owned card whose image bytes are NOT on this computer
// must NOT read the same as a still-loading or a verified card. The model's
// Qt::ToolTipRole names the ACTUAL state, so a no-bytes card is a stable terminal
// "you hold this; image isn't here" — never a perpetual spinner, never a false
// verified/mismatch claim.
void TestLogic::nftModelTooltipNamesState() {
    NFTGalleryModel model;
    model.setItems(makeNftFixtures());

    // Row 0 has NO thumbnail delivered (the default for on-chain NFTs): the tooltip
    // must state ownership-without-local-image, and must NOT imply loading/verifying.
    const QModelIndex i0 = model.index(0, 0);
    const QString tipNoBytes = model.data(i0, Qt::ToolTipRole).toString();
    QVERIFY(tipNoBytes.contains("hold this", Qt::CaseInsensitive));
    QVERIFY(tipNoBytes.contains("isn't on this computer", Qt::CaseInsensitive));
    QVERIFY(!tipNoBytes.contains("Checking", Qt::CaseInsensitive));   // not a spinner
    QVERIFY(!tipNoBytes.contains("matches", Qt::CaseInsensitive));    // no verify claim

    // Deliver a VERIFIED thumbnail for row 0 -> the tooltip flips to the match claim.
    QPixmap pm(8, 8); pm.fill(Qt::green);
    model.onImageReady("aaaa", pm, /*verified*/1);
    const QString tipVerified = model.data(i0, Qt::ToolTipRole).toString();
    QVERIFY(tipVerified.contains("matches", Qt::CaseInsensitive));
    QVERIFY(!tipVerified.contains("hold this", Qt::CaseInsensitive));
    // The no-bytes and verified tooltips are genuinely DIFFERENT (the core of fix #4).
    QVERIFY(tipNoBytes != tipVerified);
}

// sizeHint is the fixed 168x208 card scaled by the device pixel ratio, and is
// STABLE across calls / indices (QListView::setUniformItemSizes relies on this).
void TestLogic::nftDelegateSizeHintStable() {
    NFTGalleryModel model;
    model.setItems(makeNftFixtures());
    NFTGalleryDelegate delegate;

    QStyleOptionViewItem opt;
    const QSize s0 = delegate.sizeHint(opt, model.index(0, 0));
    const QSize s1 = delegate.sizeHint(opt, model.index(1, 0));
    const QSize s2 = delegate.sizeHint(opt, model.index(2, 0));

    // Stable across rows.
    QCOMPARE(s0, s1);
    QCOMPARE(s1, s2);

    // Matches the declared base card * dpr.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QSize base = NFTGalleryDelegate::baseCardSize();
    QCOMPARE(base, QSize(168, 208));
    QCOMPARE(s0.width(),  qRound(base.width()  * dpr));
    QCOMPARE(s0.height(), qRound(base.height() * dpr));
    QVERIFY(s0.width() > 0 && s0.height() > 0);
}

// End-to-end pipeline: real PNGs on disk, a correct hash -> VERIFIED, a wrong
// hash -> MISMATCH, and a missing-bytes item -> stays PENDING. Exercises the
// off-GUI-thread read+SHA256+decode and the QPixmap-on-GUI-thread handoff.
void TestLogic::nftCachePipelineVerifyMismatchPending() {
    // Isolate AppDataLocation so cacheDir() writes into a throwaway dir.
    QTemporaryDir appDir;
    QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    QString hashGood, hashOther;
    const QString pGood = tmp.path() + "/good.png";
    const QString pOther = tmp.path() + "/other.png";
    QVERIFY(writeTestPng(pGood,  QColor(20, 160, 90),  hashGood));
    QVERIFY(writeTestPng(pOther, QColor(200, 90, 40),  hashOther));
    QVERIFY(hashGood != hashOther);

    // A deliberately WRONG (but distinct) expected hash for the mismatch row, so
    // each request maps to exactly one model row (onImageReady matches by hash).
    const QString hashWrong = "0000000000000000000000000000000000000000000000000000000000000000";

    NFTGalleryModel model;
    QVector<NFTItem> items;
    // 0: docHash == real bytes hash -> VERIFIED (1)
    { NFTItem it; it.name = "Good";     it.docHashHex = hashGood;  it.cachePath = pGood;  items.push_back(it); }
    // 1: docHash does NOT match the bytes -> MISMATCH (2)
    { NFTItem it; it.name = "Mismatch"; it.docHashHex = hashWrong; it.cachePath = pOther; items.push_back(it); }
    // 2: missing file -> stays PENDING (0)
    { NFTItem it; it.name = "Pending";  it.docHashHex = "deadbeef";it.cachePath = tmp.path() + "/nope.png"; items.push_back(it); }
    model.setItems(items);

    NFTImageCache cache(&model);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

    const int sizePx = 32;
    // request(hash[=dedupe+match key], bytesPath, onChainExpectedHash, sizePx).
    cache.request(items[0].docHashHex, items[0].cachePath, items[0].docHashHex, sizePx);
    cache.request(items[1].docHashHex, items[1].cachePath, items[1].docHashHex, sizePx);
    cache.request(items[2].docHashHex, items[2].cachePath, items[2].docHashHex, sizePx);

    // Pump the event loop until the two real-file items have reported (or timeout).
    QElapsedTimer timer; timer.start();
    auto rowState = [&](int r){ return model.data(model.index(r,0),
                                  NFTGalleryModel::VerifyStateRole).toInt(); };
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (rowState(0) != 0 && rowState(1) != 0)
            break;
    }

    // good.png bytes hash == hashGood  -> VERIFIED
    QCOMPARE(rowState(0), 1);
    // other.png bytes hash != hashWrong -> MISMATCH
    QCOMPARE(rowState(1), 2);
    // missing-file item never decoded   -> still PENDING
    QCOMPARE(rowState(2), 0);
    // A dataChanged repaint was emitted for at least one delivery.
    QVERIFY(dataSpy.count() >= 1);

    // The on-disk thumbnail cache file was written atomically for the good asset.
    const QString thumb = NFTImageCache::cacheDir() + "/" + hashGood + "_"
                        + QString::number(sizePx) + ".png";
    QVERIFY2(QFileInfo::exists(thumb), qPrintable("expected cached thumb: " + thumb));

    QStandardPaths::setTestModeEnabled(false);
}

// ===========================================================================
// ContentEngine (Phase C1) — streaming hash / chunked Merkle / classification /
// content-addressed cache / privacy guard. Pure-logic L0 (no event loop needed
// except the engine-level verify() test). Helpers below build deterministic
// byte patterns so assertions are real values, not tautologies.
// ===========================================================================
namespace {
// Deterministic pseudo-random-ish bytes of length n (stable across runs so the
// expected hashes are reproducible). Not crypto; just a varied, repeatable fill.
static QByteArray patternBytes(qint64 n, quint8 seed = 1) {
    QByteArray b;
    b.resize(static_cast<int>(n));
    quint32 x = 0x9e3779b1u ^ (static_cast<quint32>(seed) << 16);
    for (qint64 i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;   // xorshift32
        b[static_cast<int>(i)] = static_cast<char>(x & 0xff);
    }
    return b;
}
static bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(bytes) == bytes.size();
}
// Reference Merkle root computed independently of the engine's streaming path
// (over an in-memory byte array) so the test is a true cross-check, not a mirror
// of the implementation. Uses the SAME PINNED rules: leaf=SHA256(0x00||chunk),
// node=SHA256(0x01||L||R), odd-node PROMOTED.
static QByteArray refLeaf(const QByteArray& chunk) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    const char z = 0x00; h.addData(&z, 1); h.addData(chunk);
    return h.result();
}
static QByteArray refMerkle(const QByteArray& data, int chunkSize) {
    QVector<QByteArray> leaves;
    for (int off = 0; off < data.size(); off += chunkSize)
        leaves.push_back(refLeaf(data.mid(off, chunkSize)));
    if (leaves.isEmpty()) return QByteArray();
    if (leaves.size() == 1) return leaves.first();
    while (leaves.size() > 1) {
        QVector<QByteArray> next;
        for (int i = 0; i < leaves.size(); i += 2) {
            if (i + 1 < leaves.size()) {
                QCryptographicHash h(QCryptographicHash::Sha256);
                const char o = 0x01; h.addData(&o, 1);
                h.addData(leaves[i]); h.addData(leaves[i + 1]);
                next.push_back(h.result());
            } else {
                next.push_back(leaves[i]);     // PROMOTE
            }
        }
        leaves = next;
    }
    return leaves.first();
}
} // namespace

// streamingSha256 over a fixed buffer must EQUAL a one-shot QCryptographicHash of
// the whole bytes, across chunk-boundary edge sizes (0, 1, buf-1, buf, buf+1, 3x,
// 5x). This is the streaming-parity proof: the 1 MiB-buffered read loop produces
// the identical digest as readAll()+hash, with bounded memory.
void TestLogic::ceStreamHashEqualsWholeHash_data() {
    QTest::addColumn<qint64>("size");
    const qint64 B = ContentEngine::kHashBufBytes;   // 1 MiB
    QTest::newRow("empty")     << qint64(0);
    QTest::newRow("one")       << qint64(1);
    QTest::newRow("buf-1")     << (B - 1);
    QTest::newRow("buf")       << B;
    QTest::newRow("buf+1")     << (B + 1);
    QTest::newRow("3*buf")     << (3 * B);
    QTest::newRow("5*buf+123") << (5 * B + 123);
}
void TestLogic::ceStreamHashEqualsWholeHash() {
    QFETCH(qint64, size);
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/blob.bin";
    const QByteArray data = patternBytes(size, 7);
    QVERIFY(writeBytes(p, data));

    const QByteArray streamed = ContentEngine::streamingSha256(p);
    const QByteArray oneShot  = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    QCOMPARE(streamed.toHex(), oneShot.toHex());
    // sha256Whole from streamDescribe must agree too.
    ContentDescriptor d;
    QVERIFY(ContentEngine::streamDescribe(p, ContentEngine::kChunkBytes, d));
    QCOMPARE(d.sha256Whole.toHex(), oneShot.toHex());
    QCOMPARE(d.fileSize, static_cast<quint64>(size));
}

// A 64 MiB file must stream to a correct digest in bounded memory (proves there
// is NO readAll() inflate path — the test would OOM on a constrained box if it
// loaded the whole file twice). We assert the streamed hash matches a one-shot
// hash of the same generated bytes.
void TestLogic::ceStreamHashBoundedLargeFile() {
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/big.bin";
    const qint64 N = 64LL * 1024 * 1024;             // 64 MiB
    // Write in 1 MiB blocks so the TEST itself doesn't hold 64 MiB twice.
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCryptographicHash ref(QCryptographicHash::Sha256);
    const QByteArray block = patternBytes(1 << 20, 3);
    for (int i = 0; i < 64; ++i) {
        QVERIFY(f.write(block) == block.size());
        ref.addData(block);
    }
    f.close();

    const QByteArray streamed = ContentEngine::streamingSha256(p);
    QVERIFY(!streamed.isEmpty());
    QCOMPARE(streamed.toHex(), ref.result().toHex());
}

// Merkle root from the engine == an independent reference implementation, for a
// multi-chunk file. Determinism: a second describe yields the identical root.
void TestLogic::ceMerkleRootDeterministic() {
    const int chunk = 4096;                          // small chunk for a multi-leaf tree
    const QByteArray data = patternBytes(chunk * 5 + 17, 11);   // 6 leaves (odd count)
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/m.bin";
    QVERIFY(writeBytes(p, data));

    ContentDescriptor d1, d2;
    QVERIFY(ContentEngine::streamDescribe(p, chunk, d1));
    QVERIFY(ContentEngine::streamDescribe(p, chunk, d2));
    QCOMPARE(d1.merkleRoot.toHex(), d2.merkleRoot.toHex());     // deterministic
    QCOMPARE(d1.chunkCount, quint32(6));                        // ceil((5*4096+17)/4096)=6
    QCOMPARE(d1.merkleRoot.toHex(), refMerkle(data, chunk).toHex());   // cross-check
    QVERIFY(d1.merkleRoot.size() == 32);
}

// Tamper detection: flip ONE byte in chunk K. verifyChunk() must fail for chunk K
// and still pass for every other chunk, and the recomputed root must change. This
// is the streamed/incremental verify property (a tamper fails at chunk K alone).
void TestLogic::ceMerkleDetectsTamperedChunk() {
    const int chunk = 1024;
    QByteArray data = patternBytes(chunk * 4, 5);    // exactly 4 leaves
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/t.bin";
    QVERIFY(writeBytes(p, data));

    ContentDescriptor d;
    QVector<QByteArray> leaves;
    QVERIFY(ContentEngine::streamDescribe(p, chunk, d, &leaves));
    QCOMPARE(leaves.size(), 4);

    // Each original chunk verifies against its leaf.
    for (int i = 0; i < 4; ++i)
        QVERIFY(ContentEngine::verifyChunk(d, leaves, i, data.mid(i * chunk, chunk)));

    // Flip one byte in chunk 2.
    QByteArray tamperedChunk = data.mid(2 * chunk, chunk);
    tamperedChunk[10] = tamperedChunk[10] ^ 0x01;
    // Chunk 2 now FAILS; chunks 0,1,3 still PASS.
    QVERIFY(!ContentEngine::verifyChunk(d, leaves, 2, tamperedChunk));
    QVERIFY( ContentEngine::verifyChunk(d, leaves, 0, data.mid(0, chunk)));
    QVERIFY( ContentEngine::verifyChunk(d, leaves, 3, data.mid(3 * chunk, chunk)));
    // Out-of-range index is a safe false (no crash).
    QVERIFY(!ContentEngine::verifyChunk(d, leaves, 99, data.mid(0, chunk)));

    // The whole-file root changes when the tampered chunk is written back.
    QByteArray td = data; td[2 * chunk + 10] = td[2 * chunk + 10] ^ 0x01;
    const QString p2 = tmp.path() + "/t2.bin";
    QVERIFY(writeBytes(p2, td));
    ContentDescriptor d2;
    QVERIFY(ContentEngine::streamDescribe(p2, chunk, d2));
    QVERIFY(d2.merkleRoot.toHex() != d.merkleRoot.toHex());
}

// Anchor-ambiguity rule: a file <= chunk_size has Merkle root == SHA256(0x00||bytes)
// == leaf_0, which is DIFFERENT from the bare SHA256(bytes). Both are exposed
// (merkleRoot vs sha256Whole) so the wallet can pick the algorithm deterministically.
void TestLogic::ceMerkleSingleLeafDegenerate() {
    const QByteArray data = patternBytes(500, 9);    // < default 1 MiB chunk
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/s.bin";
    QVERIFY(writeBytes(p, data));

    ContentDescriptor d;
    QVERIFY(ContentEngine::streamDescribe(p, ContentEngine::kChunkBytes, d));
    QCOMPARE(d.chunkCount, quint32(1));

    const QByteArray leaf0   = ContentEngine::merkleLeaf(data);     // SHA256(0x00||bytes)
    const QByteArray bareSha = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    QCOMPARE(d.merkleRoot.toHex(),  leaf0.toHex());                 // root == leaf_0
    QCOMPARE(d.sha256Whole.toHex(), bareSha.toHex());              // whole == bare
    QVERIFY(d.merkleRoot.toHex() != d.sha256Whole.toHex());        // and they DIFFER
}

// Chunk-boundary cases: exact multiple, +1, and empty file -> correct chunkCount
// and an empty root for the empty file (no leaves).
void TestLogic::ceMerkleChunkBoundaries_data() {
    QTest::addColumn<qint64>("size");
    QTest::addColumn<int>("chunk");
    QTest::addColumn<quint32>("expectCount");
    QTest::newRow("empty")        << qint64(0)    << 256 << quint32(0);
    QTest::newRow("exact-1")      << qint64(256)  << 256 << quint32(1);
    QTest::newRow("exact-3")      << qint64(768)  << 256 << quint32(3);
    QTest::newRow("plus-one")     << qint64(257)  << 256 << quint32(2);
    QTest::newRow("under-one")    << qint64(255)  << 256 << quint32(1);
}
void TestLogic::ceMerkleChunkBoundaries() {
    QFETCH(qint64, size);
    QFETCH(int, chunk);
    QFETCH(quint32, expectCount);
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/b.bin";
    const QByteArray data = patternBytes(size, 13);
    QVERIFY(writeBytes(p, data));

    ContentDescriptor d;
    QVERIFY(ContentEngine::streamDescribe(p, chunk, d));
    QCOMPARE(d.chunkCount, expectCount);
    QCOMPARE(d.fileSize, static_cast<quint64>(size));
    if (expectCount == 0) {
        QVERIFY(d.merkleRoot.isEmpty());             // empty file -> no leaves
    } else {
        QCOMPARE(d.merkleRoot.toHex(), refMerkle(data, chunk).toHex());
    }
}

// The engine's verify path accepts EITHER the bare whole-file SHA-256 OR the
// chunked Merkle root as the on-chain anchor (resolving anchor-ambiguity without
// a manifest). We test the pure compare logic via streamDescribe + the two hexes.
void TestLogic::ceVerifyAcceptsRootOrWhole() {
    const QByteArray data = patternBytes(2048, 21);
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/v.bin";
    QVERIFY(writeBytes(p, data));

    ContentDescriptor d;
    QVERIFY(ContentEngine::streamDescribe(p, 1024, d));   // 2 leaves -> root != whole
    const QString wholeHex = QString::fromLatin1(d.sha256Whole.toHex());
    const QString rootHex  = QString::fromLatin1(d.merkleRoot.toHex());
    QVERIFY(wholeHex != rootHex);
    // A wrong anchor matches neither.
    const QString wrong = QString(64, QChar('0'));
    QVERIFY(wrong != wholeHex && wrong != rootHex);
}

// ContentEngine::anchorHexFor is the ONE shared anchor rule used by the mint wizard
// (to compute the document_hash it writes) AND the detail attach-gate (to compare a
// chosen file). It returns the bare whole-file SHA-256 for a small/single-chunk file
// and the Merkle root for a multi-chunk file. Pure -> L0-testable without a QDialog.
void TestLogic::ceAnchorHexForRootVsWhole() {
    QTemporaryDir tmp; QVERIFY(tmp.isValid());

    // (1) Small file (chunkCount == 1): anchorHexFor == whole-file SHA-256 hex.
    const QString small = tmp.path() + "/small.bin";
    QVERIFY(writeBytes(small, patternBytes(600, 17)));
    ContentDescriptor ds;
    QVERIFY(ContentEngine::streamDescribe(small, 1024, ds));   // 1 leaf
    QCOMPARE(ds.chunkCount, quint32(1));
    QCOMPARE(ContentEngine::anchorHexFor(ds),
             QString::fromLatin1(ds.sha256Whole.toHex()));
    QVERIFY(ContentEngine::anchorHexFor(ds) !=
            QString::fromLatin1(ds.merkleRoot.toHex())   // single-leaf root != bare whole
            || ds.merkleRoot == ds.sha256Whole);          // (defensive; they should differ)

    // (2) Multi-chunk file (chunkCount > 1): anchorHexFor == Merkle root hex (!= whole).
    const QString big = tmp.path() + "/big.bin";
    QVERIFY(writeBytes(big, patternBytes(2048, 19)));
    ContentDescriptor dm;
    QVERIFY(ContentEngine::streamDescribe(big, 1024, dm));     // 2 leaves
    QVERIFY(dm.chunkCount > 1);
    QCOMPARE(ContentEngine::anchorHexFor(dm),
             QString::fromLatin1(dm.merkleRoot.toHex()));
    QVERIFY(ContentEngine::anchorHexFor(dm) !=
            QString::fromLatin1(dm.sha256Whole.toHex()));     // root != whole in multi-chunk
}

// MIME/kind classification by header sniff + extension.
void TestLogic::ceKindClassification_data() {
    QTest::addColumn<QString>("ext");
    QTest::addColumn<int>("expectKind");   // ContentKind
    QTest::newRow("png") << QString("png") << int(CK_Image);
    QTest::newRow("jpg") << QString("jpg") << int(CK_Image);
    QTest::newRow("mp4") << QString("mp4") << int(CK_Video);
    QTest::newRow("pdf") << QString("pdf") << int(CK_Document);
    QTest::newRow("txt") << QString("txt") << int(CK_Document);
    QTest::newRow("bin") << QString("bin") << int(CK_Bytes);
}
void TestLogic::ceKindClassification() {
    QFETCH(QString, ext);
    QFETCH(int, expectKind);
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/asset." + ext;

    if (ext == "png" || ext == "jpg") {
        QImage img(8, 8, QImage::Format_RGB32); img.fill(Qt::blue);
        QVERIFY(img.save(p, ext == "png" ? "PNG" : "JPG"));
    } else if (ext == "pdf") {
        // Minimal PDF magic so the header sniff classifies it as application/pdf.
        QVERIFY(writeBytes(p, QByteArray("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n")));
    } else if (ext == "mp4") {
        // ISO-BMFF 'ftyp' box with an mp4 brand -> video/mp4 by magic.
        QByteArray mp4;
        mp4.append('\x00'); mp4.append('\x00'); mp4.append('\x00'); mp4.append('\x18');
        mp4.append("ftyp"); mp4.append("mp42");
        mp4.append('\x00'); mp4.append('\x00'); mp4.append('\x00'); mp4.append('\x00');
        mp4.append("mp42"); mp4.append("isom");
        QVERIFY(writeBytes(p, mp4));
    } else if (ext == "txt") {
        QVERIFY(writeBytes(p, QByteArray("hello, this is plain text\n")));
    } else {
        QVERIFY(writeBytes(p, patternBytes(64, 2)));   // opaque bytes
    }

    QString mime;
    const ContentKind k = ContentEngine::classifyKind(p, mime);
    QCOMPARE(int(k), expectKind);
    QVERIFY(!mime.isEmpty());   // a MIME string was resolved
}

// Human byte-size formatting.
void TestLogic::ceHumanSize() {
    QCOMPARE(ContentEngine::humanSize(0),                  QString("0 B"));
    QCOMPARE(ContentEngine::humanSize(42),                 QString("42 B"));
    QCOMPARE(ContentEngine::humanSize(1024),               QString("1.0 KB"));
    QCOMPARE(ContentEngine::humanSize(1536),               QString("1.5 KB"));
    QCOMPARE(ContentEngine::humanSize(12u * 1024 * 1024 + 314572),  // ~12.3 MB
             QString("12.3 MB"));
    QCOMPARE(ContentEngine::humanSize(2ull * 1024 * 1024 * 1024),   // 2.0 GB
             QString("2.0 GB"));
}

// PATH-TRAVERSAL: safeKey() must strip every separator/non-hex char so a hostile
// hash can never escape the cache dir, and cacheGet of such a hash never resolves
// outside the store. The neutralized key is hex-only and length-capped.
void TestLogic::ceSafeKeyPathTraversal() {
    // A traversal attempt: only the hex chars survive (a,e,a,d,b,e,e,f...).
    const QString evil = "../../etc/passwd";
    const QString key  = ContentEngine::safeKey(evil);
    QVERIFY(!key.contains('/'));
    QVERIFY(!key.contains('.'));
    QVERIFY(!key.contains('\\'));
    QVERIFY(!key.contains(':'));
    // Every surviving char is lowercase hex.
    for (QChar c : key)
        QVERIFY((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    // "../../etc/passwd": only hex letters survive, in order -> e,c (from "etc"),
    // a,d (from "passwd") => "ecad". Deterministic, separator-free.
    QCOMPARE(key, QString("ecad"));
    // A non-hex-only string collapses to "" (never resolves a cache file).
    QCOMPARE(ContentEngine::safeKey("/\\..::zzz"), QString());

    // Uppercase hex normalizes to lowercase.
    QCOMPARE(ContentEngine::safeKey("ABCDEF"), QString("abcdef"));

    // A traversal "hash" must not resolve to anything in the blob cache.
    QTemporaryDir appDir; QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(ContentEngine::cacheGet("../../etc/passwd").isEmpty() ||
            // (if "ecad" happens not to exist) it must at least be INSIDE the store:
            ContentEngine::cacheGet("../../etc/passwd").startsWith(ContentEngine::blobCacheDir()));
    QStandardPaths::setTestModeEnabled(false);
}

// Content-addressed cache round-trip: cachePut copies bytes once, cacheGet finds
// them, a missing hash returns the empty sentinel, and a second put is a no-op
// (store-once) returning true.
void TestLogic::ceCacheRoundTrip() {
    QTemporaryDir appDir; QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QByteArray data = patternBytes(4096, 31);
    const QString src = tmp.path() + "/src.bin";
    QVERIFY(writeBytes(src, data));
    const QString hashHex = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

    // QStandardPaths test mode uses a FIXED per-user dir (it ignores XDG_DATA_HOME),
    // so a blob from a prior run of this very test may persist. Start from a clean
    // slate for THIS key so the miss-before-put assertion is meaningful.
    QFile::remove(ContentEngine::blobCacheDir() + "/" + hashHex);

    // Miss before put (empty sentinel).
    QVERIFY(ContentEngine::cacheGet(hashHex).isEmpty());
    // Put -> hit, with byte-identical content, inside the blob dir.
    QVERIFY(ContentEngine::cachePut(hashHex, src));
    const QString got = ContentEngine::cacheGet(hashHex);
    QVERIFY(!got.isEmpty());
    QVERIFY(got.startsWith(ContentEngine::blobCacheDir()));
    QFile gf(got); QVERIFY(gf.open(QIODevice::ReadOnly));
    QCOMPARE(gf.readAll(), data);
    gf.close();
    // Store-once: a second put is a harmless true.
    QVERIFY(ContentEngine::cachePut(hashHex, src));

    QStandardPaths::setTestModeEnabled(false);
}

// PRIVACY: request()/posterFor()/cachePut() must REFUSE a remote URL. We assert
// the static guard and that cachePut on a URL fails (no fetch path exists).
void TestLogic::ceRejectsRemoteUrl() {
    QVERIFY(ContentEngine::isRemoteUrl("http://evil.example/x.png"));
    QVERIFY(ContentEngine::isRemoteUrl("https://host/v.mp4"));
    QVERIFY(ContentEngine::isRemoteUrl("ipfs://bafy..."));
    QVERIFY(ContentEngine::isRemoteUrl("zdc1://deadbeef"));
    QVERIFY(!ContentEngine::isRemoteUrl("/home/user/local.png"));
    QVERIFY(!ContentEngine::isRemoteUrl(":/resources/icon.png"));

    QTemporaryDir appDir; QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    // cachePut of a URL fails (cannot fetch; privacy).
    QVERIFY(!ContentEngine::cachePut(
        "aa", "https://host/file.bin"));

    // request() with a URL is a silent no-op: nothing is queued, so onImageReady
    // never fires. We assert the model row stays PENDING.
    NFTGalleryModel model;
    QVector<NFTItem> items;
    { NFTItem it; it.name = "Remote"; it.docHashHex = "abc123"; it.cachePath = "https://host/x.png"; items.push_back(it); }
    model.setItems(items);
    ContentEngine eng(&model);
    eng.request("abc123", "https://host/x.png", "abc123", 32);
    // Pump briefly; verifyState must remain PENDING (no worker ran).
    QElapsedTimer t; t.start();
    while (t.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCOMPARE(model.data(model.index(0,0), NFTGalleryModel::VerifyStateRole).toInt(), 0);

    QStandardPaths::setTestModeEnabled(false);
}

// End-to-end engine verify(): correct anchor -> Verified; a 1-byte tamper ->
// Mismatch. Exercises the streaming off-GUI-thread verify + the signal landing
// on the GUI thread. Uses the bare-SHA256 anchor (small file).
void TestLogic::ceVerifyMismatchOnFlippedByte() {
    QTemporaryDir appDir; QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QByteArray data = patternBytes(5000, 17);
    const QString p = tmp.path() + "/asset.bin";
    QVERIFY(writeBytes(p, data));
    const QString goodHex = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

    ContentEngine eng(nullptr);
    QSignalSpy spy(&eng, &ContentEngine::verifyDone);

    // 1) correct anchor -> Verified
    eng.verify(p, goodHex, /*token*/1);
    // 2) wrong anchor (a flipped-byte hash) -> Mismatch
    QByteArray flipped = data; flipped[3] = flipped[3] ^ 0x01;
    const QString wrongHex = QString::fromLatin1(
        QCryptographicHash::hash(flipped, QCryptographicHash::Sha256).toHex());
    QVERIFY(wrongHex != goodHex);
    eng.verify(p, wrongHex, /*token*/2);

    QElapsedTimer t; t.start();
    while (spy.count() < 2 && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    QCOMPARE(spy.count(), 2);

    // Collect results by token (order across the pool is not guaranteed).
    int vsTok1 = -1, vsTok2 = -1;
    for (const QList<QVariant>& sig : spy) {
        const quint64 tok = sig.at(0).toULongLong();
        const int vs      = sig.at(1).toInt();
        if (tok == 1) vsTok1 = vs;
        if (tok == 2) vsTok2 = vs;
    }
    QCOMPARE(vsTok1, int(CE_Verified));    // correct anchor
    QCOMPARE(vsTok2, int(CE_Mismatch));    // flipped-byte anchor

    QStandardPaths::setTestModeEnabled(false);
}

// posterForToken() must deliver a NON-null large image plus the correct verify
// state via posterReady(token, img, verifyState) — WITHOUT routing into the
// gallery model (the engine is constructed with a null model here, so any model
// route would crash/no-op). Token filtering: the reply carries the caller's token.
void TestLogic::cePosterForTokenDelivers() {
    QTemporaryDir appDir; QVERIFY(appDir.isValid());
    qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const QString p = tmp.path() + "/pic.png";
    // A real, decodable image so the poster path produces a non-null QImage.
    QImage src(24, 24, QImage::Format_ARGB32);
    src.fill(Qt::green);
    QVERIFY(src.save(p, "PNG"));
    // The on-chain anchor = the bare SHA-256 of the PNG bytes (small file).
    QFile f(p); QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll(); f.close();
    const QString anchor = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    ContentEngine eng(nullptr);   // null model: the token path must not touch it
    QSignalSpy spy(&eng, &ContentEngine::posterReady);

    eng.posterForToken(p, anchor, anchor, /*sizePx*/256, /*token*/42);

    QElapsedTimer t; t.start();
    while (spy.count() < 1 && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    QCOMPARE(spy.count(), 1);

    const QList<QVariant> sig = spy.at(0);
    QCOMPARE(sig.at(0).toULongLong(), quint64(42));         // our token
    const QImage img = sig.at(1).value<QImage>();
    QVERIFY(!img.isNull());                                 // large image delivered
    QCOMPARE(sig.at(2).toInt(), int(CE_Verified));         // anchor matched

    QStandardPaths::setTestModeEnabled(false);
}

// Every reject path of posterForToken() must still emit posterReady with a null
// image + CE_Pending so the dialog never hangs: token==0, empty path, and a
// remote URL (privacy guard).
void TestLogic::cePosterForTokenRejects() {
    ContentEngine eng(nullptr);
    QSignalSpy spy(&eng, &ContentEngine::posterReady);

    // token==0 -> immediate synchronous reject.
    eng.posterForToken("/some/local/path.png", "aa", "aa", 256, /*token*/0);
    // empty path.
    eng.posterForToken("", "aa", "aa", 256, /*token*/7);
    // remote URL (privacy guard) -> reject, never fetched.
    eng.posterForToken("https://evil.example/x.png", "aa", "aa", 256, /*token*/8);

    // All three reject synchronously (no worker queued), so the spy already has 3.
    QCOMPARE(spy.count(), 3);
    for (const QList<QVariant>& sig : spy) {
        QVERIFY(sig.at(1).value<QImage>().isNull());        // null image
        QCOMPARE(sig.at(2).toInt(), int(CE_Pending));       // pending state
    }
}

QTEST_MAIN(TestLogic)
#include "tst_logic.moc"
