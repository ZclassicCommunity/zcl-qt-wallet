// ============================================================================
// ShieldReceiveDialog implementation — see shieldreceivedialog.h.
// doc/nft/NATIVE_DISPLAY_UX.md §6.5 / PRIVACY_TECH.md §5.4.
// ============================================================================
#include "shieldreceivedialog.h"
#include "nftdatachannel.h"   // DataTransferState mapping + honest copy
#include "rpc.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QFrame>
#include <QFileDialog>
#include <QFile>
#include <QPointer>

ShieldReceiveDialog::ShieldReceiveDialog(RPC* rpc, QWidget* parent)
    : QDialog(parent), m_rpc(rpc) {
    setWindowTitle(tr("Receive a private file"));
    setObjectName("shieldReceiveDialog");
    setMinimumWidth(560);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- Honesty header -----------------------------------------------------
    auto* banner = new QLabel(
        tr("Open a private file someone sent you. Only the file's contents are private — "
           "who owns an NFT is always public. The wallet verifies the file against its "
           "on-chain fingerprint BEFORE decrypting, and never opens a file that doesn't "
           "match."), this);
    banner->setObjectName("shieldReceiveBanner");
    banner->setWordWrap(true);
    banner->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(banner);

    // --- This-session transfers list (honest scope) -------------------------
    auto* listTitle = new QLabel(tr("Files this wallet sent this session"), this);
    listTitle->setStyleSheet("font-weight:600;");
    outer->addWidget(listTitle);

    auto* listNote = new QLabel(
        tr("Sent this session only — not a full receive history."), this);
    listNote->setObjectName("shieldReceiveListNote");
    listNote->setProperty("hint", true);
    listNote->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    listNote->setWordWrap(true);
    outer->addWidget(listNote);

    m_list = new QListWidget(this);
    m_list->setObjectName("shieldReceiveList");
    m_list->setMaximumHeight(120);
    outer->addWidget(m_list);

    auto* refreshRow = new QHBoxLayout();
    refreshRow->addStretch(1);
    m_refreshBtn = new QPushButton(tr("Refresh list"), this);
    m_refreshBtn->setObjectName("shieldReceiveRefreshButton");
    refreshRow->addWidget(m_refreshBtn);
    outer->addLayout(refreshRow);

    auto* sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep1);

    // --- Open by id / fingerprint (cross-wallet) ----------------------------
    auto* openTitle = new QLabel(tr("Open a private file"), this);
    openTitle->setStyleSheet("font-weight:600;");
    outer->addWidget(openTitle);

    auto* form = new QFormLayout();
    m_idEdit = new QLineEdit(this);
    m_idEdit->setObjectName("shieldReceiveIdEdit");
    m_idEdit->setPlaceholderText(tr("Transfer id (16 hex) or file fingerprint (64 hex)"));
    form->addRow(tr("Transfer id or fingerprint:"), m_idEdit);

    m_addrEdit = new QLineEdit(this);
    m_addrEdit->setObjectName("shieldReceiveAddrEdit");
    m_addrEdit->setPlaceholderText(tr("Optional — your Sapling (zs…) address"));
    form->addRow(tr("Receiving address (optional):"), m_addrEdit);

    m_verifyEdit = new QLineEdit(this);
    m_verifyEdit->setObjectName("shieldReceiveVerifyEdit");
    m_verifyEdit->setPlaceholderText(tr("Optional — fingerprint the sender gave you"));
    form->addRow(tr("Expected fingerprint (optional):"), m_verifyEdit);
    outer->addLayout(form);

    auto* lookupRow = new QHBoxLayout();
    lookupRow->addStretch(1);
    m_lookupBtn = new QPushButton(tr("Verify and open"), this);
    m_lookupBtn->setObjectName("shieldReceiveLookupButton");
    m_lookupBtn->setDefault(true);
    lookupRow->addWidget(m_lookupBtn);
    outer->addLayout(lookupRow);

    // --- result --------------------------------------------------------------
    m_stateLine = new QLabel(this);
    m_stateLine->setObjectName("shieldReceiveStateLine");
    m_stateLine->setWordWrap(true);
    m_stateLine->setMinimumHeight(18);
    outer->addWidget(m_stateLine);

    m_metaLine = new QLabel(this);
    m_metaLine->setObjectName("shieldReceiveMetaLine");
    m_metaLine->setWordWrap(true);
    m_metaLine->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(m_metaLine);

    auto* saveRow = new QHBoxLayout();
    saveRow->addStretch(1);
    m_saveBtn = new QPushButton(tr("Save decrypted file…"), this);
    m_saveBtn->setObjectName("shieldReceiveSaveButton");
    m_saveBtn->setEnabled(false);   // ONLY after the fingerprint verifies + decrypts
    saveRow->addWidget(m_saveBtn);
    outer->addLayout(saveRow);

    connect(m_refreshBtn, &QPushButton::clicked, this, &ShieldReceiveDialog::onRefreshList);
    connect(m_list,       &QListWidget::itemActivated, this, &ShieldReceiveDialog::onListRowActivated);
    connect(m_lookupBtn,  &QPushButton::clicked, this, &ShieldReceiveDialog::onLookup);
    connect(m_saveBtn,    &QPushButton::clicked, this, &ShieldReceiveDialog::onSaveDecrypted);

    onRefreshList();   // pull this-session transfers up front (best-effort)
}

