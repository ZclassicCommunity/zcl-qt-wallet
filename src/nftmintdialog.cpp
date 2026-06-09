// ============================================================================
// NftMintDialog implementation — see nftmintdialog.h. NATIVE_NFT_GUIDE §2.5.
// ============================================================================
#include "nftmintdialog.h"
#include "rpc.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QPointer>
#include <QCloseEvent>

namespace {
    // A unique-enough token for the single in-flight hash job per dialog.
    static quint64 nextMintHashToken() { static quint64 s = 0; return ++s; }
}

NftMintDialog::NftMintDialog(ContentEngine* engine, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_engine(engine), m_rpc(rpc) {
    setWindowTitle(tr("Make a collectible"));
    setMinimumWidth(460);
    setAcceptDrops(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(10);

    // ---- dropzone ----
    m_dropLabel = new QLabel(tr("Drag a file here, or choose one below."), this);
    m_dropLabel->setObjectName("nftMintDropzone");
    m_dropLabel->setAlignment(Qt::AlignCenter);
    m_dropLabel->setMinimumHeight(96);
    m_dropLabel->setWordWrap(true);
    m_dropLabel->setStyleSheet(
        "border:1px dashed #2a2d35; border-radius:8px; color:#9aa0a6; padding:8px;");
    outer->addWidget(m_dropLabel);

    auto* chooseRow = new QHBoxLayout();
    auto* chooseBtn = new QPushButton(tr("Choose a file…"), this);
    chooseRow->addStretch(1);
    chooseRow->addWidget(chooseBtn);
    chooseRow->addStretch(1);
    outer->addLayout(chooseRow);

    m_fpLabel = new QLabel(this);
    m_fpLabel->setObjectName("nftMintFingerprintLabel");
    m_fpLabel->setWordWrap(true);
    m_fpLabel->setMinimumHeight(18);
    m_fpLabel->setStyleSheet("color:#9aa0a6;");
    outer->addWidget(m_fpLabel);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2d35;");
    outer->addWidget(sep);

    // ---- details ----
    outer->addWidget(new QLabel(tr("Name"), this));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("nftMintNameEdit");
    m_nameEdit->setMaxLength(50);
    m_nameEdit->setPlaceholderText(tr("Required"));
    outer->addWidget(m_nameEdit);

    outer->addWidget(new QLabel(tr("Collection (optional)"), this));
    m_tickerEdit = new QLineEdit(this);
    outer->addWidget(m_tickerEdit);

    outer->addWidget(new QLabel(tr("Link (optional)"), this));
    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(tr("https://… (a hint — we never open it)"));
    outer->addWidget(m_urlEdit);

    // ---- honesty block (load-bearing): ONE amber public/permanence line + ONE grey
    // "file never uploaded" line. ZSLP genesis is PUBLIC; ownership is always public —
    // only the file CONTENT stays on your computer. (Collapsed from 3 stacked labels.)
    m_visLabel = new QLabel(
        tr("Public — name, collection and fingerprint go on the public ledger "
           "permanently and can't be removed."), this);
    m_visLabel->setObjectName("nftMintPermanenceLabel");
    m_visLabel->setWordWrap(true);
    m_visLabel->setStyleSheet("color:#d9822b; font-size:12pt;");
    outer->addWidget(m_visLabel);

    auto* honest = new QLabel(
        tr("Your file is never uploaded — only its fingerprint is recorded."), this);
    honest->setObjectName("nftMintFileNeverUploadedLabel");
    honest->setProperty("hint", true);
    honest->setWordWrap(true);
    honest->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    outer->addWidget(honest);

    // ---- result + action bar ----
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("nftMintResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_createBtn = new QPushButton(tr("Create"), this);
    m_createBtn->setObjectName("nftMintCreateButton");
    m_createBtn->setDefault(true);
    // Item B (honesty): the permanence / public-ledger reality. Creating a collectible
    // writes the name, collection and fingerprint to a permanent, PUBLIC ledger; there
    // is no edit or delete. The file itself stays private (see the honest line above).
    m_createBtn->setWhatsThis(
        tr("Creating a collectible writes its name, collection and fingerprint to the "
           "public ledger permanently. This can't be undone, edited, or hidden later. "
           "Your file itself is never uploaded — only its fingerprint goes on-chain."));
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_createBtn);
    outer->addLayout(btnRow);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(chooseBtn, &QPushButton::clicked, this, &NftMintDialog::onChooseFile);
    connect(m_createBtn, &QPushButton::clicked, this, &NftMintDialog::onCreate);
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this]{ refreshCreateEnabled(); });

    if (m_engine)
        connect(m_engine, &ContentEngine::descriptorReady,
                this, &NftMintDialog::onDescriptorReady);

    refreshCreateEnabled();
}

void NftMintDialog::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void NftMintDialog::dropEvent(QDropEvent* e) {
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QUrl u = urls.first();
    if (!u.isLocalFile()) {
        // PRIVACY: reject a web link drop inline (never auto-fetch).
        m_fpLabel->setText(tr("For your privacy, drop a local file — not a web link."));
        m_fpLabel->setStyleSheet("color:#c0392b;");
        return;
    }
    setPickedFile(u.toLocalFile());
}

