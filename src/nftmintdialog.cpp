// ============================================================================
// NftMintDialog implementation — see nftmintdialog.h. NATIVE_NFT_GUIDE §2.5.
// ============================================================================
#include "nftmintdialog.h"
#include "rpc.h"
#include "guiutil.h"     // makeLabelsSelectable / makeButtonsFit
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QFrame>
#include <QScrollArea>
#include <QWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QPointer>
#include <QCloseEvent>
#include <QStandardItemModel>

namespace {
    // A unique-enough token for the single in-flight hash job per dialog.
    static quint64 nextMintHashToken() { static quint64 s = 0; return ++s; }
}

NftMintDialog::NftMintDialog(ContentEngine* engine, RPC* rpc, QWidget* parent)
    : NftAsyncDialog(parent), m_engine(engine), m_rpc(rpc) {
    setWindowTitle(tr("Make a collectible"));
    setMinimumWidth(460);
    setAcceptDrops(true);

    // ---- scrollable content host (small-screen fit) ----
    // Mirror NFTDetailDialog: `this` holds ONLY a QScrollArea (the body) + the action
    // button row, so a short screen scrolls the body instead of forcing the window
    // taller than the screen (kept the dialog's minimumSizeHint().height() under the
    // 640px small-screen cap once the optional collection combo was added). All the
    // rich content lives under `content`; the Create/Cancel row stays OUTSIDE the
    // scroll area so it's always visible.
    auto* dlgLayout = new QVBoxLayout(this);
    dlgLayout->setContentsMargins(0, 0, 0, 0);
    dlgLayout->setSpacing(0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("nftMintScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    dlgLayout->addWidget(scroll, 1);

    auto* outer = new QVBoxLayout(content);
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

    // ---- optional: add this card to one of YOUR collections (COLLECTIONS Phase-1) ----
    // The first row is the "standalone" sentinel (no collection). Each owned collection
    // follows; a sealed one (no spendable authority unit) is shown DISABLED with a note.
    // The combo + hint are added even before the list loads, so the layout never jumps.
    outer->addWidget(new QLabel(tr("Add to a collection (optional)"), this));
    m_collectionCombo = new QComboBox(this);
    m_collectionCombo->setObjectName("nftMintCollectionCombo");
    m_collectionCombo->addItem(tr("Not part of a collection"));   // sentinel, group_id ""
    m_collectionCombo->setEnabled(false);                          // until the list loads
    outer->addWidget(m_collectionCombo);

    m_collectionHint = new QLabel(this);
    m_collectionHint->setObjectName("nftMintCollectionHint");
    m_collectionHint->setProperty("hint", true);
    m_collectionHint->setWordWrap(true);
    m_collectionHint->setStyleSheet("color:#9aa0a6; font-size:12pt;");
    m_collectionHint->setText(tr("Loading your collections…"));
    outer->addWidget(m_collectionHint);

    connect(m_collectionCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &NftMintDialog::onCollectionChanged);

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

    // ---- result line (stays inside the scroll body, beside the fields) ----
    m_resultLine = new QLabel(this);
    m_resultLine->setObjectName("nftMintResultLine");
    m_resultLine->setWordWrap(true);
    m_resultLine->setMinimumHeight(18);
    outer->addWidget(m_resultLine);

    // ---- action bar: lives OUTSIDE the scroll area (on the dialog) so Create/Cancel
    // are always visible no matter how the body scrolls. Parented to `this`.
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(18, 8, 18, 18);
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
    dlgLayout->addLayout(btnRow);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(chooseBtn, &QPushButton::clicked, this, &NftMintDialog::onChooseFile);
    connect(m_createBtn, &QPushButton::clicked, this, &NftMintDialog::onCreate);
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this]{ refreshCreateEnabled(); });

    if (m_engine)
        connect(m_engine, &ContentEngine::descriptorReady,
                this, &NftMintDialog::onDescriptorReady);

    refreshCreateEnabled();

    // Make all text here copy-pasteable (esp. the result/error line, fingerprint, ids) and ensure
    // no button clips its label at the user's font (deferred so sizeHint() is post-style).
    makeLabelsSelectable(this);
    QTimer::singleShot(0, this, [this]{ makeButtonsFit(this); });

    // Populate the optional collection picker from the wallet's owned collections.
    loadCollections();
}

