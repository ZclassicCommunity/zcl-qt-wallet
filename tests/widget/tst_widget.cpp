// ============================================================================
// tst_widget — L1 widget / integration tests for the ZClassic Qt5 GUI wallet.
//
// Links the full app minus main.cpp, constructs the REAL MainWindow (which
// creates the REAL RPC) under QT_QPA_PLATFORM=offscreen, and asserts the
// privacy UX in-process via the public widget API (MainWindow::ui).
//
// SAFETY: a fresh isolated HOME is installed (see main(), below) BEFORE the
// QApplication so that:
//   * QStandardPaths::HomeLocation never resolves to the real ~/.zclassic
//     (autoDetectZClassicConf would otherwise find the live peer#1 conf), and
//   * QSettings writes land in a throwaway dir, never the user's real config.
// useEmbedded is forced false so the deferred ConnectionLoader can never launch
// a daemon. The D-series tests never spin the event loop long enough for the
// loader's singleShot(1) to fire at all (setChecked() emits toggled()
// synchronously), so loadConnection() is inert for them.
//
// D-SERIES (Phase 3c, private-by-default Receive). The Receive page rests on ONE
// shielded Sapling z-address with a green "Private" badge; the transparent +
// legacy-Sprout radios are tucked behind a collapsed "Other address types
// (advanced)" QToolButton (MainWindow::btnReceiveAdvanced). The D-series drives
// that disclosure exactly as a user would — expand the toggle, then select an
// option — and asserts:
//   D1  private-by-default RESTING state (badge shown, panel collapsed, t-Addr
//       hidden, no PUBLIC caption);
//   D2  expanding reveals t-Addr and selecting it shows the red PUBLIC caption;
//   D3  selecting legacy-Sprout DISABLES new-address (no Sprout backdoor);
//   D4  collapsing RETURNS TO PRIVATE (caption cleared, badge restored, Sapling
//       re-selected);
//   D5  selecting Sapling clears the transparent caption.
// Each fails if the private-by-default IA regresses (radios exposed at rest, the
// disclosure not hiding t-Addr, or collapsing not returning to private).
// ============================================================================
#include <QtTest/QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QRadioButton>
#include <QToolButton>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QFontMetrics>
#include <QWidget>
#include <QDialog>
#include <QTimer>
#include <QSettings>
#include <QMessageBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QDropEvent>
#include <QCryptographicHash>
#include <QImage>
#include <QPixmap>
#include <QPalette>
#include <QColor>
#include <QStyle>
#include <memory>

#include <QLocalSocket>
#include <QSignalSpy>

// PERF harness (perf16/perf22) extra includes.
#include <QElapsedTimer>
#include <QVector>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "rpc.h"
#include "settings.h"
#include "connection.h"
#include "notifyserver.h"
#include "nft.h"
#include "contentengine.h"
#include "nftdetaildialog.h"
#include "nftmintdialog.h"
#include "nftsenddialog.h"
#include "nftselldialog.h"
#include "nftbuydialog.h"
#include "shieldsenddialog.h"
#include "shieldreceivedialog.h"
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QListWidget>
#include <QComboBox>
#include <QTemporaryFile>

// Kept alive for the whole process so QStandardPaths/QSettings stay isolated.
static QTemporaryDir* g_homeDir = nullptr;

// ---- NOTIFY-SRV test helpers (free, no class state) -------------------------
// A short, per-test unix-socket path under the temp dir (well within the ~108-char
// AF_UNIX limit). NotifyServer::start() clears any stale socket first.
static QString notifySockPath(const QString& tag) {
    return QDir(QDir::tempPath()).filePath("zqwn_" + tag + ".sock");
}
// Connect a client, send `payload`, return the connected socket (caller spins the
// event loop / reads). Returns nullptr if the connect failed.
static QLocalSocket* notifySendLine(const QString& path, const QByteArray& payload) {
    QLocalSocket* c = new QLocalSocket();
    c->connectToServer(path);
    if (!c->waitForConnected(1000)) { delete c; return nullptr; }
    c->write(payload);
    c->flush();
    c->waitForBytesWritten(1000);
    return c;
}

class TestWidget : public QObject {
    Q_OBJECT

private:
    // Construct a MainWindow safely for an isolated, daemon-free test. The
    // caller owns it. useEmbedded is already false (set in main()); we also
    // force a non-syncing, peerful, recent-daemon Settings baseline so the
    // setupRecieveTab Sprout branch and confirm-dialog warnings behave
    // deterministically.
    MainWindow* makeWindow() {
        Settings::getInstance()->setUseEmbedded(false);
        Settings::getInstance()->setHeadless(true);
        Settings::getInstance()->setSyncing(false);
        Settings::getInstance()->setPeers(8);
        // >= 2000425 so the legacy-Sprout security warning is NOT auto-shown by
        // the rdioZAddr branch (keeps D3/D5 assertions about the shared
        // lblSproutWarning label clean).
        Settings::getInstance()->setZClassicdVersion(2001250);
        MainWindow* w = new MainWindow(nullptr);
        // Map the window so child-widget isVisible() reflects effective
        // visibility (a hidden top-level forces every child isVisible()==false).
        // Under the offscreen QPA this maps synchronously with no real surface,
        // and we do NOT spin the event loop here, so the RPC's deferred
        // loadConnection() singleShot never fires (no daemon, no modal).
        w->show();
        return w;
    }

private slots:

    // Between every test, invalidate any stray modal-dismisser poll chain (bump the
    // generation so a leftover QTimer::singleShot re-arm self-cancels on its next
    // fire) and reset the shared ack/confirm flags. We deliberately do NOT spin the
    // event loop here: the D-series relies on the RPC's deferred loadConnection()
    // singleShot never firing (no daemon), so processEvents() must not be called.
    void cleanup() {
        ++_modalGen;
        _ackSeen = false;
        _confirmAccepted = false;
    }

    // ---- STANDARDIZATION GUARD: text controls must never be shorter than their
    // own line box. This is the regression guard for the Receive-tab "Export
    // private key…" clip and the removed badge max-height:22px clamp. It loads the
    // SHIPPED res/styles/dark.qss (exactly as main.cpp does) and asserts every
    // representative text-bearing control sizes from its font metrics. The
    // invariant is RELATIVE to each widget's own font, so it is robust across
    // platforms / DPI. Any future rule that floors a control below its text — or
    // re-introduces a max-height clamp on a text control — fails HERE in CI.
    // See the control-height tokens + conventions in res/styles/dark.qss.
    void controlSizing_textControlsNeverClipBelowLineBox() {
        const QString prevSheet = qApp->styleSheet();
        QFile qssFile(":/styles/res/styles/dark.qss");
        QVERIFY2(qssFile.open(QFile::ReadOnly),
                 "shipped res/styles/dark.qss must be loadable from the qrc");
        qApp->setStyleSheet(QString::fromUtf8(qssFile.readAll()));

        QWidget host;                       // parents the throwaway controls
        QVBoxLayout hostLayout(&host);

        auto fits = [&](QWidget* w, const char* name) {
            w->ensurePolished();
            const int lineBox = QFontMetrics(w->font()).height();
            QVERIFY2(w->sizeHint().height() >= lineBox,
                     qPrintable(QStringLiteral(
                         "%1: styled height (%2px) is below its own font line box (%3px) — "
                         "text will clip. Size text controls from font metrics (min-height + "
                         "symmetric padding); never clamp a text control with max-height.")
                         .arg(name).arg(w->sizeHint().height()).arg(lineBox)));
        };

        auto* btnExport = new QPushButton(tr("Export private key…"), &host);
        btnExport->setObjectName("btnReceiveExportKey");
        hostLayout.addWidget(btnExport);

        auto* btnCopy = new QPushButton(tr("Copy"), &host);
        btnCopy->setObjectName("btnReceiveCopy");
        hostLayout.addWidget(btnCopy);

        auto* btnPlain = new QPushButton(tr("New Address"), &host);
        hostLayout.addWidget(btnPlain);

        auto* combo = new QComboBox(&host);
        combo->addItem(QStringLiteral("t1ExampleTransparentAddress pqgjy"));
        hostLayout.addWidget(combo);

        auto* edit = new QLineEdit(QStringLiteral("zs1descenders pqgjy"), &host);
        hostLayout.addWidget(edit);

        auto* bar = new QProgressBar(&host);
        bar->setTextVisible(true);
        bar->setFormat(QStringLiteral("Scanning pgy %p%"));
        bar->setValue(42);
        hostLayout.addWidget(bar);

        auto* badge = new QLabel(QStringLiteral("PUBLIC pgy"), &host);
        badge->setProperty("badge", true);
        badge->setProperty("tone", "public");
        hostLayout.addWidget(badge);

        host.show();

        fits(btnExport, "btnReceiveExportKey");
        fits(btnCopy,   "btnReceiveCopy");
        fits(btnPlain,  "QPushButton (generic)");
        fits(combo,     "QComboBox");
        fits(edit,      "QLineEdit");
        fits(bar,       "QProgressBar");
        fits(badge,     "QLabel[badge=true]");

        // The badge was the one true hard-clamp (max-height:22px). Guard the class
        // directly: its styled height must clear its line box with breathing room,
        // proving no max-height clamp near the line box can creep back in.
        QVERIFY2(badge->sizeHint().height() >= QFontMetrics(badge->font()).height() + 4,
                 "QLabel[badge=true] must not be clamped near its line box "
                 "(regression guard for the removed max-height:22px clip).");

        qApp->setStyleSheet(prevSheet);     // never leak the sheet to later tests
    }

    // NOTE on visibility throughout the D-series: the Receive widgets live on tab
    // page 2, which is NOT the current page, so effective isVisible() is always
    // false for any widget on a non-current tab regardless of its own flag. The
    // behavior under test is each widget's OWN visible flag (does setup/the
    // disclosure explicitly hide/show it), which is exactly what !isHidden()
    // reports — independent of whether the parent page/window is mapped.

    // ---- D1: PRIVATE-BY-DEFAULT resting state (Phase 3c) -------------------
    // At rest the Receive page shows ONLY the shielded Sapling path: the green
    // "Private" badge is shown, the advanced disclosure exists but is COLLAPSED,
    // the t-Addr radio is NOT visible, and there is NO PUBLIC/scary caption.
    // FAILS if private-by-default regresses (e.g. the radios are exposed at rest
    // or the t-Addr option leaks onto the resting page).
    void d1_receiveIsPrivateByDefault() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // The disclosure machinery must have been built.
        QVERIFY2(w->btnReceiveAdvanced != nullptr,
                 "Phase-3c: the 'Other address types (advanced)' toggle must exist");
        QVERIFY2(w->receiveAdvancedPanel != nullptr,
                 "Phase-3c: the advanced disclosure panel must exist");
        QVERIFY2(w->lblReceivePrivate != nullptr,
                 "Phase-3c: the green 'Private' badge must exist");

        // Resting affordance: green Private badge SHOWN, advanced panel COLLAPSED.
        QVERIFY2(!w->lblReceivePrivate->isHidden(),
                 "at rest the green 'Private' badge must be visible");
        QVERIFY2(w->lblReceivePrivate->text().contains("Private", Qt::CaseSensitive),
                 "the badge text must read 'Private'");
        QVERIFY2(w->receiveAdvancedPanel->isHidden(),
                 "at rest the advanced (t-Addr/Sprout) panel must be COLLAPSED/hidden");
        QVERIFY2(!w->btnReceiveAdvanced->isChecked(),
                 "the advanced disclosure must default to COLLAPSED");

        // The transparent option must NOT be reachable on the resting page. It is
        // a child of the advanced panel (structural sanity), and that panel is
        // COLLAPSED (asserted above via the panel's own hidden flag), so t-Addr is
        // not effectively visible to the user. We test the panel's explicit flag,
        // not rdioTAddr->isVisible(), because the whole Receive page sits on a
        // non-current tab where every child's effective isVisible() is false
        // regardless — see the D-series note.
        QVERIFY2(ui->rdioTAddr->parentWidget() == w->receiveAdvancedPanel,
                 "rdioTAddr must be reparented INTO the advanced panel");
        QVERIFY2(w->receiveAdvancedPanel->isHidden(),
                 "the t-Addr option is unreachable at rest (its panel is collapsed)");
        // The Sapling radio is the hidden private default, not a visible choice.
        QVERIFY2(ui->rdioZSAddr->isHidden(),
                 "the Sapling radio is the hidden private default, not a visible choice");

        // No scary caption at rest — receiving is private, full stop.
        QVERIFY2(ui->lblSproutWarning->isHidden(),
                 "no PUBLIC/Sprout caption may be shown on the private resting page");

        delete w;
    }

    // ---- D2: expanding the disclosure reveals t-Addr -> PUBLIC caption -----
    // Driving the disclosure exactly as a user would: expand it, then select the
    // transparent option, and assert the red PUBLIC caption (reusing
    // lblSproutWarning) with the correct copy. FAILS if expanding stops revealing
    // t-Addr, or if selecting t-Addr stops warning that it is PUBLIC.
    void d2_advancedRevealsTransparentPublicWarning() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // Expand "Other address types (advanced)" via its public toggle, the same
        // signal a user click emits.
        w->btnReceiveAdvanced->setChecked(true);
        QVERIFY2(!w->receiveAdvancedPanel->isHidden(),
                 "expanding the disclosure must REVEAL the advanced panel");
        QVERIFY2(!ui->rdioTAddr->isHidden(),
                 "expanding the disclosure must reveal the transparent (t-Addr) option");
        // The green private badge steps aside while in advanced mode.
        QVERIFY2(w->lblReceivePrivate->isHidden(),
                 "the 'Private' badge must hide while the advanced panel is open");

        // Now SELECT the transparent option -> the red PUBLIC caption appears.
        ui->rdioTAddr->setChecked(true);
        QVERIFY2(!ui->lblSproutWarning->isHidden(),
                 "selecting t-Addr must reveal the PUBLIC warning caption");

        const QString t = ui->lblSproutWarning->text();
        QVERIFY2(t.contains("Transparent address.", Qt::CaseSensitive),
                 qPrintable("warning text missing 'Transparent address.': " + t));
        QVERIFY2(t.contains("PUBLIC", Qt::CaseSensitive),
                 qPrintable("warning text missing 'PUBLIC': " + t));
        QVERIFY2(t.contains("permanently visible", Qt::CaseSensitive),
                 qPrintable("warning text missing 'permanently visible': " + t));

        delete w;
    }

    // ---- D3: under legacy-Sprout the New-Address button is DISABLED --------
    // The disclosure must never let a fresh deprecated Sprout address be minted:
    // selecting the (read-only) legacy-Sprout radio DISABLES the new-address
    // button, and the private Sapling default re-enables it. FAILS if the Sprout
    // backdoor (new-address-while-Sprout) is reintroduced.
    void d3_sproutDisablesNewAddressButton() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // rdioZAddr (legacy Sprout) is the read-only Sprout view inside the
        // disclosure. Selecting it must disable new-address creation.
        ui->rdioZAddr->setChecked(true);
        QVERIFY2(!ui->btnRecieveNewAddr->isEnabled(),
                 "legacy Sprout (rdioZAddr) must DISABLE the new-address button (backdoor removed)");

        // Returning to the private Sapling default re-enables it.
        ui->rdioZSAddr->setChecked(true);
        QVERIFY2(ui->btnRecieveNewAddr->isEnabled(),
                 "the private Sapling default (rdioZSAddr) must re-enable the new-address button");

        delete w;
    }

    // ---- D4: collapsing the disclosure RETURNS TO PRIVATE ------------------
    // The disclosure is also a "go back to private" control: after showing the
    // transparent PUBLIC caption, collapsing the disclosure must clear the caption,
    // restore the green Private badge, and hide the panel. FAILS if collapsing
    // leaves the page resting on a transparent/PUBLIC selection.
    void d4_collapsingReturnsToPrivate() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // Expand + select transparent so the PUBLIC caption is up and the badge is
        // hidden (the non-private state).
        w->btnReceiveAdvanced->setChecked(true);
        ui->rdioTAddr->setChecked(true);
        QVERIFY(!ui->lblSproutWarning->isHidden());
        QVERIFY(w->lblReceivePrivate->isHidden());

        // Collapse the disclosure -> back to private.
        w->btnReceiveAdvanced->setChecked(false);
        QVERIFY2(w->receiveAdvancedPanel->isHidden(),
                 "collapsing must hide the advanced panel");
        QVERIFY2(ui->lblSproutWarning->isHidden(),
                 "collapsing back to private must CLEAR the PUBLIC caption");
        QVERIFY2(!w->lblReceivePrivate->isHidden(),
                 "collapsing back to private must restore the green 'Private' badge");
        QVERIFY2(ui->rdioZSAddr->isChecked(),
                 "collapsing must re-select the private Sapling default");

        delete w;
    }

    // ---- D5: selecting Sapling CLEARS the transparent warning -------------
    // Independent of the disclosure arrow: directly switching the underlying state
    // machine from t-Addr to shielded Sapling must clear the PUBLIC caption (no
    // scary caption on a private address). FAILS if Sapling stops clearing it.
    void d5_saplingClearsTransparentWarning() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // Show the transparent PUBLIC warning first.
        ui->rdioTAddr->setChecked(true);
        QVERIFY(!ui->lblSproutWarning->isHidden());

        // Switching to shielded Sapling must clear it.
        ui->rdioZSAddr->setChecked(true);
        QVERIFY2(ui->lblSproutWarning->isHidden(),
                 "switching t-Addr -> Sapling must HIDE the warning label");

        delete w;
    }