void ShieldReceiveDialog::prefillFingerprint(const QString& fingerprint64) {
    if (fingerprint64.isEmpty())
        return;
    m_idEdit->setText(fingerprint64);
    runLookup(QString(), fingerprint64);   // Mode A: open the file linked to a held NFT
}

void ShieldReceiveDialog::onRefreshList() {
    if (m_rpc == nullptr)
        return;
    m_list->clear();
    QPointer<ShieldReceiveDialog> self(this);
    m_rpc->listDataTransfers(
        [self](QVector<DataTransferRow> rows) {
            if (self.isNull())
                return;
            if (rows.isEmpty()) {
                self->m_list->addItem(self->tr("(no transfers recorded this session)"));
                self->m_list->setEnabled(false);
                return;
            }
            self->m_list->setEnabled(true);
            for (const DataTransferRow& r : rows) {
                const QString name = r.filename.isEmpty()
                                         ? self->tr("(unnamed)") : r.filename;
                auto* item = new QListWidgetItem(
                    self->tr("%1 — %2 frames — %3")
                        .arg(name).arg(r.frames).arg(r.fingerprint.left(12) + "…"),
                    self->m_list);
                // Stash the fingerprint (and id) so a row activation can look it up.
                item->setData(Qt::UserRole,     r.fingerprint);
                item->setData(Qt::UserRole + 1, r.transferId);
            }
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            // Channel off / not connected: state it calmly; the open box still works
            // for a node that has it on (the lookup will surface the same calm error).
            self->m_list->addItem(errStr);
            self->m_list->setEnabled(false);
        }
    );
}

void ShieldReceiveDialog::onListRowActivated() {
    auto* item = m_list->currentItem();
    if (item == nullptr)
        return;
    const QString fp = item->data(Qt::UserRole).toString();
    const QString id = item->data(Qt::UserRole + 1).toString();
    if (fp.isEmpty() && id.isEmpty())
        return;   // a placeholder / error row
    if (!fp.isEmpty())
        m_idEdit->setText(fp);
    else
        m_idEdit->setText(id);
    runLookup(id, fp);
}

void ShieldReceiveDialog::onLookup() {
    const QString raw = m_idEdit->text().trimmed();
    if (raw.isEmpty()) {
        m_stateLine->setText(tr("Enter a transfer id or a file fingerprint to open."));
        m_stateLine->setStyleSheet("color:#c0392b;");
        return;
    }
    // Classify the pasted value by length: 64 hex == fingerprint, 16 hex == transfer
    // id. Anything else we pass as a fingerprint and let the daemon judge.
    static const QRegularExpression hex64("^[0-9a-fA-F]{64}$");
    static const QRegularExpression hex16("^[0-9a-fA-F]{16}$");
    QString transferId, fingerprint;
    if (hex16.match(raw).hasMatch())
        transferId = raw;
    else if (hex64.match(raw).hasMatch())
        fingerprint = raw;
    else
        fingerprint = raw;   // best-effort; daemon returns an honest error if invalid
    runLookup(transferId, fingerprint);
}