void NftMintDialog::onChooseFile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a file"));
    if (!path.isEmpty())
        setPickedFile(path);
}

void NftMintDialog::setPickedFile(const QString& path) {
    if (ContentEngine::isRemoteUrl(path)) {
        m_fpLabel->setText(tr("For your privacy, drop a local file — not a web link."));
        m_fpLabel->setStyleSheet("color:#c0392b;");
        return;
    }
    m_srcPath   = path;
    m_anchorHex = QString();   // invalidate any previous fingerprint
    const QFileInfo fi(path);
    m_dropLabel->setText(fi.fileName());
    // Prefill the name from the basename (without extension) if empty.
    if (m_nameEdit->text().isEmpty())
        m_nameEdit->setText(fi.completeBaseName());

    if (m_engine == nullptr) {
        m_fpLabel->setText(tr("Cannot read this file right now."));
        m_fpLabel->setStyleSheet("color:#c0392b;");
        return;
    }
    m_hashing = true;
    m_fpLabel->setText(tr("Reading your file…"));
    m_fpLabel->setStyleSheet("color:#9aa0a6;");
    m_hashToken = nextMintHashToken();
    m_engine->hashFile(path, m_hashToken);
    refreshCreateEnabled();
}

void NftMintDialog::onDescriptorReady(quint64 token, ContentDescriptor d) {
    if (token != m_hashToken)
        return;   // a stale hash job
    m_hashing = false;
    if (!d.ok) {
        m_anchorHex = QString();
        m_fpLabel->setText(tr("We couldn't read that file. Try another."));
        m_fpLabel->setStyleSheet("color:#c0392b;");
    } else {
        m_anchorHex = ContentEngine::anchorHexFor(d);
        m_fpLabel->setText(tr("Fingerprint ready — %1 · %2")
                               .arg(m_anchorHex.left(8) + QStringLiteral("…"),
                                    ContentEngine::humanSize(d.fileSize)));
        m_fpLabel->setStyleSheet("color:#34c759;");
    }
    refreshCreateEnabled();
}

void NftMintDialog::refreshCreateEnabled() {
    if (m_succeeded)
        return;   // post-success: Create is gone (replaced by Done) — never re-enable
    const bool nameOk   = !m_nameEdit->text().trimmed().isEmpty();
    const bool anchorOk = !m_anchorHex.isEmpty();
    m_createBtn->setEnabled(nameOk && anchorOk && !m_hashing && !isInFlight());
}

void NftMintDialog::onCreate() {
    if (isInFlight() || m_succeeded || m_rpc == nullptr || m_anchorHex.isEmpty())
        return;
    const QString name   = m_nameEdit->text().trimmed();
    const QString ticker = m_tickerEdit->text().trimmed();
    const QString url    = m_urlEdit->text().trimmed();
    if (name.isEmpty())
        return;

    // Shared scaffold: latch in-flight, relabel Create -> "Creating…", disable it
    // and Cancel (can't bail mid-flight — also closes the UAF window).
    beginPrimary(m_createBtn, tr("Creating…"), m_cancelBtn);
    m_resultLine->clear();

    // Capture the source path so we can cache the bytes on success (the new card
    // then verifies green immediately, content-addressed by the anchor).
    const QString srcPath = m_srcPath;
    const QString anchor  = m_anchorHex;

    // LIFETIME (review fix #5): the modal exec() can still be torn down (the [X] is
    // swallowed while in-flight, but be belt-and-suspenders) — guard the reply with a
    // QPointer and bail before touching any member if we've been deleted.
    QPointer<NftMintDialog> self(this);
    m_rpc->mintNFT(name, ticker, anchor, url,
        [self, srcPath, anchor](QString txid, QString tokenid) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->m_lastTxid    = txid;
            self->m_lastTokenId = tokenid;
            // Cache the bytes under the anchor so the new card paints + verifies
            // immediately once it confirms (store-once; never a remote URL).
            if (!srcPath.isEmpty() && !anchor.isEmpty())
                ContentEngine::cachePut(anchor, srcPath);
            // VISIBLE SUCCESS (review fix #3): do NOT accept() here — that would make the
            // confirmation vanish before the user could read it. Show the honest 0-conf
            // line, then retire Create -> terminal "Done" (shared scaffold).
            self->m_succeeded = true;
            self->m_resultLine->setText(
                tr("Created — it'll appear once confirmed."));
            self->m_resultLine->setStyleSheet("color:#34c759;");
            self->finishPrimaryAsDone(self->m_createBtn, self->m_cancelBtn);
        },
        [self](QString errStr) {
            if (self.isNull())
                return;   // dialog gone — safe no-op
            self->finishPrimaryAsRetry(self->m_createBtn, self->m_cancelBtn);
            self->m_resultLine->setText(errStr);   // honest daemon message, never fabricated
            self->m_resultLine->setStyleSheet("color:#c0392b;");
            self->refreshCreateEnabled();
        }
    );
}
