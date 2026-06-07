#ifndef RPCCLIENT_H
#define RPCCLIENT_H

#include "precompiled.h"

#include <QStringList>   // NFTVerifyResult::reasons (header-signature type, #119/PART2)
#include <QVector>       // nftListOffers callback carries QVector<NFTOfferRow>

#include "balancestablemodel.h"
#include "txtablemodel.h"
#include "ui_mainwindow.h"
#include "mainwindow.h"
#include "connection.h"
#include "notifyserver.h"

using json = nlohmann::json;

class Turnstile;

struct TransactionItem {
    QString         type;
    qint64            datetime;
    QString         address;
    QString         txid;
    double          amount;
    unsigned long   confirmations;
    QString         fromAddr;
    QString         memo;
};

// NFT SELL pillar (PART 2). Plain PODs (C++14 in-class init, no methods) carried by
// VALUE across the synchronous doRPC callback boundary (mirroring TransactionItem) —
// the wrappers fire onDone directly from doRPC's direct-connected lambda, NOT via a
// queued signal, so NO Q_DECLARE_METATYPE/qRegisterMetaType is needed. priceZat is the
// daemon-authoritative price in zatoshi (the verify result echoes the on-chain truth).
struct NFTVerifyResult {
    bool        ok = false;          // verify passed -> a real green verdict, else amber
    QString     tokenId;
    QString     payoutAddr;
    QString     buyerNftAddr;
    qint64      priceZat = 0;
    qint64      expiryHeight = 0;
    QStringList reasons;             // one honest reason per failed check (amber detail)
};

struct NFTOfferRow {
    QString offerId;
    QString tokenId;
    QString role;                    // "sell" (every local-store record)
    QString status;                  // open / filled / expired / canceled
    qint64  priceZat = 0;
    qint64  expiryHeight = 0;
};

// SHIELD (private file send/receive) PODs over the ZDC1 shielded data-channel
// (doc/nft/PRIVACY_TECH.md). Carried by VALUE across the synchronous doRPC
// callback boundary (same pattern as NFTOfferRow) so no metatype registration is
// needed. HONESTY: the data-channel makes only FILE CONTENT private — ownership
// is ALWAYS public, and the ciphertext is stored publicly on-chain forever.

// One row of z_listdatatransfers — session/in-memory only ("sent" this session),
// NOT a durable history (the wrapper's caller must label it as such).
struct DataTransferRow {
    QString transferId;              // 16-hex
    QString fingerprint;             // 64-hex ciphertext anchor (== NFT document_hash)
    QString direction;               // always "sent" in this build (no received list)
    QString status;                  // always "recorded" in this build
    QString fromAddress;
    QString toAddress;
    QString filename;
    int     frames = 0;
};

// The result of z_getdatatransfer (reassemble + verify-before-decrypt). hexData is
// the plaintext and is non-empty ONLY when verified && complete (the daemon never
// returns plaintext on a verify/decrypt failure). `error` carries the daemon's
// honest zdc::status_str text for the four distinct failure states.
struct DataTransferResult {
    QString transferId;
    QString fingerprint;
    bool    verified = false;
    bool    complete = false;
    int     framesReceived = 0;
    QString hexData;                 // plaintext, hex — only if verified+decrypted
    int     size = 0;                // plaintext byte length
    QString filename;
    QString contentType;
    QString onchainFingerprint;      // present for mismatch diagnosis
    QString expectedFingerprint;     // present for mismatch diagnosis
    QString error;                   // daemon's honest status text (empty on success)
};

// The result of z_senddatafile (the immediate, pre-async-result object). After
// this returns the operation runs async; the load-bearing facts (fingerprint =
// NFT document_hash, the per-transfer disclosure key) are already known here.
struct SendDataFileResult {
    QString operationId;
    QString transferId;              // 16-hex
    QString fingerprint;             // 64-hex == NFT document_hash
    QString key;                     // hex per-transfer L3 disclosure key (the secret to safeguard)
    int     frames = 0;
};

struct WatchedTx {
    QString opid;
    Tx tx;
    std::function<void(QString, QString)> completed;
    std::function<void(QString, QString)> error;
};

class RPC
{
public:
    RPC(MainWindow* main);
    ~RPC();

    void setConnection(Connection* c);
    void setEZClassicd(QProcess* p);
    const QProcess* getEZClassicD() { return ezclassicd; }

    // The localhost-only, token-gated push trigger (NOTIFY-SRV). Constructed (and the
    // per-session token minted) ONLY for a non-headless session; null in headless/CLI.
    // The socket is started, and notified() wired, ONLY for the node WE launch (the
    // owned-daemon path) — a foreign/systemd daemon never reaches that wiring, so it
    // keeps the timer poll. See startEmbeddedZClassicd()/setEZClassicd().
    NotifyServer* getNotifyServer() { return notifyServer; }

    void refresh(bool force = false);

    void refreshAddresses();    
    
    void checkForUpdate(bool silent = true);
    void refreshZCLPrice();

    void executeTransaction(Tx tx, 
        const std::function<void(QString opid)> submitted,
        const std::function<void(QString opid, QString txid)> computed,
        const std::function<void(QString opid, QString errStr)> error);

    void fillTxJsonParams(json& params, Tx tx);
    void sendZTransaction(json params, const std::function<void(json)>& cb, const std::function<void(QString)>& err);
    void watchTxStatus();

    const QMap<QString, WatchedTx> getWatchingTxns() { return watchingOps; }
    void addNewTxToWatch(const QString& newOpid, WatchedTx wtx); 

