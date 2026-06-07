// ============================================================================
// ShieldSendDialog implementation — see shieldsenddialog.h.
// doc/nft/NATIVE_DISPLAY_UX.md §6.4 / PRIVACY_TECH.md §5.3.
// ============================================================================
#include "shieldsenddialog.h"
#include "nftdatachannel.h"   // ZDC_MAX_FILE_BYTES (the 40000-byte cap)
#include "rpc.h"
#include "settings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QPointer>
#include <QApplication>
#include <QClipboard>

ShieldSendDialog::ShieldSendDialog(RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Send a private file"));
    setObjectName("shieldSendDialog");
    setMinimumWidth(520);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- Honesty banner (load-bearing §6): a plain lead line + a grey caveat block.
    // The caveat keeps the does-NOT-hide-ownership + stored-forever facts verbatim.
    auto* banner = new QLabel(
        tr("Only this file's contents are encrypted — just the person you send it to "
           "can open it."), this);
    banner->setObjectName("shieldSendBanner");
    banner->setWordWrap(true);
    outer->addWidget(banner);

    auto* bannerCaveat = new QLabel(
        tr("It does NOT hide who owns an NFT — that's always public. The encrypted file "
           "is stored on every node forever; it can never be deleted, only kept "
           "unreadable."), this);
    bannerCaveat->setObjectName("shieldSendBannerCaveat");
    bannerCaveat->setProperty("hint", true);
    bannerCaveat->setWordWrap(true);
    bannerCaveat->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(bannerCaveat);

    // --- Metadata-leak honesty (§6.6) — part of the one honesty block up top.
    auto* leakNote = new QLabel(
        tr("Sending a private file is itself visible on the ledger (the encrypted "
           "contents are not). Private does not mean undetectable."), this);
    leakNote->setObjectName("shieldSendLeakNote");
    leakNote->setProperty("hint", true);
    leakNote->setWordWrap(true);
    leakNote->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(leakNote);

    auto* sep0 = new QFrame(this);
    sep0->setFrameShape(QFrame::HLine);
    sep0->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep0);

    // --- Pick the file ------------------------------------------------------
    auto* fileTitle = new QLabel(tr("File to send"), this);
    fileTitle->setStyleSheet("font-weight:600;");
    outer->addWidget(fileTitle);

    auto* fileRow = new QHBoxLayout();
    m_pickBtn = new QPushButton(tr("Choose a file…"), this);
    m_pickBtn->setObjectName("shieldSendPickButton");
    m_fileLine = new QLabel(tr("No file chosen."), this);
    m_fileLine->setObjectName("shieldSendFileLine");
    m_fileLine->setWordWrap(true);
    fileRow->addWidget(m_pickBtn);
    fileRow->addWidget(m_fileLine, 1);
    outer->addLayout(fileRow);

    auto* capNote = new QLabel(
        tr("Files must be 40,000 bytes or smaller to fit in one private transfer."), this);
    capNote->setObjectName("shieldSendCapNote");
    capNote->setProperty("hint", true);
    capNote->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    capNote->setWordWrap(true);
    outer->addWidget(capNote);

    auto* sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep1);

    // --- From / To (Sapling z-addresses) ------------------------------------
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    m_fromCombo = new QComboBox(this);
    m_fromCombo->setObjectName("shieldSendFromCombo");
    // Populate with the wallet's OWN Sapling z-addresses (a spending key is required;
    // the daemon rejects watch-only sends). zaddresses can be null pre-refresh.
    if (m_rpc && m_rpc->getAllZAddresses()) {
        for (const QString& z : *m_rpc->getAllZAddresses()) {
            if (Settings::getInstance()->isSaplingAddress(z))
                m_fromCombo->addItem(z);
        }
    }
    if (m_fromCombo->count() == 0)
        m_fromCombo->addItem(tr("(no Sapling address in this wallet)"));
    form->addRow(tr("From (your Sapling address):"), m_fromCombo);

    m_toEdit = new QLineEdit(this);
    m_toEdit->setObjectName("shieldSendToEdit");
    m_toEdit->setPlaceholderText(tr("Recipient's Sapling (zs…) address"));
    form->addRow(tr("To (recipient's Sapling address):"), m_toEdit);
    outer->addLayout(form);

    m_toStatus = new QLabel(this);
    m_toStatus->setObjectName("shieldSendToStatus");
    m_toStatus->setWordWrap(true);
    m_toStatus->setMinimumHeight(18);   // reserved height so layout never jumps
    outer->addWidget(m_toStatus);

    // --- Mandatory permanence consent (§6.3 / daemon acknowledge_permanent) --
    m_consent = new QCheckBox(
        tr("I understand this encrypted file is stored permanently and publicly, and "
           "can never be deleted."), this);
    m_consent->setObjectName("shieldSendConsentCheck");
    outer->addWidget(m_consent);

    // --- result line + action bar -------------------------------------------
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("shieldSendResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_sendBtn   = new QPushButton(tr("Send privately"), this);
    m_sendBtn->setObjectName("shieldSendButton");
    m_sendBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_sendBtn);
    outer->addLayout(btnRow);

    connect(m_pickBtn,   &QPushButton::clicked, this, &ShieldSendDialog::onPickFile);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_sendBtn,   &QPushButton::clicked, this, &ShieldSendDialog::onSendClicked);
    connect(m_toEdit,    &QLineEdit::textChanged, this, &ShieldSendDialog::onRecipientChanged);
    connect(m_consent,   &QCheckBox::toggled,     this, &ShieldSendDialog::onConsentToggled);

    onRecipientChanged(QString());   // seed the disabled/status state
}