#ifdef ZCL_WIDGET_TEST
    // ---- E2: confirmTx with a t-recipient reveals publicWarning -----------
    // confirmTx() is private and blocks on d.exec(); the test-only public seam
    // MainWindow::testConfirmTx (guarded by ZCL_WIDGET_TEST) lets us call it,
    // and a QTimer::singleShot(0) modal-dismisser reads the confirm dialog's
    // publicWarning visibility BEFORE closing the dialog, then injects the
    // answer (reject) so exec() returns without a deadlock.
    void e2_publicWarningVisibleForTransparentRecipient() {
        MainWindow* w = makeWindow();

        // confirmTx dereferences rpc->getAllBalances()->value(fromAddr); give the
        // RPC a non-null balances map via the test seam so it never crashes.
        w->testSeedBalances();

        // A genuine checksum-valid mainnet t-address (base58check, ver 0x1CB8) and
        // a genuine bech32 Sapling zs-address; both must pass Settings::
        // isValidAddress so isTAddress()/the de-shield decision exercise their
        // FULL path (regex + checksum), not the early-out.
        const QString validT  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString validZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        // ---- t-recipient -> publicWarning MUST be visible -----------------
        {
            Tx tx;
            tx.fromAddr = validZS;
            tx.fee = 0.0001;
            ToFields f;
            f.addr = validT;        // transparent recipient => de-shield/public
            f.amount = 1.0;
            tx.toAddrs.append(f);

            int captured = -1;          // 1 = visible, 0 = hidden, -1 = not seen
            armConfirmDismisser(&captured);
            w->testConfirmTx(tx);
            QVERIFY2(captured != -1, "confirm dialog never appeared (t-recipient)");
            QVERIFY2(captured == 1,
                     "publicWarning must be VISIBLE for a transparent recipient");
        }

        // ---- all-shielded -> publicWarning MUST stay hidden ---------------
        {
            Tx tx;
            tx.fromAddr = validZS;
            tx.fee = 0.0001;
            ToFields f;
            f.addr = validZS;       // shielded recipient => stays private
            f.amount = 1.0;
            f.txtMemo = "";
            tx.toAddrs.append(f);

            int captured = -1;
            armConfirmDismisser(&captured);
            w->testConfirmTx(tx);
            QVERIFY2(captured != -1, "confirm dialog never appeared (all-shielded)");
            QVERIFY2(captured == 0,
                     "publicWarning must stay HIDDEN for an all-shielded send");
        }

        delete w;
    }

    // ---- F3: permanent silence after backup -------------------------------
    // options/walletbackedup == true => promptWalletBackup() returns BEFORE it
    // ever constructs/exec()s the modal nag (mainwindow.cpp:1159-1160). The nag
    // is shown via box.exec() (a BLOCKING nested loop): had it fired, this call
    // would not return synchronously and there would be an active modal widget
    // afterward. We assert it DOES return and leaves NO modal up — without
    // spinning the event loop (which would wake the RPC's deferred
    // loadConnection chain), so the test stays daemon-free and crash-free.
    void f3_backupNagSilentAfterBackup() {
        MainWindow* w = makeWindow();

        QSettings().setValue("options/walletbackedup", true);

        w->promptWalletBackup();     // returns immediately iff it shows no nag

        QVERIFY2(QApplication::activeModalWidget() == nullptr,
                 "promptWalletBackup() must show NO nag once options/walletbackedup is set");

        QSettings().setValue("options/walletbackedup", false);  // restore
        delete w;
    }

    // ---- P1: confirmTx shows the right four-way badge + warning per quadrant ---
    // PRIV-11/UX-12: the REAL MainWindow::classifySend drives confirmTx's privacy
    // badge ("confirmPrivacyBadge") + the red publicWarning. We capture both for all
    // four quadrants. FAILS if any quadrant mislabels (e.g. z->t not flagged red, or
    // a private z->z showing a public warning).
    void p1_confirmDialogFourWayClassification() {
        MainWindow* w = makeWindow();
        w->testSeedBalances();

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        struct Case { QString from, to, badgeContains; bool warn; SendCategory cat; };
        QList<Case> cases = {
            { ZS, ZS, "PRIVATE",   false, SendCategory::ZToZ_private   },
            { T,  ZS, "SHIELDING", false, SendCategory::TToZ_shielding },
            { ZS, T,  "DE-SHIELD", true,  SendCategory::ZToT_deshield  },
            { T,  T,  "PUBLIC",    true,  SendCategory::TToT_public    },
        };

        for (const Case& c : cases) {
            Tx tx; tx.fromAddr = c.from; tx.fee = 0.0001;
            ToFields f; f.addr = c.to; f.amount = 1.0; f.txtMemo = "";
            tx.toAddrs.append(f);

            // The REAL classifier must agree with the expected quadrant.
            QCOMPARE(int(MainWindow::classifySend(tx)), int(c.cat));

            QString badge; int warnVis = -1;
            // De-shield pops a SECOND (ack) modal after accept; for this read-only
            // capture we REJECT the confirm dialog so no ack box appears.
            armBadgeReader(&badge, &warnVis, /*accept=*/false);
            w->testConfirmTx(tx);

            QVERIFY2(warnVis != -1, qPrintable("confirm dialog never appeared for " + c.from + "->" + c.to));
            QVERIFY2(badge.contains(c.badgeContains, Qt::CaseSensitive),
                     qPrintable(QString("badge '%1' missing '%2' for %3->%4")
                                .arg(badge, c.badgeContains, c.from, c.to)));
            QCOMPARE(warnVis == 1, c.warn);
        }
        settleAndDelete(w);
    }

    // ---- P2: ONLY z->t (de-shield) adds the acknowledgement gate (PRIV-12) ------
    // After the user ACCEPTS the confirm dialog, a z->t send must pop a SECOND
    // "De-shield to a public address?" ack (default No) before confirmTx returns
    // true; the other three categories must NOT add that friction. We accept the
    // confirm dialog and detect whether a follow-up ack modal appears.
    void p2_deshieldRequiresAcknowledgement() {
        MainWindow* w = makeWindow();
        w->testSeedBalances();

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        // z->t : ack REQUIRED. Accept confirm, then DECLINE the ack -> confirmTx must
        // return false (the send is blocked by the missing acknowledgement).
        {
            Tx tx; tx.fromAddr = ZS; tx.fee = 0.0001;
            ToFields f; f.addr = T; f.amount = 1.0; tx.toAddrs.append(f);
            armAckHandler(/*ackAnswer=*/QMessageBox::No);
            bool ret = w->testConfirmTx(tx);
            QVERIFY2(_ackSeen, "z->t de-shield MUST pop an acknowledgement after accept");
            QVERIFY2(!ret, "declining the de-shield ack MUST block the send (confirmTx==false)");
        }

        // z->t : accept confirm AND accept the ack -> confirmTx returns true.
        {
            Tx tx; tx.fromAddr = ZS; tx.fee = 0.0001;
            ToFields f; f.addr = T; f.amount = 1.0; tx.toAddrs.append(f);
            armAckHandler(/*ackAnswer=*/QMessageBox::Yes);
            bool ret = w->testConfirmTx(tx);
            QVERIFY2(_ackSeen, "z->t de-shield ack must appear");
            QVERIFY2(ret, "accepting the de-shield ack must let the send proceed (confirmTx==true)");
        }

        // z->z private : NO ack. Accept confirm -> returns true with NO second modal.
        {
            Tx tx; tx.fromAddr = ZS; tx.fee = 0.0001;
            ToFields f; f.addr = ZS; f.amount = 1.0; f.txtMemo = ""; tx.toAddrs.append(f);
            armAckHandler(/*ackAnswer=*/QMessageBox::No);
            bool ret = w->testConfirmTx(tx);
            QVERIFY2(!_ackSeen, "z->z private MUST NOT add a de-shield acknowledgement");
            QVERIFY2(ret, "z->z private accept must proceed without extra friction");
        }

        // t->t public : NO ack either (it is public, but not a de-shield).
        {
            Tx tx; tx.fromAddr = T; tx.fee = 0.0001;
            ToFields f; f.addr = T; f.amount = 1.0; tx.toAddrs.append(f);
            armAckHandler(/*ackAnswer=*/QMessageBox::No);
            bool ret = w->testConfirmTx(tx);
            QVERIFY2(!_ackSeen, "t->t public MUST NOT add a de-shield acknowledgement");
            QVERIFY2(ret, "t->t public accept must proceed without the de-shield gate");
        }

        settleAndDelete(w);
    }

    // ---- P3: PRIV-10 auto-shield change is routed to a Sapling z-address --------
    // With auto-shield ON, a transparent FROM, and a Sapling z-address available,
    // createTxFromSendPage() MUST append a CHANGE output to the Sapling address (so
    // change is never silently left transparent). We drive the real send page.
    void p3_autoShieldRoutesChangeToSapling() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QSettings().setValue("options/autoshield", true);   // PRIV-9 default, explicit here
        QSettings().sync();

        // Seed: t-from holds 10 ZCL, and a Sapling z-address exists as the change sink.
        auto* bals = new QMap<QString, double>();
        (*bals)[T] = 10.0;
        w->getRPC()->testSetBalances(bals);
        // MAJOR-2: the change is now based on the CONFIRMED, spendable UTXO total (not
        // getAllBalances(), which includes unconfirmed). Seed a confirmed UTXO so the
        // confirmed basis covers the send + fee.
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "10.0", /*conf=*/10, /*spendable=*/true });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(ZS);
        w->getRPC()->testSetZAddresses(zs);

        // From = the transparent address; recipient = a Sapling z-addr (so the only
        // non-recipient Sapling addr is ZS, free to be the change sink); send 3 ZCL.
        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 10.0);
        ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText("zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s");
        ui->Amount1->setText("3");

        Tx tx = w->testCreateTxFromSendPage();

        // There must be a change output to the Sapling z-address ZS (10 - 3 - fee).
        bool changeToSapling = false;
        for (const auto& to : tx.toAddrs) {
            if (to.addr == ZS) {
                changeToSapling = true;
                QVERIFY2(to.amount > 6.9 && to.amount < 7.0,
                         qPrintable(QString("change amount unexpected: %1").arg(to.amount)));
            }
        }
        QVERIFY2(changeToSapling,
                 "PRIV-10: transparent change MUST be routed to a Sapling z-address, "
                 "never left transparent");

        // And NO change output may be a transparent address.
        for (const auto& to : tx.toAddrs)
            QVERIFY2(!(to.addr == T),
                     "PRIV-10: change must NEVER be routed back to a transparent address");

        QSettings().remove("options/autoshield");
        QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P4: PRIV-18 Home "Shield public funds" initiates a real t->z shield ----
    // The fix-it button now drives shieldPublicFunds(): pick the largest t-source as
    // From, fill the default Sapling z-address as the recipient, check Max, navigate
    // to Send. FAILS if it regresses to a bare navigate (recipient left empty).
    void p4_shieldPublicFundsSetsUpRealShield() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        auto* bals = new QMap<QString, double>();
        (*bals)[T] = 5.0;
        w->getRPC()->testSetBalances(bals);
        auto* zs = new QList<QString>(); zs->append(ZS);
        w->getRPC()->testSetZAddresses(zs);
        auto* ts = new QList<QString>(); ts->append(T);
        w->getRPC()->testSetTAddresses(ts);
        // MINOR-1: shieldPublicFunds() now mirrors ensureSaplingProvisioned()'s
        // readiness guard (connection up + z-addresses loaded). Install a sentinel
        // Connection so the guard passes in-process (it is never actually USED here:
        // the seeded Sapling z-address resolves before any RPC).
        installSentinelConnection(w);

        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 5.0);

        w->testShieldPublicFunds();

        // Real shield: recipient is the default Sapling z-address, From is the
        // transparent source, Max is checked, and we are on the Send page.
        QCOMPARE(ui->Address1->text(), ZS);
        QVERIFY2(ui->inputsCombo->currentText().startsWith(T),
                 "shield From must be the transparent source");
        QVERIFY2(ui->Max1->isChecked(), "shield must check Max (send the whole t-balance)");
        QCOMPARE(ui->tabWidget->currentIndex(), 1);   // Send page

        settleAndDelete(w);
    }

    // ---- P5: capture a confirm-dialog screenshot for EACH classification --------
    // PRIV-11/UX-12 visual evidence. Drives the REAL confirmTx() for all four
    // quadrants and grabs the dialog (QWidget::grab) to a PNG showing the badge text
    // + the appropriate warning. Output dir overridable via ZCL_SHOTS_DIR (the build
    // wrapper binds /build/shots). This is a capture aid, not a strict assertion, but
    // it still verifies the dialog renders for each category.
    void p5_screenshotFourWayConfirm() {
        MainWindow* w = makeWindow();
        w->testSeedBalances();

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QString dir = qEnvironmentVariable("ZCL_SHOTS_DIR", "/build/shots");
        QDir().mkpath(dir);

        struct Shot { QString from, to, file; };
        QList<Shot> shots = {
            { ZS, ZS, "confirm_zz_private.png"   },
            { T,  ZS, "confirm_tz_shielding.png" },
            { ZS, T,  "confirm_zt_deshield.png"  },
            { T,  T,  "confirm_tt_public.png"    },
        };

        for (const Shot& s : shots) {
            Tx tx; tx.fromAddr = s.from; tx.fee = 0.0001;
            ToFields f; f.addr = s.to; f.amount = 1.0; f.txtMemo = "";
            tx.toAddrs.append(f);

            QString path = QDir(dir).filePath(s.file);
            bool grabbed = armGrabber(path);
            Q_UNUSED(grabbed);
            w->testConfirmTx(tx);

            QVERIFY2(QFile::exists(path),
                     qPrintable("confirm screenshot not written: " + path));
        }
        settleAndDelete(w);
    }

    // ---- P6: PRIV-10/19/28 last-resort create — fail-OPEN vs fail-CLOSED --------
    // The reviewer proved the suite passed even if createSaplingAddressSync() always
    // returned "" (the change silently stayed PUBLIC). This test pins BOTH halves of
    // the last-resort path, driving the REAL createTxFromSendPage() with auto-shield
    // ON, a transparent FROM, and NO Sapling z-address present (so the eager-provision
    // path is exhausted and the synchronous create is the only route):
    //   (a) when the newZaddr(true) stub yields a VALID zs-address, the change routes
    //       to THAT freshly-created Sapling address and NEVER to a t-address;
    //   (b) when the stub FAILS (returns ""), the send FAILS CLOSED — createTx returns
    //       an INVALID Tx (empty fromAddr) and NO transparent-change output is built.
    // Goes RED if MAJOR-1 (fail-closed) regresses to the old silent fail-open.
    void p6_autoShieldLastResortFailClosed() {
        const QString T       = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString FRESH_ZS= "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString RECIP_ZS= "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        auto seed = [&](MainWindow* w) {
            auto* ui = w->ui;
            // getAllBalances() reports 10 ZCL (it SUMS unconfirmed), but only 8 ZCL is
            // CONFIRMED + spendable; the other 2 is an unconfirmed (conf==0) utxo.
            // MAJOR-2: the change MUST be based on the 8 confirmed (= 8 - 3 - fee), NOT
            // on the 10 reported by getAllBalances(). The assertion below pins this.
            auto* bals = new QMap<QString, double>(); (*bals)[T] = 10.0;
            w->getRPC()->testSetBalances(bals);
            auto* utxos = new QList<UnspentOutput>();
            utxos->append(UnspentOutput{ T, "txid0", "8.0", /*conf=*/10, /*spendable=*/true });
            utxos->append(UnspentOutput{ T, "txid1", "2.0", /*conf=*/0,  /*spendable=*/true });
            w->getRPC()->testSetUTXOs(utxos);
            // NO Sapling z-address present -> findUnusedSaplingChangeAddr() returns ""
            // and the synchronous create is the only path to a change sink.
            w->getRPC()->testSetZAddresses(new QList<QString>());

            ui->inputsCombo->clear();
            ui->inputsCombo->addItem(T, 10.0);
            ui->inputsCombo->setCurrentIndex(0);
            ui->Address1->setText(RECIP_ZS);   // Sapling recipient (not Sprout)
            ui->Amount1->setText("3");
        };

        // (a) FAIL-OPEN path: the create SUCCEEDS -> change routes to the fresh zs addr.
        {
            MainWindow* w = makeWindow();
            seed(w);
            w->getRPC()->testSetNextZaddrResult(FRESH_ZS);   // create returns a valid zs
            // Arm a dismisser so that IF the create were (wrongly) to fail -- e.g. the
            // reviewer's mutation where createSaplingAddressSync() always returns "" --
            // the fail-closed warning is dismissed and this case goes cleanly RED on the
            // assertion below (a non-empty fromAddr) instead of hanging on the modal.
            armModalDismisser();

            Tx tx = w->testCreateTxFromSendPage();

            QVERIFY2(!tx.fromAddr.isEmpty(),
                     "valid create: the send must NOT abort (regression: "
                     "createSaplingAddressSync() returning \"\" makes this RED)");
            bool changeToFresh = false;
            for (const auto& to : tx.toAddrs) {
                if (to.addr == FRESH_ZS) {
                    changeToFresh = true;
                    // MAJOR-2: change = CONFIRMED(8) - amount(3) - fee(0.0001) ~= 4.9999.
                    // If this were (wrongly) based on getAllBalances()'s 10, it'd be ~7.0,
                    // emitting an UNFUNDABLE change the daemon would reject.
                    QVERIFY2(to.amount > 4.9 && to.amount < 5.0,
                             qPrintable(QString("MAJOR-2: change must be based on CONFIRMED "
                                                "balance (~4.9999), got: %1").arg(to.amount)));
                }
                QVERIFY2(to.addr != T,
                         "change must NEVER be routed back to a transparent address");
            }
            QVERIFY2(changeToFresh,
                     "PRIV-10: change MUST route to the freshly-created Sapling address");
            settleAndDelete(w);
        }

        // (b) FAIL-CLOSED path: the create FAILS ("") -> the send aborts, NO transparent
        //     change is built. A blocking warning would pop; dismiss any stray modal.
        {
            MainWindow* w = makeWindow();
            seed(w);
            // Do NOT install a next-zaddr result -> newZaddr(true) delivers nothing,
            // createSaplingAddressSync() returns "" -> fail-closed.
            armModalDismisser();

            Tx tx = w->testCreateTxFromSendPage();

            QVERIFY2(tx.fromAddr.isEmpty(),
                     "MAJOR-1 fail-closed: a failed Sapling create MUST abort the send "
                     "(invalid Tx), never proceed with transparent change");
            for (const auto& to : tx.toAddrs)
                QVERIFY2(to.addr != T,
                         "MAJOR-1 fail-closed: NO transparent-change output may be built");
            settleAndDelete(w);
        }

        QSettings().remove("options/autoshield");
        QSettings().sync();
    }

    // ---- P7: PRIV-27 — Sprout recipient change-warning shown once, then silenced --
    // Auto-shield ON, transparent FROM, ONE legacy-Sprout (zc...) recipient: the
    // change cannot be shielded (turnstile forbids Sprout+Sapling mixing), so the
    // "change cannot be shielded for this send" info box is shown EXACTLY ONCE and
    // suppressed on repeat (the sproutChangeWarned one-shot). FAILS if the warning
    // is silently dropped or nags on every build.
    void p7_sproutRecipientChangeWarnOnce() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZC = "zcEgrceTwvoiFdEvPWcsJHAMrpLsprMF6aRJiQa3fan5ZphyXLPuHghnEPrEPRoEVzUy65GnMVyCTRdkT6BYBepnXh6NBYs";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        auto* bals = new QMap<QString, double>(); (*bals)[T] = 10.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "10.0", 10, true });
        w->getRPC()->testSetUTXOs(utxos);
        w->getRPC()->testSetZAddresses(new QList<QString>());

        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 10.0);
        ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(ZC);    // legacy Sprout recipient
        ui->Amount1->setText("3");

        // First build: the Sprout change info box MUST appear exactly once.
        _infoSeen = 0;
        armInfoCounter();
        (void)w->testCreateTxFromSendPage();
        QVERIFY2(_infoSeen == 1,
                 qPrintable(QString("PRIV-27: Sprout-change info box must show ONCE on "
                                    "first build, saw %1").arg(_infoSeen)));

        // Second build (same window/run): suppressed by the one-shot guard.
        _infoSeen = 0;
        armInfoCounter();
        (void)w->testCreateTxFromSendPage();
        QVERIFY2(_infoSeen == 0,
                 qPrintable(QString("PRIV-27: Sprout-change info box must be SUPPRESSED "
                                    "on repeat, saw %1").arg(_infoSeen)));

        QSettings().remove("options/autoshield");
        QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P9: auto-shield change EXCLUDES coinbase UTXOs + integer-zatoshi exactness ---
    // z_sendmany will not spend coinbase UTXOs in a has-change (multi-output) send
    // (consensus: bad-txns-coinbase-spend-has-transparent-outputs; coinbase goes only to a
    // single zaddr, whole value consumed). So the auto-shield change MUST be based on the
    // CONFIRMED, spendable, NON-COINBASE total only -- otherwise the change overshoots the
    // funds the daemon can actually spend and the send fails "insufficient funds". This
    // also pins the integer-zatoshi math: the change is EXACTLY 5 - 3 - 0.0001 = 1.9999
    // (199990000 zat), not a float-drifted value. FAILS if coinbase leaks into the basis
    // or the math regresses to double arithmetic.
    void p9_autoShieldExcludesCoinbase() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        // T holds 9 ZCL: 5 non-coinbase + 4 mined (coinbase). Only the 5 non-coinbase is
        // eligible to back a shielded change output.
        auto* bals = new QMap<QString, double>(); (*bals)[T] = 9.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "5.0", 10,  true, /*coinbase=*/false });
        utxos->append(UnspentOutput{ T, "txid1", "4.0", 100, true, /*coinbase=*/true  });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(ZS);
        w->getRPC()->testSetZAddresses(zs);

        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 9.0);
        ui->inputsCombo->setCurrentIndex(0);
        // Recipient is a DIFFERENT Sapling addr so ZS stays free as the change sink.
        ui->Address1->setText("zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s");
        ui->Amount1->setText("3");

        Tx tx = w->testCreateTxFromSendPage();

        QVERIFY2(!tx.fromAddr.isEmpty(),
                 "coinbase-excluded change must NOT abort: 5 non-coinbase ZCL covers 3 + fee");
        bool changeToSapling = false;
        for (const auto& to : tx.toAddrs) {
            if (to.addr == ZS) {
                changeToSapling = true;
                // Based on the 5 NON-coinbase ZCL only: 5 - 3 - 0.0001 = 1.9999.
                QVERIFY2(to.amount > 1.9 && to.amount < 2.0,
                         qPrintable(QString("change must exclude coinbase (~1.9999), got: %1")
                                    .arg(to.amount)));
                // Integer-zatoshi EXACTNESS: exactly 199990000 zat, no float drift.
                QCOMPARE((qint64)llround(to.amount * 1e8), (qint64)199990000);
            }
            QVERIFY2(to.addr != T,
                     "PRIV-10: change must NEVER be routed back to a transparent address");
        }
        QVERIFY2(changeToSapling,
                 "auto-shield change (from the non-coinbase balance) must route to Sapling");

        QSettings().remove("options/autoshield");
        QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P10: coinbase-only "shield everything" is NOT blocked (no change output) ------
    // A miner shielding a coinbase-only t-address via "Send max" sends the whole balance
    // to a single Sapling z-address with NO change -- the daemon legitimately consumes the
    // coinbase UTXOs in full to that lone zaddr. The auto-shield path must NOT abort this
    // (it has no non-coinbase change to shield) and must NOT fabricate a change output
    // (which would make the tx multi-output and the coinbase unspendable). FAILS if the
    // coinbase exclusion regresses this into a fail-closed abort or adds a stray output.
    void p10_autoShieldCoinbaseOnlyShieldEverything() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        // T holds 4 ZCL, ALL of it mined (coinbase). No non-coinbase funds exist.
        auto* bals = new QMap<QString, double>(); (*bals)[T] = 4.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txidcb", "4.0", 100, true, /*coinbase=*/true });
        w->getRPC()->testSetUTXOs(utxos);
        // Deliberately seed NO Sapling z-address: the no-change path must not need (nor
        // try to create) a change sink. If the logic wrongly treated this as change>0 it
        // would hit the fail-closed create and return an INVALID Tx -- caught below.
        w->getRPC()->testSetZAddresses(new QList<QString>());

        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 4.0);
        ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(ZS);     // single Sapling recipient
        ui->Amount1->setText("3.9999"); // send everything: 4.0 - 0.0001 fee

        Tx tx = w->testCreateTxFromSendPage();

        QVERIFY2(!tx.fromAddr.isEmpty(),
                 "coinbase-only 'shield everything' must NOT abort (no non-coinbase change "
                 "to shield; the daemon consumes the coinbase fully to the lone z-recipient)");
        QCOMPARE(tx.toAddrs.size(), 1);                // recipient only, NO change output
        QCOMPARE(tx.toAddrs[0].addr, ZS);
        for (const auto& to : tx.toAddrs)
            QVERIFY2(to.addr != T,
                     "no transparent output may be built for a coinbase shield");

        QSettings().remove("options/autoshield");
        QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P11: SAFE-RACE — the pre-send gate aborts when the eligible set diverges ------
    // The auto-shield change is sized against the GUI's cached UTXO snapshot but the
    // z_sendmany fires later (after the confirm dwell). If a confirmed non-coinbase t-UTXO
    // lands at the from-addr in that gap, the daemon would re-select a larger live set and
    // emit the surplus as PUBLIC transparent change. verifyAutoShieldUnchanged() re-polls
    // (via the injected testSetRepollUTXOs seam) right before send and MUST abort on any
    // divergence, proceed when unchanged, and be a no-op for non-auto-shield sends.
    void p11_autoShieldGateAbortsOnSnapshotRace() {
        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString RECIP = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        // Build an auto-shield-change Tx: T holds 5 ZCL (non-coinbase), send 3 to RECIP,
        // change 1.9999 -> ZS. The build stamps autoShieldGuardActive + builtEligibleZat=5e8.
        auto buildGuardedTx = [&](MainWindow* w) -> Tx {
            auto* ui = w->ui;
            auto* bals = new QMap<QString, double>(); (*bals)[T] = 5.0;
            w->getRPC()->testSetBalances(bals);
            auto* utxos = new QList<UnspentOutput>();
            utxos->append(UnspentOutput{ T, "txid0", "5.0", 10, true, /*coinbase=*/false });
            w->getRPC()->testSetUTXOs(utxos);
            auto* zs = new QList<QString>(); zs->append(ZS);
            w->getRPC()->testSetZAddresses(zs);
            ui->inputsCombo->clear();
            ui->inputsCombo->addItem(T, 5.0);
            ui->inputsCombo->setCurrentIndex(0);
            ui->Address1->setText(RECIP);
            ui->Amount1->setText("3");
            Tx tx = w->testCreateTxFromSendPage();
            return tx;
        };

        // (a) DIVERGENCE: a 2 ZCL non-coinbase UTXO confirmed at T between build and send.
        {
            MainWindow* w = makeWindow();
            Tx tx = buildGuardedTx(w);
            QVERIFY2(tx.autoShieldGuardActive, "an explicit-change auto-shield send must arm the gate");
            auto* fresh = new QList<UnspentOutput>();
            fresh->append(UnspentOutput{ T, "txid0", "5.0", 10, true, false });
            fresh->append(UnspentOutput{ T, "txid1", "2.0", 10, true, false });   // newly confirmed
            w->getRPC()->testSetRepollUTXOs(fresh);
            armModalDismisser();                          // the gate pops a blocking warning
            QVERIFY2(!w->testVerifyAutoShield(tx),
                     "a grown eligible set MUST abort the send (would leak surplus public change)");
            settleAndDelete(w);
        }

        // (b) NO DIVERGENCE: the fresh re-poll matches the build snapshot -> send proceeds.
        {
            MainWindow* w = makeWindow();
            Tx tx = buildGuardedTx(w);
            auto* fresh = new QList<UnspentOutput>();
            fresh->append(UnspentOutput{ T, "txid0", "5.0", 10, true, false });
            w->getRPC()->testSetRepollUTXOs(fresh);
            QVERIFY2(w->testVerifyAutoShield(tx),
                     "an unchanged eligible set MUST let the send proceed");
            settleAndDelete(w);
        }

        // (c) NO-OP: a non-auto-shield Tx (guard inactive) is never re-polled -> proceeds
        // immediately. No re-poll seam is installed; if the gate wrongly re-polled it would
        // hit the no-seam path and return false.
        {
            MainWindow* w = makeWindow();
            Tx plain; plain.fromAddr = T; plain.fee = 0.0001;
            ToFields f; f.addr = RECIP; f.amount = 1.0; plain.toAddrs.append(f);
            QVERIFY2(!plain.autoShieldGuardActive, "a hand-built Tx must default to guard-inactive");
            QVERIFY2(w->testVerifyAutoShield(plain),
                     "a non-auto-shield send must pass the gate with no re-poll");
            settleAndDelete(w);
        }

        QSettings().remove("options/autoshield");
        QSettings().sync();
    }

    // ---- P12: multi-recipient coinbase-only send aborts (daemon allows coinbase only to
    // a SINGLE zaddr). The (eligible, total] coinbase-aware guard must catch it up front
    // with the shield-mined-funds message instead of letting the daemon reject.
    void p12_multiRecipientCoinbaseAborts() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;
        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString R1 = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString R2 = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        auto* bals = new QMap<QString, double>(); (*bals)[T] = 4.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "cb0", "2.0", 100, true, /*coinbase=*/true });
        utxos->append(UnspentOutput{ T, "cb1", "2.0", 100, true, /*coinbase=*/true });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(R1);
        w->getRPC()->testSetZAddresses(zs);

        ui->inputsCombo->clear();
        ui->inputsCombo->addItem(T, 4.0);
        ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(R1);
        ui->Amount1->setText("2");
        ui->addAddressButton->click();                  // add a second recipient
        auto* addr2 = ui->sendToWidgets->findChild<QLineEdit*>("Address2");
        auto* amt2  = ui->sendToWidgets->findChild<QLineEdit*>("Amount2");
        QVERIFY2(addr2 && amt2, "second recipient (Address2/Amount2) must be created");
        addr2->setText(R2);
        amt2->setText("1.9999");                        // total 3.9999 -> needs coinbase

        armModalDismisser();
        Tx tx = w->testCreateTxFromSendPage();
        QVERIFY2(tx.fromAddr.isEmpty(),
                 "multi-recipient coinbase-only send must abort (coinbase -> single zaddr only)");

        QSettings().remove("options/autoshield");
        QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P13: changeZat==0 boundary. Exactly amount+fee non-coinbase -> NO change output
    // and the gate stays inactive; one zatoshi more -> a 1-zat Sapling change IS built and
    // the gate arms. Pins the integer boundary the float code used to get wrong.
    void p13_changeBoundaryExactZero() {
        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString R  = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true);
        QSettings().sync();

        // (a) Exact zero: eligible 3.0001 == amount 3 + fee 0.0001 -> no change, no guard.
        {
            MainWindow* w = makeWindow(); auto* ui = w->ui;
            auto* bals = new QMap<QString, double>(); (*bals)[T] = 3.0001;
            w->getRPC()->testSetBalances(bals);
            auto* utxos = new QList<UnspentOutput>();
            utxos->append(UnspentOutput{ T, "txid0", "3.0001", 10, true, false });
            w->getRPC()->testSetUTXOs(utxos);
            auto* zs = new QList<QString>(); zs->append(ZS);
            w->getRPC()->testSetZAddresses(zs);
            ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 3.0001); ui->inputsCombo->setCurrentIndex(0);
            ui->Address1->setText(R); ui->Amount1->setText("3");

            Tx tx = w->testCreateTxFromSendPage();
            QVERIFY2(!tx.fromAddr.isEmpty(), "exact-spend boundary must not abort");
            QCOMPARE(tx.toAddrs.size(), 1);                       // recipient only, NO change row
            // No change output, but the WHOLE confirmed balance is consumed -> the full-
            // consume race guard arms: a UTXO confirming during the dwell would let the daemon
            // leave a surplus as public change, so the gate re-verifies the total before send.
            QVERIFY2(tx.autoShieldGuardActive && tx.autoShieldFullConsume,
                     "exact full-consume (no change) must arm the full-consume race guard");
            settleAndDelete(w);
        }
        // (b) +1 zatoshi: eligible 3.00010001 -> a 1-zat change row is built and the gate arms.
        {
            MainWindow* w = makeWindow(); auto* ui = w->ui;
            auto* bals = new QMap<QString, double>(); (*bals)[T] = 3.00010001;
            w->getRPC()->testSetBalances(bals);
            auto* utxos = new QList<UnspentOutput>();
            utxos->append(UnspentOutput{ T, "txid0", "3.00010001", 10, true, false });
            w->getRPC()->testSetUTXOs(utxos);
            auto* zs = new QList<QString>(); zs->append(ZS);
            w->getRPC()->testSetZAddresses(zs);
            ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 3.00010001); ui->inputsCombo->setCurrentIndex(0);
            ui->Address1->setText(R); ui->Amount1->setText("3");

            Tx tx = w->testCreateTxFromSendPage();
            QVERIFY2(tx.autoShieldGuardActive, "a 1-zat change must arm the snapshot guard");
            bool changeToZS = false;
            for (const auto& to : tx.toAddrs)
                if (to.addr == ZS) { changeToZS = true; QCOMPARE((qint64)llround(to.amount * 1e8), (qint64)1); }
            QVERIFY2(changeToZS, "the 1-zat change must route to Sapling");
            settleAndDelete(w);
        }

        QSettings().remove("options/autoshield");
        QSettings().sync();
    }

    // ---- P14: custom-fee change sizing. The change and the stamped target must reflect a
    // non-default fee, in exact zatoshis (5 - 3 - 0.001 = 1.999).
    void p14_customFeeChangeSizing() {
        MainWindow* w = makeWindow(); auto* ui = w->ui;
        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString R  = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true); QSettings().sync();
        Settings::getInstance()->setAllowCustomFees(true);

        auto* bals = new QMap<QString, double>(); (*bals)[T] = 5.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "5.0", 10, true, false });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(ZS);
        w->getRPC()->testSetZAddresses(zs);
        ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 5.0); ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(R); ui->Amount1->setText("3");
        ui->minerFeeAmt->setText("0.001");              // non-default custom fee

        Tx tx = w->testCreateTxFromSendPage();
        bool ok = false;
        for (const auto& to : tx.toAddrs)
            if (to.addr == ZS) { ok = true; QCOMPARE((qint64)llround(to.amount * 1e8), (qint64)199900000); }
        QVERIFY2(ok, "change must route to Sapling and reflect the custom fee (1.999)");
        QCOMPARE(tx.builtTargetZat, (qint64)300100000);  // 3.0 + 0.001

        Settings::getInstance()->setAllowCustomFees(false);
        QSettings().remove("options/autoshield"); QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P15: coinbase-aware band message. A single-recipient PARTIAL coinbase spend
    // (target fits the confirmed total but exceeds the non-coinbase eligible, and is not a
    // full-consume shield) must abort up front with the shield-mined-funds guidance.
    void p15_coinbaseBandMessageAborts() {
        MainWindow* w = makeWindow(); auto* ui = w->ui;
        const QString T  = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
        const QString R  = "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";

        QSettings().setValue("options/autoshield", true); QSettings().sync();

        // 2 non-coinbase + 5 coinbase. Send 3: fits the 7 total, exceeds the 2 non-coinbase
        // eligible, and is NOT a full-consume single-recipient shield -> band abort.
        auto* bals = new QMap<QString, double>(); (*bals)[T] = 7.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "2.0", 10,  true, /*coinbase=*/false });
        utxos->append(UnspentOutput{ T, "cb0",   "5.0", 100, true, /*coinbase=*/true  });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(ZS);
        w->getRPC()->testSetZAddresses(zs);
        ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 7.0); ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(R); ui->Amount1->setText("3");

        armModalDismisser();
        Tx tx = w->testCreateTxFromSendPage();
        QVERIFY2(tx.fromAddr.isEmpty(),
                 "partial coinbase spend (target in (eligible, total]) must abort with the "
                 "shield-mined-funds message, not silently build a doomed tx");

        QSettings().remove("options/autoshield"); QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P16: full-consume "shield everything" is ALSO race-guarded ---------------------
    // A single-Sapling-recipient shield of the whole (mixed coinbase + non-coinbase) balance
    // builds NO change output, but is still leak-prone: if a non-coinbase UTXO confirms during
    // the dwell the daemon can fund the frozen amount from non-coinbase alone and leave the
    // surplus as PUBLIC change. The full-consume guard must arm and the gate must abort on a
    // grown total (regression guard for the reviewer-found leak in the exemption path).
    void p16_autoShieldFullConsumeRaceAborts() {
        MainWindow* w = makeWindow(); auto* ui = w->ui;
        const QString T = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString R = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QSettings().setValue("options/autoshield", true); QSettings().sync();

        // T holds 3 non-coinbase + 2 coinbase (total 5). Shield EVERYTHING to one zs recipient.
        auto* bals = new QMap<QString, double>(); (*bals)[T] = 5.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "nc0", "3.0", 10,  true, /*coinbase=*/false });
        utxos->append(UnspentOutput{ T, "cb0", "2.0", 100, true, /*coinbase=*/true  });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(R);
        w->getRPC()->testSetZAddresses(zs);
        ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 5.0); ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(R); ui->Amount1->setText("4.9999");   // whole balance minus fee

        Tx tx = w->testCreateTxFromSendPage();
        QVERIFY2(tx.autoShieldGuardActive && tx.autoShieldFullConsume,
                 "mixed full-consume shield must arm the full-consume race guard");
        QCOMPARE(tx.toAddrs.size(), 1);                            // recipient only, NO change row
        QVERIFY2(tx.builtTargetZat == (qint64)500000000, "full-consume target is the whole 5.0 ZCL");

        // (a) total GREW (a 2.5 non-coinbase confirmed during the dwell) -> MUST abort.
        {
            auto* grown = new QList<UnspentOutput>();
            grown->append(UnspentOutput{ T, "nc0", "3.0", 10,  true, false });
            grown->append(UnspentOutput{ T, "cb0", "2.0", 100, true, true  });
            grown->append(UnspentOutput{ T, "nc1", "2.5", 10,  true, false });
            w->getRPC()->testSetRepollUTXOs(grown);
            armModalDismisser();
            QVERIFY2(!w->testVerifyAutoShield(tx),
                     "a grown total in a full-consume send MUST abort (surplus would leak public change)");
        }
        // (b) total UNCHANGED -> the send still proceeds.
        {
            auto* same = new QList<UnspentOutput>();
            same->append(UnspentOutput{ T, "nc0", "3.0", 10,  true, false });
            same->append(UnspentOutput{ T, "cb0", "2.0", 100, true, true  });
            w->getRPC()->testSetRepollUTXOs(same);
            QVERIFY2(w->testVerifyAutoShield(tx), "an unchanged total => full-consume gate passes");
        }

        QSettings().remove("options/autoshield"); QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P17: exact non-coinbase consume with IDLE COINBASE is race-guarded --------------
    // changeZat==0 leg: target == the non-coinbase eligible set while coinbase sits idle
    // (target < confirmedTotal). No change row is built, but the daemon funds the non-coinbase
    // set EXACTLY (zero change) -- a clean, fundable shield, race-prone like changeZat>0: if a
    // non-coinbase UTXO confirms during the dwell, the surplus leaks as PUBLIC change. The
    // ELIGIBLE guard must arm and the gate must abort on a grown eligible set. (Regression for
    // the review-found unguarded leak leg; the prior comment wrongly claimed "nothing to guard".)
    void p17_autoShieldExactNonCoinbaseConsumeRaceGuard() {
        MainWindow* w = makeWindow(); auto* ui = w->ui;
        const QString T = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString R = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        QSettings().setValue("options/autoshield", true); QSettings().sync();

        // 5 non-coinbase + 4 coinbase. Send EXACTLY the non-coinbase 5 (4.9999 + 0.0001 fee).
        auto* bals = new QMap<QString, double>(); (*bals)[T] = 9.0;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "nc0", "5.0", 10,  true, /*coinbase=*/false });
        utxos->append(UnspentOutput{ T, "cb0", "4.0", 100, true, /*coinbase=*/true  });
        w->getRPC()->testSetUTXOs(utxos);
        auto* zs = new QList<QString>(); zs->append(R);
        w->getRPC()->testSetZAddresses(zs);
        ui->inputsCombo->clear(); ui->inputsCombo->addItem(T, 9.0); ui->inputsCombo->setCurrentIndex(0);
        ui->Address1->setText(R); ui->Amount1->setText("4.9999");

        Tx tx = w->testCreateTxFromSendPage();
        QVERIFY2(!tx.fromAddr.isEmpty(), "exact non-coinbase consume must not abort");
        QCOMPARE(tx.toAddrs.size(), 1);                       // recipient only, NO change row
        QVERIFY2(tx.autoShieldGuardActive && !tx.autoShieldFullConsume,
                 "changeZat==0 with idle coinbase must arm the ELIGIBLE guard (not full-consume)");
        QCOMPARE(tx.builtEligibleZat, (qint64)500000000);     // sized against the 5.0 non-coinbase
        for (const auto& to : tx.toAddrs)
            QVERIFY2(to.addr != T, "no transparent output for the exact-consume shield");

        // (a) eligible GREW (a 2.0 non-coinbase confirmed during the dwell) -> MUST abort.
        {
            auto* grown = new QList<UnspentOutput>();
            grown->append(UnspentOutput{ T, "nc0", "5.0", 10,  true, false });
            grown->append(UnspentOutput{ T, "cb0", "4.0", 100, true, true  });
            grown->append(UnspentOutput{ T, "nc1", "2.0", 10,  true, false });
            w->getRPC()->testSetRepollUTXOs(grown);
            armModalDismisser();
            QVERIFY2(!w->testVerifyAutoShield(tx),
                     "a grown non-coinbase eligible set MUST abort (surplus would leak public change)");
        }
        // (b) UNCHANGED -> the send proceeds.
        {
            auto* same = new QList<UnspentOutput>();
            same->append(UnspentOutput{ T, "nc0", "5.0", 10,  true, false });
            same->append(UnspentOutput{ T, "cb0", "4.0", 100, true, true  });
            w->getRPC()->testSetRepollUTXOs(same);
            QVERIFY2(w->testVerifyAutoShield(tx), "an unchanged eligible set => gate passes");
        }

        QSettings().remove("options/autoshield"); QSettings().sync();
        settleAndDelete(w);
    }

    // ---- P18: doSendTxValidations must use INTEGER zatoshis (no float false-abort) --------
    // A "send max" / shield of the whole balance makes amount+fee == balance, which floats a
    // fraction-of-a-zatoshi ABOVE the balance (0.1199 + 0.0001 -> 0.120000000002 vs a 0.12
    // balance -> 0.119999999996). The generic insufficient-funds guard must NOT false-abort
    // "Not enough funds" — the exact dead-end the user hit on the combined bundle.
    void p18_doSendTxValidationsNoFloatFalseAbort() {
        MainWindow* w = makeWindow();
        const QString T = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString R = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        auto* bals = new QMap<QString, double>(); (*bals)[T] = 0.12;
        w->getRPC()->testSetBalances(bals);
        auto* utxos = new QList<UnspentOutput>();
        utxos->append(UnspentOutput{ T, "txid0", "0.12", 56, true, /*coinbase=*/false });
        w->getRPC()->testSetUTXOs(utxos);

        Tx tx; tx.fromAddr = T; tx.fee = 0.0001;
        ToFields f; f.addr = R; f.amount = 0.1199;            // Max shield: 0.1199 + 0.0001 == 0.12
        tx.toAddrs.append(f);

        const QString err = w->doSendTxValidations(tx);
        QVERIFY2(err.isEmpty(),
                 qPrintable("shielding the whole balance (amount+fee==balance) must NOT false-abort: " + err));

        // And a genuine overspend STILL aborts (don't over-correct): send 0.13 of a 0.12 balance.
        Tx over; over.fromAddr = T; over.fee = 0.0001;
        ToFields g; g.addr = R; g.amount = 0.13; over.toAddrs.append(g);
        QVERIFY2(!w->doSendTxValidations(over).isEmpty(), "a real overspend must still be blocked");

        settleAndDelete(w);
    }

    // ---- P8: P0-2 — the at-rest "● Synced" pill ↔ loud banner swap ---------------
    // The headline visual-polish change: when fully caught up, the full-width
    // colored "Synced" bar is DEMOTED to a quiet inline pill (green lives on the
    // balance hero, not the chrome); ANY syncing/error/connecting state brings the
    // loud banner label back and hides the pill. No screenshot can prove this in the
    // daemon-less harness (the offscreen poll never reaches the synced state on its
    // own — every shot shows the gray "Connecting…" bar), so this asserts the swap
    // logic directly. isVisibleTo(w) is used (not isVisible()) so the check is
    // deterministic under the offscreen QPA: it reports the explicit show/hide state
    // relative to the window without depending on a mapped top-level surface.
    void p8_syncBannerQuietPillSwap() {
        MainWindow* w = makeWindow();
        w->ui->tabWidget->setCurrentIndex(0);   // Home/Balance page hosts the banner
        qApp->processEvents();

        QVERIFY2(w->syncQuietPill   != nullptr, "P0-2: quiet pill must be built by setupSyncBanner");
        QVERIFY2(w->syncStatusLabel != nullptr, "P0-2: loud banner label must exist");

        // SYNCED / at rest: quiet pill shown, loud label hidden.
        w->setSyncStatus(false, 1700000, 1700000, 1.0);
        qApp->processEvents();
        QVERIFY2( w->syncQuietPill->isVisibleTo(w),
                 "P0-2 synced: the quiet '● Synced' pill must be shown at rest");
        QVERIFY2(!w->syncStatusLabel->isVisibleTo(w),
                 "P0-2 synced: the loud full-width banner label must be HIDDEN at rest");

        // SYNCING: loud banner returns, quiet pill steps aside.
        w->setSyncStatus(true, 850000, 1700000, 0.5);
        qApp->processEvents();
        QVERIFY2(!w->syncQuietPill->isVisibleTo(w),
                 "P0-2 syncing: the quiet pill must hide");
        QVERIFY2( w->syncStatusLabel->isVisibleTo(w),
                 "P0-2 syncing: the loud banner label must be shown");

        // Back to SYNCED (re-entrant — a later poll catches up again): pill returns.
        w->setSyncStatus(false, 1700001, 1700001, 1.0);
        qApp->processEvents();
        QVERIFY2( w->syncQuietPill->isVisibleTo(w),
                 "P0-2 re-synced: the quiet pill must return");
        QVERIFY2(!w->syncStatusLabel->isVisibleTo(w),
                 "P0-2 re-synced: the loud banner label must hide again");

        settleAndDelete(w);
    }

    // ====================================================================
    // NOTIFY-SRV (NotifyServer) — localhost-only, token-gated push trigger.
    // Driven directly via a QLocalSocket (no daemon), pinning every security
    // invariant: CSPRNG token gate, injection-proof payload, read cap, idle
    // reap, teardown, and "trigger-not-data" response.
    // ====================================================================
    void n1_listensWithCsprngToken() {
        NotifyServer srv;
        const QString p = notifySockPath("n1");
        QVERIFY2(srv.start(p), "NotifyServer must start listening on the unix socket");
        QVERIFY(srv.isListening());
        QRegularExpression hex64("^[0-9a-f]{64}$");
        QVERIFY2(hex64.match(srv.token()).hasMatch(),
                 qPrintable("session token not 64-hex (256-bit): " + srv.token()));
        QVERIFY2(srv.start(p), "start() must be idempotent while listening");
    }

    void n2_validNotifyEmitsOnceTriggerOnly() {
        NotifyServer srv;
        const QString p = notifySockPath("n2");
        QVERIFY(srv.start(p));
        QSignalSpy spy(&srv, &NotifyServer::notified);

        const QByteArray id(64, 'a');   // a valid 64-hex id
        QLocalSocket* c = notifySendLine(p, srv.token().toLatin1() + " " + id + "\n");
        QVERIFY2(c, "client failed to connect to the notify socket");

        // Validated notify -> notified(id) emitted exactly once.
        QVERIFY2(spy.wait(2000), "notified() not emitted for a valid token+id");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QString::fromLatin1(id));

        // The peer receives ONLY an "OK" ack — never wallet data.
        QByteArray resp;
        for (int i = 0; i < 10 && resp.isEmpty(); ++i) { c->waitForReadyRead(200); resp += c->readAll(); }
        QCOMPARE(resp, QByteArray("OK\n"));
        c->disconnectFromServer(); c->deleteLater();
    }

    void n3_wrongTokenDropped() {
        NotifyServer srv;
        const QString p = notifySockPath("n3");
        QVERIFY(srv.start(p));
        QSignalSpy spy(&srv, &NotifyServer::notified);

        const QByteArray badTok(64, 'b');   // 64-hex shape, WRONG value
        QLocalSocket* c = notifySendLine(p, badTok + " " + QByteArray(64, 'a') + "\n");
        QVERIFY(c);
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);            // no refresh on a wrong token
        c->deleteLater();
    }

    void n4_injectionPayloadRejected() {
        NotifyServer srv;
        const QString p = notifySockPath("n4");
        QVERIFY(srv.start(p));
        QSignalSpy spy(&srv, &NotifyServer::notified);

        // RIGHT token, but the id is shell-injection / wrong-length / non-hex. The
        // strict ^[0-9a-f]{64}$ gate must reject every one (no refresh fires).
        const QByteArray tok = srv.token().toLatin1();
        const QList<QByteArray> evil = {
            "$(rm -rf ~)", "abc;reboot", QByteArray(63, 'a'), QByteArray(65, 'a'),
            "ZZZZ", "../../etc/passwd", "AAAA" + QByteArray(60, 'a')   // upper-hex
        };
        for (const QByteArray& bad : evil) {
            QLocalSocket* c = notifySendLine(p, tok + " " + bad + "\n");
            QVERIFY(c);
            QTest::qWait(100);
            c->deleteLater();
        }
        QCOMPARE(spy.count(), 0);
    }

    void n5_oversizedDropped() {
        NotifyServer srv;
        const QString p = notifySockPath("n5");
        QVERIFY(srv.start(p));
        QSignalSpy spy(&srv, &NotifyServer::notified);

        // > kMaxBytes with NO newline: must be dropped, never buffered unbounded.
        QByteArray flood(NotifyServer::kMaxBytes + 1024, 'a');
        QLocalSocket* c = notifySendLine(p, flood);
        QVERIFY(c);
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);
        c->deleteLater();
    }

    void n6_absentTokenDropped() {
        NotifyServer srv;
        const QString p = notifySockPath("n6");
        QVERIFY(srv.start(p));
        QSignalSpy spy(&srv, &NotifyServer::notified);
        // A single field (id only, no token) -> rejected.
        QLocalSocket* c = notifySendLine(p, QByteArray(64, 'a') + "\n");
        QVERIFY(c);
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);
        c->deleteLater();
    }

    void n7_stopTearsDownAndInvalidates() {
        NotifyServer srv;
        const QString p = notifySockPath("n7");
        QVERIFY(srv.start(p));
        srv.stop();
        QVERIFY(!srv.isListening());
        QLocalSocket c;
        c.connectToServer(p);
        QVERIFY2(!c.waitForConnected(500), "socket must be gone after stop()");
    }

    void n8_idleConnectionReaped() {
        NotifyServer srv;
        const QString p = notifySockPath("n8");
        QVERIFY(srv.start(p));
        srv.testSetIdleMs(150);            // shorten the reap window for speed
        QSignalSpy spy(&srv, &NotifyServer::notified);

        QLocalSocket* c = new QLocalSocket();
        c->connectToServer(p);
        QVERIFY(c->waitForConnected(1000));
        // Send nothing. qWait pumps the main-thread event loop (incl. the server's
        // reaper QTimer); after the idle window the server must have closed us.
        QTest::qWait(150 + 500);
        QVERIFY2(c->state() == QLocalSocket::UnconnectedState,
                 "idle connection was not reaped by the server");
        QCOMPARE(spy.count(), 0);
        c->deleteLater();
    }

    void n9_connectorTokenFileAndGates() {
        NotifyServer srv;
        const QString p = notifySockPath("n9");
        QVERIFY(srv.start(p));

        // Token file: written 0600, and reads back EXACTLY the session token (the
        // connector reads the token from here, never from argv/ps).
        QVERIFY2(srv.writeTokenFile(), "writeTokenFile() failed");
        const QString fileTok = NotifyServer::readTokenFile();
        QCOMPARE(fileTok, srv.token());
        QFile tf(NotifyServer::defaultTokenPath());
        QVERIFY(tf.exists());
        const QFile::Permissions groupOther =
            QFileDevice::ReadGroup  | QFileDevice::WriteGroup |
            QFileDevice::ReadOther  | QFileDevice::WriteOther;
        QVERIFY2((tf.permissions() & groupOther) == 0, "token file must be 0600 (no group/other)");

        // sendNotify input gates — these return BEFORE any socket I/O, so they are
        // deterministic single-threaded:
        const QString id(64, 'a');
        QCOMPARE(NotifyServer::sendNotify(p, QString(), id),     3);   // no token provisioned
        QCOMPARE(NotifyServer::sendNotify(p, fileTok, "nothex"), 2);   // non-hex id rejected
        // Nothing listening on a fresh path -> not-connected.
        QCOMPARE(NotifyServer::sendNotify(notifySockPath("n9_nobody"), fileTok, id), 4);

        // NOTE: the full token->OK->notified round-trip is proven by n2 (async, via
        // the server's real token). sendNotify's synchronous waitForReadyRead cannot
        // be driven single-threaded in-process (it does not pump the SERVER's event
        // loop); the real cross-process connector is exercised by the L2 E2E with a
        // live daemon firing -walletnotify.

        // stop() invalidates the token on disk (connector can no longer read it).
        srv.stop();
        QVERIFY2(!QFile::exists(NotifyServer::defaultTokenPath()),
                 "stop() must remove the token file");
    }

    // ====================================================================
    // NOTIFY-SRV push WIRING (STEP 1.2c / 1.3). The launch-arg + poll-interval
    // decisions are factored into pure static helpers (RPC::buildNotifyArgs /
    // RPC::desiredPollMs) so they are asserted here with NO daemon; the debounce
    // is pinned at the QTimer-semantics level (the full RPC->refresh round trip
    // needs a live daemon and is covered by the L2 E2E).
    // ====================================================================

    // The -walletnotify/-blocknotify args wire the daemon to OUR `--notify %s`
    // connector and MUST NOT leak the token onto the command line (it lives only in
    // the 0600 token file). Assert: exactly one of each arg, each ends with
    // "--notify %s", each quotes the (possibly spaced) exe path, and NEITHER carries a
    // 64-hex token.
    void n10_notifyArgsShapeAndNoToken() {
        const QString exe = "/path with spaces/ZclWallet";
        const QStringList args = RPC::buildNotifyArgs(exe);

        int wallet = 0, block = 0;
        for (const QString& a : args) {
            if (a.startsWith("-walletnotify=")) ++wallet;
            if (a.startsWith("-blocknotify="))  ++block;
        }
        QCOMPARE(args.size(), 2);
        QCOMPARE(wallet, 1);
        QCOMPARE(block, 1);

        QRegularExpression tokenRe("[0-9a-f]{64}");
        for (const QString& a : args) {
            // Connector form: "<exe>" --notify %s  (token NEVER on the arg).
            QVERIFY2(a.endsWith("--notify %s"), qPrintable("arg must end with --notify %s: " + a));
            QVERIFY2(a.contains("\"" + exe + "\""),
                     qPrintable("exe path must be quoted (may contain spaces): " + a));
            QVERIFY2(!tokenRe.match(a).hasMatch(),
                     qPrintable("notify arg must NOT carry a 64-hex token: " + a));
        }
    }

    // The poll-interval picker: while syncing -> always quick (lively banner); synced +
    // a healthy push channel -> heartbeat only (push drives updates); synced + stale/no
    // push (foreign/headless/quiet) -> the normal 20s fallback poll.
    void n11_desiredPollMs() {
        // synced + healthy push -> heartbeat
        QCOMPARE(RPC::desiredPollMs(false, true),  RPC::kHeartbeatPollMs);
        QCOMPARE(RPC::desiredPollMs(false, true),  120 * 1000);
        // synced + stale / no push -> normal poll fallback (PERF-5 / PERF-24)
        QCOMPARE(RPC::desiredPollMs(false, false), Settings::updateSpeed);
        // syncing always polls fast, regardless of push health
        QCOMPARE(RPC::desiredPollMs(true,  false), Settings::quickUpdateSpeed);
        QCOMPARE(RPC::desiredPollMs(true,  true),  Settings::quickUpdateSpeed);
    }

    // Debounce semantics: the production debounce window is the single-shot
    // kNotifyDebounceMs, and a burst of pushes inside one window must coalesce to ONE
    // fire after the quiet point. Pin the constant + the single-shot coalescing here at
    // the QTimer level (the production timer in RPC uses exactly this shape).
    void n12_debounceCoalescesBurst() {
        QCOMPARE(RPC::kNotifyDebounceMs, 200);

        int fired = 0;
        QTimer debounce;
        debounce.setSingleShot(true);
        QObject::connect(&debounce, &QTimer::timeout, [&]() { ++fired; });

        // A burst of 5 pushes each (re)starts the single-shot timer; only the last
        // restart's window elapses -> exactly one fire.
        for (int i = 0; i < 5; ++i) {
            debounce.start(RPC::kNotifyDebounceMs);   // mirrors RPC::onNotifyPush()
            QTest::qWait(20);                          // well inside the 200ms window
        }
        QCOMPARE(fired, 0);                            // still coalescing, not yet fired
        QTest::qWait(RPC::kNotifyDebounceMs + 200);    // let the quiet window elapse
        QCOMPARE(fired, 1);                            // burst coalesced to a single fire

        // A second, separate push after the quiet window fires again (one more).
        debounce.start(RPC::kNotifyDebounceMs);
        QTest::qWait(RPC::kNotifyDebounceMs + 200);
        QCOMPARE(fired, 2);
    }

    // A NotifyServer::notified BURST (the real signal, driven over the socket) coalesces
    // to a single downstream refresh when fed through the same single-shot debounce
    // RPC wires in onNotifyPush(). This exercises the real signal -> debounce seam
    // (without needing a live RPC/daemon): N validated notifies -> 1 refresh.
    void n13_notifiedBurstCoalescesToOneRefresh() {
        NotifyServer srv;
        const QString p = notifySockPath("n13");
        QVERIFY(srv.start(p));

        int refreshes = 0;
        QTimer debounce;                               // stand-in for RPC::notifyDebounce
        debounce.setSingleShot(true);
        QObject::connect(&debounce, &QTimer::timeout, [&]() { ++refreshes; });
        // Same wiring RPC::setEZClassicd() installs: notified -> (re)start the debounce.
        QObject::connect(&srv, &NotifyServer::notified, [&](const QString&) {
            debounce.start(RPC::kNotifyDebounceMs);
        });

        const QByteArray tok = srv.token().toLatin1();
        const QByteArray id(64, 'a');
        // Fire a burst of validated notifies tightly together (block + its wallet txns).
        for (int i = 0; i < 4; ++i) {
            QLocalSocket* c = notifySendLine(p, tok + " " + id + "\n");
            QVERIFY(c);
            QTest::qWait(15);                          // inside the debounce window
            c->disconnectFromServer(); c->deleteLater();
        }
        QTest::qWait(RPC::kNotifyDebounceMs + 300);    // quiet window elapses
        QCOMPARE(refreshes, 1);                        // 4 notifies -> 1 refresh
    }

    // n14 — REAL production seam (not a stand-in): a non-headless RPC must construct
    // its NotifyServer (1.2c ctor gate), and the ACTUAL RPC::onNotifyPush() must arm
    // the ACTUAL single-shot RPC::notifyDebounce (1.3). Catches a regression that the
    // mirror-based n12/n13 cannot (e.g. ctor gate removed, debounce not (re)started).
    // makeWindow() forces headless=true (notifyServer is null there), so this builds a
    // non-headless window directly. refresh(true) on the eventual fire safely no-ops
    // (getInfoThenRefresh early-returns on conn==nullptr).
    void n14_realRpcNotifyWiring() {
        auto* S = Settings::getInstance();
        const bool savedHeadless = S->isHeadless();
        S->setUseEmbedded(false);
        S->setHeadless(false);             // non-headless -> RPC ctor builds notifyServer
        S->setSyncing(false);
        S->setPeers(8);
        S->setZClassicdVersion(2001250);
        MainWindow* w = new MainWindow(nullptr);
        w->show();
        RPC* rpc = w->getRPC();

        QVERIFY2(rpc->getNotifyServer() != nullptr,
                 "non-headless RPC must construct a NotifyServer (1.2c ctor gate)");

        // Drive the REAL onNotifyPush -> REAL notifyDebounce (single-shot, 200ms).
        rpc->testFireNotifyPush();
        QVERIFY2(rpc->testNotifyDebounceActive(),
                 "onNotifyPush() must arm the real notifyDebounce");
        QTest::qWait(RPC::kNotifyDebounceMs + 200);
        QVERIFY2(!rpc->testNotifyDebounceActive(),
                 "the debounce must be single-shot (fired once, now inactive)");

        settleAndDelete(w);
        S->setHeadless(savedHeadless);
    }

    // ---- POLISH: production-dark-theme page screenshots -------------------------
    // VISUAL POLISH pass evidence. Loads the SHIPPED res/styles/dark.qss (so the
    // shots show the PRODUCTION dark theme, not the default light Fusion look),
    // seeds funded/empty balances, drives the Home dashboard + Receive page into
    // their resting and advanced states, and grabs each page widget (QWidget::grab)
    // to a PNG. Output dir overridable via ZCL_SHOTS_DIR. Capture aid, not a strict
    // assertion (but still verifies each page renders the polished theme).
    void polish_screenshots() {
        QString dir = qEnvironmentVariable("ZCL_SHOTS_DIR", "/build/shots/polish");
        QDir().mkpath(dir);

        // ---- Home FUNDED ----------------------------------------------------
        {
            MainWindow* w = makeWindow();
            applyProductionTheme();
            auto* bals = new QMap<QString, double>();
            (*bals)["zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u"] = 123.45;
            (*bals)["t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi"] = 7.0;
            w->getRPC()->testSetBalances(bals);
            // Seed the canonical balance labels + hero exactly as rpc.cpp does, then
            // surface the fix-it card via the real updateHomeFixIt path (t>0).
            w->ui->balSheilded->setText(Settings::getZCLDisplayFormat(123.45));
            w->ui->balTransparent->setText(Settings::getZCLDisplayFormat(7.0));
            w->ui->balTotal->setText(Settings::getZCLDisplayFormat(130.45));
            w->updateHomeFixIt(7.0);
            w->ui->tabWidget->setCurrentIndex(0);
            grabPage(w->ui->tab, QDir(dir).filePath("home-funded.png"));
            settleAndDelete(w);
        }

        // ---- Home SYNCED (P0-2 evidence: the quiet "● Synced" pill at rest) -
        // The daemon-less harness never drives the synced state on its own (the
        // other home/receive shots show the gray "Connecting…" bar), so call
        // setSyncStatus(false,…) explicitly to render the at-rest quiet pill — the
        // visual proof of the headline P0-2 banner demotion (asserted in p8).
        {
            MainWindow* w = makeWindow();
            applyProductionTheme();
            auto* bals = new QMap<QString, double>();
            (*bals)["zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u"] = 123.45;
            w->getRPC()->testSetBalances(bals);
            w->ui->balSheilded->setText(Settings::getZCLDisplayFormat(123.45));
            w->ui->balTransparent->setText(Settings::getZCLDisplayFormat(0.0));
            w->ui->balTotal->setText(Settings::getZCLDisplayFormat(123.45));
            w->updateHomeFixIt(0.0);
            w->ui->tabWidget->setCurrentIndex(0);
            w->setSyncStatus(false, 1700000, 1700000, 1.0);  // caught up -> quiet pill
            grabPage(w->ui->tab, QDir(dir).filePath("home-synced.png"));
            settleAndDelete(w);
        }

        // ---- Home EMPTY -----------------------------------------------------
        {
            MainWindow* w = makeWindow();
            applyProductionTheme();
            w->getRPC()->testSetBalances(new QMap<QString, double>());
            w->ui->balSheilded->setText(Settings::getZCLDisplayFormat(0.0));
            w->ui->balTransparent->setText(Settings::getZCLDisplayFormat(0.0));
            w->ui->balTotal->setText(Settings::getZCLDisplayFormat(0.0));
            w->updateHomeFixIt(0.0);     // hides fix-it, shows empty-state helper, Receive primary
            w->ui->tabWidget->setCurrentIndex(0);
            grabPage(w->ui->tab, QDir(dir).filePath("home-empty.png"));
            settleAndDelete(w);
        }

        // ---- Receive RESTING (private-by-default) ---------------------------
        {
            MainWindow* w = makeWindow();
            applyProductionTheme();
            // Put a real address in the box + QR so the framed card has content.
            const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
            w->ui->txtRecieve->setPlainText(ZS);
            w->ui->qrcodeDisplay->setQrcodeString(ZS);
            w->btnReceiveAdvanced->setChecked(false);   // collapsed = private resting
            w->ui->tabWidget->setCurrentIndex(2);
            grabPage(w->ui->tab_3, QDir(dir).filePath("receive-resting.png"));
            settleAndDelete(w);
        }

        // ---- Receive ADVANCED (disclosure open, t-Addr selected) ------------
        {
            MainWindow* w = makeWindow();
            applyProductionTheme();
            const QString ZS = "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
            w->ui->txtRecieve->setPlainText(ZS);
            w->ui->qrcodeDisplay->setQrcodeString(ZS);
            // Switch to the Receive tab FIRST (its currentChanged handler collapses
            // the disclosure back to private), THEN expand + select transparent so
            // the advanced panel + the softened PUBLIC callout are what we capture.
            w->ui->tabWidget->setCurrentIndex(2);
            w->btnReceiveAdvanced->setChecked(true);
            w->ui->rdioTAddr->setChecked(true);     // shows the softened PUBLIC callout
            grabPage(w->ui->tab_3, QDir(dir).filePath("receive-advanced.png"));
            settleAndDelete(w);
        }

        // Verify all four landed.
        for (const QString& f : { "home-funded.png", "home-empty.png",
                                  "receive-resting.png", "receive-advanced.png" }) {
            QVERIFY2(QFile::exists(QDir(dir).filePath(f)),
                     qPrintable("polish screenshot not written: " + f));
        }
    }

    // ======================================================================
    // PERF / LATENCY HARNESS  (instant-update path)
    // ----------------------------------------------------------------------
    // perf16_modelJank  — THE GUARANTEED, fully-deterministic deliverable. Pure
    //   CPU, no daemon, no event loop: seed a synthetic LARGE wallet (~5,000
    //   address->balance entries + matching UTXOs) into the REAL RPC via the
    //   testSetBalances/testSetUTXOs seams, then drive the REAL updateUI() path
    //   N times. The production updateUI() (under ZCL_WIDGET_TEST) times each
    //   balances-model setNewData() rebuild into testBalanceModelSamplesNs(); we
    //   compute p50/p95/p100 across those samples. Soft assert (QWARN if p95 >
    //   16ms — a dropped 60Hz frame); HARD fail only if p100 > 50ms (egregious
    //   jank). Always PRINTS a single parseable "PERF perf16 ..." line so the
    //   baseline is recorded run-to-run.
    //
    //   NOTE the setNewData() fingerprint short-circuit: identical inputs early-out
    //   without a rebuild. To measure the REAL rebuild cost on every iteration we
    //   alternate between two distinct datasets (A/B) so each call sees changed
    //   data and actually does the work.
    // ---- NFT detail dialog: no-local-bytes is a TERMINAL state, never a spinner -----
    // Review fix #1/#2: an on-chain NFT whose image bytes are NOT on this computer
    // (the DEFAULT for every freshly-listed token: cachePath="") must show a neutral,
    // factual verify line — NOT a perpetual "Checking this image…" with no work in
    // flight, and NOT an instruction the dialog cannot fulfil ("Open to fetch it
    // yourself" with no fetch affordance). We construct the dialog with a real engine
    // and a no-bytes item, then assert the verify line settled to the terminal copy.
    void nftDetail_noBytesIsTerminalNotSpinner() {
        ContentEngine engine(nullptr);   // a real engine; null model is fine (token path)

        NFTItem it;
        it.name       = "Orphan";
        it.collection = "Strays";
        it.txid       = "deadbeef";
        it.docHashHex = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        it.cachePath  = "";              // NO local bytes -> the no-fetch terminal path
        it.isPrivate  = false;
        it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        // rpc == nullptr: the dialog's backfill RPCs are guarded on m_rpc==nullptr, so
        // this exercises the pure poster/verify path with no daemon.
        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);

        auto* verifyLine = dlg->findChild<QLabel*>("nftDetailVerifyLine");
        QVERIFY2(verifyLine, "detail dialog must expose its verify line by objectName");

        // The no-bytes decision is synchronous (requestPoster early-returns), so the
        // verify line is already terminal — but pump a few cycles to be safe.
        QElapsedTimer t; t.start();
        while (verifyLine->text().contains("Checking", Qt::CaseInsensitive)
               && t.elapsed() < 2000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);

        const QString line = verifyLine->text();
        // (a) NOT a perpetual spinner.
        QVERIFY2(!line.contains("Checking", Qt::CaseInsensitive),
                 qPrintable("verify line stuck spinning: " + line));
        // (b) NOT a dead-end instruction (no "fetch it yourself" call-to-action with
        //     no affordance) — it states a FACT.
        QVERIFY2(!line.contains("fetch it yourself", Qt::CaseInsensitive),
                 qPrintable("verify line gives an impossible instruction: " + line));
        // (c) It's the honest neutral "can't check — not on this computer" terminal.
        QVERIFY2(line.contains("Can't check", Qt::CaseInsensitive)
                     && line.contains("isn't on this computer", Qt::CaseInsensitive),
                 qPrintable("verify line is not the terminal no-bytes copy: " + line));

        // The single state-aware verify button (objectName nftVerifyFileButton, formerly
        // the Re-check/attach pair). With NO local bytes it must NOT offer a dead "Re-check"
        // (there is nothing to re-check) — it flips to the actionable "Check my file…" path
        // (the user picks the local file they hold; nothing is uploaded). This test item
        // HAS an on-chain fingerprint (it.docHashHex set), so that path is the ONLY route to
        // a green badge and the button is enabled; a hash-less NFT would disable it instead.
        auto* verify = dlg->findChild<QPushButton*>("nftVerifyFileButton");
        QVERIFY2(verify, "detail dialog must expose the verify button by objectName");
        QVERIFY2(verify->text().contains("Check my file", Qt::CaseInsensitive),
                 qPrintable("no-bytes verify button must offer 'Check my file…', not a dead "
                            "re-check: " + verify->text()));
        QVERIFY2(!verify->text().contains("Re-check", Qt::CaseInsensitive),
                 qPrintable("no-bytes state must NOT label the button 'Re-check' (nothing to "
                            "re-check): " + verify->text()));
        QVERIFY2(verify->isEnabled(),
                 "with an on-chain fingerprint the user CAN check their local file");

        dlg->close();   // WA_DeleteOnClose -> deleted; closing the UAF surface too
    }

    // ======================================================================
    // NFT dialog L1 tests (items A + E). Driven via the RPC test seams
    // (testSetNextMintResult/SendResult/NftError) + the detail dialog's
    // testAttachFile seam, all under ZCL_WIDGET_TEST + offscreen QPA. No daemon.
    // ----------------------------------------------------------------------