    const TxTableModel*               getTransactionsModel() { return transactionsTableModel; }
    const QList<QString>*             getAllZAddresses()     { return zaddresses; }
    const QList<QString>*             getAllTAddresses()     { return taddresses; }
    const QList<UnspentOutput>*       getUTXOs()             { return utxos; }
    const QMap<QString, double>*      getAllBalances()       { return allBalances; }
    const QMap<QString, bool>*        getUsedAddresses()     { return usedAddresses; }

#ifdef ZCL_WIDGET_TEST
    // TEST-ONLY SEAMS (L1 widget tests). Compiled in ONLY under ZCL_WIDGET_TEST;
    // NEVER in the shipped app. Install balances/z-address state so confirmTx() and
    // createTxFromSendPage() (PRIV-10 change routing) run in-process without a live
    // daemon. testSetZAddresses lets the PRIV-10 fail-open test control whether a
    // Sapling change-sink exists.
    void testSetBalances(QMap<QString, double>* b) { delete allBalances; allBalances = b; }
    void testSetZAddresses(QList<QString>* z)       { delete zaddresses; zaddresses = z; }
    void testSetTAddresses(QList<QString>* t)       { delete taddresses; taddresses = t; }
    // Install the wallet's UTXO set so confirmedSpendableZat() (the confirmed,
    // non-coinbase change basis) can be driven without a daemon.
    void testSetUTXOs(QList<UnspentOutput>* u)      { delete utxos; utxos = u; }
    // SAFE-RACE: install the "fresh" transparent set the NEXT repollTransparentUnspent()
    // splices in (simulates a UTXO landing/leaving at the from-addr during the confirm
    // dwell). One-shot: consumed by the re-poll. Never present in the shipped app build.
    void testSetRepollUTXOs(QList<UnspentOutput>* u) { delete testRepollUTXOs; testRepollUTXOs = u; }
    // MAJOR-3: control the result of the next newZaddr(true) sync-create. A non-empty
    // string makes the create SUCCEED (returns that address); leaving it empty makes
    // the create FAIL (callback never fires), exercising the fail-closed path. The
    // string is consumed (one-shot) so a subsequent create with no fresh value fails.
    void testSetNextZaddrResult(const QString& addr) { testNextZaddrResult = addr; }
    // MINOR-1: install a sentinel Connection so the readiness guard in
    // shieldPublicFunds()/ensureSaplingProvisioned() (which mirrors the real
    // "connection up?" check) is satisfied in-process without a live daemon. The
    // installed Connection is never actually USED on these paths (the seeded
    // Sapling address resolves first), only its non-null-ness is checked.
    void testSetConnection(Connection* c) { conn = c; }
    // NOTIFY-SRV (1.3) real-seam: drive the ACTUAL onNotifyPush()/notifyDebounce
    // (not a stand-in) so a regression in the production wiring is caught.
    void testFireNotifyPush() { onNotifyPush(); }
    bool testNotifyDebounceActive() const { return notifyDebounce && notifyDebounce->isActive(); }

    // PERF harness seam (perf16_modelJank). Each call to updateUI() (under
    // ZCL_WIDGET_TEST) appends the wall-clock nanoseconds the balances-model
    // setNewData() rebuild took to this vector; the perf slot reads it to compute
    // p50/p95/p100. Entirely absent from the shipped app build (guard is compile-time).
    QVector<qint64>& testBalanceModelSamplesNs() { return balanceModelSamplesNs; }
    // Drive the REAL updateUI() path directly (no daemon) so the perf harness can
    // repeatedly re-run the balances-model rebuild over a seeded large dataset. The
    // anyUnconfirmed arg is forwarded straight through.
    void testUpdateUI(bool anyUnconfirmed) { updateUI(anyUnconfirmed); }

    // NFT dialog seams (L1, E-PREREQ): one-shot install the result the NEXT
    // mintNFT/sendNFT delivers, mirroring testSetNextZaddrResult. A non-empty txid
    // => the success cb fires; a non-empty error (set via testSetNextNftError) takes
    // priority and fires the error cb. With NOTHING installed the call returns
    // WITHOUT firing either callback — it simulates an in-flight RPC that never
    // resolves, which drives the closeEvent-swallowed-while-in-flight test. The
    // production path below the guard is byte-identical (zero behavior change).
    void testSetNextMintResult(const QString& txid, const QString& tokenid)
        { testNextMintTxid = txid; testNextMintTokenId = tokenid; }
    void testSetNextSendResult(const QString& txid) { testNextSendTxid = txid; }
    void testSetNextNftError(const QString& err)    { testNextNftError = err; }
    // NFT SELL seams (PART 2): mirror the mint/send seams — a non-empty testNextNftError
    // wins on ALL of these (the calm-error path is L1-testable); else the installed
    // result fires onDone; else the call returns WITHOUT firing either callback (the
    // perpetual-in-flight closeEvent-swallow test). One-shot (consumed on use). The
    // verify seam needs an explicit set flag because an ok==false result is still a valid
    // delivery (we must distinguish "deliver a fail verdict" from "nothing installed").
    void testSetNextOfferResult(const QString& blob, const QString& id, const QString& outpoint)
        { testNextOfferBlob = blob; testNextOfferId = id; testNextOfferOutpoint = outpoint; }
    void testSetNextVerifyResult(const NFTVerifyResult& vr)
        { testNextVerify = vr; testVerifySet = true; }
    void testSetNextTakeResult(const QString& txid, qint64 overshootZat)
        { testNextTakeTxid = txid; testNextTakeOvershoot = overshootZat; }
    void testSetNextOfferList(const QVector<NFTOfferRow>& rows)
        { testNextOfferList = rows; testOfferListSet = true; }
    void testSetNextCancelResult(const QString& txid) { testNextCancelTxid = txid; }
    // SHIELD seams (mirror the SELL seams): a non-empty testNextNftError wins on all of
    // these (the calm-error path is L1-testable, incl. the channel-off -32601 message);
    // else the installed result fires onDone; else the call returns WITHOUT firing either
    // callback (the perpetual-in-flight closeEvent-swallow test). One-shot. The get/list
    // results need explicit "set" flags because an unverified/empty result is still a
    // valid delivery (we must distinguish "deliver a fail verdict" from "nothing installed").
    void testSetNextSendDataFileResult(const SendDataFileResult& r)
        { testNextSendDataFile = r; testSendDataFileSet = true; }
    void testSetNextDataTransferList(const QVector<DataTransferRow>& rows)
        { testNextDataXferList = rows; testDataXferListSet = true; }
    void testSetNextDataTransferResult(const DataTransferResult& r)
        { testNextDataXfer = r; testDataXferSet = true; }
    // txReceivedDate seam: the confs/blocktime the NEXT call delivers. confs<0 (or
    // testReceivedSet left false) routes to the error cb instead.
    void testSetNextReceived(qint64 confs, qint64 blocktime)
        { testNextReceivedConfs = confs; testNextReceivedBlocktime = blocktime; testReceivedSet = true; }
#endif

