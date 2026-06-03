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
// ============================================================================
#include <QtTest/QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QRadioButton>
#include <QLabel>
#include <QPushButton>
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

    // ---- D1: rdioTAddr -> red PUBLIC caption on lblSproutWarning -----------
    void d1_tAddrShowsPublicWarning() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // NOTE on visibility: lblSproutWarning lives on the Receive tab (page 2),
        // which is NOT the current page, so effective isVisible() is always false
        // for a widget on a non-current tab. The behavior under test is whether
        // setupRecieveTab() EXPLICITLY un-hides the label, which is exactly what
        // !isHidden() reports (the widget's own visible flag, independent of
        // whether its parent page/window is currently mapped).

        // Precondition: warning hidden after construction.
        QVERIFY(ui->lblSproutWarning->isHidden());

        ui->rdioTAddr->setChecked(true);

        QVERIFY2(!ui->lblSproutWarning->isHidden(),
                 "selecting rdioTAddr must reveal the PUBLIC warning label");

        const QString t = ui->lblSproutWarning->text();
        QVERIFY2(t.contains("Transparent address.", Qt::CaseSensitive),
                 qPrintable("warning text missing 'Transparent address.': " + t));
        QVERIFY2(t.contains("PUBLIC", Qt::CaseSensitive),
                 qPrintable("warning text missing 'PUBLIC': " + t));
        QVERIFY2(t.contains("permanently visible", Qt::CaseSensitive),
                 qPrintable("warning text missing 'permanently visible': " + t));

        delete w;
    }

    // ---- D3: under rdioZAddr the New-Address button is DISABLED ------------
    void d3_zAddrDisablesNewAddressButton() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        ui->rdioZAddr->setChecked(true);

        QVERIFY2(!ui->btnRecieveNewAddr->isEnabled(),
                 "Sprout (rdioZAddr) must DISABLE the new-address button (backdoor removed)");

        // And the Sapling radio re-enables it (belt-and-suspenders).
        ui->rdioZSAddr->setChecked(true);
        QVERIFY2(ui->btnRecieveNewAddr->isEnabled(),
                 "selecting Sapling (rdioZSAddr) must re-enable the new-address button");

        delete w;
    }

    // ---- D5: rdioTAddr then rdioZSAddr CLEARS the warning ------------------
    void d5_saplingClearsTransparentWarning() {
        MainWindow* w = makeWindow();
        auto* ui = w->ui;

        // Show the transparent PUBLIC warning first (label on the non-current
        // Receive page, so assert via !isHidden() — see d1's note).
        ui->rdioTAddr->setChecked(true);
        QVERIFY(!ui->lblSproutWarning->isHidden());

        // Switching to shielded Sapling must clear it (no scary caption on a
        // private address).
        ui->rdioZSAddr->setChecked(true);
        QVERIFY2(ui->lblSproutWarning->isHidden(),
                 "switching rdioTAddr -> rdioZSAddr must HIDE the warning label");

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