// Fill the collection combo from RPC::listMyCollections (owned collections). The reply is
// QPointer-guarded (the modal can close before it lands). A SEALED collection (no spendable
// authority unit AND no baton-mintable unit, i.e. balance==0) is shown DISABLED with an
// honest note rather than hidden — so the user understands why they can't pick it.
void NftMintDialog::loadCollections() {
    if (m_rpc == nullptr || m_collectionCombo == nullptr)
        return;

    QPointer<NftMintDialog> self(this);
    m_rpc->listMyCollections(
        [self](QVector<CollectionRow> rows) {
            if (self.isNull())
                return;
            self->m_collections = rows;
            self->m_collectionCombo->setEnabled(true);

            // The combo's underlying model lets us DISABLE a sealed row (QComboBox itself
            // has no per-item enabled flag). Cast is safe for the default combo model.
            auto* model = qobject_cast<QStandardItemModel*>(self->m_collectionCombo->model());

            for (int i = 0; i < rows.size(); ++i) {
                const CollectionRow& c = rows.at(i);
                // A spendable authority unit is needed to authorize a child. balance == the
                // wallet's authority-unit balance; 0 means sealed for this wallet (no unit to
                // burn — the daemon would refuse unless allow_baton, which we never auto-do).
                const bool sealed = (c.balance <= 0);
                QString label = c.name.isEmpty()
                                    ? (c.ticker.isEmpty() ? c.tokenId.left(10) + QStringLiteral("…")
                                                          : c.ticker)
                                    : c.name;
                if (sealed)
                    label += QObject::tr(" — full (no card slots left)");
                else
                    label += QObject::tr(" — %1 card slot(s) left").arg(c.balance);
                self->m_collectionCombo->addItem(label);
                if (sealed && model != nullptr) {
                    // Disable the sealed row so it can't be chosen.
                    if (auto* item = model->item(self->m_collectionCombo->count() - 1))
                        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                }
            }

            if (rows.isEmpty()) {
                self->m_collectionHint->setText(QObject::tr(
                    "You don't own any collections yet. Create one first to group cards into a set."));
            } else {
                self->m_collectionHint->setText(QObject::tr(
                    "Choose a collection to add this card to. Adding a card uses one of its slots."));
            }
        },
        [self](QString /*errStr*/) {
            if (self.isNull())
                return;
            // Honest, non-blocking: the picker just stays on "standalone". An old node /
            // index-off surfaces here; we don't pop a dialog — minting standalone still works.
            self->m_collectionCombo->setEnabled(false);
            self->m_collectionHint->setText(QObject::tr(
                "Couldn't load your collections. You can still make this as a standalone collectible."));
        }
    );
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

// The chosen collection changed. Refresh the honesty hint (sealed rows are already
// disabled in the model, so index 0 == standalone; index N>0 == m_collections[N-1]).
void NftMintDialog::onCollectionChanged(int index) {
    if (m_collectionHint == nullptr)
        return;
    if (index <= 0) {
        m_collectionHint->setText(
            m_collections.isEmpty()
                ? tr("You don't own any collections yet. Create one first to group cards into a set.")
                : tr("Choose a collection to add this card to. Adding a card uses one of its slots."));
        return;
    }
    const int row = index - 1;   // skip the standalone sentinel
    if (row < 0 || row >= m_collections.size())
        return;
    const CollectionRow& c = m_collections.at(row);
    m_collectionHint->setText(
        tr("This card joins \"%1\" as a verified member and uses one of its slots.")
            .arg(c.name.isEmpty() ? c.ticker : c.name));
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

    // Resolve the chosen collection (combo index 0 == standalone sentinel; N>0 maps to
    // m_collections[N-1]). A disabled (sealed) row can't be selected, so groupId here is
    // always either "" or a collection the wallet can still authorize a child into.
    QString groupId;
    const bool intoCollection = m_collectionCombo && m_collectionCombo->currentIndex() > 0;
    if (intoCollection) {
        const int row = m_collectionCombo->currentIndex() - 1;
        if (row >= 0 && row < m_collections.size())
            groupId = m_collections.at(row).tokenId;
    }
    const bool joining = !groupId.isEmpty();

    // LIFETIME (review fix #5): the modal exec() can still be torn down (the [X] is
    // swallowed while in-flight, but be belt-and-suspenders) — guard the reply with a
    // QPointer and bail before touching any member if we've been deleted.
    QPointer<NftMintDialog> self(this);
    // ONE code path: mintCardIntoCollection with an empty groupId is byte-identical to a
    // standalone mintNFT (the daemon just omits group_id), so we always route here and let
    // the chosen collection (if any) flow through.
    m_rpc->mintCardIntoCollection(name, ticker, anchor, url, groupId,
        [self, srcPath, anchor, joining](QString txid, QString tokenid) {
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
                joining ? tr("Added to your collection — it'll appear once confirmed.")
                        : tr("Created — it'll appear once confirmed."));
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