    // SAFE-RACE: synchronously-driven re-poll of ONLY the transparent UTXOs, spliced into
    // the cached `utxos` WITHOUT dropping the shielded-note rows. Fires the async
    // listunspent (production) or applies an injected test set, and invokes cb(true) once
    // fresh data has landed. Returns false (and cb(false)) when it could not fire (no
    // connection / no test seam), so the caller fails closed. See verifyAutoShieldUnchanged.
    bool repollTransparentUnspent(const std::function<void(bool)>& cb);

    void newZaddr(bool sapling, const std::function<void(json)>& cb);
    void newTaddr(const std::function<void(json)>& cb);

    void getZPrivKey(QString addr, const std::function<void(json)>& cb);
    void getTPrivKey(QString addr, const std::function<void(json)>& cb);
    void importZPrivKey(QString addr, bool rescan, const std::function<void(json)>& cb);
    void importTPrivKey(QString addr, bool rescan, const std::function<void(json)>& cb);

    void shutdownZClassicd();

    // Called from QCoreApplication::aboutToQuit, i.e. on EVERY quit route -- window
    // close, File->Exit, SIGINT, AND the macOS app-menu "Quit" / Cmd-Q, which calls
    // QApplication::quit() directly and bypasses MainWindow::closeEvent()/
    // shutdownZClassicd(). Marks the shutdown as expected and stops the pollers so the
    // async RPC handlers never flash a spurious "error connecting to zclassicd" dialog
    // while the embedded node is being torn down.
    void onAboutToQuit();

    // SAFETY GATE for the manual Help -> Repair path: positively confirm the
    // RPC-owned embedded node is NOT running before any datadir mutation. Polls
    // state(); if still alive, terminate() + waitForFinished, then escalate to
    // kill() + waitForFinished. Returns true only when confirmed NotRunning (or
    // there is no embedded node / it's an external daemon we don't own). The 30s
    // soft cap in shutdownZClassicd() means "quit promptly", NOT "confirmed
    // exited", so the repair path must call this before touching blocks/chainstate.
    bool confirmEmbeddedStopped();

    // preserveBalances=true => SOFT disconnect: keep the last-painted balance labels
    // and tables, just de-confidence the hero to the WARMING "refreshing" state. Used
    // by the debounced getinfo error path so a single failed poll never blanks a
    // funded wallet. Default false keeps every existing caller's full-teardown behavior.
    void noConnection(bool preserveBalances = false);
    bool isEmbedded() { return ezclassicd != nullptr; }

    // ---- NOTIFY-SRV push-wiring pure helpers (used by prod + asserted by L1) ----
    // Build the two daemon notify args (-walletnotify / -blocknotify) that invoke THIS
    // exe's `--notify %s` connector. The exe path is QUOTED (it may contain spaces);
    // `%s` is OUTSIDE the quotes so the daemon substitutes the txid/blockhash. NO token
    // ever rides the command line (it lives in a 0600 file) -- the L1 test asserts the
    // args carry no 64-hex token. Static + pure so a test can assert it without a daemon.
    static QStringList buildNotifyArgs(const QString& exePath);

    // Decide the refresh poll interval. While a healthy push channel is delivering
    // notifies we back the timer WAY off (heartbeat only); when push is stale/absent
    // (foreign/headless or the channel went quiet) we keep the existing 20s/5s poll.
    // Static + pure so the decision is unit-tested without an RPC/timer/daemon.
    static int desiredPollMs(bool isSyncing, bool pushHealthy);

    // Push-channel tuning. kHeartbeatPollMs: the slow poll used while push is healthy
    // (a sanity heartbeat, not the primary update path). kPushHealthyWindowSecs: how
    // recent the last validated notify must be for the channel to count as "healthy"
    // (PERF-5: auto-resume the 20s poll when the channel goes stale).
    static constexpr int   kHeartbeatPollMs       = 120 * 1000;   // 120 sec
    static constexpr qint64 kPushHealthyWindowSecs = 180;          // 3 min
    // Debounce window: coalesce a notify BURST (a block + its wallet txns all fire at
    // once) into a single refresh shortly after the quiet point.
    static constexpr int   kNotifyDebounceMs      = 200;