void ShieldReceiveDialog::runLookup(const QString& transferId, const QString& fingerprint) {
    if (m_rpc == nullptr)
        return;
    clearResult();
    m_stateLine->setText(tr("Checking the file on-chain…"));
    m_stateLine->setStyleSheet("color:#9aa0a6;");
    m_lookupBtn->setEnabled(false);

    const QString addr   = m_addrEdit->text().trimmed();
    const QString verify = m_verifyEdit->text().trimmed();

    QPointer<ShieldReceiveDialog> self(this);
    m_rpc->getDataTransfer(transferId, fingerprint, addr, verify,
        [self](DataTransferResult r) {
            if (self.isNull())
                return;
            self->m_lookupBtn->setEnabled(true);

            // Verify-before-decrypt succeeded: the daemon only returns plaintext when
            // verified+complete. Decode the hex binary-safe (never via a QString) and
            // offer Save ONLY now.
            if (r.verified && r.complete && !r.hexData.isEmpty()) {
                self->m_plaintext = QByteArray::fromHex(r.hexData.toLatin1());
                self->m_lastFilename = r.filename;
                self->m_stateLine->setText(
                    self->tr("File verified and ready to open."));
                self->m_stateLine->setStyleSheet("color:#34c759;");
                const QString name = r.filename.isEmpty()
                                         ? self->tr("(unnamed)") : r.filename;
                self->m_metaLine->setText(
                    self->tr("%1 · %2 bytes%3")
                        .arg(name).arg(r.size)
                        .arg(r.contentType.isEmpty() ? QString()
                                                     : (" · " + r.contentType)));
                self->m_saveBtn->setEnabled(true);
                return;
            }

            // Failure space: map the daemon's honest status string to a distinct state
            // and show the matching plain-language line — NEVER a generic "failed" and
            // never a fake "try again" on a hard refusal.
            nftdc::DataTransferState st = nftdc::mapDataTransferError(r.error);
            if (st == nftdc::DTS_None) {
                // No explicit error but not verified+complete: treat an incomplete set
                // of frames as "still arriving", else a generic honest terminal.
                st = (!r.complete) ? nftdc::DTS_Incomplete : nftdc::DTS_OtherError;
            }
            // Translate the canonical copy through tr() at the call site.
            QString copy;
            switch (st) {
                case nftdc::DTS_Incomplete:
                    copy = self->tr("Some pieces haven't confirmed yet. Try again in a few minutes.");
                    break;
                case nftdc::DTS_NoKey:
                    copy = self->tr("This file isn't addressed to you, or its key isn't on-chain for this wallet.");
                    break;
                case nftdc::DTS_HashMismatch:
                    copy = self->tr("The file on-chain doesn't match the expected fingerprint. Not opened.");
                    break;
                case nftdc::DTS_AeadFail:
                    copy = self->tr("Couldn't decrypt — the file may be tampered or the key is wrong. Not opened.");
                    break;
                default:
                    copy = self->tr("This private file couldn't be opened.");
                    break;
            }
            self->m_stateLine->setText(copy);
            self->m_stateLine->setStyleSheet("color:#c0392b;");
            // mismatch diagnosis (honest, optional): show both fingerprints if present.
            if (st == nftdc::DTS_HashMismatch
                && !r.onchainFingerprint.isEmpty() && !r.expectedFingerprint.isEmpty()) {
                self->m_metaLine->setText(
                    self->tr("On-chain: %1…  ·  expected: %2…")
                        .arg(r.onchainFingerprint.left(12), r.expectedFingerprint.left(12)));
            }
        },
        [self](QString errStr) {
            if (self.isNull())
                return;
            self->m_lookupBtn->setEnabled(true);
            // Transport / channel-off: the calm message from datachannelCalmError.
            self->m_stateLine->setText(errStr);
            self->m_stateLine->setStyleSheet("color:#c0392b;");
        }
    );
}

void ShieldReceiveDialog::clearResult() {
    m_plaintext.clear();
    m_lastFilename.clear();
    m_metaLine->clear();
    m_saveBtn->setEnabled(false);
}

void ShieldReceiveDialog::onSaveDecrypted() {
    if (m_plaintext.isEmpty()) {
        m_stateLine->setText(tr("There's no verified file to save yet."));
        m_stateLine->setStyleSheet("color:#c0392b;");
        return;
    }
    const QString suggested = m_lastFilename.isEmpty()
                                  ? QStringLiteral("private-file") : m_lastFilename;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save decrypted file"), suggested);
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        m_stateLine->setText(tr("Couldn't write that file."));
        m_stateLine->setStyleSheet("color:#c0392b;");
        return;
    }
    f.write(m_plaintext);   // binary-safe write of the verified plaintext bytes
    f.close();
    m_stateLine->setText(tr("Saved."));
    m_stateLine->setStyleSheet("color:#34c759;");
}