private:
    // Pump the event loop until `pred()` is true or the bound elapses. Returns the
    // final pred() value so a test can assert it (never an open-ended sleep).
    template <typename Pred>
    bool pumpUntil(Pred pred, int maxMs = 5000) {
        QElapsedTimer t; t.start();
        while (!pred() && t.elapsed() < maxMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        return pred();
    }
    // Write a real, decodable PNG of a given color; return its path + true SHA-256
    // (the bare whole-file hash — small files anchor on the whole hash). The detail
    // attach-gate + posterForToken both accept that as the on-chain anchor.
    QString writePngWithHash(const QString& dir, const QString& name,
                             const QColor& fill, QString& outHashHex) {
        const QString p = QDir(dir).filePath(name);
        QImage img(40, 40, QImage::Format_ARGB32);
        img.fill(fill);
        if (!img.save(p, "PNG")) return QString();
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        const QByteArray bytes = f.readAll(); f.close();
        outHashHex = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        return p;
    }

private slots:

    // -- MINT: Create stays disabled until name + fingerprint, hashing reaches ready
    void nftMint_createGatedUntilNameAndFingerprint() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        MainWindow* w = makeWindow();
        NftMintDialog* dlg = new NftMintDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* create = dlg->findChild<QPushButton*>("nftMintCreateButton");
        auto* name   = dlg->findChild<QLineEdit*>("nftMintNameEdit");
        auto* fp     = dlg->findChild<QLabel*>("nftMintFingerprintLabel");
        QVERIFY(create && name && fp);

        QVERIFY2(!create->isEnabled(), "Create must be disabled at open (no name, no file)");

        // Type a name -> still disabled (no fingerprint yet).
        name->setText("My collectible");
        QVERIFY2(!create->isEnabled(), "Create must stay disabled with a name but no fingerprint");

        // Drive a real file hash through the engine (the same descriptorReady the
        // dialog listens for). The dialog computes the anchor + reaches "ready".
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "a.png", Qt::blue, hashHex);
        QVERIFY(!png.isEmpty());
        // Drive the dialog's own pick path (same as a choose/drop) via the test seam.
        dlg->testPickFile(png);

        QVERIFY2(pumpUntil([&]{ return fp->text().contains("Fingerprint ready", Qt::CaseInsensitive); }),
                 qPrintable("fingerprint never reached ready: " + fp->text()));
        QVERIFY2(create->isEnabled(), "Create must be ENABLED once name + fingerprint are present");

        dlg->close();
        w->deleteLater();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- MINT: a remote-URL drop is rejected + Create stays disabled
    void nftMint_remoteUrlDropRejected() {
        ContentEngine engine(nullptr);
        MainWindow* w = makeWindow();
        NftMintDialog* dlg = new NftMintDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* create = dlg->findChild<QPushButton*>("nftMintCreateButton");
        auto* name   = dlg->findChild<QLineEdit*>("nftMintNameEdit");
        auto* fp     = dlg->findChild<QLabel*>("nftMintFingerprintLabel");
        QVERIFY(create && name && fp);
        name->setText("Has a name");   // name present so the gate hinges on the rejected file

        // A remote/URL "file" must be rejected by the privacy guard (same path a
        // non-local drop takes): isRemoteUrl(path) -> red copy, no hashing, no anchor.
        dlg->testPickFile("https://example.com/cat.png");

        QVERIFY2(fp->text().contains("local file", Qt::CaseInsensitive)
                 || fp->text().contains("web link", Qt::CaseInsensitive),
                 qPrintable("remote drop not rejected with the privacy copy: " + fp->text()));
        QVERIFY2(!create->isEnabled(), "Create must stay DISABLED after a rejected remote drop");

        dlg->close();
        w->deleteLater();
    }

    // -- MINT: success -> Create becomes Done + honest "confirming" line + NOT accepted
    void nftMint_successBecomesDoneNotAutoAccepted() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        NftMintDialog* dlg = new NftMintDialog(&engine, rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* create = dlg->findChild<QPushButton*>("nftMintCreateButton");
        auto* name   = dlg->findChild<QLineEdit*>("nftMintNameEdit");
        auto* fp     = dlg->findChild<QLabel*>("nftMintFingerprintLabel");
        auto* result = dlg->findChild<QLabel*>("nftMintResultLine");
        QVERIFY(create && name && fp && result);

        name->setText("Done test");
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "b.png", Qt::red, hashHex);
        dlg->testPickFile(png);
        QVERIFY(pumpUntil([&]{ return create->isEnabled(); }));

        // Install the success result the next mintNFT delivers, then click Create.
        rpc->testSetNextMintResult("txdeadbeef", "tokdeadbeef");
        create->click();

        QVERIFY2(pumpUntil([&]{ return create->text() == QString("Done"); }),
                 qPrintable("Create did not retire to Done: " + create->text()));
        QVERIFY2(result->text().contains("confirm", Qt::CaseInsensitive),
                 qPrintable("result line missing the honest confirming copy: " + result->text()));
        QCOMPARE(dlg->lastTxid(), QString("txdeadbeef"));
        QVERIFY2(dlg->result() != QDialog::Accepted,
                 "dialog must NOT auto-accept on success (user reads the confirmation first)");

        dlg->close();
        w->deleteLater();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- MINT: the window [X]/close is swallowed while the RPC is in flight
    void nftMint_closeSwallowedWhileInFlight() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        NftMintDialog* dlg = new NftMintDialog(&engine, rpc, w);
        dlg->show();   // NOT WA_DeleteOnClose: we assert it survives the close

        auto* create = dlg->findChild<QPushButton*>("nftMintCreateButton");
        auto* name   = dlg->findChild<QLineEdit*>("nftMintNameEdit");
        QVERIFY(create && name);
        name->setText("Inflight");
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "c.png", Qt::green, hashHex);
        dlg->testPickFile(png);
        QVERIFY(pumpUntil([&]{ return create->isEnabled(); }));

        // Install NOTHING -> the seam returns without firing either callback, so the
        // dialog stays in-flight (m_inFlight==true) indefinitely.
        create->click();
        QVERIFY2(pumpUntil([&]{ return create->text() == QString("Creating…"); }),
                 "Create should read Creating… while in flight");

        // Attempt to close: the closeEvent must be swallowed (dialog still visible).
        dlg->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QVERIFY2(dlg->isVisible(), "dialog must NOT close while the mint RPC is in flight");
        QVERIFY2(dlg->result() != QDialog::Accepted, "must not have accepted while in flight");

        delete dlg;   // test owns it (no WA_DeleteOnClose); explicit teardown
        w->deleteLater();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- SEND: 4-state recipient validation; zs rejected; Send only for a valid t-addr
    void nftSend_fourStateRecipientValidation() {
        const QString TADDR = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        const QString ZSADDR =
            "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";

        MainWindow* w = makeWindow();
        NFTItem it; it.name = "Gift"; it.txid = "tok1"; it.verifyState = 1;
        NFTSendDialog* dlg = new NFTSendDialog(it, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* rcpt   = dlg->findChild<QLineEdit*>("nftSendRecipientEdit");
        auto* status = dlg->findChild<QLabel*>("nftSendAddrStatus");
        auto* send   = dlg->findChild<QPushButton*>("nftSendButton");
        QVERIFY(rcpt && status && send);

        // (1) empty -> status clear, Send disabled.
        QVERIFY2(status->text().isEmpty(), "empty recipient should clear the status");
        QVERIFY(!send->isEnabled());

        // (2) garbage -> "doesn't look like a ZClassic address", Send disabled.
        rcpt->setText("not-an-address");
        QVERIFY2(status->text().contains("doesn't look like", Qt::CaseInsensitive)
                 || status->text().contains("ZClassic address", Qt::CaseInsensitive),
                 qPrintable("garbage addr status: " + status->text()));
        QVERIFY(!send->isEnabled());

        // (3) valid t-addr -> "public" status, Send ENABLED.
        rcpt->setText(TADDR);
        QVERIFY2(status->text().contains("public", Qt::CaseInsensitive),
                 qPrintable("valid t-addr status should mention public: " + status->text()));
        QVERIFY2(send->isEnabled(), "Send must be ENABLED for a valid t-addr (verifyState 1)");

        // (4) valid zs-addr -> private gifts coming soon, Send DISABLED.
        rcpt->setText(ZSADDR);
        QVERIFY2(status->text().contains("Private gifts", Qt::CaseInsensitive)
                 || status->text().contains("public (transparent) address", Qt::CaseInsensitive),
                 qPrintable("zs-addr should be rejected with the private-soon copy: " + status->text()));
        QVERIFY2(!send->isEnabled(), "a valid zs-addr must NOT enable Send (public path only)");

        dlg->close();
        w->deleteLater();
    }

    // -- SEND: the LOAD-BEARING mismatch guard. verifyState==2 disables Send even with
    //    a valid t-addr; verifyState==1 with the same addr allows it.
    void nftSend_mismatchDisablesSendEvenWithValidTAddr() {
        const QString TADDR = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        MainWindow* w = makeWindow();

        // MISMATCH item (verifyState==2): the red warning is present AND Send stays
        // disabled even after typing a valid t-addr.
        NFTItem bad; bad.name = "Tampered"; bad.txid = "tokbad"; bad.verifyState = 2;
        NFTSendDialog* dBad = new NFTSendDialog(bad, w->getRPC(), w);
        dBad->setAttribute(Qt::WA_DeleteOnClose);
        dBad->show();
        auto* warn = dBad->findChild<QLabel*>("nftSendMismatchWarning");
        auto* rcptBad = dBad->findChild<QLineEdit*>("nftSendRecipientEdit");
        auto* sendBad = dBad->findChild<QPushButton*>("nftSendButton");
        QVERIFY(rcptBad && sendBad);
        QVERIFY2(warn, "a verifyState==2 item must surface the red mismatch warning");
        QVERIFY2(warn->text().contains("doesn't match", Qt::CaseInsensitive),
                 qPrintable("mismatch warning copy: " + warn->text()));
        rcptBad->setText(TADDR);
        QVERIFY2(!sendBad->isEnabled(),
                 "MISMATCH: Send must stay DISABLED even with a valid t-addr (the guard)");
        dBad->close();

        // VERIFIED sibling (verifyState==1): the same valid t-addr enables Send.
        NFTItem ok; ok.name = "Good"; ok.txid = "tokok"; ok.verifyState = 1;
        NFTSendDialog* dOk = new NFTSendDialog(ok, w->getRPC(), w);
        dOk->setAttribute(Qt::WA_DeleteOnClose);
        dOk->show();
        auto* warnOk = dOk->findChild<QLabel*>("nftSendMismatchWarning");
        auto* rcptOk = dOk->findChild<QLineEdit*>("nftSendRecipientEdit");
        auto* sendOk = dOk->findChild<QPushButton*>("nftSendButton");
        QVERIFY(rcptOk && sendOk);
        QVERIFY2(!warnOk, "a verified item must NOT show the mismatch warning");
        rcptOk->setText(TADDR);
        QVERIFY2(sendOk->isEnabled(),
                 "VERIFIED: a valid t-addr must enable Send (proves the guard is mismatch-only)");
        dOk->close();

        w->deleteLater();
    }

    // -- SEND: success -> Send becomes Done + "on its way" copy + NOT auto-accepted
    void nftSend_successBecomesDone() {
        const QString TADDR = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        NFTItem it; it.name = "Sendable"; it.txid = "toksend"; it.verifyState = 1;
        NFTSendDialog* dlg = new NFTSendDialog(it, rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* rcpt = dlg->findChild<QLineEdit*>("nftSendRecipientEdit");
        auto* send = dlg->findChild<QPushButton*>("nftSendButton");
        auto* result = dlg->findChild<QLabel*>("nftSendAddrStatus");   // status reused
        QVERIFY(rcpt && send);
        rcpt->setText(TADDR);
        QVERIFY(pumpUntil([&]{ return send->isEnabled(); }));

        rpc->testSetNextSendResult("txsent");
        send->click();

        QVERIFY2(pumpUntil([&]{ return send->text() == QString("Done"); }),
                 qPrintable("Send did not retire to Done: " + send->text()));
        QVERIFY2(dlg->result() != QDialog::Accepted,
                 "send dialog must NOT auto-accept on success");
        (void)result;

        dlg->close();
        w->deleteLater();
    }

    // -- DETAIL: VERIFIED says "matches its on-chain fingerprint"
    void nftDetail_verifiedSaysMatches() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "good.png", Qt::cyan, hashHex);
        QVERIFY(!png.isEmpty());

        NFTItem it; it.name = "Verified"; it.txid = "tokv";
        it.docHashHex = hashHex; it.cachePath = png; it.isPrivate = false; it.verifyState = 1;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* line = dlg->findChild<QLabel*>("nftDetailVerifyLine");
        QVERIFY(line);
        QVERIFY2(pumpUntil([&]{ return line->text().contains("matches its on-chain fingerprint", Qt::CaseInsensitive); }),
                 qPrintable("verified verify line: " + line->text()));

        dlg->close();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- DETAIL: MISMATCH says "does NOT match"
    void nftDetail_mismatchSaysDoesNotMatch() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString realHash;
        const QString png = writePngWithHash(tmp.path(), "real.png", Qt::magenta, realHash);
        QVERIFY(!png.isEmpty());
        // docHashHex is a WRONG anchor -> the engine recomputes from the real bytes and
        // reports CE_Mismatch.
        const QString wrong = QString(64, QChar('0'));

        NFTItem it; it.name = "Tampered"; it.txid = "tokm";
        it.docHashHex = wrong; it.cachePath = png; it.isPrivate = false; it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* line = dlg->findChild<QLabel*>("nftDetailVerifyLine");
        QVERIFY(line);
        QVERIFY2(pumpUntil([&]{ return line->text().contains("does NOT match", Qt::CaseInsensitive); }),
                 qPrintable("mismatch verify line: " + line->text()));

        dlg->close();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- DETAIL item-A: attach a MATCHING file -> badge resolves VERIFIED + cache filled
    void nftDetail_attachFileVerifiesBadge() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "attach.png", Qt::yellow, hashHex);
        QVERIFY(!png.isEmpty());
        // Clean any stale blob for this key so cacheGet starts empty.
        QFile::remove(ContentEngine::blobCacheDir() + "/" + hashHex);
        QVERIFY2(ContentEngine::cacheGet(hashHex).isEmpty(), "precondition: no cached blob");

        // A RECEIVED NFT: cachePath="" (privacy), but docHashHex IS recorded.
        NFTItem it; it.name = "Received"; it.txid = "tokr";
        it.docHashHex = hashHex; it.cachePath = ""; it.isPrivate = false; it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* line   = dlg->findChild<QLabel*>("nftDetailVerifyLine");
        auto* attach = dlg->findChild<QLabel*>("nftAttachStatus");
        QVERIFY(line && attach);
        // Opens in the no-bytes terminal.
        QVERIFY(pumpUntil([&]{ return line->text().contains("Can't check", Qt::CaseInsensitive); }));

        // Drive the attach seam with the MATCHING file.
        dlg->testAttachFile(png);
        QVERIFY2(pumpUntil([&]{ return attach->text().contains("matches the on-chain fingerprint", Qt::CaseInsensitive); }),
                 qPrintable("attach status (match) copy: " + attach->text()));
        // The verify line resolves to VERIFIED, and the blob is now cached.
        QVERIFY2(pumpUntil([&]{ return line->text().contains("matches its on-chain fingerprint", Qt::CaseInsensitive); }),
                 qPrintable("verify line after attach: " + line->text()));
        QVERIFY2(!ContentEngine::cacheGet(hashHex).isEmpty(),
                 "a matching attach must cachePut the bytes (content-addressed)");

        dlg->close();
        QStandardPaths::setTestModeEnabled(false);
    }

    // -- DETAIL item-A: attach a NON-MATCHING file -> stays unverified, NOT cached
    void nftDetail_attachNonMatchStaysUnverified() {
        QTemporaryDir appDir; QVERIFY(appDir.isValid());
        qputenv("XDG_DATA_HOME", appDir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        ContentEngine engine(nullptr);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString goodHash, otherHash;
        const QString good  = writePngWithHash(tmp.path(), "good2.png",  Qt::darkGreen, goodHash);
        const QString other = writePngWithHash(tmp.path(), "other.png",  Qt::darkRed,   otherHash);
        QVERIFY(!good.isEmpty() && !other.isEmpty());
        QVERIFY(goodHash != otherHash);
        QFile::remove(ContentEngine::blobCacheDir() + "/" + goodHash);
        QVERIFY(ContentEngine::cacheGet(goodHash).isEmpty());

        // The NFT records goodHash, but the user attaches the WRONG file (other).
        NFTItem it; it.name = "Received2"; it.txid = "tokr2";
        it.docHashHex = goodHash; it.cachePath = ""; it.isPrivate = false; it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* line   = dlg->findChild<QLabel*>("nftDetailVerifyLine");
        auto* attach = dlg->findChild<QLabel*>("nftAttachStatus");
        QVERIFY(line && attach);
        QVERIFY(pumpUntil([&]{ return line->text().contains("Can't check", Qt::CaseInsensitive); }));

        dlg->testAttachFile(other);   // wrong bytes
        QVERIFY2(pumpUntil([&]{ return attach->text().contains("does NOT match", Qt::CaseInsensitive); }),
                 qPrintable("attach status (non-match) copy: " + attach->text()));
        // No cachePut happened for the NFT's real hash, and the verify line never went green.
        QVERIFY2(ContentEngine::cacheGet(goodHash).isEmpty(),
                 "a non-matching attach must NOT cache the bytes");
        QVERIFY2(!line->text().contains("matches its on-chain fingerprint", Qt::CaseInsensitive),
                 qPrintable("verify line must NOT go verified on a non-match: " + line->text()));

        dlg->close();
        QStandardPaths::setTestModeEnabled(false);
    }

    // ======================================================================
    // #119 HONESTY-FIX regression locks + NFT SELL/BUY L1 tests (PART 2).
    // All driven via the RPC test seams (testSetNextOfferResult / VerifyResult /
    // TakeResult / CancelResult / NftError) + the buy dialog's testPasteOffer seam,
    // under ZCL_WIDGET_TEST + offscreen QPA. No daemon.
    // ----------------------------------------------------------------------

    // -- #119 (b): the detail pill must NEVER claim shielded/private OWNERSHIP.
    void nftDetail_noShieldedOwnershipClaim() {
        ContentEngine engine(nullptr);
        NFTItem it; it.name = "Pub"; it.txid = "tok1";
        it.docHashHex = "aa"; it.cachePath = ""; it.isPrivate = false; it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);

        auto* pill = dlg->findChild<QLabel*>("nftDetailPrivacyPill");
        QVERIFY2(pill, "detail dialog must expose its ownership pill by objectName");
        const QString t = pill->text();
        QVERIFY2(!t.contains("shielded", Qt::CaseInsensitive),
                 qPrintable("pill must never claim shielded ownership: " + t));
        QVERIFY2(!t.contains("only you", Qt::CaseInsensitive),
                 qPrintable("pill must never claim only-you-can-see: " + t));
        QVERIFY2(!t.contains("Private", Qt::CaseInsensitive),
                 qPrintable("pill must never imply a private owner: " + t));
        QVERIFY2(t.contains("Public", Qt::CaseInsensitive)
                     && t.contains("public ledger", Qt::CaseInsensitive),
                 qPrintable("pill must state the honest public truth: " + t));
        dlg->close();
    }

    // -- #119 (b): even when an item somehow carries isPrivate==true, the pill stays
    //    honest (the dead branch was removed — it can never resurrect the lie).
    void nftDetail_pillHonestEvenIfIsPrivateTrue() {
        ContentEngine engine(nullptr);
        NFTItem it; it.name = "Legacy"; it.txid = "tok2";
        it.docHashHex = "bb"; it.cachePath = ""; it.isPrivate = true; it.verifyState = 0;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* pill = dlg->findChild<QLabel*>("nftDetailPrivacyPill");
        QVERIFY(pill);
        QVERIFY2(!pill->text().contains("shielded", Qt::CaseInsensitive),
                 qPrintable("isPrivate==true must NOT resurrect the shielded claim: " + pill->text()));
        QVERIFY2(pill->text().contains("Public", Qt::CaseInsensitive),
                 qPrintable("pill must stay Public regardless of isPrivate: " + pill->text()));
        dlg->close();
    }

    // -- #119 (a): the NFTItem default must be PUBLIC (isPrivate==false).
    void nftItem_defaultsPublic() {
        NFTItem it;
        QVERIFY2(it.isPrivate == false, "NFTItem must default to public (isPrivate==false)");
    }

    // -- #119 (d): a mismatch item's Send opens with Send HARD-disabled and there is
    //    NO "Send anyway?" override modal (the override offered an action that doesn't
    //    exist). We drive onSendGift indirectly: with the prompt removed, the Send
    //    button on the detail dialog is enabled (the *send dialog* is where the gate
    //    lives); the regression is that NO QMessageBox blocks. Since exec() on the
    //    nested NFTSendDialog would block offscreen, we assert the SOURCE invariant via
    //    the send dialog gate (proven elsewhere) + that the detail Sell/Send buttons
    //    behave honestly for a mismatch (Sell disabled).
    void nftDetail_mismatchSellDisabledNoOverride() {
        ContentEngine engine(nullptr);
        NFTItem it; it.name = "Tampered"; it.txid = "tok3";
        it.docHashHex = "cc"; it.cachePath = ""; it.isPrivate = false; it.verifyState = 2;
        QVector<NFTItem> ordered; ordered.push_back(it);

        NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* sell = dlg->findChild<QPushButton*>("nftDetailSellButton");
        QVERIFY2(sell, "detail dialog must expose the Sell button");
        QVERIFY2(!sell->isEnabled(), "Sell must be disabled for a tampered (mismatch) collectible");
        dlg->close();
    }

    // -- L1 lock on the price->zat conversion (the ONE money conversion).
    void nftSell_zclToZatConversionExact() {
        QCOMPARE(RPC::zclToZat(1.0),        (qint64)100000000);
        QCOMPARE(RPC::zclToZat(0.00000001), (qint64)1);
        QCOMPARE(RPC::zclToZat(1.23456789), (qint64)123456789);
        QCOMPARE(RPC::zclToZat(0.0),        (qint64)0);
        QCOMPARE(RPC::zclToZat(-5.0),       (qint64)0);
    }

    // -- SELL: List gated until a valid price AND a valid buyer t-address; zs rejected.
    void nftSell_listGatedUntilPriceAndBuyerAddr() {
        MainWindow* w = makeWindow();
        NFTItem it; it.name = "ForSale"; it.txid = "toks1";
        it.docHashHex = "dd"; it.isPrivate = false; it.verifyState = 1;

        NFTSellDialog* dlg = new NFTSellDialog(it, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* price = dlg->findChild<QLineEdit*>("nftSellPriceEdit");
        auto* buyer = dlg->findChild<QLineEdit*>("nftSellBuyerAddrEdit");
        auto* list  = dlg->findChild<QPushButton*>("nftSellListButton");
        auto* note  = dlg->findChild<QLabel*>("nftSellPublicNote");
        QVERIFY(price && buyer && list && note);

        // Honest public-settlement line present: the trade is public and BOTH addresses
        // (plus the price) are visible on the ledger. New copy: "This trade is public —
        // the price and both addresses are visible on the ledger."
        QVERIFY2(note->text().contains("public", Qt::CaseInsensitive)
                     && note->text().contains("addresses are visible", Qt::CaseInsensitive),
                 qPrintable("sell must state the public-settlement truth: " + note->text()));

        QVERIFY2(!list->isEnabled(), "List disabled at open (no price, no buyer)");

        price->setText("100");
        QVERIFY2(!list->isEnabled(), "List stays disabled with a price but no buyer addr");

        // A shielded address must be rejected (the daemon needs a public t-addr).
        buyer->setText("zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u");
        QVERIFY2(!list->isEnabled(), "List must reject a shielded (zs) buyer address");

        buyer->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");
        QVERIFY2(list->isEnabled(), "List enabled with a valid price + buyer t-address");

        // Clearing the price re-disables.
        price->clear();
        QVERIFY2(!list->isEnabled(), "List re-disabled when the price is cleared");
        dlg->close();
    }

    // -- SELL: a mismatch item shows a red warning + List hard-disabled.
    void nftSell_mismatchDisablesList() {
        MainWindow* w = makeWindow();
        NFTItem it; it.name = "Bad"; it.txid = "toks2";
        it.docHashHex = "ee"; it.isPrivate = false; it.verifyState = 2;

        NFTSellDialog* dlg = new NFTSellDialog(it, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* warn  = dlg->findChild<QLabel*>("nftSellMismatchWarning");
        auto* price = dlg->findChild<QLineEdit*>("nftSellPriceEdit");
        auto* buyer = dlg->findChild<QLineEdit*>("nftSellBuyerAddrEdit");
        auto* list  = dlg->findChild<QPushButton*>("nftSellListButton");
        QVERIFY2(warn, "mismatch item must show the red warning label");
        QVERIFY(price && buyer && list);

        // Even with a valid price + buyer, List stays disabled for a tampered item.
        price->setText("100");
        buyer->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");
        QVERIFY2(!list->isEnabled(), "List must stay disabled for a verifyState==2 item");
        dlg->close();
    }

    // -- SELL: a successful List shows the blob + Copy/Save/Cancel + a "Listed" badge.
    void nftSell_successShowsBlobCopySaveCancel() {
        MainWindow* w = makeWindow();
        w->getRPC()->testSetNextOfferResult("znftoffer:AAAA", "offerid01", "tok:0");

        NFTItem it; it.name = "Goods"; it.txid = "toks3";
        it.docHashHex = "ff"; it.isPrivate = false; it.verifyState = 1;
        NFTSellDialog* dlg = new NFTSellDialog(it, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* price = dlg->findChild<QLineEdit*>("nftSellPriceEdit");
        auto* buyer = dlg->findChild<QLineEdit*>("nftSellBuyerAddrEdit");
        auto* list  = dlg->findChild<QPushButton*>("nftSellListButton");
        QVERIFY(price && buyer && list);
        price->setText("100");
        buyer->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");
        QVERIFY(list->isEnabled());
        list->click();

        QVERIFY2(pumpUntil([&]{ return dlg->findChild<QPlainTextEdit*>("nftSellOfferBlob") != nullptr; }),
                 "the offer blob view must appear after a successful List");
        auto* blob   = dlg->findChild<QPlainTextEdit*>("nftSellOfferBlob");
        auto* copy   = dlg->findChild<QPushButton*>("nftSellCopyButton");
        auto* save   = dlg->findChild<QPushButton*>("nftSellSaveButton");
        auto* cancel = dlg->findChild<QPushButton*>("nftSellCancelButton");
        auto* badge  = dlg->findChild<QLabel*>("nftSellListedBadge");
        QVERIFY2(blob && copy && save && cancel && badge,
                 "listed state must expose blob + Copy + Save + Cancel + Listed badge");
        QCOMPARE(blob->toPlainText(), QString("znftoffer:AAAA"));
        QVERIFY2(badge->text().contains("Listed", Qt::CaseInsensitive),
                 qPrintable("listed badge copy: " + badge->text()));
        QCOMPARE(dlg->lastOfferId(), QString("offerid01"));
        dlg->close();
    }

    // -- BUY: a green verify renders the green verdict + enables Buy (after ack).
    void nftBuy_verifyGreenEnablesBuy() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTVerifyResult vr; vr.ok = true; vr.tokenId = "tokbuy01";
        vr.priceZat = 250000000; vr.expiryHeight = 99999;
        w->getRPC()->testSetNextVerifyResult(vr);

        NFTBuyDialog* dlg = new NFTBuyDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* verdict = dlg->findChild<QLabel*>("nftBuyVerdict");
        auto* price   = dlg->findChild<QLabel*>("nftBuyPrice");
        auto* buy     = dlg->findChild<QPushButton*>("nftBuyButton");
        auto* ack     = dlg->findChild<QCheckBox*>("nftBuyAcknowledge");
        QVERIFY(verdict && price && buy && ack);

        QVERIFY2(!buy->isEnabled(), "Buy must be disabled before any verify");
        dlg->testPasteOffer("znftoffer:GREEN");

        QVERIFY2(pumpUntil([&]{ return verdict->text().contains("Verified", Qt::CaseInsensitive); }),
                 qPrintable("verdict after green verify: " + verdict->text()));
        QVERIFY2(price->text().contains("2.5", Qt::CaseInsensitive),
                 qPrintable("price must render the ZCL amount: " + price->text()));
        // Buy still disabled until the overshoot acknowledgement is checked.
        QVERIFY2(!buy->isEnabled(), "Buy must require the overshoot acknowledgement");
        ack->setChecked(true);
        QVERIFY2(buy->isEnabled(), "Buy must enable on a green verify + ack");
        dlg->close();
    }

    // -- BUY: a failed verify shows AMBER + the reason and keeps Buy disabled (no
    //    green badge without a real ok).
    void nftBuy_verifyFailShowsAmberReasonBuyDisabled() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTVerifyResult vr; vr.ok = false; vr.tokenId = "tokbad01";
        vr.priceZat = 100000000;
        vr.reasons << "vin[0] is not a live (unspent) UTXO";
        w->getRPC()->testSetNextVerifyResult(vr);

        NFTBuyDialog* dlg = new NFTBuyDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* verdict = dlg->findChild<QLabel*>("nftBuyVerdict");
        auto* buy     = dlg->findChild<QPushButton*>("nftBuyButton");
        auto* ack     = dlg->findChild<QCheckBox*>("nftBuyAcknowledge");
        QVERIFY(verdict && buy && ack);

        dlg->testPasteOffer("znftoffer:FORGED");
        QVERIFY2(pumpUntil([&]{ return verdict->text().contains("Don't pay", Qt::CaseInsensitive); }),
                 qPrintable("amber verdict after failed verify: " + verdict->text()));
        QVERIFY2(verdict->text().contains("not a live", Qt::CaseInsensitive),
                 qPrintable("amber verdict must carry the honest reason: " + verdict->text()));
        QVERIFY2(!verdict->text().contains("Verified", Qt::CaseInsensitive),
                 "a failed verify must NEVER say Verified");
        // Even checking ack cannot enable Buy on a non-ok verify.
        ack->setChecked(true);
        QVERIFY2(!buy->isEnabled(), "Buy must stay disabled on a failed verify even with ack");
        dlg->close();
    }

    // -- BUY: editing the offer after a green verify re-disables Buy (anti-stale).
    void nftBuy_editAfterVerifyResetsGate() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTVerifyResult vr; vr.ok = true; vr.tokenId = "tokstale";
        vr.priceZat = 100000000;
        w->getRPC()->testSetNextVerifyResult(vr);

        NFTBuyDialog* dlg = new NFTBuyDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* input = dlg->findChild<QPlainTextEdit*>("nftBuyOfferInput");
        auto* buy   = dlg->findChild<QPushButton*>("nftBuyButton");
        auto* ack   = dlg->findChild<QCheckBox*>("nftBuyAcknowledge");
        QVERIFY(input && buy && ack);

        dlg->testPasteOffer("znftoffer:GREEN2");
        QVERIFY(pumpUntil([&]{ return dlg->verifyResult().ok; }));
        ack->setChecked(true);
        QVERIFY(buy->isEnabled());

        // Edit the offer -> the green verdict is invalidated, Buy re-disabled.
        input->setPlainText("znftoffer:TAMPERED");
        QVERIFY2(!buy->isEnabled(), "editing the offer after a green verify must re-disable Buy");
        QVERIFY2(!dlg->verifyResult().ok, "an edit must clear the verified flag");
        dlg->close();
    }

    // -- BUY: a successful take becomes "Done" + shows the overshoot, NOT auto-accepted.
    void nftBuy_successBecomesDoneOnItsWay() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTVerifyResult vr; vr.ok = true; vr.tokenId = "tokwin";
        vr.priceZat = 100000000;
        w->getRPC()->testSetNextVerifyResult(vr);
        w->getRPC()->testSetNextTakeResult("txwin0001", 12345);   // overshoot 12345 zat

        NFTBuyDialog* dlg = new NFTBuyDialog(&engine, w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* buy    = dlg->findChild<QPushButton*>("nftBuyButton");
        auto* ack    = dlg->findChild<QCheckBox*>("nftBuyAcknowledge");
        auto* result = dlg->findChild<QLabel*>("nftBuyResultLine");
        QVERIFY(buy && ack && result);

        dlg->testPasteOffer("znftoffer:WIN");
        QVERIFY(pumpUntil([&]{ return dlg->verifyResult().ok; }));
        ack->setChecked(true);
        QVERIFY(buy->isEnabled());
        buy->click();

        QVERIFY2(pumpUntil([&]{ return buy->text() == QString("Done"); }),
                 "Buy must become Done after a successful purchase");
        QVERIFY2(result->text().contains("on its way", Qt::CaseInsensitive),
                 qPrintable("success copy: " + result->text()));
        QVERIFY2(result->text().contains("fee", Qt::CaseInsensitive),
                 qPrintable("overshoot/fee must be shown: " + result->text()));
        QCOMPARE(dlg->lastTxid(), QString("txwin0001"));
        QCOMPARE(dlg->lastOvershootZat(), (qint64)12345);
        // NOT auto-accepted: the dialog is still visible (the user dismisses via Done).
        QVERIFY2(dlg->isVisible(), "success must NOT auto-accept the dialog");
        dlg->close();
    }

    // -- BUY: nothing installed -> perpetual in-flight -> [X] swallowed (UAF guard).
    void nftBuy_closeSwallowedWhileInFlight() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTVerifyResult vr; vr.ok = true; vr.tokenId = "tokhold";
        vr.priceZat = 100000000;
        w->getRPC()->testSetNextVerifyResult(vr);
        // Install NO take result -> nftTakeOffer returns without firing a callback.

        NFTBuyDialog* dlg = new NFTBuyDialog(&engine, w->getRPC(), w);
        // NOT WA_DeleteOnClose here: we assert the dialog survives the close attempt.
        dlg->show();

        auto* buy = dlg->findChild<QPushButton*>("nftBuyButton");
        auto* ack = dlg->findChild<QCheckBox*>("nftBuyAcknowledge");
        QVERIFY(buy && ack);
        dlg->testPasteOffer("znftoffer:HOLD");
        QVERIFY(pumpUntil([&]{ return dlg->verifyResult().ok; }));
        ack->setChecked(true);
        QVERIFY(buy->isEnabled());
        buy->click();
        QVERIFY2(pumpUntil([&]{ return buy->text() == QString("Buying…"); }),
                 "Buy must enter the in-flight Buying… state");

        dlg->close();   // must be SWALLOWED while in flight
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QVERIFY2(dlg->isVisible(), "close must be swallowed while the take RPC is in flight");
        delete dlg;     // we own it (no WA_DeleteOnClose)
    }

    // -- C-4 (DRY): the public-trade honesty sentence must be IDENTICAL in the sell
    //    and buy dialogs (both now read it from nftPublicTradeNote()). A drift here
    //    would split the translation entry and risk one dialog saying something the
    //    other doesn't. Also assert it carries the load-bearing honesty words.
    void nftCommon_publicTradeNoteIdenticalSellAndBuy() {
        MainWindow* w = makeWindow();
        ContentEngine engine(nullptr);

        NFTItem it; it.txid = "tokcommon01"; it.name = "Common"; it.verifyState = 1;
        NFTSellDialog* sell = new NFTSellDialog(it, w->getRPC(), w);
        sell->setAttribute(Qt::WA_DeleteOnClose);
        NFTBuyDialog* buy = new NFTBuyDialog(&engine, w->getRPC(), w);
        buy->setAttribute(Qt::WA_DeleteOnClose);

        auto* sellNote = sell->findChild<QLabel*>("nftSellPublicNote");
        auto* buyNote  = buy->findChild<QLabel*>("nftBuyPublicNote");
        QVERIFY(sellNote && buyNote);
        QCOMPARE(sellNote->text(), buyNote->text());   // single source -> identical
        // New copy: "This trade is public — the price and both addresses are visible on the
        // ledger." The load-bearing honesty word is "public" (the trade is not private).
        QVERIFY2(sellNote->text().contains("public", Qt::CaseInsensitive)
                     && sellNote->text().contains("visible", Qt::CaseInsensitive),
                 qPrintable("public-trade note must stay honest: " + sellNote->text()));
        sell->close();
        buy->close();
    }

    // -- C-4 (DRY): the shared 4-state t-address validator must paint the SAME copy
    //    in the send and sell dialogs for the same input (both call
    //    nftValidateTAddrInto). We drive the green (valid t-address) state and the
    //    not-an-address red state; the shielded-specific hint legitimately differs.
    void nftCommon_tAddrValidatorSharedCopy() {
        MainWindow* w = makeWindow();

        NFTItem it; it.txid = "tokcommon02"; it.name = "Common2"; it.verifyState = 1;
        NFTSendDialog* send = new NFTSendDialog(it, w->getRPC(), w);
        send->setAttribute(Qt::WA_DeleteOnClose);
        NFTSellDialog* sell = new NFTSellDialog(it, w->getRPC(), w);
        sell->setAttribute(Qt::WA_DeleteOnClose);

        auto* sendRcpt   = send->findChild<QLineEdit*>("nftSendRecipientEdit");
        auto* sendStatus = send->findChild<QLabel*>("nftSendAddrStatus");
        auto* sellBuyer  = sell->findChild<QLineEdit*>("nftSellBuyerAddrEdit");
        auto* sellStatus = sell->findChild<QLabel*>("nftSellBuyerStatus");
        QVERIFY(sendRcpt && sendStatus && sellBuyer && sellStatus);

        // Valid t-address -> identical green/amber "Looks good" copy in both dialogs.
        const QString tAddr = "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi";
        sendRcpt->setText(tAddr);
        sellBuyer->setText(tAddr);
        QCOMPARE(sendStatus->text(), sellStatus->text());
        QVERIFY2(sendStatus->text().contains("Looks good", Qt::CaseInsensitive),
                 qPrintable("valid-taddr copy: " + sendStatus->text()));

        // Garbage -> identical "doesn't look like a ZClassic address" copy.
        sendRcpt->setText("not-an-address");
        sellBuyer->setText("not-an-address");
        QCOMPARE(sendStatus->text(), sellStatus->text());
        QVERIFY2(sendStatus->text().contains("doesn't look like", Qt::CaseInsensitive),
                 qPrintable("invalid-addr copy: " + sendStatus->text()));
        send->close();
        sell->close();
    }

    // =======================================================================
    // SHIELD — private file send/receive (ZDC1 data-channel). Driven via the RPC
    // test seams (testSetNextSendDataFileResult / DataTransferResult / List) +
    // the ShieldSendDialog::testPickFile seam, under ZCL_WIDGET_TEST + offscreen.
    // The coin is ZCL; the feature makes only FILE CONTENT private.
    // =======================================================================

    static const char* SAPLING_ZS_A() {
        return "zs1gv64eu0v2wx7raxqxlmj354y9ycznwaau9kduljzczxztvs4qcl00kn2sjxtejvrxnkucw5xx9u";
    }
    static const char* SAPLING_ZS_B() {
        return "zs10m00rvkhfm4f7n23e4sxsx275r7ptnggx39ygl0vy46j9mdll5c97gl6dxgpk0njuptg2mn9w5s";
    }

    // Write a small binary file (with a NUL + high byte) and return its path.
    QString writeSmallBinary(const QString& dir, const QString& name, int bytes) {
        const QString p = QDir(dir).filePath(name);
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return QString();
        QByteArray data;
        for (int i = 0; i < bytes; ++i)
            data.append(char(i % 256));   // includes 0x00 and high bytes -> binary-safe path
        f.write(data); f.close();
        return p;
    }

    // -- SHIELD SEND: Send gated until file + recipient + consent; consent is mandatory.
    void shieldSend_consentMandatoryAndGating() {
        MainWindow* w = makeWindow();
        // Install a wallet Sapling z-addr so the From combo has a valid spending addr.
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        w->getRPC()->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        QVERIFY(to && consent && send);
        QVERIFY2(!send->isEnabled(), "Send must be disabled at open (no file/recipient/consent)");

        // Pick a valid small binary file (the From combo is already a valid Sapling addr).
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const QString file = writeSmallBinary(tmp.path(), "payload.bin", 256);
        QVERIFY(!file.isEmpty());
        dlg->testPickFile(file);

        // A valid Sapling recipient -> still disabled until consent is ticked.
        to->setText(SAPLING_ZS_B());
        QVERIFY2(!send->isEnabled(), "Send must stay DISABLED until permanence consent is ticked");

        // Tick consent -> Send enabled.
        consent->setChecked(true);
        QVERIFY2(send->isEnabled(), "Send must enable once file + recipient + consent are all present");

        // Untick consent -> disabled again (consent is not a one-way latch).
        consent->setChecked(false);
        QVERIFY2(!send->isEnabled(), "unticking consent must disable Send again");

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD SEND: a too-large file is rejected UP FRONT (no RPC), Send stays disabled.
    void shieldSend_oversizeRejectedUpFront() {
        MainWindow* w = makeWindow();
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        w->getRPC()->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(w->getRPC(), w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        auto* fileLine= dlg->findChild<QLabel*>("shieldSendFileLine");
        QVERIFY(to && consent && send && fileLine);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const QString big = writeSmallBinary(tmp.path(), "big.bin", 41000);   // > 40000 cap
        QVERIFY(!big.isEmpty());
        dlg->testPickFile(big);

        QVERIFY2(fileLine->text().contains("too large", Qt::CaseInsensitive),
                 qPrintable("oversize message: " + fileLine->text()));
        // Even with a valid recipient + consent, Send stays disabled (no hex payload set).
        to->setText(SAPLING_ZS_B());
        consent->setChecked(true);
        QVERIFY2(!send->isEnabled(), "an oversize file must keep Send disabled (rejected up front)");

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD SEND: success -> Done + fingerprint + disclosure key surfaced + NOT auto-accepted.
    void shieldSend_successShowsFingerprintAndKey() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        rpc->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        QVERIFY(to && consent && send);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const QString file = writeSmallBinary(tmp.path(), "p.bin", 100);
        dlg->testPickFile(file);
        to->setText(SAPLING_ZS_B());
        consent->setChecked(true);
        QVERIFY(pumpUntil([&]{ return send->isEnabled(); }));

        SendDataFileResult r;
        r.operationId = "opid-1";
        r.transferId  = "00112233aabbccdd";
        r.fingerprint = QString(64, QChar('a'));
        r.key         = QString(64, QChar('b'));
        r.frames      = 2;
        rpc->testSetNextSendDataFileResult(r);
        send->click();

        QVERIFY2(pumpUntil([&]{ return send->text() == QString("Done"); }),
                 qPrintable("Send did not retire to Done: " + send->text()));
        QVERIFY2(dlg->result() != QDialog::Accepted,
                 "shield send must NOT auto-accept on success");

        // The fingerprint + disclosure key fields are now present + carry the values.
        auto* fpField  = dlg->findChild<QLineEdit*>("shieldSendFingerprintField");
        auto* keyField = dlg->findChild<QLineEdit*>("shieldSendKeyField");
        QVERIFY2(fpField,  "the content fingerprint field must appear on success");
        QVERIFY2(keyField, "the disclosure key field must appear on success");
        QCOMPARE(fpField->text(), r.fingerprint);
        QCOMPARE(keyField->text(), r.key);

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD SEND: the [X]/close is swallowed while the send RPC is in flight (UAF guard).
    void shieldSend_closeSwallowedWhileInFlight() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        rpc->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        QVERIFY(to && consent && send);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        dlg->testPickFile(writeSmallBinary(tmp.path(), "p.bin", 80));
        to->setText(SAPLING_ZS_B());
        consent->setChecked(true);
        QVERIFY(pumpUntil([&]{ return send->isEnabled(); }));

        // Nothing installed -> the send RPC never resolves -> perpetual in-flight.
        send->click();
        QVERIFY2(pumpUntil([&]{ return send->text() == QString("Encrypting and sending…"); }),
                 qPrintable("Send did not enter in-flight state: " + send->text()));
        dlg->close();   // must be SWALLOWED while in flight
        QCoreApplication::processEvents();
        QVERIFY2(dlg->isVisible(), "close must be swallowed while the send RPC is in flight");
        QVERIFY2(dlg->result() != QDialog::Accepted, "must not accept while in flight");

        w->deleteLater();
    }

    // -- SHIELD SEND: channel-off (-32601) maps to the calm "enable in Settings" line.
    void shieldSend_channelOffCalmError() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        rpc->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        auto* result  = dlg->findChild<QLabel*>("shieldSendResultLine");
        QVERIFY(to && consent && send && result);

        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        dlg->testPickFile(writeSmallBinary(tmp.path(), "p.bin", 60));
        to->setText(SAPLING_ZS_B());
        consent->setChecked(true);
        QVERIFY(pumpUntil([&]{ return send->isEnabled(); }));

        // The seam fires the error cb with the calm channel-off message (as the real
        // datachannelCalmError would for -32601).
        rpc->testSetNextNftError(QStringLiteral(
            "Private file transfers are turned off on your node. Turn on “Enable private "
            "file transfers” in Settings → Privacy, then restart, to use this."));
        send->click();

        QVERIFY2(pumpUntil([&]{ return result->text().contains("turned off", Qt::CaseInsensitive)
                                       && result->text().contains("Settings", Qt::CaseInsensitive); }),
                 qPrintable("channel-off calm error: " + result->text()));
        // After an error the primary returns to a retryable state (Try again), not Done.
        QVERIFY2(send->text() == QString("Try again"),
                 qPrintable("after error Send should read Try again: " + send->text()));

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD RECEIVE: verified+complete -> success line + Save enabled.
    void shieldReceive_verifiedEnablesSave() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        ShieldReceiveDialog* dlg = new ShieldReceiveDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* id    = dlg->findChild<QLineEdit*>("shieldReceiveIdEdit");
        auto* look  = dlg->findChild<QPushButton*>("shieldReceiveLookupButton");
        auto* state = dlg->findChild<QLabel*>("shieldReceiveStateLine");
        auto* save  = dlg->findChild<QPushButton*>("shieldReceiveSaveButton");
        QVERIFY(id && look && state && save);
        QVERIFY2(!save->isEnabled(), "Save must be disabled until a file verifies");

        DataTransferResult r;
        r.verified = true; r.complete = true;
        r.hexData  = QString::fromLatin1(QByteArray("hello").toHex());
        r.size     = 5; r.filename = "hello.txt"; r.contentType = "text/plain";
        rpc->testSetNextDataTransferResult(r);

        id->setText(QString(64, QChar('a')));
        look->click();

        QVERIFY2(pumpUntil([&]{ return state->text().contains("verified", Qt::CaseInsensitive); }),
                 qPrintable("verified state line: " + state->text()));
        QVERIFY2(save->isEnabled(), "Save must enable ONLY after verify+decrypt");

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD RECEIVE: the four honest failure states stay DISTINCT (no generic "failed",
    //    no fake "try again" on a hard refusal), and Save NEVER enables on failure.
    void shieldReceive_distinctFailureStates() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        struct Case { QString error; bool complete; QString expect; bool refusal; };
        const QList<Case> cases = {
            { "content hash mismatch after reassembly",          true,  "doesn't match", true  },
            { "AEAD verification failed (tamper or wrong key)",  true,  "tampered",      true  },
            { "key not yet available (sealed)",                  true,  "addressed to you", false },
            { "transfer incomplete (missing frames)",            false, "haven't confirmed", false },
        };
        for (const Case& c : cases) {
            ShieldReceiveDialog* dlg = new ShieldReceiveDialog(rpc, w);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
            auto* id    = dlg->findChild<QLineEdit*>("shieldReceiveIdEdit");
            auto* look  = dlg->findChild<QPushButton*>("shieldReceiveLookupButton");
            auto* state = dlg->findChild<QLabel*>("shieldReceiveStateLine");
            auto* save  = dlg->findChild<QPushButton*>("shieldReceiveSaveButton");
            QVERIFY(id && look && state && save);

            DataTransferResult r;
            r.verified = false; r.complete = c.complete; r.error = c.error;
            rpc->testSetNextDataTransferResult(r);
            id->setText(QString(64, QChar('c')));
            look->click();

            QVERIFY2(pumpUntil([&]{ return state->text().contains(c.expect, Qt::CaseInsensitive); }),
                     qPrintable("state for [" + c.error + "] -> " + state->text()));
            QVERIFY2(!save->isEnabled(), "Save must NEVER enable on a failure");
            if (c.refusal)
                QVERIFY2(!state->text().contains("try again", Qt::CaseInsensitive),
                         qPrintable("a hard refusal must NOT say try again: " + state->text()));
            dlg->close();
        }
        w->deleteLater();
    }

    // -- SHIELD SEND: the recipient field is a FULL 4-state validator, and (unlike the
    //    NFT-send t-addr validator) the data-channel needs a SAPLING (zs…) recipient.
    //    States: (1) empty -> no status, Send disabled; (2) garbage -> red "doesn't
    //    look like a ZClassic address"; (3) a valid t-addr or Sprout z-addr -> red
    //    "need a Sapling (zs…) recipient" (the channel is Sapling-only) — and Send
    //    stays disabled even with file+consent present; (4) a valid Sapling zs-addr ->
    //    green "Looks good". This is the inverse-polarity validator the spec (§6.4)
    //    requires, so it gets its own coverage separate from the NFT-send t-addr one.
    void shieldSend_recipientFourStateValidation() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        auto* zs = new QList<QString>(); zs->append(SAPLING_ZS_A());
        rpc->testSetZAddresses(zs);

        ShieldSendDialog* dlg = new ShieldSendDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* to      = dlg->findChild<QLineEdit*>("shieldSendToEdit");
        auto* status  = dlg->findChild<QLabel*>("shieldSendToStatus");
        auto* consent = dlg->findChild<QCheckBox*>("shieldSendConsentCheck");
        auto* send    = dlg->findChild<QPushButton*>("shieldSendButton");
        QVERIFY(to && status && consent && send);

        // Pre-arm the OTHER gates (valid file + consent) so the recipient state is the
        // sole remaining lever on Send for the t-addr/Sprout case below.
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        dlg->testPickFile(writeSmallBinary(tmp.path(), "p.bin", 64));
        consent->setChecked(true);

        // State 1 — empty: no status text, Send disabled.
        to->setText("");
        QVERIFY2(status->text().isEmpty(), qPrintable("empty -> no status: " + status->text()));
        QVERIFY2(!send->isEnabled(), "empty recipient must keep Send disabled");

        // State 2 — garbage: red "doesn't look like a ZClassic address".
        to->setText("not-an-address");
        QVERIFY2(status->text().contains("doesn't look like", Qt::CaseInsensitive),
                 qPrintable("garbage -> red invalid copy: " + status->text()));
        QVERIFY2(!send->isEnabled(), "garbage recipient must keep Send disabled");

        // State 3 — a valid t-addr is a real ZClassic address but the channel is
        // Sapling-only: it must be REFUSED with the "need a Sapling (zs…)" hint, and
        // Send must stay disabled even though file + consent are present.
        to->setText("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi");
        QVERIFY2(status->text().contains("Sapling", Qt::CaseInsensitive),
                 qPrintable("t-addr -> Sapling-only hint: " + status->text()));
        QVERIFY2(!send->isEnabled(),
                 "a valid t-addr recipient must NOT enable a Sapling-only private send");

        // State 4 — a valid Sapling zs-addr: green "Looks good", and with file+consent
        // already set, Send finally enables.
        to->setText(SAPLING_ZS_B());
        QVERIFY2(status->text().contains("Looks good", Qt::CaseInsensitive),
                 qPrintable("zs-addr -> green ok copy: " + status->text()));
        QVERIFY2(send->isEnabled(),
                 "a valid Sapling recipient (+ file + consent) must enable Send");

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD RECEIVE (Mode A): the cross-wallet "open the file linked to a token you
    //    hold" path. prefillFingerprint(<64-hex>) fills the lookup box with the NFT's
    //    document_hash and immediately runs verify-before-decrypt by FINGERPRINT (no
    //    transfer_id). On a verified+complete result the success line shows and Save
    //    enables — exercising the registry-free cross-wallet receive seam.
    void shieldReceive_crossWalletByFingerprintPrefill() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        DataTransferResult r;
        r.verified = true; r.complete = true;
        r.hexData  = QString::fromLatin1(QByteArray("xwallet-bytes").toHex());
        r.size     = 13; r.filename = "art.png"; r.contentType = "image/png";
        const QString fp = QString(64, QChar('f'));
        r.fingerprint = fp;
        rpc->testSetNextDataTransferResult(r);

        ShieldReceiveDialog* dlg = new ShieldReceiveDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* id    = dlg->findChild<QLineEdit*>("shieldReceiveIdEdit");
        auto* state = dlg->findChild<QLabel*>("shieldReceiveStateLine");
        auto* save  = dlg->findChild<QPushButton*>("shieldReceiveSaveButton");
        QVERIFY(id && state && save);

        // Mode A entry: pre-fill the held token's fingerprint and run the lookup.
        dlg->prefillFingerprint(fp);

        // The lookup box reflects the fingerprint and the verify-by-fingerprint path
        // reached a verified result -> Save enabled.
        QVERIFY2(pumpUntil([&]{ return state->text().contains("verified", Qt::CaseInsensitive); }),
                 qPrintable("cross-wallet by-fingerprint verified line: " + state->text()));
        QCOMPARE(id->text(), fp);
        QVERIFY2(save->isEnabled(), "a verified cross-wallet open must enable Save");

        dlg->close();
        w->deleteLater();
    }

    // -- SHIELD RECEIVE: verify-BEFORE-open means a HASH_MISMATCH refusal leaks NO
    //    plaintext. The daemon never returns hexdata on a verify failure, and the
    //    dialog must hold none: Save stays disabled AND clicking Save (defensively)
    //    surfaces "no verified file to save" rather than writing bytes. The mismatch
    //    diagnosis shows BOTH fingerprints (honest), but never any file content.
    void shieldReceive_mismatchVerifyBeforeOpenNoPlaintext() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        ShieldReceiveDialog* dlg = new ShieldReceiveDialog(rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();

        auto* id    = dlg->findChild<QLineEdit*>("shieldReceiveIdEdit");
        auto* look  = dlg->findChild<QPushButton*>("shieldReceiveLookupButton");
        auto* state = dlg->findChild<QLabel*>("shieldReceiveStateLine");
        auto* meta  = dlg->findChild<QLabel*>("shieldReceiveMetaLine");
        auto* save  = dlg->findChild<QPushButton*>("shieldReceiveSaveButton");
        QVERIFY(id && look && state && meta && save);

        // The daemon refuses (HASH_MISMATCH) and returns NO hexdata, with both
        // fingerprints for honest diagnosis.
        DataTransferResult r;
        r.verified = false; r.complete = true;
        r.error    = "content hash mismatch after reassembly";
        r.hexData  = QString();   // daemon NEVER returns plaintext on a refusal
        r.onchainFingerprint  = QString(64, QChar('1'));
        r.expectedFingerprint = QString(64, QChar('2'));
        rpc->testSetNextDataTransferResult(r);

        id->setText(QString(64, QChar('2')));   // expected anchor (out-of-band)
        look->click();

        QVERIFY2(pumpUntil([&]{ return state->text().contains("doesn't match", Qt::CaseInsensitive); }),
                 qPrintable("mismatch refusal line: " + state->text()));
        QVERIFY2(state->text().contains("Not opened", Qt::CaseInsensitive),
                 qPrintable("a refusal must say it did NOT open the file: " + state->text()));
        // No plaintext: Save stays disabled, and the honest diagnosis shows the two
        // fingerprints (abbreviated) — never any file bytes/content.
        QVERIFY2(!save->isEnabled(), "Save must stay disabled on a verify refusal (no plaintext)");
        QVERIFY2(meta->text().contains("On-chain", Qt::CaseInsensitive)
                     && meta->text().contains("expected", Qt::CaseInsensitive),
                 qPrintable("mismatch diagnosis should show both fingerprints: " + meta->text()));

        // Defensive: forcing the save slot must NOT write — there is no verified file.
        QMetaObject::invokeMethod(dlg, "onSaveDecrypted");
        QVERIFY2(state->text().contains("no verified file", Qt::CaseInsensitive),
                 qPrintable("forcing Save with no verified file must refuse: " + state->text()));

        dlg->close();
        w->deleteLater();
    }

    // ======================================================================
    // BUG #1 — NFT-CAPABILITY gating (the user attached to an OLDER node that
    // lacks the NFT RPCs, so minting 404'd with a cryptic "method not found").
    // ======================================================================

    // (a) nodeSupportsNFT=false => Collections shows the guidance panel AND the
    //     Mint/Buy/Send-private/Receive-private entry points are DISABLED with the
    //     honest tooltip. (Sell lives on the per-item detail dialog; the gallery
    //     entry points are Mint/Buy/Send-private/Receive-private.)
    void nftCapability_unsupportedShowsGuidanceAndDisablesEntries() {
        Settings::getInstance()->setShowNFTGallery(true);
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        QVERIFY2(w->isNFTGalleryActive(), "gallery tab must be built for this test");

        // Force the probe result OFF (no daemon) and re-render the gating.
        rpc->testSetNodeSupportsNFT(false);
        w->onNFTCapabilityResolved();

        auto* panel = w->findChild<QWidget*>("nftUnsupportedPanel");
        auto* label = w->findChild<QLabel*>("nftUnsupportedLabel");
        auto* view  = w->findChild<QWidget*>("nftGalleryView");
        QVERIFY2(panel && label && view, "gallery must expose the unsupported panel + view");

        // Assert the EXPLICIT show/hide flag (isHidden): the Collections tab is not the
        // current QTabWidget page in this isolated test, so isVisibleTo(w) is false for
        // everything on it — isHidden() reflects the gating decision regardless.
        QVERIFY2(!panel->isHidden(), "unsupported node => guidance panel must be shown");
        QVERIFY2(view->isHidden(),   "unsupported node => the empty grid must be hidden");
        // The guidance is HONEST + actionable (and ZCL, never ZEC) — and never the
        // raw RPC error.
        QVERIFY2(label->text().contains("built-in node", Qt::CaseInsensitive),
                 qPrintable("guidance must point at the built-in node: " + label->text()));
        QVERIFY2(label->text().contains("beta7", Qt::CaseInsensitive),
                 qPrintable("guidance must name the version that works: " + label->text()));
        QVERIFY2(!label->text().contains("method not found", Qt::CaseInsensitive),
                 "guidance must NEVER surface the raw 'method not found'");

        // Every gallery entry point disabled with the matching tooltip.
        for (const char* name : { "nftMakeButton", "nftBuyAnNftButton",
                                  "nftSendPrivateFileButton", "nftReceivePrivateFileButton" }) {
            auto* b = w->findChild<QPushButton*>(name);
            QVERIFY2(b, qPrintable(QString("missing entry button ") + name));
            QVERIFY2(!b->isEnabled(),
                     qPrintable(QString("entry button must be disabled when unsupported: ") + name));
            QVERIFY2(b->toolTip().contains("built-in node", Qt::CaseInsensitive),
                     qPrintable(QString("entry button needs the honest tooltip: ") + name));
        }

        // And the inverse: a SUPPORTED node restores the page (no panel, entries on).
        rpc->testSetNodeSupportsNFT(true);
        w->onNFTCapabilityResolved();
        QVERIFY2(panel->isHidden(), "supported node => guidance panel hidden");
        QVERIFY2(!view->isHidden(), "supported node => the grid is shown");
        auto* mint = w->findChild<QPushButton*>("nftMakeButton");
        QVERIFY2(mint && mint->isEnabled(), "supported node => Mint re-enabled");
        // A supported node must NOT leave the unsupported guidance stuck on the button.
        // applyNFTSupportGating() RESTORES each button's own honesty tooltip (stashed in
        // the honestTooltip dynamic property) rather than clearing it — so the tooltip is
        // the button's content-only-privacy/affordance copy, never the "built-in node"
        // unsupported guidance.
        QVERIFY2(!mint->toolTip().contains("built-in node", Qt::CaseInsensitive),
                 qPrintable("supported node => the unsupported guidance must be gone from the "
                            "tooltip, not stuck: " + mint->toolTip()));
        QCOMPARE(mint->toolTip(), mint->property("honestTooltip").toString());

        w->deleteLater();
    }

    // (b) An NFT RPC returning -32601 yields the calm guidance string (NOT "method
    //     not found"). nftUnsupportedGuidance() is the single string the -32601 path
    //     in zslpCalmError()/datachannelCalmError() returns, so asserting it locks
    //     the honest message that backs EVERY NFT/zslp wrapper's -32601 mapping.
    void nftCapability_minusThirtyTwoSixOhOneIsCalmGuidance() {
        const QString g = RPC::nftUnsupportedGuidance();
        QVERIFY2(!g.contains("method not found", Qt::CaseInsensitive),
                 qPrintable("the -32601 guidance must never say 'method not found': " + g));
        QVERIFY2(!g.contains("-32601"),
                 qPrintable("the -32601 guidance must not leak the raw code: " + g));
        // It must be ACTIONABLE: name the fix (quit the other node + restart, or upgrade).
        QVERIFY2(g.contains("Quit", Qt::CaseInsensitive)
                     && g.contains("restart", Qt::CaseInsensitive),
                 qPrintable("guidance must tell the user to quit + restart: " + g));
        // It must offer the upgrade path. New copy names the version that works
        // ("ZClassic v2.1.2-beta7 or newer") instead of the bare word "upgrade" — same
        // honest actionable upgrade route, more specific.
        QVERIFY2(g.contains("beta7", Qt::CaseInsensitive)
                     && g.contains("newer", Qt::CaseInsensitive),
                 qPrintable("guidance must offer the version-upgrade path: " + g));
        // Coin is ZCL, never ZEC/Zcash in user copy.
        QVERIFY2(!g.contains("ZEC") && !g.contains("Zcash"),
                 qPrintable("user copy must say ZClassic/ZCL, never ZEC/Zcash: " + g));

        // End-to-end: the dialog seam routes the SAME calm guidance to the result line
        // (never a raw error), exactly as the production -32601 mapping would.
        Settings::getInstance()->setShowNFTGallery(true);
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();
        ContentEngine engine(nullptr);
        NftMintDialog* dlg = new NftMintDialog(&engine, rpc, w);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        auto* create = dlg->findChild<QPushButton*>("nftMintCreateButton");
        auto* name   = dlg->findChild<QLineEdit*>("nftMintNameEdit");
        auto* result = dlg->findChild<QLabel*>("nftMintResultLine");
        QVERIFY(create && name && result);
        name->setText("OldNode");
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QString hashHex;
        const QString png = writePngWithHash(tmp.path(), "d.png", Qt::cyan, hashHex);
        dlg->testPickFile(png);
        QVERIFY(pumpUntil([&]{ return create->isEnabled(); }));
        // The seam injects what the real -32601 mapping produces.
        rpc->testSetNextNftError(g);
        create->click();
        QVERIFY2(pumpUntil([&]{ return result->text().contains("built-in node", Qt::CaseInsensitive); }),
                 qPrintable("mint -32601 must surface the calm guidance: " + result->text()));
        QVERIFY2(!result->text().contains("method not found", Qt::CaseInsensitive),
                 "mint must never dead-end on a raw 'method not found'");
        dlg->close();
        w->deleteLater();
    }

    // (c) DIRECT mapping assertion: the shared zslpCalmError() mapper, given a JSON-RPC
    //     body with error.code == -32601 and a null reply (exactly what every NFT/zslp
    //     wrapper passes on a daemon error), must return nftUnsupportedGuidance()
    //     VERBATIM — never the raw "method not found", never ZEC/Zcash. This locks the
    //     -32601 -> calm-guidance mapping at the source, not just through a dialog.
    void nftCalmError_minus32601MapsToGuidanceDirect() {
        const QString mapped   = RPC::testCalmErrorForCode(-32601);
        const QString guidance = RPC::nftUnsupportedGuidance();
        QCOMPARE(mapped, guidance);
        QVERIFY2(!mapped.contains("method not found", Qt::CaseInsensitive),
                 qPrintable("the -32601 mapping must never surface 'method not found': " + mapped));
        QVERIFY2(!mapped.contains("-32601"),
                 qPrintable("the -32601 mapping must not leak the raw code: " + mapped));
        QVERIFY2(!mapped.contains("ZEC") && !mapped.contains("Zcash"),
                 qPrintable("user copy must say ZClassic/ZCL, never ZEC/Zcash: " + mapped));

        // Sanity: a DIFFERENT code must NOT map to the unsupported guidance (proves the
        // -32601 branch is doing real work, not a constant return).
        QVERIFY2(RPC::testCalmErrorForCode(-13) != guidance,
                 "only -32601 (method-not-found) maps to the unsupported guidance");
    }

    // ======================================================================
    // BUG #2 — responsive sizing. The mint + detail dialogs must fit a SMALL
    // screen (1280x720 with chrome, and the height stays <= ~640 so it fits
    // 1024x600 after the user shrinks it). We assert minimumSizeHint() — the
    // smallest the dialog can become — plus the detail dialog's chosen open size.
    // ======================================================================
    void nftDialogs_fitSmallScreen() {
        const int kMaxW = 1280;
        const int kMaxH = 640;

        ContentEngine engine(nullptr);
        MainWindow* w = makeWindow();

        // MINT (where the user hit the bug): its smallest size must fit.
        {
            NftMintDialog* dlg = new NftMintDialog(&engine, w->getRPC(), w);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
            const QSize ms = dlg->minimumSizeHint();
            QVERIFY2(ms.width()  <= kMaxW,
                     qPrintable(QString("mint min width %1 must be <= %2").arg(ms.width()).arg(kMaxW)));
            QVERIFY2(ms.height() <= kMaxH,
                     qPrintable(QString("mint min height %1 must be <= %2").arg(ms.height()).arg(kMaxH)));
            dlg->close();
        }

        // DETAIL: the tallest dialog. The OLD broken dialog used a hard
        // setMinimumSize(760,560) with no scroll area, so its content min width was
        // ~878 and it could not shrink onto a small screen — yet a loose `<= kMaxW`
        // (1280) bound passed anyway (tautological). We now assert the REAL contract:
        //   (a) minimumSizeHint().width() <= 700  -> catches the old ~878 content-min,
        //   (b) the body lives in a QScrollArea named "nftDetailScroll" (so a tall
        //       layout scrolls instead of forcing the window off-screen), and
        //   (c) after shrinking to 600x420 the dialog actually shrinks to that size
        //       (its required minimum fits within the given size — nothing clips).
        {
            NFTItem it; it.name = "Fits"; it.txid = "tokfit";
            it.docHashHex = "ab"; it.cachePath = ""; it.isPrivate = false; it.verifyState = 1;
            QVector<NFTItem> ordered; ordered.push_back(it);
            NFTDetailDialog* dlg = new NFTDetailDialog(it, ordered, 0, &engine, nullptr, w);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();

            // (a) the smallest the dialog can become must be genuinely small.
            const QSize ms = dlg->minimumSizeHint();
            QVERIFY2(ms.width()  <= 700,
                     qPrintable(QString("detail minimumSizeHint width %1 must be <= 700 "
                                        "(the old un-scrolled dialog was ~878)").arg(ms.width())));
            QVERIFY2(ms.height() <= kMaxH,
                     qPrintable(QString("detail min height %1 must be <= %2").arg(ms.height()).arg(kMaxH)));

            // (b) the body must be in the scroll area, so a tall layout scrolls.
            QScrollArea* scroll = dlg->findChild<QScrollArea*>("nftDetailScroll");
            QVERIFY2(scroll != nullptr,
                     "detail dialog body must live in a QScrollArea named 'nftDetailScroll'");

            // The CHOSEN open size must also fit (the constructor clamps to the screen).
            QVERIFY2(dlg->size().width()  <= kMaxW,
                     qPrintable(QString("detail open width %1 must be <= %2").arg(dlg->size().width()).arg(kMaxW)));
            QVERIFY2(dlg->size().height() <= kMaxH,
                     qPrintable(QString("detail open height %1 must be <= %2").arg(dlg->size().height()).arg(kMaxH)));

            // (c) shrink it to a tiny window: the layout must NOT require more than the
            // given size. After resize+activate the actual size must match the request,
            // which proves the layout's minimum fits inside it (no clipped controls).
            dlg->resize(600, 420);
            QVERIFY(QTest::qWaitForWindowExposed(dlg));
            qApp->processEvents();
            QVERIFY2(dlg->size().width()  <= 600,
                     qPrintable(QString("detail must shrink to width 600, got %1 "
                                        "(layout requires more than given size)").arg(dlg->size().width())));
            QVERIFY2(dlg->size().height() <= 420,
                     qPrintable(QString("detail must shrink to height 420, got %1 "
                                        "(layout requires more than given size)").arg(dlg->size().height())));
            if (QLayout* lay = dlg->layout()) {
                QVERIFY2(lay->minimumSize().width()  <= 600,
                         qPrintable(QString("detail layout minimum width %1 must fit 600")
                                        .arg(lay->minimumSize().width())));
                QVERIFY2(lay->minimumSize().height() <= 420,
                         qPrintable(QString("detail layout minimum height %1 must fit 420")
                                        .arg(lay->minimumSize().height())));
            }
            dlg->close();
        }

        w->deleteLater();
    }

    void perf16_modelJank() {
        MainWindow* w = makeWindow();
        RPC* rpc = w->getRPC();

        const int N_ENTRIES = 5000;

        // Build two distinct large datasets so consecutive updates never hit the
        // model's unchanged-data fingerprint short-circuit (which would measure a
        // no-op instead of the rebuild). Dataset B perturbs every balance.
        auto makeDataset = [N_ENTRIES](double salt,
                                       QMap<QString, double>** balsOut,
                                       QList<UnspentOutput>** utxoOut) {
            auto* bals = new QMap<QString, double>();
            auto* utxos = new QList<UnspentOutput>();
            for (int i = 0; i < N_ENTRIES; ++i) {
                // A realistic-length transparent address keyed by index.
                QString addr = QString("t1Perf%1Addr000000000000000000000")
                                   .arg(i, 8, 10, QChar('0'));
                double bal = (double)(i + 1) + salt;
                (*bals)[addr] = bal;
                // One matching UTXO per address (the model copies + scans these).
                utxos->push_back(UnspentOutput{
                    addr,
                    QString("%1txid000000000000000000000000000000000000000000000000000000")
                        .arg(i, 8, 10, QChar('0')),
                    QString::number(bal, 'f', 8),
                    (i % 7 == 0) ? 0 : 10,   // ~1/7 unconfirmed -> red-row scan work
                    true });
            }
            *balsOut = bals;
            *utxoOut = utxos;
        };

        QMap<QString, double>* balsA; QList<UnspentOutput>* utxoA;
        QMap<QString, double>* balsB; QList<UnspentOutput>* utxoB;
        makeDataset(0.0, &balsA, &utxoA);
        makeDataset(0.5, &balsB, &utxoB);

        rpc->testBalanceModelSamplesNs().clear();

        // One untimed warm-up update to fault in code paths / allocate the model,
        // then alternate A/B so every measured call does a real rebuild.
        rpc->testSetBalances(new QMap<QString, double>(*balsA));
        rpc->testSetUTXOs(new QList<UnspentOutput>(*utxoA));
        rpc->testUpdateUI(true);
        rpc->testBalanceModelSamplesNs().clear();   // discard the warm-up sample

        const int ITERS = 30;
        for (int i = 0; i < ITERS; ++i) {
            const bool useA = (i % 2 == 0);
            rpc->testSetBalances(new QMap<QString, double>(useA ? *balsA : *balsB));
            rpc->testSetUTXOs(new QList<UnspentOutput>(useA ? *utxoA : *utxoB));
            rpc->testUpdateUI(true);
        }

        QVector<qint64> samples = rpc->testBalanceModelSamplesNs();
        QVERIFY2(samples.size() == ITERS,
                 qPrintable(QString("expected %1 model samples, got %2 — the "
                                    "updateUI timing seam did not fill")
                                .arg(ITERS).arg(samples.size())));

        std::sort(samples.begin(), samples.end());
        auto pct = [&samples](double p) -> qint64 {
            if (samples.isEmpty()) return 0;
            int idx = (int)std::ceil(p / 100.0 * samples.size()) - 1;
            idx = qBound(0, idx, samples.size() - 1);
            return samples[idx];
        };
        const qint64 p50 = pct(50), p95 = pct(95), p100 = samples.last();
        const double p50ms = p50 / 1e6, p95ms = p95 / 1e6, p100ms = p100 / 1e6;

        // Single parseable perf-log line (ns + ms) so the baseline is recorded.
        qInfo().noquote() << QString(
            "PERF perf16 balances entries=%1 iters=%2 "
            "p50=%3ns(%4ms) p95=%5ns(%6ms) p100=%7ns(%8ms)")
            .arg(N_ENTRIES).arg(ITERS)
            .arg(p50).arg(p50ms, 0, 'f', 3)
            .arg(p95).arg(p95ms, 0, 'f', 3)
            .arg(p100).arg(p100ms, 0, 'f', 3);

        // Soft: a single balances-model rebuild should stay under one 60Hz frame.
        if (p95ms > 16.0)
            QWARN(qPrintable(QString("perf16: p95 balances-model rebuild %1ms > 16ms "
                                     "(dropped-frame budget)").arg(p95ms, 0, 'f', 3)));
        // Hard: egregious jank (>50ms = visibly stuttery) fails the build.
        QVERIFY2(p100ms <= 50.0,
                 qPrintable(QString("perf16: p100 balances-model rebuild %1ms > 50ms "
                                    "(egregious jank)").arg(p100ms, 0, 'f', 3)));

        delete balsA; delete utxoA; delete balsB; delete utxoB;
        settleAndDelete(w);
    }

    // perf22_warmLatency — BEST-EFFORT warm end-to-end latency: the wall time from
    //   t0 = RPC::onNotifyPush() (driven via testFireNotifyPush) to
    //   t1 = MainWindow::heroBalancesPainted() (emitted at the end of
    //   updateHomeFixIt once the hero repaints), across N notify cycles. The
    //   mandatory 200ms notify debounce (RPC::kNotifyDebounceMs) is the HARD FLOOR,
    //   so a healthy number would be ~debounce + a few ms of rebuild.
    //
    //   *** DEFERRED in this environment (the harness QSKIPs it). ***
    //   Measuring t0->t1 requires refresh() to actually run end-to-end, which needs a
    //   live Connection to a daemon. A tests/mock/mockrpc.py subprocess on an isolated
    //   loopback port (the helpers findPython/waitForMockHealth/installMockConnection/
    //   shutdownMock below mirror tests/e2e/mocke2e.sh) DOES wire up cleanly and the
    //   first refresh fires ("Payment UI now ready!"), but under QT_QPA_PLATFORM=
    //   offscreen in proot the notify->debounce->refresh->paint cycle does not settle
    //   deterministically inside a bounded QTest::qWait: setConnection()'s cascade of
    //   async getinfo/getblockchaininfo/balance polls keeps the event loop churning so
    //   the loop neither observes a clean heroBalancesPainted nor reaches its skip in
    //   bounded time (it WEDGED the whole suite for minutes in testing). Per the design
    //   brief — "Prefer a reliable perf16 number over a flaky perf22" — perf22 is
    //   DEFERRED rather than allowed to flake/hang the L1 run. perf16_modelJank() is the
    //   robust, deterministic deliverable and records the real model-rebuild baseline.
    //
    //   The production t0/t1 seams (heroBalancesPainted signal + testFireNotifyPush)
    //   AND the mock-orchestration helpers are all in place, so finishing perf22 later
    //   is purely a matter of making the refresh-settle loop robust (e.g. drive a
    //   single-shot refresh against a static balance fixture and a QSignalSpy on
    //   heroBalancesPainted with a hard wall-clock cap), not new wiring.
    void perf22_warmLatency() {
        QSKIP("perf22 DEFERRED: warm t0(onNotifyPush)->t1(heroBalancesPainted) latency "
              "needs a settled refresh() cycle; the mock wires up but the async poll "
              "cascade does not settle deterministically under offscreen/proot within a "
              "bounded wait. perf16_modelJank is the reliable deliverable. See the slot "
              "comment for the exact reason and the path to finishing it.");

        // --- Reference implementation kept (compiled, but unreachable after QSKIP) so
        // the t0/t1 wiring is documented in code and trivially re-enabled. ---
        const QString py = findPython();
        if (py.isEmpty()) return;
        const QString mockScript =
            QDir(QCoreApplication::applicationDirPath()).filePath("../mock/mockrpc.py");
        if (!QFile::exists(mockScript)) return;

        const int port = 31900 + (QRandomGenerator::global()->bounded(500));
        QProcess mock;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("MOCK_PORT", QString::number(port));
        env.insert("MOCK_SCENARIO", "funded-synced");
        env.insert("MOCK_LOG", QDir::tempPath() + "/zqw_perf22_mock.log");
        mock.setProcessEnvironment(env);
        mock.start(py, { mockScript });
        if (!mock.waitForStarted(3000)) return;
        if (!waitForMockHealth(port, 3000)) { shutdownMock(mock, port); return; }

        auto* S = Settings::getInstance();
        const bool savedHeadless = S->isHeadless();
        S->setUseEmbedded(false);
        S->setHeadless(false);
        S->setSyncing(false);
        S->setPeers(8);
        S->setZClassicdVersion(2001250);
        MainWindow* w = new MainWindow(nullptr);
        w->show();
        RPC* rpc = w->getRPC();
        installMockConnection(w, port);

        QElapsedTimer t;
        QVector<qint64> latNs;
        bool armed = false;
        QObject::connect(w, &MainWindow::heroBalancesPainted, w, [&]() {
            if (armed) { latNs.push_back(t.nsecsElapsed()); armed = false; }
        });

        const int CYCLES = 6;
        for (int c = 0; c < CYCLES; ++c) {
            armed = true;
            t.restart();
            rpc->testFireNotifyPush();                 // t0
            QElapsedTimer budget; budget.start();
            while (armed && budget.elapsed() < 5000)
                QTest::qWait(20);
        }
        shutdownMock(mock, port);

        if (!latNs.isEmpty()) {
            std::sort(latNs.begin(), latNs.end());
            auto pct = [&latNs](double p) -> qint64 {
                int idx = (int)std::ceil(p / 100.0 * latNs.size()) - 1;
                idx = qBound(0, idx, latNs.size() - 1);
                return latNs[idx];
            };
            qInfo().noquote() << QString(
                "PERF perf22 warmLatency cycles=%1 floor=%2ms p50=%3ms p95=%4ms")
                .arg(latNs.size()).arg(RPC::kNotifyDebounceMs)
                .arg(pct(50) / 1e6, 0, 'f', 3).arg(pct(95) / 1e6, 0, 'f', 3);
        }
        settleAndDelete(w);
        S->setHeadless(savedHeadless);
    }
#endif

private:
#ifdef ZCL_WIDGET_TEST
    // ---- perf22 helpers (mock orchestration; mirror tests/e2e/mocke2e.sh) ----
    // Locate a python3 interpreter on PATH (proot maps /usr/bin/python3).
    QString findPython() {
        for (const QString& cand : { "/usr/bin/python3", "/usr/local/bin/python3",
                                     "python3" }) {
            if (cand.startsWith('/')) {
                if (QFile::exists(cand)) return cand;
            } else {
                return cand;   // rely on PATH lookup
            }
        }
        return QString();
    }

    // GET http://127.0.0.1:<port>/ and return true once it answers 200 within budget.
    bool waitForMockHealth(int port, int budgetMs) {
        QElapsedTimer budget; budget.start();
        while (budget.elapsed() < budgetMs) {
            QTcpSocket sock;
            sock.connectToHost(QHostAddress::LocalHost, port);
            if (sock.waitForConnected(200)) {
                sock.write(QString("GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
                               .toUtf8());
                sock.flush();
                if (sock.waitForReadyRead(500)) {
                    QByteArray resp = sock.readAll();
                    if (resp.contains("200") || resp.contains("\"ok\""))
                        return true;
                }
            }
            QTest::qWait(50);
        }
        return false;
    }

    // GET /__shutdown then terminate as a fallback so no mock leaks across runs.
    void shutdownMock(QProcess& mock, int port) {
        QTcpSocket sock;
        sock.connectToHost(QHostAddress::LocalHost, port);
        if (sock.waitForConnected(300)) {
            sock.write(QString("GET /__shutdown HTTP/1.0\r\nHost: localhost\r\n\r\n")
                           .toUtf8());
            sock.flush();
            sock.waitForReadyRead(300);
        }
        if (mock.state() != QProcess::NotRunning) {
            mock.terminate();
            if (!mock.waitForFinished(1500)) { mock.kill(); mock.waitForFinished(1000); }
        }
    }

    // Build a REAL Connection pointed at the mock's loopback port and install it on
    // the RPC so refresh()/getInfoThenRefresh() actually issue RPC and repaint.
    void installMockConnection(MainWindow* w, int port) {
        auto cfg = std::make_shared<ConnectionConfig>();
        cfg->host  = "127.0.0.1";
        cfg->port  = QString::number(port);
        cfg->rpcuser = "test";
        cfg->rpcpassword = "test";
        cfg->zclassicDir = QDir::tempPath();
        auto* nam = new QNetworkAccessManager(w);
        auto* req = new QNetworkRequest();
        QString userpass = cfg->rpcuser + ":" + cfg->rpcpassword;
        QString headerData = "Basic " + userpass.toUtf8().toBase64();
        req->setUrl(QUrl(QString("http://127.0.0.1:%1/").arg(port)));
        req->setRawHeader("Authorization", headerData.toLocal8Bit());
        req->setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto* c = new Connection(w, nam, req, cfg);
        w->getRPC()->setConnection(c);
    }

    // Apply the SHIPPED production dark theme (Fusion + dark palette + dark.qss from
    // the qrc) to the running QApplication so grabbed shots match production, not the
    // default light Fusion look. Mirrors src/main.cpp's theme setup.
    void applyProductionTheme() {
        QApplication::setStyle("Fusion");
        QPalette pal;
        const QColor base("#0f1115"), surface("#1d2027"), text("#e6e6e6"),
                     dim("#9aa0a6"), accent("#1f7a1f");
        pal.setColor(QPalette::Window,          base);
        pal.setColor(QPalette::WindowText,      text);
        pal.setColor(QPalette::Base,            surface);
        pal.setColor(QPalette::AlternateBase,   base);
        pal.setColor(QPalette::Text,            text);
        pal.setColor(QPalette::Button,          surface);
        pal.setColor(QPalette::ButtonText,      text);
        pal.setColor(QPalette::BrightText,      QColor("#ffffff"));
        pal.setColor(QPalette::Highlight,       accent);
        pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        pal.setColor(QPalette::PlaceholderText, dim);
        qApp->setPalette(pal);

        QFile qssFile(":/styles/res/styles/dark.qss");
        if (qssFile.open(QFile::ReadOnly | QFile::Text))
            qApp->setStyleSheet(QString::fromUtf8(qssFile.readAll()));
        qApp->processEvents();
    }

    // Grab a page widget to PNG at a realistic window size. Resize + polish so the
    // qss-computed geometry is applied, process events, then grab().
    void grabPage(QWidget* page, const QString& path) {
        if (!page) return;
        QWidget* top = page->window();
        top->resize(1100, 720);
        qApp->processEvents();
        page->ensurePolished();
        qApp->processEvents();
        page->grab().save(path, "PNG");
    }

    // Poll for the confirm dialog, grab() it to a PNG, then reject it (a de-shield's
    // ack only appears AFTER accept, so rejecting avoids the second modal here).
    bool armGrabber(const QString& path, int tries = 60) {
        int gen = (tries == 60) ? ++_modalGen : _modalGen;
        QTimer::singleShot(tries > 50 ? 0 : 10, [=]() {
            if (gen != _modalGen) return;
            QWidget* dlg = QApplication::activeModalWidget();
            if (!dlg || !dlg->findChild<QLabel*>("confirmPrivacyBadge")) {
                if (tries > 0) armGrabber(path, tries - 1);
                return;
            }
            dlg->grab().save(path, "PNG");
            if (auto* qd = qobject_cast<QDialog*>(dlg)) qd->reject();
            else dlg->close();
        });
        return true;
    }
#endif

private:
#ifdef ZCL_WIDGET_TEST

    // Read the confirm dialog's privacy badge text + publicWarning visibility, then
    // either accept or reject it. Polls until the confirm dialog (the one carrying
    // 'confirmPrivacyBadge') is up. badgeOut/warnOut are filled before closing.
    // Generation counter: every arm* bumps it; a poll for a stale generation simply
    // stops, so a leftover re-arm chain from an earlier test/case can never fire into
    // a later one and dismiss its dialog early.
    int _modalGen = 0;

    // Tear down a P-series window. Bumps the modal generation first so any stray
    // modal-dismisser poll self-cancels, then deletes the window. This is safe because
    // the test build skips the RPC auto-connect chain (rpc.cpp ZCL_WIDGET_TEST guard),
    // so there is no self-perpetuating doAutoConnect() re-arm holding dangling captures.
    void settleAndDelete(MainWindow* w) {
        ++_modalGen;
        delete w;
    }

    // Self-rearming singleShot poll (mirrors the proven armConfirmDismisser). Finds
    // the active modal, captures the confirm dialog's badge + publicWarning, then
    // accepts/rejects it. Re-arms (up to a bound) until a modal is up.
    void armBadgeReader(QString* badgeOut, int* warnOut, bool accept, int tries = 60) {
        int gen = (tries == 60) ? ++_modalGen : _modalGen;   // bump on the initial arm
        QTimer::singleShot(tries > 50 ? 0 : 10, [=]() {
            if (gen != _modalGen) return;                    // superseded -> stop
            QWidget* dlg = QApplication::activeModalWidget();
            if (!dlg) {
                if (tries > 0) armBadgeReader(badgeOut, warnOut, accept, tries - 1);
                return;
            }
            auto* badge = dlg->findChild<QLabel*>("confirmPrivacyBadge");
            if (badge && badgeOut) *badgeOut = badge->text();
            auto* pw = dlg->findChild<QLabel*>("publicWarning");
            if (warnOut) *warnOut = (pw && !pw->isHidden()) ? 1 : 0;
            if (auto* qd = qobject_cast<QDialog*>(dlg)) { accept ? qd->accept() : qd->reject(); }
            else dlg->close();
        });
    }

    // Handle the confirm-then-ack sequence for the de-shield path. Polls active
    // modals: ACCEPTS the confirm dialog (the one with 'confirmPrivacyBadge'); when a
    // SUBSEQUENT message box appears WITHOUT that badge (the de-shield ack), records
    // that it was seen and answers it with ackAnswer. Self-rearming; stops after the
    // ack (or after accepting confirm if no ack follows, detected by a short idle).
    // Member state for the confirm-then-ack sequence (PRIV-12). Using members (not
    // stack pointers captured in a lambda) removes any dangling-pointer risk if a
    // poll fires after the test slot returns.
    bool _ackSeen = false;
    QMessageBox::StandardButton _ackAnswer = QMessageBox::No;
    bool _confirmAccepted = false;

    // Drive the confirm-then-ack sequence. Self-rearming singleShot poll:
    //   * ACCEPT the confirm dialog (the modal carrying 'confirmPrivacyBadge');
    //   * a de-shield then pops a SECOND modal that is a QMessageBox WITHOUT that
    //     badge (the ack) — set _ackSeen and answer it with _ackAnswer;
    //   * non-deshield categories pop NO second QMessageBox — _ackSeen stays false.
    void armAckHandler(QMessageBox::StandardButton ackAnswer) {
        _ackSeen = false;
        _confirmAccepted = false;
        _ackAnswer = ackAnswer;
        _ackGen = ++_modalGen;
        // `idleTries` counts ONLY polls where no modal is up; it is reset whenever a
        // modal is present (so we never give up while a dialog is still on screen).
        pollAck(60);
    }
    int _ackGen = 0;
    void pollAck(int idleTries) {
        const int gen = _ackGen;
        QTimer::singleShot(5, [this, idleTries, gen]() {
            if (gen != _modalGen) return;   // superseded by a newer arm -> stop
            QWidget* dlg = QApplication::activeModalWidget();
            if (!dlg) {
                // No modal up. After the confirm was accepted, a short idle with no
                // QMessageBox means the category had NO ack (the non-deshield cases).
                if (idleTries > 0) pollAck(idleTries - 1);
                return;
            }

            // The confirm dialog carries the privacy badge; accept it once.
            if (dlg->findChild<QLabel*>("confirmPrivacyBadge")) {
                if (!_confirmAccepted) {
                    _confirmAccepted = true;
                    if (auto* qd = qobject_cast<QDialog*>(dlg)) qd->accept();
                }
                pollAck(60);   // reset idle budget: a modal is/was up, keep watching
                return;
            }

            // A QMessageBox after the confirm dialog == the de-shield acknowledgement
            // (objectName "deshieldAck"). It is an explicit instance, so done() on it
            // reliably makes its exec() return the chosen StandardButton.
            if (_confirmAccepted) {
                if (auto* mb = qobject_cast<QMessageBox*>(dlg)) {
                    _ackSeen = true;
                    // Dismiss with the chosen StandardButton. Under the offscreen QPA a
                    // single done() can race the modal loop's start, so we re-arm and
                    // keep issuing done() until the box is gone (idempotent).
                    mb->setResult(_ackAnswer);
                    mb->done(_ackAnswer);
                    pollAck(60);
                    return;
                }
            }
            pollAck(60);       // some other modal up; keep watching (reset idle)
        });
    }

    // Arm a one-shot that finds the active modal confirm dialog, reads its
    // 'publicWarning' QLabel visibility into *out, then rejects the dialog so
    // the blocking exec() returns. Re-arms itself until the dialog is found.
    void armConfirmDismisser(int* out) {
        QTimer::singleShot(0, [out]() {
            QWidget* dlg = QApplication::activeModalWidget();
            if (!dlg) {
                // Dialog not up yet; poll again shortly.
                QTimer::singleShot(10, [out]() {
                    QWidget* d2 = QApplication::activeModalWidget();
                    if (d2) {
                        auto* pw = d2->findChild<QLabel*>("publicWarning");
                        *out = (pw && !pw->isHidden()) ? 1 : 0;
                        if (auto* qd = qobject_cast<QDialog*>(d2)) qd->reject();
                        else d2->close();
                    }
                });
                return;
            }
            auto* pw = dlg->findChild<QLabel*>("publicWarning");
            *out = (pw && !pw->isHidden()) ? 1 : 0;
            if (auto* qd = qobject_cast<QDialog*>(dlg)) qd->reject();
            else dlg->close();
        });
    }

    // MAJOR-1 / PRIV-27: a blocking QMessageBox (warning/information/critical static)
    // spins a nested modal loop INSIDE the synchronous testCreateTxFromSendPage()
    // call. These pollers fire from that nested loop to dismiss the box (so the call
    // returns) and, for PRIV-27, to COUNT how many appeared.

    // MINOR-1: install a sentinel (non-null but unused) Connection so the readiness
    // guard in shieldPublicFunds() is satisfied without a live daemon. The Connection
    // ctor just stores pointers; nothing on the tested path actually issues RPC.
    void installSentinelConnection(MainWindow* w) {
        auto cfg = std::make_shared<ConnectionConfig>();
        auto* c  = new Connection(w, new QNetworkAccessManager(), new QNetworkRequest(), cfg);
        w->getRPC()->testSetConnection(c);
    }

    // Count of message boxes seen+dismissed since the last armInfoCounter().
    int _infoSeen = 0;

    // Self-rearming poll: whenever a modal is up, dismiss it; for the counting variant
    // bump _infoSeen. Generation-guarded so a stale chain can't bleed across cases.
    void armInfoCounter(int tries = 200) {
        const int gen = (tries == 200) ? ++_modalGen : _modalGen;
        QTimer::singleShot(tries == 200 ? 0 : 3, [this, gen, tries]() {
            if (gen != _modalGen) return;            // superseded -> stop
            QWidget* dlg = QApplication::activeModalWidget();
            if (dlg) {
                ++_infoSeen;
                if (auto* mb = qobject_cast<QMessageBox*>(dlg)) { mb->done(QMessageBox::Ok); }
                else if (auto* qd = qobject_cast<QDialog*>(dlg)) { qd->reject(); }
                else dlg->close();
            }
            if (tries > 0) armInfoCounter(tries - 1);  // keep watching for more
        });
    }

    // Dismiss any single blocking modal (used by the fail-closed path so the warning
    // box doesn't wedge the synchronous create). Re-arms until one is seen+closed.
    void armModalDismisser(int tries = 200) {
        const int gen = (tries == 200) ? ++_modalGen : _modalGen;
        QTimer::singleShot(tries == 200 ? 0 : 3, [this, gen, tries]() {
            if (gen != _modalGen) return;
            QWidget* dlg = QApplication::activeModalWidget();
            if (dlg) {
                if (auto* mb = qobject_cast<QMessageBox*>(dlg)) { mb->done(QMessageBox::Ok); return; }
                if (auto* qd = qobject_cast<QDialog*>(dlg)) { qd->reject(); return; }
                dlg->close();
                return;
            }
            if (tries > 0) armModalDismisser(tries - 1);
        });
    }
#endif
};

int main(int argc, char* argv[]) {
    // SAFETY: isolate HOME *before* QApplication so QStandardPaths::HomeLocation
    // and QSettings never touch the real ~/.zclassic or the user's config.
    g_homeDir = new QTemporaryDir();
    if (g_homeDir->isValid()) {
        qputenv("HOME", g_homeDir->path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (g_homeDir->path() + "/.config").toLocal8Bit());
        qputenv("XDG_DATA_HOME",   (g_homeDir->path() + "/.local/share").toLocal8Bit());
        qputenv("XDG_CACHE_HOME",  (g_homeDir->path() + "/.cache").toLocal8Bit());
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ZClassicTest");
    QCoreApplication::setOrganizationDomain("test.zclassic.invalid");
    QCoreApplication::setApplicationName("tst_widget");

    // Never let a stray window-close quit mid-test.
    app.setQuitOnLastWindowClosed(false);

    Settings::init();
    Settings::getInstance()->setUseEmbedded(false);
    Settings::getInstance()->setHeadless(true);

    TestWidget tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_widget.moc"