    // ---- Native NFT write path (dev/testnet; regtest-proven, not mainnet-hardened) ----
    // Mint a NEW public ZSLP NFT from a file the user picked. docHashHex MUST be 64
    // lowercase hex (the ContentEngine document_hash); name/ticker optional;
    // documentUrl is NEVER auto-fetched (pass "" unless the user typed one). On
    // success onDone(txid, tokenid) — the token is 0-conf/PENDING (invisible to the
    // confirmed-only indexer until it confirms), so the caller shows a pending card;
    // we also kick refreshNFTs(). On failure onErr(calm honest message). C++14:
    // empty-QString sentinels, no std::optional.
    void mintNFT(const QString& name, const QString& ticker,
                 const QString& docHashHex, const QString& documentUrl,
                 const std::function<void(QString txid, QString tokenid)>& onDone,
                 const std::function<void(QString errStr)>& onErr);

    // Gift/transfer an NFT (amount fixed at 1) to a recipient t-address. On success
    // the token leaves this wallet immediately (0-conf), so refreshNFTs() will drop
    // it once confirmed; onDone(txid) lets the caller show "sent — confirming". On
    // failure onErr(calm honest message).
    void sendNFT(const QString& tokenId, const QString& toAddress,
                 const std::function<void(QString txid)>& onDone,
                 const std::function<void(QString errStr)>& onErr);

    // Detail-dialog provenance back-fill: zslp_gettoken "tokenId" -> the success cb
    // receives the unwrapped token-metadata object (ticker/name/documenthash/...).
    // Any error -> the err cb fires with a calm message; the dialog keeps honest
    // defaults (Creator "Unknown", Set "Not part of a set"), never a dialog.
    void nftProvenance(const QString& tokenId,
                       const std::function<void(json tokenMeta)>& onDone,
                       const std::function<void(QString errStr)>& onErr);

    // Detail-dialog received-date back-fill: gettransaction "txid" -> the success cb
    // receives (confirmations, blocktime). confs<10 => "Just arrived — confirming…";
    // >=10 => ISO date + "block N" (the caller formats). Any error -> onErr.
    void txReceivedDate(const QString& txid,
                        const std::function<void(qint64 confirmations, qint64 blocktime)>& onDone,
                        const std::function<void(QString errStr)>& onErr);

    // ---- NFT SELL pillar (PART 2; non-consensus ZSLP atomic swap RPC wrappers) ----
    // Each wraps one daemon nft_* RPC (src/rpc/nftoffer.cpp). All take a SINGLE JSON
    // OBJECT param (we send real JSON over HTTP — the client.cpp CLI arg-conversion
    // table is irrelevant). Calm honest error mapping reuses zslpCalmError. DRY on
    // doRPC / nftJsonStr / nftJsonInt, exactly like mintNFT/sendNFT.

    // SELL: build a buyer-sealed atomic offer. priceZcl is the human ZCL amount (the
    // wrapper converts to zat). buyerNftAddr is REQUIRED (the daemon throws without it).
    // payoutAddr "" => the daemon uses a fresh own address. expiryHeight 0 => the daemon
    // default (~7 days). onDone(offerBlob "znftoffer:...", offerId, nftOutpoint "txid:n");
    // onErr(calm).
    void nftMakeOffer(const QString& tokenId, double priceZcl, const QString& buyerNftAddr,
                      const QString& payoutAddr, int expiryHeight,
                      const std::function<void(QString offerBlob, QString offerId, QString nftOutpoint)>& onDone,
                      const std::function<void(QString errStr)>& onErr);

    // BUY (MANDATORY pre-pay safety check): decode + live-verify the offer. onDone fires
    // with a filled NFTVerifyResult for BOTH ok and not-ok (ok==false is a valid delivery
    // carrying the reasons). onErr only on transport/parse failure.
    void nftVerifyOffer(const QString& offerBlob,
                        const std::function<void(NFTVerifyResult)>& onDone,
                        const std::function<void(QString errStr)>& onErr);

    // BUY settle. acknowledge=true sends the overshoot-to-fee consent. onDone(txid,
    // overshootZat) — overshootZat is the total network fee donated (shown, never silent).
    void nftTakeOffer(const QString& offerBlob, bool acknowledge,
                      const std::function<void(QString txid, qint64 overshootZat)>& onDone,
                      const std::function<void(QString errStr)>& onErr);

    // List local offers (the store is all "mine" — the daemon has no "mine" filter,
    // so this takes no filter arg). onDone(rows); onErr(calm).
    void nftListOffers(const std::function<void(QVector<NFTOfferRow>)>& onDone,
                       const std::function<void(QString errStr)>& onErr);

    // Cancel an outstanding sell offer by self-spending its NFT UTXO. onDone(txid).
    void nftCancelOffer(const QString& offerId,
                        const std::function<void(QString txid)>& onDone,
                        const std::function<void(QString errStr)>& onErr);

    // ---- SHIELD pillar (private file send/receive over the ZDC1 data-channel) ----
    // Each wraps one daemon z_*datafile / z_*datatransfer RPC (src/rpc/datachannel.cpp),
    // a SINGLE JSON OBJECT param. HONEST error mapping: the data-channel RPCs are only
    // registered under -datachannel (default OFF); when off the dispatcher returns
    // RPC_METHOD_NOT_FOUND (-32601) — we map that to a CALM "enable private transfers in
    // Settings first" message (datachannelOff()) instead of a scary raw error.
    //
    // The plaintext NEVER leaves the user's machine for a SEND: the GUI hex-encodes the
    // local file bytes and sends `hexdata` (not a server filepath). The 40000-byte cap
    // is enforced UP FRONT in the dialog (nftdc::ZDC_MAX_FILE_BYTES). acknowledge_permanent
    // is ALWAYS true here (the dialog earns that consent via a mandatory checkbox; the
    // daemon also refuses without it). content_type/filename are optional metadata.

