#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "addressbook.h"
#include "ui_confirm.h"
#include "ui_memodialog.h"
#include "ui_newrecurring.h"
#include "settings.h"
#include "rpc.h"
#include "recurring.h"

using json = nlohmann::json;

void MainWindow::setupSendTab() {
    // Create the validator for send to/amount fields
    auto amtValidator = new QRegExpValidator(QRegExp("[0-9]{0,8}\\.?[0-9]{0,8}"));    
    ui->Amount1->setValidator(amtValidator);

    // Send button
    QObject::connect(ui->sendTransactionButton, &QPushButton::clicked, this, &MainWindow::sendButton);

    // Cancel Button
    QObject::connect(ui->cancelSendButton, &QPushButton::clicked, this, &MainWindow::cancelButton);

    // Input Combobox current text changed
    QObject::connect(ui->inputsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &MainWindow::inputComboTextChanged);

    // Hook up add address button click
    QObject::connect(ui->addAddressButton, &QPushButton::clicked, this, &MainWindow::addAddressSection);

    // Max available Checkbox
    QObject::connect(ui->Max1, &QCheckBox::stateChanged, this, &MainWindow::maxAmountChecked);

    // The first Address button
    QObject::connect(ui->Address1, &QLineEdit::textChanged, [=] (auto text) {
        this->addressChanged(1, text);
    });

    // The first Memo button
    QObject::connect(ui->MemoBtn1, &QPushButton::clicked, [=] () {
        this->memoButtonClicked(1);
    });
    setMemoEnabled(1, false);
        
    // This is the damnest thing ever. If we do AddressBook::readFromStorage() directly, the whole file
    // doesn't get read. It needs to run in a timer after everything has finished to be able to read
    // the file properly.
    // `this` is the receiver/context object so Qt CANCELS this 2s callback if the
    // MainWindow is destroyed first — otherwise an orphaned singleShot fires
    // updateLabelsAutoComplete() on freed memory (crash on a fast quit during the
    // first 2s of startup; surfaced as a use-after-free under the test event loop).
    QTimer::singleShot(2000, this, [this]() { updateLabelsAutoComplete(); });

    // The first address book button
    QObject::connect(ui->AddressBook1, &QPushButton::clicked, [=] () {
        AddressBook::open(this, ui->Address1);
    });

    // The first Amount button
    QObject::connect(ui->Amount1, &QLineEdit::textChanged, [=] (auto text) {
        this->amountChanged(1, text);
    });

    // Fee amount changed
    // Disable custom fees if settings say no
    ui->minerFeeAmt->setReadOnly(!Settings::getInstance()->getAllowCustomFees());
    QObject::connect(ui->minerFeeAmt, &QLineEdit::textChanged, [=](auto txt) {
        ui->lblMinerFeeUSD->setText(Settings::getUSDFormat(txt.toDouble()));
    });
    ui->minerFeeAmt->setText(Settings::getDecimalString(Settings::getMinerFee()));    

     // Set up focus enter to set fees
    QObject::connect(ui->tabWidget, &QTabWidget::currentChanged, [=] (int pos) {
        if (pos == 1) {
            QString txt = ui->minerFeeAmt->text();
            ui->lblMinerFeeUSD->setText(Settings::getUSDFormat(txt.toDouble()));
        }
    });
    //Fees validator
    auto feesValidator = new QRegExpValidator(QRegExp("[0-9]{0,8}\\.?[0-9]{0,8}")); 
    ui->minerFeeAmt->setValidator(feesValidator);

    // Font for the first Memo label
    QFont f = ui->Address1->font();
    f.setPointSize(f.pointSize() - 1);
    ui->MemoTxt1->setFont(f);

    // Recurring button
    QObject::connect(ui->chkRecurring, &QCheckBox::stateChanged, [=] (int checked) {
        if (checked) {
            ui->btnRecurSchedule->setEnabled(true);            
        } else {
            ui->btnRecurSchedule->setEnabled(false);
            ui->lblRecurDesc->setText("");
        }

    });

    // Recurring schedule button
    QObject::connect(ui->btnRecurSchedule, &QPushButton::clicked, this, &MainWindow::editSchedule);

    // Hide the recurring section for now
    ui->chkRecurring->setVisible(false);
    ui->lblRecurDesc->setVisible(false);
    ui->btnRecurSchedule->setVisible(false);

    // Set the default state for the whole page
    removeExtraAddresses();
}

void MainWindow::editSchedule() {
    // Open the edit schedule dialog
    Recurring::showEditDialog(this, this, createTxFromSendPage());

}

void MainWindow::updateLabelsAutoComplete() {
    QList<QString> list;
    auto labels = AddressBook::getInstance()->getAllAddressLabels();
    
    std::transform(labels.begin(), labels.end(), std::back_inserter(list), [=] (auto la) -> QString {
        return la.first % "/" % la.second;
    });
    
    delete labelCompleter;
    labelCompleter = new QCompleter(list, this);
    labelCompleter->setCaseSensitivity(Qt::CaseInsensitive);

    // Then, find all the address fields and update the completer.
    QRegExp re("Address[0-9]+", Qt::CaseInsensitive);
    for (auto target: ui->sendToWidgets->findChildren<QLineEdit *>(re)) {
        target->setCompleter(labelCompleter);
    }
}

void MainWindow::setDefaultPayFrom() {
    auto findMax = [=] (QString startsWith) {
        double max_amt = 0;
        int    idx     = -1;

        for (int i=0; i < ui->inputsCombo->count(); i++) {
            auto addr = ui->inputsCombo->itemText(i);
            if (addr.startsWith(startsWith)) {
                auto amt = rpc->getAllBalances()->value(addr);
                if (max_amt < amt) {
                    max_amt = amt;
                    idx = i;
                }
            }                
        }

        return idx;
    };

    // By default, select the z-address with the most balance from the inputs combo
    auto maxZ = findMax("z");
    if (maxZ >= 0) {
        ui->inputsCombo->setCurrentIndex(maxZ);                
    } else {
        auto maxT = findMax("t");
        maxT  = maxT >= 0 ? maxT : 0;
        ui->inputsCombo->setCurrentIndex(maxT);
    }
};