void ShieldSendDialog::onPickFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a file to send privately"));
    if (path.isEmpty())
        return;
    beginPick(path);
}

void ShieldSendDialog::beginPick(const QString& path) {
    QFile f(path);
    QFileInfo fi(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_hexData.clear(); m_fileBytes = 0; m_fileName.clear(); m_filePath.clear();
        m_fileLine->setText(tr("Couldn't read that file."));
        m_fileLine->setStyleSheet("color:#c0392b;");
        refreshSendEnabled();
        return;
    }
    // Binary-safe whole-file read. Enforce the 40000-byte cap UP FRONT with a clear
    // message (the daemon also re-projects the real serialized size, but we never want
    // to round-trip an obviously-too-large file).
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.size() > nftdc::ZDC_MAX_FILE_BYTES) {
        m_hexData.clear(); m_fileBytes = 0; m_fileName.clear(); m_filePath.clear();
        m_fileLine->setText(
            tr("Too large: %1 bytes. The limit is 40,000.").arg(bytes.size()));
        m_fileLine->setStyleSheet("color:#c0392b;");
        refreshSendEnabled();
        return;
    }
    if (bytes.isEmpty()) {
        m_hexData.clear(); m_fileBytes = 0; m_fileName.clear(); m_filePath.clear();
        m_fileLine->setText(tr("That file is empty — there's nothing to send."));
        m_fileLine->setStyleSheet("color:#c0392b;");
        refreshSendEnabled();
        return;
    }

    m_filePath = path;
    m_fileName = fi.fileName();
    m_hexData  = QString::fromLatin1(bytes.toHex());   // lowercase hex, binary-safe
    m_fileBytes = bytes.size();
    m_fileLine->setText(tr("%1 — %2 bytes").arg(m_fileName).arg(m_fileBytes));
    m_fileLine->setStyleSheet("color:#34c759;");
    refreshSendEnabled();
}

void ShieldSendDialog::onRecipientChanged(const QString& text) {
    const QString addr = text.trimmed();
    if (addr.isEmpty()) {
        m_toStatus->clear();
    } else if (!Settings::isValidAddress(addr)) {
        m_toStatus->setText(tr("That doesn't look like a ZClassic address."));
        m_toStatus->setStyleSheet("color:#c0392b;");
    } else if (Settings::getInstance()->isSaplingAddress(addr)) {
        m_toStatus->setText(tr("Looks good — a Sapling (zs…) address."));
        m_toStatus->setStyleSheet("color:#34c759;");
    } else {
        // A t-addr or a Sprout z-addr: the data-channel is Sapling-only.
        m_toStatus->setText(
            tr("Private file transfers need a Sapling (zs…) recipient address."));
        m_toStatus->setStyleSheet("color:#c0392b;");
    }
    refreshSendEnabled();
}

void ShieldSendDialog::onConsentToggled(bool /*checked*/) {
    refreshSendEnabled();
}

void ShieldSendDialog::refreshSendEnabled() {
    if (m_succeeded)
        return;   // post-success: Send is gone (replaced by Done)
    const QString to = m_toEdit->text().trimmed();
    const bool toOk   = Settings::getInstance()->isSaplingAddress(to);
    const bool fileOk = !m_hexData.isEmpty();
    const bool fromOk = m_fromCombo->count() > 0
                        && Settings::getInstance()->isSaplingAddress(m_fromCombo->currentText());
    const bool consentOk = m_consent->isChecked();
    m_sendBtn->setEnabled(toOk && fileOk && fromOk && consentOk && !isInFlight());
}

QString ShieldSendDialog::inferContentType(const QString& path) const {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "png")  return QStringLiteral("image/png");
    if (ext == "jpg" || ext == "jpeg") return QStringLiteral("image/jpeg");
    if (ext == "gif")  return QStringLiteral("image/gif");
    if (ext == "webp") return QStringLiteral("image/webp");
    if (ext == "svg")  return QStringLiteral("image/svg+xml");
    if (ext == "pdf")  return QStringLiteral("application/pdf");
    if (ext == "txt")  return QStringLiteral("text/plain");
    return QString();   // unknown -> let the daemon default it
}