    // SEND a private file. `hexData` is the lowercase-hex of the local file bytes (the
    // caller reads them binary-safe and hex-encodes). fromAddr/toAddr are Sapling
    // z-addresses (zs..). z_senddatafile is ASYNC and returns an operationid; the
    // immediate object carries the fingerprint (= NFT document_hash) + per-transfer key,
    // which is what the success screen needs — so we surface that immediate result via
    // onDone (no operation poll needed for the load-bearing facts). onErr(calm honest).
    void sendDataFile(const QString& fromAddr, const QString& toAddr,
                      const QString& hexData, const QString& filename,
                      const QString& contentType,
                      const std::function<void(SendDataFileResult)>& onDone,
                      const std::function<void(QString errStr)>& onErr);

    // LIST the data transfers this node sent THIS SESSION (z_listdatatransfers; in-memory,
    // 72h TTL — NOT a durable or received history; the caller labels it honestly).
    // onDone(rows); onErr(calm). -32601 (channel off) routes to onErr(datachannelOff()).
    void listDataTransfers(const std::function<void(QVector<DataTransferRow>)>& onDone,
                           const std::function<void(QString errStr)>& onErr);

    // GET (reassemble + VERIFY-BEFORE-DECRYPT) a private file. Identify by transfer_id
    // (16-hex) OR fingerprint (64-hex) — pass the one you have, "" for the other. `address`
    // optional (defaults to the recorded toaddress, else scans viewable addrs) supports
    // CROSS-WALLET receive. `verifyFingerprint` optional out-of-band 64-hex anchor. The
    // daemon verifies BEFORE any decrypt and returns plaintext ONLY when verified+complete;
    // onDone always fires with a filled DataTransferResult (even on a verify/decrypt
    // FAILURE — the .error field carries the honest state). onErr only on transport/off.
    void getDataTransfer(const QString& transferId, const QString& fingerprint,
                         const QString& address, const QString& verifyFingerprint,
                         const std::function<void(DataTransferResult)>& onDone,
                         const std::function<void(QString errStr)>& onErr);

    // PROVENANCE: the PUBLIC transfer history of an NFT (zslp_listtransfers, newest-first,
    // reorg-safe). HONEST: ZSLP ownership is always public — this is the on-chain
    // chain-of-custody, not a private log. onDone(array of JSON transfer rows); onErr(calm).
    void nftListTransfers(const QString& tokenId,
                          const std::function<void(json transfers)>& onDone,
                          const std::function<void(QString errStr)>& onErr);

    // Pure conversion helper (L0-testable): human ZCL (double) -> zatoshi (qint64),
    // rounded to the nearest zat. Clamped at 0. The ONE place price strings become zat.
    static qint64 zclToZat(double zcl);

    // Honest gating: the PRIVATE (ZDC1 shielded) mint/gift path is not built yet, so
    // this is hard-false. The mint/send dialogs disable the Private tile as "Coming
    // in this release" rather than ship a dead Create button (spec §2.5 polarity).
    static bool isPrivateMintWired() { return false; }

    QString getDefaultSaplingAddress();
    QString getDefaultTAddress();

    void getAllPrivKeys(const std::function<void(QList<QPair<QString, QString>>)>);

    Turnstile*  getTurnstile()  { return turnstile; }
    Connection* getConnection() { return conn; }

    // ---- NFT-CAPABILITY probe (BUG #1 fix) ----
    // True UNLESS we have CONFIRMED the currently-attached node lacks NFT support. The
    // user can attach to an OLDER foreign ZClassic node (e.g. a running beta6 release
    // daemon) that has no zslp_*/nft_*/z_*datafile RPCs even though THIS wallet's
    // embedded node does — gating the NFT surfaces on this flag turns the cryptic
    // "method not found" into honest, actionable guidance. We PROBE an actual NFT RPC
    // (we NEVER parse the daemon version string, which the embedded node mis-reports).
    // Fail-OPEN while the probe is unresolved (returns true) so the gallery never
    // flashes the "unsupported" panel during the brief warmup before the first reply.
    // Re-probed on every setConnection() (a reconnect may be a different node).
    bool nodeSupportsNFT() const { return nftCapability != NftCap::Unsupported; }
    // True once the probe RESOLVED, so the UI can tell "still checking" from a
    // confirmed yes/no (it shows the unsupported panel ONLY on a resolved-No).
    bool nodeNFTProbeResolved() const { return nftCapability != NftCap::Unknown; }
    // The ONE honest guidance string shown when the attached node lacks NFT support:
    // the Collections panel, the disabled-entry tooltips, AND every NFT/zslp RPC
    // wrapper's -32601 mapping all share it, so an action can never dead-end on a raw
    // "method not found".
    static QString nftUnsupportedGuidance();
    // Fire the probe against the current connection. Safe no-op when conn==nullptr;
    // on resolve it notifies the MainWindow so the gallery + entry points re-gate.
    void probeNFTCapability();

#ifdef ZCL_WIDGET_TEST
    // TEST SEAM (BUG #1): force the capability flag without a daemon so the L1 tests
    // can drive the "unsupported node" gating + calm -32601 mapping deterministically.
    void testSetNodeSupportsNFT(bool supported) {
        nftCapability = supported ? NftCap::Supported : NftCap::Unsupported;
    }
    // TEST SEAM: run the SHARED zslpCalmError() mapper against a synthetic JSON-RPC
    // error body { "error": { "code": <code> } } with a null reply, exactly as every
    // NFT/zslp wrapper does on a daemon error. Lets the unit test assert the -32601 ->
    // nftUnsupportedGuidance() mapping directly (no daemon, no widgets).
    static QString testCalmErrorForCode(int errorCode);
#endif

private:
    void refreshBalances();

    void refreshTransactions();
    void refreshSentZTrans();
    void refreshReceivedZTrans(QList<QString> zaddresses);