void MainWindow::updateFromCombo() {
    if (!rpc || !rpc->getAllBalances())
        return;

    auto lastFromAddr = ui->inputsCombo->currentText();

    ui->inputsCombo->clear();
    auto i = rpc->getAllBalances()->constBegin();

    // Add all the addresses into the inputs combo box
    while (i != rpc->getAllBalances()->constEnd()) {
        ui->inputsCombo->addItem(i.key(), i.value());
        if (i.key() == lastFromAddr) ui->inputsCombo->setCurrentText(i.key());

        ++i;
    }

    if (lastFromAddr.isEmpty()) {
        setDefaultPayFrom();
    }
    else {
        ui->inputsCombo->setCurrentText(lastFromAddr);
    }
}

void MainWindow::inputComboTextChanged(int index) {
    auto addr   = ui->inputsCombo->itemText(index);
    auto bal    = rpc->getAllBalances()->value(addr);
    auto balFmt = Settings::getZCLDisplayFormat(bal);

    ui->sendAddressBalance->setText(balFmt);
    ui->sendAddressBalanceUSD->setText(Settings::getUSDFormat(bal));
}

    
void MainWindow::addAddressSection() {
    int itemNumber = ui->sendToWidgets->children().size() - 1;

    auto verticalGroupBox = new QGroupBox(ui->sendToWidgets);
    verticalGroupBox->setTitle(QString(tr("Recipient ")) % QString::number(itemNumber));
    verticalGroupBox->setObjectName(QString("AddressGroupBox") % QString::number(itemNumber));
    auto sendAddressLayout = new QVBoxLayout(verticalGroupBox);
    sendAddressLayout->setSpacing(6);
    sendAddressLayout->setContentsMargins(11, 11, 11, 11);

    auto horizontalLayout_12 = new QHBoxLayout();
    horizontalLayout_12->setSpacing(6);
    auto label_4 = new QLabel(verticalGroupBox);
    label_4->setText(tr("Address"));
    horizontalLayout_12->addWidget(label_4);

    auto Address1 = new QLineEdit(verticalGroupBox);
    Address1->setObjectName(QString("Address") % QString::number(itemNumber)); 
    Address1->setPlaceholderText(tr("Address"));
    QObject::connect(Address1, &QLineEdit::textChanged, [=] (auto text) {
        this->addressChanged(itemNumber, text);
    });
    Address1->setCompleter(labelCompleter);

    horizontalLayout_12->addWidget(Address1);

    auto addressBook1 = new QPushButton(verticalGroupBox);
    addressBook1->setObjectName(QStringLiteral("AddressBook") % QString::number(itemNumber));
    addressBook1->setText(tr("Address Book"));
    QObject::connect(addressBook1, &QPushButton::clicked, [=] () {
        AddressBook::open(this, Address1);
    });

    horizontalLayout_12->addWidget(addressBook1);

    sendAddressLayout->addLayout(horizontalLayout_12);

    auto horizontalLayout_13 = new QHBoxLayout();
    horizontalLayout_13->setSpacing(6);
        
    auto label_6 = new QLabel(verticalGroupBox);
    label_6->setText(tr("Amount"));
    horizontalLayout_13->addWidget(label_6);

    auto Amount1 = new QLineEdit(verticalGroupBox);
    Amount1->setPlaceholderText(tr("Amount"));    
    Amount1->setObjectName(QString("Amount") % QString::number(itemNumber));   
    Amount1->setBaseSize(QSize(200, 0));
    Amount1->setAlignment(Qt::AlignRight);    
    // Create the validator for send to/amount fields
    auto amtValidator = new QRegExpValidator(QRegExp("[0-9]{0,8}\\.?[0-9]{0,8}")); 
    Amount1->setValidator(amtValidator);
    QObject::connect(Amount1, &QLineEdit::textChanged, [=] (auto text) {
        this->amountChanged(itemNumber, text);
    });

    horizontalLayout_13->addWidget(Amount1);

    auto AmtUSD1 = new QLabel(verticalGroupBox);
    AmtUSD1->setObjectName(QString("AmtUSD") % QString::number(itemNumber));   
    horizontalLayout_13->addWidget(AmtUSD1);

    auto horizontalSpacer_4 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    horizontalLayout_13->addItem(horizontalSpacer_4);

    auto MemoBtn1 = new QPushButton(verticalGroupBox);
    MemoBtn1->setObjectName(QString("MemoBtn") % QString::number(itemNumber));
    MemoBtn1->setText(tr("Memo"));    
    // Connect Memo Clicked button
    QObject::connect(MemoBtn1, &QPushButton::clicked, [=] () {
        this->memoButtonClicked(itemNumber);
    });
    horizontalLayout_13->addWidget(MemoBtn1);
    setMemoEnabled(itemNumber, false);

    sendAddressLayout->addLayout(horizontalLayout_13);

    auto MemoTxt1 = new QLabel(verticalGroupBox);
    MemoTxt1->setObjectName(QString("MemoTxt") % QString::number(itemNumber));
    QFont font1 = Address1->font();
    font1.setPointSize(font1.pointSize()-1);
    MemoTxt1->setFont(font1);
    MemoTxt1->setWordWrap(true);
    sendAddressLayout->addWidget(MemoTxt1);

    ui->sendToLayout->insertWidget(itemNumber-1, verticalGroupBox);         

    // Set focus into the address
    Address1->setFocus();

    // Delay the call to scroll to allow the scroll window to adjust. `this` is the
    // context object so Qt cancels the callback if the MainWindow is destroyed
    // within 10ms (same orphaned-singleShot UAF class as the updateLabelsAutoComplete
    // fix above — captures `ui`, which dies with the window).
    QTimer::singleShot(10, this, [this] () {ui->sendToScrollArea->ensureWidgetVisible(ui->addAddressButton);});
}

