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
#include <QWidget>
#include <QDialog>
#include <QTimer>
#include <QSettings>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "rpc.h"
#include "settings.h"
#include "connection.h"

// Kept alive for the whole process so QStandardPaths/QSettings stay isolated.
static QTemporaryDir* g_homeDir = nullptr;

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
#endif

private:
#ifdef ZCL_WIDGET_TEST

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