    // Phase C1: fetch the wallet's REAL on-chain ZSLP NFTs and feed the native
    // Collections gallery (MainWindow::setNFTItems). zslp_listmytokens lists the
    // tokens this wallet holds (intersect of the indexed tokens with our t-addresses
    // — ZSLP rides transparent dust); for each we zslp_gettoken for the on-chain
    // document hash / url / decimals / genesis height, then map to NFTItem.
    //
    // GRACEFUL FALLBACK: zslp_listmytokens throws RPC_MISC_ERROR (code -1) when the
    // daemon lacks -zslpindex; we catch that in the error callback and feed an empty
    // "index off" set (no crash, no error dialog). An index-on wallet that owns no
    // tokens simply yields an empty gallery.
    //
    // PRIVACY: we read metadata + the on-chain document hash ONLY and set
    // cachePath="" on every item — the GUI never auto-fetches a remote documenturl
    // image (no IP / interest leak). Bytes arrive later from the local cache or an
    // explicit user action. Read-only: no key material, no node mutation.
    void refreshNFTs();

    bool processUnspent     (const json& reply, QMap<QString, double>* newBalances, QList<UnspentOutput>* newUtxos);
    // SAFE-RACE: build a new owned list = the shielded-note rows kept from `oldUtxos`
    // (classified by !Settings::isTAddress) + every row of `freshT` (the re-polled
    // transparent set). Frees both inputs. Used by repollTransparentUnspent so a
    // transparent-only re-poll never drops the wallet's z-notes.
    QList<UnspentOutput>* mergeTransparentUnspent(QList<UnspentOutput>* oldUtxos,
                                                  QList<UnspentOutput>* freshT);
    void updateUI           (bool anyUnconfirmed);

    void getInfoThenRefresh(bool force);

    // Deliverable A: read-only refresh of the "you're helping the network" panel
    // (MainWindow::netHelp* labels) from getnetworkinfo + getpeerinfo. `connections`
    // is passed from the enclosing getinfo poll. Never mutates node state; both inner
    // calls are error-ignoring so an old/foreign/warming daemon degrades silently.
    void refreshNetworkHelpPanel(int connections);

    // NOTIFY-SRV: handle a validated push (NotifyServer::notified). Records the epoch
    // (so getInfoThenRefresh's interval picker sees the channel as healthy) and
    // (re)starts the single-shot debounce; the debounce timeout runs refresh(TRUE).
    void onNotifyPush();

    // RUNTIME STUB AUTO-HEAL: evaluate (conservatively) whether the live node is a
    // healthy-but-stuck STUB — it started fine, loaded a tiny non-genesis chain from an
    // aborted P2P sync, and now finds 0 peers — and, if so, route into the SAME
    // redownloadChain() ladder the manual Help -> Repair uses (via
    // MainWindow::autoHealStubChain). Called from the getblockchaininfo sync poll only
    // when (isSyncing && connections==0 && ezNoPeerPolls>=3). Fires ONLY when ALL hold:
    //   (1) connections == 0 sustained well past the banner (ezNoPeerPolls >= 12, ~60s
    //       at the 5s syncing cadence);
    //   (2) the on-disk blocks/ store is unambiguously a STUB — its total size is below
    //       a hard 1 GiB floor (a synced chain is ~10 GiB; a stub is tens of MB),
    //       measured via ConnectionLoader::blocksDirSizeBytes from the active datadir.
    //       A large blocks/ (a fully-synced node that is merely OFFLINE) is NEVER
    //       wiped — that is the dangerous lookalike;
    //   (3) it has NOT already auto-redownloaded this run (ezStubAutoHealTried) AND the
    //       persisted per-install cooldown (rpc/stubHeal.count, capped) is not exhausted,
    //       so a genuinely peerless environment can't loop wipe->rebootstrap->wipe.
    // Never fires for an external daemon we don't own, or when blocks/ can't be measured.
    // Returns true ONLY if it actually launched a heal (the caller then suppresses the
    // peerless banner for this tick).
    bool maybeAutoHealStubChain(int connections);

    // Async getbootstrapinfo poll (warmup-EXEMPT) that refreshes the ezBootstrap*
    // cache; fired from the peerless path so an in-progress bootstrap-snapshot
    // download is shown as "Downloading blockchain snapshot — X%" instead of
    // "waiting for peers", and so the stub-heal never wipes a live download.
    void pollBootstrapSnapshotStatus();

    // Once-per-process latch so the runtime stub auto-heal launches at most one
    // re-download per app run; combined with the persisted cooldown below.
    bool                        ezStubAutoHealTried         = false;
    // Once-per-session latch for clearing the persisted per-install stub-heal cap on
    // the first peered poll (so we don't rewrite QSettings every tick).
    bool                        ezStubHealCooldownCleared   = false;

    void getBalance(const std::function<void(json)>& cb);

    // INSTANT-BALANCE fast path: one getwalletsummary call returns the wallet's
    // transparent + private (shielded) totals in a single round-trip (no per-address
    // listunspent join), letting the hero balance paint instantly on connect. The
    // success cb receives the raw summary json; the err cb receives the parsed
    // JSON-RPC body so the caller can detect -32601 ("Method not found") on an OLD
    // daemon that lacks the method and latch summaryCapable=false. KEY-1: this reads
    // ONLY balance numbers — no key material is ever returned, serialized, or logged.
    void getWalletSummary(const std::function<void(json)>& cb,
                          const std::function<void(const json&)>& err);

    void getTransparentUnspent  (const std::function<void(json)>& cb, const std::function<void(void)>& err);
    void getZUnspent            (const std::function<void(json)>& cb, const std::function<void(void)>& err);
    void getTransactions        (const std::function<void(json)>& cb);
    void getZAddresses          (const std::function<void(json)>& cb);
    void getTAddresses          (const std::function<void(json)>& cb);

