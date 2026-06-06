// ============================================================================
// NFTDetailDialog — native (no-browser) detail view for one NFT
// (NATIVE_NFT_GUIDE.md §2.4). Programmatic QDialog (no .ui file), opened
// modeless-modal via open() (NOT exec()) so the RPC poll loop keeps flowing and
// the provenance/received-date back-fill lands.
//
// It is handed the EXISTING ContentEngine instance (nftImgCache, which IS a
// ContentEngine) — it NEVER constructs a second engine (no duplicate thread
// pool). The large verified image arrives via the additive
// ContentEngine::posterReady(token, img, verifyState) signal, filtered on this
// dialog's own per-open token (so a fast prev/next drops a stale neighbor).
//
// HONESTY (normative, NATIVE_NFT_GUIDE §4): the verify badge means ONLY "these
// bytes match the on-chain fingerprint" — never genuine/official/original.
// Ownership is PENDING until ~10 confirmations. The explorer link is gated to
// (configured && public && confirmed) and goes through a one-time confirm.
// Video = poster + Open-in-external-player (no in-app playback). C++14 only.
// ============================================================================
#ifndef NFTDETAILDIALOG_H
#define NFTDETAILDIALOG_H

#include "precompiled.h"
#include "nft.h"
#include "contentengine.h"   // ContentDescriptor by value in a slot signature

#include <QDialog>
#include <QVector>
#include <QImage>
#include <QPixmap>

class QLabel;
class QPushButton;
class ContentEngine;
class RPC;

class NFTDetailDialog : public QDialog {
    Q_OBJECT
public:
    // `ordered` is the gallery's ordered POD list (by value) so prev/next never
    // touch a model pointer. `engine` is the EXISTING ContentEngine (upcast from
    // nftImgCache). `rpc` issues the provenance / received-date / send RPCs.
    explicit NFTDetailDialog(const NFTItem& item, const QVector<NFTItem>& ordered,
                             int startIndex, ContentEngine* engine, RPC* rpc,
                             QWidget* parent = nullptr);

private slots:
    void onPosterReady(quint64 token, QImage img, int verifyState);
    void onSendGift();
    void onSell();   // open NFTSellDialog for this owned NFT (#119/PART2)
    void onSaveImage();
    void onCopyId();
    void onCopyFingerprint();
    void onRecheck();
    void onViewInExplorer();
    // Item A: the user explicitly picks a LOCAL file to verify against the on-chain
    // fingerprint (privacy: never auto-fetched). The picker runs in onAttachFile;
    // the reject->hash->match-gate logic lives in beginAttach(path) so a test can
    // drive it without the file dialog (testAttachFile seam).
    void onAttachFile();
    void onAttachDescriptor(quint64 token, ContentDescriptor d);
    void goPrev();
    void goNext();

public:
#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAM (item A / E): run the SAME reject->hash->match-gate path as
    // onAttachFile() but with a caller-supplied path (no QFileDialog, which is
    // un-drivable under offscreen QPA). Never compiled into the shipped app.
    void testAttachFile(const QString& path) { beginAttach(path); }
#endif

private:
    void loadCurrent();            // (re)bind the UI to m_ordered[m_index]
    void requestPoster();          // ask the engine for a large poster (new token)
    // Item A: hash `path` and, ONLY if its anchor matches this NFT's docHashHex,
    // cachePut + re-run the poster so the badge resolves VERIFIED; otherwise an
    // honest red "doesn't match" and NO cachePut. Privacy: local files only.
    void beginAttach(const QString& path);
    void showAttachStatus(const QString& msg, const QString& color);
    void applyVerifyBadge(int verifyState);
    // Neutral TERMINAL badge for "no local bytes" — never the spinning "Checking…".
    // Shown the instant we know there is nothing to check (no work in flight).
    void applyNoBytesBadge();
    void backfillProvenance();     // zslp_gettoken -> Set/series
    void backfillReceived();       // gettransaction -> confirmations/blocktime
    const NFTItem& cur() const { return m_ordered[m_index]; }
    static QString shortId(const QString& hex);

    QVector<NFTItem> m_ordered;
    int              m_index  = 0;
    ContentEngine*   m_engine = nullptr;
    RPC*             m_rpc    = nullptr;

    // Monotonic per-open poster token (>0) so posterReady replies are filtered to
    // the currently-shown item; a stale neighbor's late reply is ignored.
    quint64          m_posterToken = 0;
    QPixmap          m_sourcePixmap;   // held large image; resize re-scales from it
    int              m_lastVerifyState = 0;
    // True only while a poster job for the current item is actually in flight (local
    // bytes present + engine handed the job). When false there is NO work pending, so
    // the verify line must NOT spin "Checking…" — it goes to a neutral terminal state.
    bool             m_posterInFlight = false;

    // Item A (attach-the-file) state. m_attachToken filters the descriptorReady reply
    // to the in-flight attach hash (so a stale neighbor's reply is ignored); m_attachPath
    // is the chosen LOCAL file we cachePut ONLY if its anchor matches docHashHex.
    quint64          m_attachToken = 0;
    QString          m_attachPath;

    // ---- widgets (rebound by loadCurrent) ----
    QLabel*      m_titleName   = nullptr;
    QLabel*      m_titleColl   = nullptr;
    QLabel*      m_stage       = nullptr;   // image stage (painted QPixmap)
    QLabel*      m_badge       = nullptr;   // verify badge glyph + tooltip
    QLabel*      m_verifyLine  = nullptr;   // verdict sentence
    QLabel*      m_privacyPill = nullptr;
    QLabel*      m_mintId      = nullptr;
    QLabel*      m_received    = nullptr;
    QLabel*      m_setLine     = nullptr;
    QLabel*      m_fingerprint = nullptr;
    QLabel*      m_attachStatus= nullptr;   // item A: green/red attach-the-file result
    QPushButton* m_explorerBtn = nullptr;
    QPushButton* m_sendBtn     = nullptr;
    QPushButton* m_sellBtn     = nullptr;   // "Sell" (opens NFTSellDialog)
    QPushButton* m_recheckBtn  = nullptr;   // disabled when there are no local bytes
    QPushButton* m_attachBtn   = nullptr;   // item A: "Attach the file you have…"
    QPushButton* m_saveBtn     = nullptr;   // disabled until a real image is loaded
    QPushButton* m_prevBtn     = nullptr;
    QPushButton* m_nextBtn     = nullptr;
};

#endif // NFTDETAILDIALOG_H