void MainWindow::addressChanged(int itemNumber, const QString& text) {
    auto addr = AddressBook::addressFromAddressLabel(text);
    setMemoEnabled(itemNumber, addr.startsWith("z"));

    // Live validation affordance: green border the instant a typed/resolved address
    // parses, red when it clearly won't, neutral while empty — so an invalid address
    // is caught as you type, not as a surprise failure at Send. isValidAddress is
    // static and touches no daemon, so it is safe on every keystroke. Single writer
    // of the field's "validation" property; the qss border rules do the rest.
    auto* fld = ui->sendToWidgets->findChild<QLineEdit*>(QString("Address") + QString::number(itemNumber));
    if (fld) {
        const char* state = text.trimmed().isEmpty() ? "empty"
                          : (Settings::isValidAddress(addr) ? "valid" : "invalid");
        if (fld->property("validation").toString() != QString(state)) {
            fld->setProperty("validation", state);
            fld->style()->unpolish(fld);   // property selectors need a re-polish
            fld->style()->polish(fld);
        }
    }
}

void MainWindow::amountChanged(int item, const QString& text) {
    auto usd = ui->sendToWidgets->findChild<QLabel*>(QString("AmtUSD") % QString::number(item));
    usd->setText(Settings::getUSDFormat(text.toDouble()));
}

void MainWindow::setMemoEnabled(int number, bool enabled) {
    auto memoBtn = ui->sendToWidgets->findChild<QPushButton*>(QString("MemoBtn") % QString::number(number));
     if (enabled) {
        memoBtn->setEnabled(true);
        memoBtn->setToolTip("");
    } else {
        memoBtn->setEnabled(false);
        memoBtn->setToolTip(tr("Only z-addresses can have memos"));
    }
}