    // Runtime daemon-crash recovery: catch the embedded node dying at runtime
    // (OOM/crash) and offer to restart it instead of stranding the user in a
    // permanent "No Connection". ezExpectedShutdown gates the normal-shutdown
    // path; ezRestartCount caps automatic restarts.
    void handleEZClassicdCrash(int exitCode, QProcess::ExitStatus status);
    void restartEmbeddedZClassicd();   // relaunch the dead node, stripping repair flags
    bool                        ezExpectedShutdown          = false;
    bool                        ezCrashDialogOpen           = false;
    int                         ezRestartCount              = 0;
    QMetaObject::Connection     ezCrashConn;

    // Re-entrancy guard for shutdownZClassicd(): the shutdown wait spins a nested
    // event loop (and the "please wait" dialog is non-modal for the first 700ms),
    // so the main window stays live. This latch makes a second Quit / window-close
    // during that window a no-op instead of stacking a second nested loop. It
    // serializes re-entrancy WITHIN one shutdown only: it is cleared at every exit
    // of shutdownZClassicd() and when setEZClassicd() adopts a fresh live node, so
    // it never latches for the process lifetime (a later restart can shut down too).
    bool                        ezShuttingDown              = false;

    // C2/F5: once we've legitimately confirmed the chain tip (blocks>=headers with
    // peers) this latch keeps us "synced" through brief peer drops so an established
    // wallet doesn't flicker back to "syncing".
    bool                        ezEverSynced                = false;

    // C9/F6: debounce the waiting-for-peers banner; a single connections==0 sample
    // shouldn't flip the banner. Counts consecutive peerless polls; reset on a peer.
    int                         ezNoPeerPolls               = 0;

    // transient-disconnect debounce: a SINGLE failed getinfo poll (one dropped
    // reply, a momentary RPC stall) must NOT wipe a funded wallet's hero to grey
    // "0 ZCL" and demote Send. Counts consecutive getinfo error replies; reset to 0
    // on any successful getinfo. The error path keeps the last balances painted
    // (soft disconnect) until this crosses the threshold, then does a full teardown.
    int                         getInfoErrCount             = 0;

    // Cached getbootstrapinfo snapshot-download state (post-connect). A node mid
    // bootstrap-snapshot download reports 0 normal P2P peers + warming RPC, which the
    // peerless banner/heal would otherwise mislabel as "waiting for peers" (and could
    // even wipe it as an abandoned stub). pollBootstrapSnapshotStatus() refreshes this
    // async; the peerless path reads it so an active download shows real progress and
    // is never healed.
    bool                        ezBootstrapActive           = false;
    int                         ezBootstrapPct              = 0;
    qint64                      ezBootstrapRecv             = 0;
    qint64                      ezBootstrapTotal            = 0;
    double                      ezBootstrapMbps             = 0.0;

    // Edit #6 (bob-fix): once-per-run guard for MainWindow::showForeignNodeStuck(). The
    // poller fires that actionable dialog EXACTLY ONCE when an ATTACHED FOREIGN node
    // (ezclassicd == nullptr — NOT one we launched) has been peerless/stuck for a
    // sustained window. An OWNED embedded node (it self-heals + has bootstrap peers) and a
    // FOREIGN node that has peers / is syncing/synced NEVER trigger it. This bool keeps it
    // from re-showing every poll.
    bool                        ezForeignStuckShown         = false;

    // W1-4: SUSTAINED-timeout gate for the foreign-node-stuck dialog. The poll-count
    // gate (ezNoPeerPolls) alone can be reached quickly if the sync cadence is fast,
    // false-firing the dialog during a legitimate multi-minute first-run warmup where
    // a foreign node is briefly peerless. Require the peerless condition to ALSO have
    // persisted for a wall-clock window (~120s, mirroring the connection.cpp warmup
    // patience) before surfacing it. Started on the first peerless sample, invalidated
    // the moment any peer appears.
    QElapsedTimer               ezForeignStuckSince;

    // G6: the warmup-wedge cap (heal/attempts.warmupRestart) must only be cleared by
    // SUSTAINED health, never by one getinfo — otherwise a node that answers a single
    // getinfo and then re-wedges would reset the counter forever and never escalate
    // to NEEDS_MANUAL. clearHealLedger() (one getinfo) intentionally leaves
    // warmupRestart alone; we clear it here only after several consecutive healthy
    // sync polls OR after the chain tip has demonstrably advanced past warmup.
    int                         ezHealthyPolls              = 0;
    bool                        ezWarmupWedgeCleared        = false;

    // INSTANT-BALANCE daemon-capability gate. We assume the connected daemon supports
    // getwalletsummary — the single-round-trip total (read from the daemon's cached
    // note values, no per-note re-decrypt) used to paint the hero balance fast on
    // every refresh. When true, getwalletsummary {0} is the SOLE hero source and the
    // slow z_gettotalbalance is skipped; when false z_gettotalbalance is the SOLE
    // source instead. The FIRST refreshBalances() this session doubles as the version
    // probe: if getwalletsummary returns JSON-RPC -32601 ("Method not found") — i.e.
    // an OLD daemon — this latches to false and we NEVER call it again this session,
    // falling back EXACTLY to the legacy z_gettotalbalance path. Any OTHER (transient)
    // summary error does NOT latch; that cycle paints from z_gettotalbalance and the
    // next poll retries the summary. getwalletsummary {0} and z_gettotalbalance {0}
    // return the same satoshi totals, so the displayed number is identical regardless
    // of source. The per-address listunspent join remains the sole owner of the
    // per-address balances model / UTXO set and never touches the hero labels.
    bool                        summaryCapable              = true;