void ShieldSendDialog::onSendClicked() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr)
        return;
    const QString from = m_fromCombo->currentText();
    const QString to   = m_toEdit->text().trimmed();
    if (m_hexData.isEmpty() || !Settings::getInstance()->isSaplingAddress(to)
        || !Settings::getInstance()->isSaplingAddress(from) || !m_consent->isChecked())
        return;   // guard: the button shouldn't be enabled, but never trust that alone

    beginPrimary(m_sendBtn, tr("Encrypting and sending…"), m_cancelBtn);
    m_pickBtn->setEnabled(false);
    m_resultLine->setText(tr("Encrypting and sending…"));
    m_resultLine->setStyleSheet("color:#9aa0a6;");

    // LIFETIME (UAF guard): QPointer the reply; bail before touching any member if the
    // dialog was destroyed while the send RPC was in flight.
    QPointer<ShieldSendDialog> self(this);
    m_rpc->sendDataFile(from, to, m_hexData, m_fileName, inferContentType(m_filePath),
        [self](SendDataFileResult r) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->m_succeeded = true;
            self->m_resultLine->setText(
                tr("Sent. The encrypted file is on its way — confirming on-chain."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->m_toEdit->setEnabled(false);
            self->m_consent->setEnabled(false);

            // Disclosure block: the content fingerprint (= NFT document_hash) + the
            // per-transfer key, both copyable + honestly labeled. The recipient holding
            // the matching ivk can already open it (in-band KEY frame); this key is for
            // explicit out-of-band selective disclosure to a third party.
            auto* layout = qobject_cast<QVBoxLayout*>(self->layout());
            if (layout) {
                auto* discTitle = new QLabel(tr("Share these to let someone open this file"), self);
                discTitle->setStyleSheet("font-weight:600;");
                layout->insertWidget(layout->count() - 1, discTitle);

                auto* fpRow = new QHBoxLayout();
                auto* fpLbl = new QLabel(tr("Content fingerprint:"), self);
                self->m_fpField = new QLineEdit(r.fingerprint, self);
                self->m_fpField->setObjectName("shieldSendFingerprintField");
                self->m_fpField->setReadOnly(true);
                auto* fpCopy = new QPushButton(tr("Copy"), self);
                fpCopy->setObjectName("shieldSendCopyFingerprint");
                fpRow->addWidget(fpLbl);
                fpRow->addWidget(self->m_fpField, 1);
                fpRow->addWidget(fpCopy);
                auto* fpWrap = new QWidget(self);
                fpWrap->setLayout(fpRow);
                layout->insertWidget(layout->count() - 1, fpWrap);
                connect(fpCopy, &QPushButton::clicked, self, &ShieldSendDialog::onCopyFingerprint);

                auto* keyRow = new QHBoxLayout();
                auto* keyLbl = new QLabel(tr("Disclosure key:"), self);
                self->m_keyField = new QLineEdit(r.key, self);
                self->m_keyField->setObjectName("shieldSendKeyField");
                self->m_keyField->setReadOnly(true);
                auto* keyCopy = new QPushButton(tr("Copy"), self);
                keyCopy->setObjectName("shieldSendCopyKey");
                keyRow->addWidget(keyLbl);
                keyRow->addWidget(self->m_keyField, 1);
                keyRow->addWidget(keyCopy);
                auto* keyWrap = new QWidget(self);
                keyWrap->setLayout(keyRow);
                layout->insertWidget(layout->count() - 1, keyWrap);
                connect(keyCopy, &QPushButton::clicked, self, &ShieldSendDialog::onCopyKey);

                auto* keyWarn = new QLabel(
                    tr("Anyone with this key and fingerprint can open and verify this "
                       "file — share it only with people you want to read it."), self);
                keyWarn->setObjectName("shieldSendKeyWarn");
                keyWarn->setProperty("hint", true);
                keyWarn->setWordWrap(true);
                keyWarn->setStyleSheet("color:#d9822b; font-size:12pt;");
                layout->insertWidget(layout->count() - 1, keyWarn);
            }

            self->finishPrimaryAsDone(self->m_sendBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_sendBtn, self->m_cancelBtn);
            self->m_pickBtn->setEnabled(true);
            self->m_resultLine->setText(errStr);
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshSendEnabled();
        }
    );
}

void ShieldSendDialog::onCopyFingerprint() {
    if (m_fpField)
        QApplication::clipboard()->setText(m_fpField->text());
}

void ShieldSendDialog::onCopyKey() {
    if (m_keyField)
        QApplication::clipboard()->setText(m_keyField->text());
}