void MainWindow::memoButtonClicked(int number, bool includeReplyTo) {
    // Memos can only be used with zAddrs. So check that first
    auto addr = ui->sendToWidgets->findChild<QLineEdit*>(QString("Address") + QString::number(number));
    if (!AddressBook::addressFromAddressLabel(addr->text()).startsWith("z")) {
        QMessageBox msg(QMessageBox::Critical, tr("Memos can only be used with z-addresses"),
        tr("The memo field can only be used with a z-address.\n") + addr->text() + tr("\ndoesn't look like a z-address"),
        QMessageBox::Ok, this);

        msg.exec();
        return;
    }

    // Get the current memo if it exists
    auto memoTxt = ui->sendToWidgets->findChild<QLabel *>(QString("MemoTxt") + QString::number(number));
    QString currentMemo = memoTxt->text();

    Ui_MemoDialog memoDialog;
    QDialog dialog(this);
    memoDialog.setupUi(&dialog);
    Settings::saveRestore(&dialog);

    QObject::connect(memoDialog.memoTxt, &QPlainTextEdit::textChanged, [=] () {
        QString txt = memoDialog.memoTxt->toPlainText();
        memoDialog.memoSize->setText(QString::number(txt.toUtf8().size()) + "/512");

        if (txt.toUtf8().size() <= 512) {
            // Everything is fine
            memoDialog.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
            memoDialog.memoSize->setStyleSheet("");
        }
        else {
           // Overweight
            memoDialog.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
            memoDialog.memoSize->setStyleSheet("color: red;");
        }
        
    });

    auto fnAddReplyTo = [=, &dialog]() {
        QString replyTo = ui->inputsCombo->currentText();
        if (!Settings::isZAddress(replyTo)) {
            replyTo = rpc->getDefaultSaplingAddress();
            if (replyTo.isEmpty())
                return;
        }
        auto curText = memoDialog.memoTxt->toPlainText();
        if (curText.endsWith(replyTo))
            return;

        memoDialog.memoTxt->setPlainText(curText + "\n" + tr("Reply to") + ":\n" + replyTo);

        // MacOS has a really annoying bug where the Plaintext doesn't refresh when the content is
        // updated. So we do this ugly hack - resize the window slightly to force it to refresh
        dialog.setGeometry(dialog.geometry().adjusted(0,0,0,1));
        dialog.setGeometry(dialog.geometry().adjusted(0,0,0,-1));
    };

    // Insert From Address button. Including a reply address writes the sender's OWN
    // z-address into the encrypted memo -- only the recipient can read it, but it
    // de-anonymizes the sender to that recipient and links the payment to their
    // shielded address. Educate always (tooltip) and require explicit one-time
    // consent on the FIRST manual click (persisted, never nags again).
    memoDialog.btnInsertFrom->setToolTip(tr(
        "Writes your z-address into the encrypted memo so the recipient can reply.\n"
        "This reveals your identity to that recipient and links this payment to\n"
        "your shielded address."));
    QObject::connect(memoDialog.btnInsertFrom, &QPushButton::clicked, [=, &dialog]() {
        if (!QSettings().value("memo/replyToWarned", false).toBool()) {
            auto proceed = QMessageBox::question(&dialog, tr("Reveal your address to the recipient?"),
                tr("Including a reply address writes <b>your own z-address</b> into the "
                   "encrypted memo. Only the recipient can read it, but it reveals your "
                   "identity to them and lets them link this payment to your shielded "
                   "address.<br><br>Include your reply address anyway?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (proceed != QMessageBox::Yes)
                return;
            QSettings().setValue("memo/replyToWarned", true);
        }
        fnAddReplyTo();
    });

    memoDialog.memoTxt->setPlainText(currentMemo);
    memoDialog.memoTxt->setFocus();

    // Programmatic reply-to (e.g. recurring/scheduled sends): never block on a dialog
    // -- the persistent tooltip + one-time manual consent cover the interactive case.
    if (includeReplyTo)
        fnAddReplyTo();

    if (dialog.exec() == QDialog::Accepted) {
        memoTxt->setText(memoDialog.memoTxt->toPlainText());
    }
}

void MainWindow::removeExtraAddresses() {
    // The last one is a spacer, so ignore that
    int totalItems = ui->sendToWidgets->children().size() - 2; 

    // Clear the first recipient fields
    auto addr = ui->sendToWidgets->findChild<QLineEdit*>(QString("Address1"));
    addr->clear();
    auto amt  = ui->sendToWidgets->findChild<QLineEdit*>(QString("Amount1"));
    amt->clear();
    auto amtUSD  = ui->sendToWidgets->findChild<QLabel*>(QString("AmtUSD1"));
    amtUSD->clear();
    auto max  = ui->sendToWidgets->findChild<QCheckBox*>(QString("Max1"));
    max->setChecked(false);
    auto memo = ui->sendToWidgets->findChild<QLabel*>(QString("MemoTxt1"));
    memo->clear();

    // Disable first memo btn
    setMemoEnabled(1, false);

    // Reset the fee
    ui->minerFeeAmt->setText(Settings::getDecimalString(Settings::getMinerFee()));

    // Start the deletion after the first item, since we want to keep 1 send field there all there
    for (int i=1; i < totalItems; i++) {
        auto addressGroupBox = ui->sendToWidgets->findChild<QGroupBox*>(QString("AddressGroupBox") % QString::number(i+1));
            
        delete addressGroupBox;
    }    

    // Reset the recurring button
    ui->chkRecurring->setCheckState(Qt::Unchecked);
    ui->btnRecurSchedule->setEnabled(false);
    ui->lblRecurDesc->setText("");
}

void MainWindow::maxAmountChecked(int checked) {
    if (checked == Qt::Checked) {
        ui->Amount1->setReadOnly(true);
        if (rpc->getAllBalances() == nullptr) return;
           
        // Calculate maximum amount
        double sumAllAmounts = 0.0;
        // Calculate all other amounts
        int totalItems = ui->sendToWidgets->children().size() - 2;   // The last one is a spacer, so ignore that        
        // Start counting the sum skipping the first one, because the MAX button is on the first one, and we don't
        // want to include it in the sum. 
        for (int i=1; i < totalItems; i++) {
            auto amt  = ui->sendToWidgets->findChild<QLineEdit*>(QString("Amount")  % QString::number(i+1));
            sumAllAmounts += amt->text().toDouble();
        }

        if (Settings::getInstance()->getAllowCustomFees()) {
            // += (not =): must ADD the custom fee to the other recipients' amounts
            // already summed above, else MAX is computed too large (it ignores the
            // other outputs) and the send fails with a confusing "Not enough funds".
            sumAllAmounts += ui->minerFeeAmt->text().toDouble();
        }
        else {
            sumAllAmounts += Settings::getMinerFee();
        }

        auto addr = ui->inputsCombo->currentText();

        auto maxamount  = rpc->getAllBalances()->value(addr) - sumAllAmounts;
        maxamount       = (maxamount < 0) ? 0 : maxamount;
            
        ui->Amount1->setText(Settings::getDecimalString(maxamount));
    } else if (checked == Qt::Unchecked) {
        // Just remove the readonly part, don't change the content
        ui->Amount1->setReadOnly(false);
    }
}

// PRIV-11 / UX-12 — pure four-way classifier. The actual body is the single
// source of truth `sendCategoryOf()` in sendcategory.h, which both production and
// the L0 unit suite link directly (no hand-copied mirror). This member just
// forwards so existing callers (confirmTx) and the static seam keep working.
SendCategory MainWindow::classifySend(const Tx& tx) {
    return sendCategoryOf(tx);
}

// Create a Tx from the current state of the send page.
Tx MainWindow::createTxFromSendPage() {
    Tx tx;

    bool sendChangeToSapling = Settings::getInstance()->getAutoShield();

    // Gather the from / to addresses
    tx.fromAddr = ui->inputsCombo->currentText();
    sendChangeToSapling = sendChangeToSapling && Settings::isTAddress(tx.fromAddr);

    // For each addr/amt in the sendTo tab
    int totalItems = ui->sendToWidgets->children().size() - 2;   // The last one is a spacer, so ignore that
    double totalAmt = 0;
    bool   sproutRecipient = false;
    for (int i=0; i < totalItems; i++) {
        QString addr = ui->sendToWidgets->findChild<QLineEdit*>(QString("Address") % QString::number(i+1))->text().trimmed();
        // Remove label if it exists
        addr = AddressBook::addressFromAddressLabel(addr);

        // If address is sprout, then we can't send change to sapling, because of turnstile.
        if (Settings::getInstance()->isSproutAddress(addr))
            sproutRecipient = true;
        sendChangeToSapling = sendChangeToSapling && !Settings::getInstance()->isSproutAddress(addr);

        double  amt  = ui->sendToWidgets->findChild<QLineEdit*>(QString("Amount")  % QString::number(i+1))->text().trimmed().toDouble();
        totalAmt += amt;
        QString memo = ui->sendToWidgets->findChild<QLabel*>(QString("MemoTxt")  % QString::number(i+1))->text().trimmed();

        tx.toAddrs.push_back( ToFields{addr, amt, memo, memo.toUtf8().toHex()} );
    }

    if (Settings::getInstance()->getAllowCustomFees()) {
        tx.fee = ui->minerFeeAmt->text().toDouble();
    }
    else {
        tx.fee = Settings::getMinerFee();
    }

    // PRIV-27 — for a Sprout recipient, Sapling change is suppressed (turnstile
    // forbids mixing), so the change cannot be shielded to Sapling on THIS tx.
    // Surface that constraint to the user (once) rather than silently leaving the
    // change transparent. We only nag when auto-shield is ON and the from-addr is
    // transparent (the only case where change WOULD otherwise be shielded).
    if (Settings::getInstance()->getAutoShield() && Settings::isTAddress(tx.fromAddr)
            && sproutRecipient && !sproutChangeWarned) {
        sproutChangeWarned = true;
        QMessageBox::information(this, tr("Change cannot be shielded for this send"),
            tr("Because a recipient is a legacy Sprout (zc...) z-address, this wallet "
               "cannot route the transparent change to a shielded Sapling address on the "
               "same transaction (the network forbids mixing Sprout and Sapling). Any "
               "change from this send will remain on a PUBLIC transparent address.\n\n"
               "To keep everything private, send to a Sapling (zs...) recipient instead, "
               "or migrate your Sprout funds to Sapling first."),
            QMessageBox::Ok);
    }

    // PRIV-10 / PRIV-19 / PRIV-28 — auto-shield, FAIL CLOSED on privacy. With
    // auto-shield ON and a transparent from-addr, the change MUST be routed to a
    // Sapling z-address. If a usable Sapling address exists we use it; if NONE
    // exists we auto-create one (synchronous newZaddr(true) -- the last-resort
    // fallback per DECISION #2; the common case is covered by
    // ensureSaplingProvisioned() at first funding). If a Sapling change address
    // STILL cannot be obtained, we do NOT proceed silently (z_sendmany would then
    // route the change to a PUBLIC t-address) -- we ABORT the send and return an
    // INVALID Tx. Fail CLOSED, never open.
    if (Settings::getInstance()->getAutoShield() && sendChangeToSapling) {
        QString changeAddr = findUnusedSaplingChangeAddr(tx);

        // No existing usable Sapling address -> create one NOW. This is a blocking
        // RPC, but only on the (rare) first-ever send before any Sapling key exists.
        if (changeAddr.isEmpty())
            changeAddr = createSaplingAddressSync();

        if (changeAddr.isEmpty()) {
            // MAJOR-1 fail-closed: we could not obtain a Sapling change sink, so the
            // change would otherwise stay PUBLIC. Surface a blocking warning (mirrors
            // shieldPublicFunds()'s style) and abort by returning an invalid Tx.
            QMessageBox::warning(this, tr("Could not shield"),
                tr("Could not shield — a Sapling change address could not be created, "
                   "so your change would stay PUBLIC. Please try again in a moment."),
                QMessageBox::Ok);
            return Tx();   // invalid: empty fromAddr -> doSendTxValidations aborts the send
        }

        // MAJOR-2 funds-safety: base the change on the CONFIRMED, spendable balance of
        // the from-addr. getAllBalances() SUMS unconfirmed (confirmations==0) utxos,
        // but z_sendmany spends ONLY confirmed notes, so a change computed from the
        // unconfirmed-inclusive total would emit an UNFUNDABLE change output and the
        // daemon would reject the whole tx with insufficient-funds. If the confirmed
        // spendable balance can't cover amount+fee, surface the existing not-enough-
        // funds message UP FRONT and abort rather than emitting an unfundable change.
        const double confirmed = confirmedSpendableBalance(tx.fromAddr);
        if (confirmed < totalAmt + tx.fee) {
            QMessageBox::critical(this, tr("Transaction Error"),
                tr("Not enough confirmed funds. You're trying to send %1 (including the "
                   "fee), but this address only holds %2 in confirmed, spendable funds. "
                   "Wait for your incoming funds to confirm and try again.")
                   .arg(Settings::getZCLDisplayFormat(totalAmt + tx.fee))
                   .arg(Settings::getZCLDisplayFormat(confirmed)),
                QMessageBox::Ok);
            return Tx();   // invalid: abort the send
        }

        double change = confirmed - totalAmt - tx.fee;
        if (Settings::getDecimalString(change) != "0") {
            QString changeMemo = tr("Change from ") + tx.fromAddr;
            tx.toAddrs.push_back(ToFields{ changeAddr, change, changeMemo, changeMemo.toUtf8().toHex() });
        }
    }

    return tx;
}

// MAJOR-2 — sum the CONFIRMED (confirmations > 0), spendable UTXOs the GUI already
// holds for `addr`. z_sendmany only spends confirmed notes, so this (not the
// unconfirmed-inclusive getAllBalances() total) is the correct basis for the
// auto-shield change amount. Null-safe: returns 0 if the UTXO set isn't loaded yet.
double MainWindow::confirmedSpendableBalance(const QString& addr) {
    if (!rpc || !rpc->getUTXOs())
        return 0.0;
    double sum = 0.0;
    for (const UnspentOutput& u : *rpc->getUTXOs()) {
        if (u.address == addr && u.confirmations > 0 && u.spendable)
            sum += u.amount.toDouble();
    }
    return sum;
}

// Find an existing Sapling z-address usable as the change sink: it must be a real
// Sapling address AND not collide with any recipient (zclassic rejects a duplicate
// output address). Returns "" if none exists. Factored out of createTxFromSendPage
// so the fail-open create path is a clean single branch.
QString MainWindow::findUnusedSaplingChangeAddr(const Tx& tx) {
    if (!rpc || !rpc->getAllZAddresses())
        return QString();
    for (const QString& a : *rpc->getAllZAddresses()) {
        if (!Settings::getInstance()->isSaplingAddress(a))
            continue;
        bool collides = false;
        for (const auto& t : tx.toAddrs)
            if (t.addr == a) { collides = true; break; }
        if (!collides)
            return a;
    }
    return QString();
}

// PRIV-10 fail-open last resort: synchronously create a fresh Sapling z-address via
// the daemon and return it. Blocks the GUI thread briefly; only reached when NO
// Sapling address exists at send time (the eager ensureSaplingProvisioned() path
// normally pre-creates one at first funding). Returns "" if creation fails (so the
// caller degrades gracefully rather than crashing) -- but NEVER returns a non-Sapling
// address, so change is never routed to Sprout/transparent.
QString MainWindow::createSaplingAddressSync() {
    if (!rpc)
        return QString();

    // MINOR-3 reentrancy guard: the event-loop pump below keeps the app live, so a
    // second sendButton click (or any code path) could re-enter this blocking
    // create. Refuse to nest -- a single in-flight create at a time.
    if (saplingSyncCreateInFlight)
        return QString();
    saplingSyncCreateInFlight = true;
    // RAII-clear on every return path (early-outs below included).
    struct ClearGuard { bool* f; ~ClearGuard() { *f = false; } } _clr{ &saplingSyncCreateInFlight };

    QString created;
    rpc->newZaddr(true, [&](json reply) {
        if (reply.is_string())
            created = QString::fromStdString(reply.get<json::string_t>());
    });

    // newZaddr is async on the network thread; pump the event loop briefly so the
    // reply lands before we return. Bounded so a dead daemon can never wedge a send.
    // ExcludeUserInputEvents (MINOR-3): suppress mouse/key delivery during the pump
    // so a second sendButton click can't be processed mid-create. We only pump while
    // a connection actually exists -- with no daemon connection the async create can
    // never complete, so pumping would just spin to the timeout for nothing (and the
    // caller will fail CLOSED on the empty result).
    QElapsedTimer t; t.start();
    while (created.isEmpty() && rpc->getConnection() && t.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }

    if (!created.isEmpty()) {
        // Make the new address available to the rest of the wallet immediately. Only
        // when a connection exists -- a successful create implies one; calling
        // refreshAddresses() with no connection just bounces to noConnection() (which
        // tears down the live UI state), so skip it on that impossible-in-prod path.
        if (rpc->getConnection())
            rpc->refreshAddresses();
        // Defensive: only ever return a real Sapling address (never degrade).
        if (!Settings::getInstance()->isSaplingAddress(created))
            return QString();
    }
    return created;
}

bool MainWindow::confirmTx(Tx tx) {
    auto fnSplitAddressForWrap = [=] (const QString& a) -> QString {
        if (!a.startsWith("z")) return a;

        auto half = a.length() / 2;
        auto splitted = a.left(half) + "\n" + a.right(a.length() - half);
        return splitted;
    };


    // Show a confirmation dialog
    QDialog d(this);
    Ui_confirm confirm;
    confirm.setupUi(&d);

    // Remove all existing address/amt qlabels on the confirm dialog.
    int totalConfirmAddrItems = confirm.sendToAddrs->children().size();
    for (int i = 0; i < totalConfirmAddrItems / 3; i++) {
        auto addr   = confirm.sendToAddrs->findChild<QLabel*>(QString("Addr")   % QString::number(i+1));
        auto amt    = confirm.sendToAddrs->findChild<QLabel*>(QString("Amt")    % QString::number(i+1));
        auto memo   = confirm.sendToAddrs->findChild<QLabel*>(QString("Memo")   % QString::number(i+1));
        auto amtUSD = confirm.sendToAddrs->findChild<QLabel*>(QString("AmtUSD") % QString::number(i+1));
        auto spacer = confirm.sendToAddrs->findChild<QLabel*>(QString("spacer") % QString::number(i+1));

        delete memo;
        delete addr;
        delete amt;
        delete amtUSD;
        delete spacer;
    }

    // Remove the fee labels
    delete confirm.sendToAddrs->findChild<QLabel*>("labelMinerFee");
    delete confirm.sendToAddrs->findChild<QLabel*>("minerFee");
    delete confirm.sendToAddrs->findChild<QLabel*>("minerFeeUSD");
    
    // PRIV-11 / UX-12 — four-way, from-aware privacy classification. The legacy
    // binary isPublicTx is gone; the category drives BOTH the publicWarning copy and
    // the dialog title/badge below, and (PRIV-12) whether the z->t acknowledgement
    // gate fires before the send.
    const SendCategory category = classifySend(tx);

    // For each addr/amt/memo, construct the JSON and also build the confirm dialog box
    int row = 0;
    double totalSpending = 0;

    // NIT-2 — recognize the auto-shield CHANGE output so we can label it as such in
    // the confirm dialog (rather than showing a bare zs... recipient that looks like
    // an unexpected payee). The change row is the Sapling output whose memo is the
    // change memo createTxFromSendPage() stamped: tr("Change from ") + fromAddr.
    const QString changeMemoMarker = tr("Change from ") + tx.fromAddr;

    for (int i=0; i < tx.toAddrs.size(); i++) {
        auto toAddr = tx.toAddrs[i];

        const bool isAutoShieldChange =
            Settings::getInstance()->getAutoShield()
            && Settings::isTAddress(tx.fromAddr)
            && Settings::getInstance()->isSaplingAddress(toAddr.addr)
            && toAddr.txtMemo == changeMemoMarker;

        // Add new Address widgets instead of the same one.
        {
            // Address
            auto Addr = new QLabel(confirm.sendToAddrs);
            Addr->setObjectName(QString("Addr") % QString::number(i + 1));
            Addr->setWordWrap(true);
            // NIT-2: a clear, reassuring label for the auto-shielded change row.
            if (isAutoShieldChange)
                Addr->setText(tr("Change (auto-shielded to your wallet)"));
            else
                Addr->setText(fnSplitAddressForWrap(toAddr.addr));
            confirm.gridLayout->addWidget(Addr, row, 0, 1, 1);

            // Amount (ZCL)
            auto Amt = new QLabel(confirm.sendToAddrs);
            Amt->setObjectName(QString("Amt") % QString::number(i + 1));
            Amt->setText(Settings::getZCLDisplayFormat(toAddr.amount));
            Amt->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
            confirm.gridLayout->addWidget(Amt, row, 1, 1, 1);
            totalSpending += toAddr.amount;

            // Amount (USD)
            auto AmtUSD = new QLabel(confirm.sendToAddrs);
            AmtUSD->setObjectName(QString("AmtUSD") % QString::number(i + 1));
            AmtUSD->setText(Settings::getUSDFormat(toAddr.amount));
            AmtUSD->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
            confirm.gridLayout->addWidget(AmtUSD, row, 2, 1, 1);            

            // Memo (suppressed for the auto-shield change row -- its Addr label
            // already says "Change (auto-shielded to your wallet)"; NIT-2).
            if (!isAutoShieldChange && toAddr.addr.startsWith("z") && !toAddr.txtMemo.isEmpty()) {
                row++;
                auto Memo = new QLabel(confirm.sendToAddrs);
                Memo->setObjectName(QStringLiteral("Memo") % QString::number(i + 1));
                Memo->setText(toAddr.txtMemo);
                QFont font1 = Addr->font();
                font1.setPointSize(font1.pointSize() - 1);
                Memo->setFont(font1);
                Memo->setWordWrap(true);

                confirm.gridLayout->addWidget(Memo, row, 0, 1, 3);
            }

            row ++;

            // Add an empty spacer to create a blank space
            auto spacer = new QLabel(confirm.sendToAddrs);
            spacer->setObjectName(QString("spacer") % QString::number(i + 1));
            confirm.gridLayout->addWidget(spacer, row, 0, 1, 1);

            row++;
        }
    }

    // Add fees
    {
        auto labelMinerFee = new QLabel(confirm.sendToAddrs);
        labelMinerFee->setObjectName(QStringLiteral("labelMinerFee"));
        confirm.gridLayout->addWidget(labelMinerFee, row, 0, 1, 1);
        labelMinerFee->setText(tr("Miner Fee"));

        auto minerFee = new QLabel(confirm.sendToAddrs);
        QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        minerFee->setSizePolicy(sizePolicy);
        minerFee->setObjectName(QStringLiteral("minerFee"));
        minerFee->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        confirm.gridLayout->addWidget(minerFee, row, 1, 1, 1);
        minerFee->setText(Settings::getZCLDisplayFormat(tx.fee));
        totalSpending += tx.fee;

        auto minerFeeUSD = new QLabel(confirm.sendToAddrs);
        QSizePolicy sizePolicy1(QSizePolicy::Minimum, QSizePolicy::Preferred);
        minerFeeUSD->setSizePolicy(sizePolicy1);
        minerFeeUSD->setObjectName(QStringLiteral("minerFeeUSD"));
        minerFeeUSD->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        confirm.gridLayout->addWidget(minerFeeUSD, row, 2, 1, 1);
        minerFeeUSD->setText(Settings::getUSDFormat(tx.fee));

        if (Settings::getInstance()->getAllowCustomFees() && tx.fee != Settings::getMinerFee()) {
            confirm.warningLabel->setVisible(true);            
        } else {
            // Default fee
            confirm.warningLabel->setVisible(false);
        }
    }

    // Syncing warning
    confirm.syncingWarning->setVisible(Settings::getInstance()->isSyncing());

    // No peers warning
    confirm.nopeersWarning->setVisible(Settings::getInstance()->getPeers() == 0);

    // PRIV-11 / UX-12 — drive the confirm dialog wording + a privacy badge from the
    // four-way category. Tokens mirror dark.qss / PrivacyBadgeDelegate:
    //   green #1f7a1f  amber #d9822b  red #c0392b
    // A small badge QLabel ("confirmPrivacyBadge") is created once and reused; the
    // existing red publicWarning label carries the long-form copy and is only shown
    // for the two public quadrants (z->t de-shield = strongest; t->t = public).
    {
        QString badgeText, badgeColor, dialogTitle, warningText;
        // NIT-3: the reused publicWarning label is tinted per category token instead
        // of the .ui's static "color: red;" -- amber for plain public, red (strongest)
        // for the de-shield leak. Mirrors dark.qss / PrivacyBadgeDelegate tokens.
        QString warningColor;
        bool    showWarning = false;

        switch (category) {
        case SendCategory::ZToZ_private:
            badgeText   = tr("PRIVATE  z → z");
            badgeColor  = "#1f7a1f";                       // green
            dialogTitle = tr("Confirm private transaction");
            break;
        case SendCategory::TToZ_shielding:
            badgeText   = tr("SHIELDING  t → z");
            badgeColor  = "#e6e6e6";                       // neutral text
            dialogTitle = tr("Confirm shielding transaction");
            break;
        case SendCategory::ZToT_deshield:
            badgeText    = tr("DE-SHIELD  z → t");
            badgeColor   = "#c0392b";                      // red (strongest)
            warningColor = "#c0392b";                      // red de-shield warning
            dialogTitle  = tr("Confirm DE-SHIELD (public) transaction");
            warningText  = tr("DE-SHIELD: you are moving funds from a PRIVATE (shielded) "
                             "address to a PUBLIC transparent address. The amount, the "
                             "recipient, AND the link back to your shielded sender become "
                             "permanently visible to everyone on the blockchain.");
            showWarning = true;
            break;
        case SendCategory::TToT_public:
            badgeText    = tr("PUBLIC  t → t");
            badgeColor   = "#d9822b";                      // amber
            warningColor = "#d9822b";                      // amber public warning
            dialogTitle  = tr("Confirm public transaction");
            warningText  = tr("This transaction is PUBLIC: the amount and recipient will "
                             "be permanently visible to everyone on the blockchain.");
            showWarning = true;
            break;
        }

        d.setWindowTitle(dialogTitle);

        // The privacy badge: created once on the From group's layout so it sits at
        // the very top of the dialog. objectName is stable so the L1 test can read it.
        auto* badge = d.findChild<QLabel*>("confirmPrivacyBadge");
        if (!badge) {
            badge = new QLabel(confirm.groupBox);
            badge->setObjectName("confirmPrivacyBadge");
            confirm.verticalLayout_2->insertWidget(0, badge);
        }
        badge->setText(badgeText);
        badge->setStyleSheet(QString("font-weight: 700; color: %1;").arg(badgeColor));

        // The long-form warning copy (publicWarning): shown for the two public
        // quadrants only, with category-specific text + token color (NIT-3).
        if (showWarning) {
            confirm.publicWarning->setText(warningText);
            confirm.publicWarning->setStyleSheet(
                QString("color: %1; font-weight: 600;").arg(warningColor));
        }
        confirm.publicWarning->setVisible(showWarning);
    }

    // And FromAddress in the confirm dialog
    confirm.sendFrom->setText(fnSplitAddressForWrap(tx.fromAddr));
    QString tooltip = tr("Current balance      : ") +
        Settings::getZCLUSDDisplayFormat(rpc->getAllBalances()->value(tx.fromAddr));
    tooltip += "\n" + tr("Balance after this Tx: ") +
        Settings::getZCLUSDDisplayFormat(rpc->getAllBalances()->value(tx.fromAddr) - totalSpending);
    confirm.sendFrom->setToolTip(tooltip);

    // Show the dialog and submit it if the user confirms
    if (d.exec() == QDialog::Accepted) {
        // PRIV-12 — a z->t DE-SHIELD send requires an explicit, non-default-accept
        // acknowledgement before it proceeds. The other three categories
        // (z->z private, t->z shielding, t->t public) MUST NOT add this friction.
        // Default button is No, so a careless Enter does not de-shield.
        if (category == SendCategory::ZToT_deshield) {
            // Explicit QMessageBox instance (objectName "deshieldAck") rather than the
            // blocking static, so the acknowledgement is reliably testable and the
            // default button is the SAFE No (a careless Enter does not de-shield).
            QMessageBox ack(QMessageBox::Warning, tr("De-shield to a public address?"),
                tr("This send moves funds OUT of your private (shielded) balance onto a "
                   "PUBLIC transparent address. The amount, the recipient, and the link "
                   "back to your shielded sender will be permanently visible on the "
                   "blockchain.\n\nThis cannot be undone. De-shield anyway?"),
                QMessageBox::Yes | QMessageBox::No, this);
            ack.setObjectName("deshieldAck");
            ack.setDefaultButton(QMessageBox::No);
            if (ack.exec() != QMessageBox::Yes)
                return false;
        }

        // Then delete the additional fields from the sendTo tab
        removeExtraAddresses();
        return true;
    } else {
        return false;
    }
}

// Send button clicked
void MainWindow::sendButton() {
    Tx tx = createTxFromSendPage();

    // MAJOR-1/MAJOR-2 fail-closed abort: createTxFromSendPage() returns an INVALID
    // Tx (empty fromAddr) when it has already surfaced a blocking reason -- the
    // change could not be shielded (privacy fail-closed), or there are not enough
    // confirmed funds. Bail silently here so we don't stack a second, vaguer
    // "From Address is Invalid" error on top of the precise one already shown.
    if (tx.fromAddr.isEmpty())
        return;

    // Still-syncing acknowledgement gate: during sync the balance/UTXO set is
    // incomplete, so a send can silently fail. Make the user explicitly accept
    // that risk (default No) instead of relying only on the soft confirm label.
    if (Settings::getInstance()->isSyncing()) {
        const qint64 now  = QDateTime::currentSecsSinceEpoch();
        const qint64 last = Settings::getInstance()->getLastSyncPollEpoch();
        const bool   stale = Settings::syncGateIsStale(now, last);
        // Distinguish the user's OWN import/rescan (homeImportCard visible) from a
        // network catch-up, and soften when the flag looks stale (no live poll refreshed
        // it recently) — but keep it a SOFT Yes/No warning (default No) so even a wedged
        // flag can never hard-block a send.
        const bool importBusy = (homeImportCard != nullptr && homeImportCard->isVisible());
        QString title = tr("Wallet still syncing");
        QString body;
        if (importBusy) {
            title = tr("Still importing your keys");
            body  = tr("Your wallet is still importing and rescanning, so your balance "
                       "may be incomplete and this transaction could fail.\n\nSend anyway?");
        } else if (stale) {
            body  = tr("Your wallet hasn't reported sync progress recently. Your balance "
                       "may be up to date, or syncing may have paused.\n\nSend anyway?");
        } else {
            body  = tr("Your wallet is still catching up with the network, so your balance "
                       "may be incomplete and this transaction could fail.\n\nSend anyway?");
        }
        auto res = QMessageBox::warning(this, title, body,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (res != QMessageBox::Yes)
            return;
    }

    QString error = doSendTxValidations(tx);
    if (!error.isEmpty()) {
        // Something went wrong, so show an error and exit
        QMessageBox msg(QMessageBox::Critical, tr("Transaction Error"), error,
                        QMessageBox::Ok, this);

        msg.exec();

        // abort the Tx
        return;
    }
    
    // Show a dialog to confirm the Tx
    if (confirmTx(tx)) {
        // And send the Tx
        rpc->executeTransaction(tx, 
            [=] (QString opid) {
                ui->statusBar->showMessage(tr("Computing Tx: ") % opid);
            },
            [=] (QString, QString txid) { 
                ui->statusBar->showMessage(Settings::txidStatusMessage + " " + txid);
            },
            [=] (QString opid, QString errStr) {
                ui->statusBar->showMessage(QObject::tr(" Tx ") % opid % QObject::tr(" failed"), 15 * 1000);

                if (!opid.isEmpty())
                    errStr = QObject::tr("The transaction with id ") % opid % QObject::tr(" failed. The error was") + ":\n\n" + errStr; 

                QMessageBox::critical(this, QObject::tr("Transaction Error"), errStr, QMessageBox::Ok);            
            }
        );
    }        
}

QString MainWindow::doSendTxValidations(Tx tx) {
    if (!Settings::isValidAddress(tx.fromAddr)) return QString(tr("From Address is Invalid"));    

    for (auto toAddr : tx.toAddrs) {
        if (!Settings::isValidAddress(toAddr.addr)) {
            QString addr = (toAddr.addr.length() > 100 ? toAddr.addr.left(100) + "..." : toAddr.addr);
            return QString(tr("Recipient Address ")) % addr % tr(" is Invalid");
        }

        // This technically shouldn't be possible, but issue #62 seems to have discovered a bug
        // somewhere, so just add a check to make sure. 
        if (toAddr.amount < 0) {
            return QString(tr("Amount '%1' is invalid!").arg(toAddr.amount));
        }
    }

    // Insufficient-funds guard: catch an overspend up front with a friendly
    // message instead of letting it fail minutes later with a cryptic node
    // error. Only enforced when the balance is reliable (not mid-sync), and
    // null-guarded since balances may not be loaded yet during early startup.
    if (!Settings::getInstance()->isSyncing()) {
        auto bals = rpc ? rpc->getAllBalances() : nullptr;
        if (bals && bals->contains(tx.fromAddr)) {
            double balance = bals->value(tx.fromAddr);
            double total   = tx.fee;
            for (auto toAddr : tx.toAddrs)
                total += toAddr.amount;
            if (total > balance) {
                return tr("Not enough funds. You're trying to send %1 (including the fee), "
                          "but this address only holds %2.")
                       .arg(Settings::getZCLDisplayFormat(total))
                       .arg(Settings::getZCLDisplayFormat(balance));
            }
        }
    }

    return QString();
}

void MainWindow::cancelButton() {
    removeExtraAddresses();
}

