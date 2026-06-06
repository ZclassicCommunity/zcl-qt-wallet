// ============================================================================
// NftMintDialog — native (no-browser) "Make a collectible" mint wizard
// (NATIVE_NFT_GUIDE.md §2.5). Modal QDialog (programmatic, no .ui file).
//
// Flow (single page, gated by the async fingerprint):
//   pick/drag a LOCAL file -> ContentEngine.hashFile gives the document_hash
//   (the descriptor's merkleRoot for large files else sha256Whole, lowercase
//   hex) + classifyKind for the glyph -> name (required), ticker (optional),
//   document_url (optional, NEVER auto-fetched) -> Create.
//
// VISIBILITY POLARITY (load-bearing, §2.5): the building-now write path is the
// PUBLIC zslp_genesis; the PRIVATE (ZDC1) path is not built. So the Public tile
// is wired + default-selected and the Private tile is an honest disabled
// "Coming in this release" gated by RPC::isPrivateMintWired()==false — never a
// dead Create button.
//
// It is handed the EXISTING ContentEngine (nftImgCache) — it NEVER constructs a
// second engine. On success the source bytes are cached (cachePut) so the new
// card verifies immediately. C++14 only (empty-QString sentinels).
// ============================================================================
#ifndef NFTMINTDIALOG_H
#define NFTMINTDIALOG_H

#include "precompiled.h"

#include <QDialog>
#include <QString>
#include "contentengine.h"   // ContentDescriptor by value in a slot signature

class QLabel;
class QLineEdit;
class QPushButton;
class RPC;

class NftMintDialog : public QDialog {
    Q_OBJECT
public:
    explicit NftMintDialog(ContentEngine* engine, RPC* rpc, QWidget* parent = nullptr);

    // Test/inspection seam: the txid of the last successful mint ("" until then).
    QString lastTxid()    const { return m_lastTxid; }
    QString lastTokenId() const { return m_lastTokenId; }

#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAM (E): drive the SAME setPickedFile() path a chosen/dropped file
    // takes — including the remote-URL privacy reject — without the QFileDialog or a
    // synthesized QDropEvent (un-drivable under offscreen QPA). Never in the app.
    void testPickFile(const QString& path) { setPickedFile(path); }
#endif

protected:
    // Drag/drop a local file onto the dialog (the dropzone).
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    // While the mint RPC is in flight, swallow the window [X] so the dialog can't be
    // destroyed out from under the in-flight reply (review fix #5 / UAF).
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onChooseFile();
    void onDescriptorReady(quint64 token, ContentDescriptor d);
    void onCreate();
    void onDoneClicked();   // after success: explicit dismiss (accept) of the confirmation

private:
    void setPickedFile(const QString& path);     // begin hashing a chosen file
    void refreshCreateEnabled();
    // The anchor (document_hash) rule lives in ContentEngine::anchorHexFor — the
    // ONE shared definition used by the mint wizard AND the detail attach-gate.

    ContentEngine* m_engine = nullptr;
    RPC*           m_rpc    = nullptr;

    QString  m_srcPath;          // chosen local file
    QString  m_anchorHex;        // computed document_hash (empty until ready)
    bool     m_hashing   = false;
    bool     m_inFlight  = false;   // mint RPC in flight (Cancel + [X] disabled)
    bool     m_succeeded = false;   // mint returned; dialog now shows the confirmation
    quint64  m_hashToken = 0;

    QString  m_lastTxid;
    QString  m_lastTokenId;

    QLabel*      m_dropLabel  = nullptr;   // dropzone / picked-file status
    QLabel*      m_fpLabel    = nullptr;   // "Fingerprint ready / reading…"
    QLineEdit*   m_nameEdit   = nullptr;
    QLineEdit*   m_tickerEdit = nullptr;
    QLineEdit*   m_urlEdit    = nullptr;
    QLabel*      m_visLabel   = nullptr;   // Public/Private tiles status
    QLabel*      m_resultLine = nullptr;
    QPushButton* m_createBtn  = nullptr;
    QPushButton* m_cancelBtn  = nullptr;   // disabled while in flight; "Done" after success
};

#endif // NFTMINTDIALOG_H