    // ---- NFT-CAPABILITY probe state (BUG #1 fix) ----
    // Tri-state, reset to Unknown on every setConnection() and resolved by the first
    // probeNFTCapability() reply: Supported (an NFT RPC answered) / Unsupported (-32601
    // "method not found" — an older foreign node) / Unknown (probe in flight). The
    // accessors above fail OPEN while Unknown so the UI never flashes the unsupported
    // panel during warmup. We PROBE, never parse the version string.
    enum class NftCap { Unknown, Supported, Unsupported };
    NftCap                      nftCapability               = NftCap::Unknown;

    Connection*                 conn                        = nullptr;
    QProcess*                   ezclassicd                     = nullptr;

    // NOTIFY-SRV push trigger. Owned by RPC (constructed in the ctor for a non-headless
    // session; stopped in shutdownZClassicd()/onAboutToQuit and stop()+delete in ~RPC).
    // Null in headless/CLI and never started/wired for a foreign daemon.
    NotifyServer*               notifyServer                = nullptr;
    // Epoch (secs) of the last validated push; 0 = none this session. Drives the
    // push-healthy window in getInfoThenRefresh()'s interval picker.
    qint64                      lastNotifyEpoch             = 0;

    QList<UnspentOutput>*       utxos                       = nullptr;
    QMap<QString, double>*      allBalances                 = nullptr;
    QMap<QString, bool>*        usedAddresses               = nullptr;
    QList<QString>*             zaddresses                  = nullptr;
    QList<QString>*             taddresses                  = nullptr;
    
    QMap<QString, WatchedTx>    watchingOps;

    TxTableModel*               transactionsTableModel      = nullptr;
    BalancesTableModel*         balancesTableModel          = nullptr;

    QTimer*                     timer;
    QTimer*                     txTimer;
    QTimer*                     priceTimer;
    // Single-shot debounce for NOTIFY-SRV pushes (kNotifyDebounceMs). Same
    // ownership/cleanup pattern as timer/txTimer (parented to main, deleted in ~RPC,
    // stopped in onAboutToQuit).
    QTimer*                     notifyDebounce;

    Ui::MainWindow*             ui;
    MainWindow*                 main;
    Turnstile*                  turnstile;

    // Current balance in the UI. If this number updates, then refresh the UI
    QString                     currentBalance;

    // W1-3: defer the price/update fetch out of the connect critical path. The
    // first checkForUpdate() after launch must NOT pop the update-available modal
    // on top of the freshly-opened window (it stalls perceived startup). The modal
    // is suppressed exactly once — the version is recorded so a LATER check still
    // surfaces it (and a non-silent, user-initiated Help->Check still always shows).
    bool                        firstUpdateCheckDone        = false;

    // W1-6: throttle the sent-z confirmation refresh. Cache the last-seen
    // confirmation count per sent-z txid; once a tx is deeply confirmed
    // (>= kDeepConfirmations) we stop re-querying gettransaction for it and reuse
    // the cached deep count, re-batching only the shallow/unconfirmed subset.
    QMap<QString, long>         sentZConfCache;
    static constexpr long       kDeepConfirmations          = 10;
    // W1-6 reorg safety: the chain height the cache reflects. A backward move => reorg
    // => drop the cache so a deep-confirmed row can't keep showing a stale (too-high)
    // confirmation count after the chain rewinds.
    int                         sentZConfCacheHeight        = 0;

#ifdef ZCL_WIDGET_TEST
    // MAJOR-3 test seam backing store: the address the next newZaddr(true) returns
    // (one-shot). Empty => the sync-create fails (fail-closed path). Never present
    // in the shipped app build.
    QString                     testNextZaddrResult;
    // PERF harness backing store: per-update nanoseconds spent in the balances-model
    // rebuild inside updateUI(). Filled only under ZCL_WIDGET_TEST; read by
    // perf16_modelJank. Never present in the shipped build.
    QVector<qint64>             balanceModelSamplesNs;
    // SAFE-RACE backing store: the transparent set the next repollTransparentUnspent()
    // splices in (one-shot). nullptr => the re-poll cannot fire (fail-closed).
    QList<UnspentOutput>*       testRepollUTXOs = nullptr;
    // NFT dialog seam backing stores (one-shot; consumed inside mintNFT/sendNFT/
    // txReceivedDate). Empty txid + empty error => the call returns without firing a
    // callback (the perpetual-in-flight closeEvent test). Never in the shipped build.
    QString                     testNextMintTxid, testNextMintTokenId, testNextSendTxid;
    QString                     testNextNftError;
    qint64                      testNextReceivedConfs     = 0;
    qint64                      testNextReceivedBlocktime = 0;
    bool                        testReceivedSet           = false;
    // NFT SELL seam backing stores (one-shot; consumed inside the nft offer wrappers).
    QString                     testNextOfferBlob, testNextOfferId, testNextOfferOutpoint;
    NFTVerifyResult             testNextVerify;
    bool                        testVerifySet             = false;
    QString                     testNextTakeTxid;
    qint64                      testNextTakeOvershoot     = 0;
    QVector<NFTOfferRow>        testNextOfferList;
    bool                        testOfferListSet          = false;
    QString                     testNextCancelTxid;
    // SHIELD seam backing stores (one-shot; consumed inside the data-channel wrappers).
    SendDataFileResult          testNextSendDataFile;
    bool                        testSendDataFileSet       = false;
    QVector<DataTransferRow>    testNextDataXferList;
    bool                        testDataXferListSet       = false;
    DataTransferResult          testNextDataXfer;
    bool                        testDataXferSet           = false;
#endif
};

#endif // RPCCLIENT_H
