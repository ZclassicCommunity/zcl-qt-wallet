#include "notifyserver.h"
#include "securerandom.h"   // CSPRNG per-session token

#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QPointer>

#ifndef Q_OS_WIN
#include <sys/stat.h>       // umask() so the unix socket is born owner-only
#endif

// Protocol (one line, newline-terminated): "<token> <id>\n"
//   <token> = the 64-hex per-session secret this server minted
//   <id>    = the 64-hex txid (walletnotify) or blockhash (blocknotify)
// Both fields are hex with no spaces, so a single space-split is unambiguous.

NotifyServer::NotifyServer(QObject* parent) : QObject(parent) {
    // Per-session secret: 32 CSPRNG bytes = 64 lowercase-hex = 256 bits. Minted
    // here so every NotifyServer (every GUI session) has a fresh, independent token
    // and a token captured from a previous run is worthless.
    sessionToken      = secureRandomHex(32);
    sessionTokenBytes = sessionToken.toLatin1();   // cached for the constant-time compare
}

NotifyServer::~NotifyServer() {
    stop();
}

bool NotifyServer::isListening() const {
    return server != nullptr && server->isListening();
}

bool NotifyServer::start(const QString& socketPath) {
    if (isListening())
        return true;                       // idempotent

    path = socketPath;

    // Clear a stale socket file left by a crashed prior session (otherwise listen()
    // fails with AddressInUseError on a dead socket path).
    QLocalServer::removeServer(path);

    if (!server) {
        server = new QLocalServer(this);
        // Owner-only access: on Unix this chmods the socket to user-only; on Windows
        // it builds a DACL granting only the current user. Defense-in-depth — the
        // token is the real gate, but a co-resident user shouldn't even connect.
        server->setSocketOptions(QLocalServer::UserAccessOption);
        // Backpressure: notifies arrive one-at-a-time from the daemon, so a small
        // queue is plenty; this bounds how many connections can pile up at once.
        server->setMaxPendingConnections(16);
        connect(server, &QLocalServer::newConnection, this, &NotifyServer::onNewConnection);
    }

#ifndef Q_OS_WIN
    // Born owner-only: tighten umask across the bind so the socket file is created
    // 0600 with no group/other bits for any instant, then restore.
    mode_t oldMask = ::umask(0077);
#endif
    bool ok = server->listen(path);
#ifndef Q_OS_WIN
    ::umask(oldMask);
#endif
    return ok;
}

void NotifyServer::stop() {
    if (server) {
        server->close();
        QLocalServer::removeServer(path);
        // deleteLater (not delete): stop() may be reached from inside a slot driven
        // by one of this server's child sockets (e.g. a future notified() handler
        // that tears the server down) — a synchronous delete there would free the
        // socket whose lambda is still on the stack. Defer to the event loop.
        server->deleteLater();
        server = nullptr;
    }
}

void NotifyServer::onNewConnection() {
    while (server && server->hasPendingConnections()) {
        QLocalSocket* c = server->nextPendingConnection();
        if (!c) break;

        auto buf = QSharedPointer<QByteArray>::create();

        // Idle reaper: single-shot, parented to the socket (so it dies with it).
        // The timeout is bound to `this` and QPointer-guarded, so a queued timeout
        // can NEVER deref a socket that has already been freed (the bug that made
        // an accumulated-state run crash).
        QPointer<QLocalSocket> guard(c);
        QTimer* reaper = new QTimer(c);
        reaper->setSingleShot(true);
        connect(reaper, &QTimer::timeout, this, [this, guard]() {
            if (guard) finish(guard.data(), QByteArray());   // drop, no signal
        });
        reaper->start(idleMs);

        // A peer that hangs up before sending a full line is cleaned up.
        connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);

        connect(c, &QLocalSocket::readyRead, this, [this, c, buf, reaper]() {
            // Read with a hard per-drain cap so `buf` can never exceed kMaxBytes+1
            // even if the kernel delivers a huge single chunk (readAll() would copy
            // it all). A peer that floods without a newline is then dropped.
            const qint64 room = (qint64)kMaxBytes + 1 - buf->size();
            if (room > 0)
                buf->append(c->read(room));
            if (buf->size() > kMaxBytes) {
                reaper->stop();
                finish(c, QByteArray());
                return;
            }

            // Need a full first line before we decide anything.
            const int nl = buf->indexOf('\n');
            if (nl < 0)
                return;

            reaper->stop();   // from here we decide exactly once and close

            // First line only; trim a trailing \r and surrounding space.
            const QByteArray line = buf->left(nl).trimmed();
            const QList<QByteArray> fields = line.split(' ');

            QString id;
            QByteArray ack;   // non-empty ("OK\n") ONLY on a fully-validated notify
            if (fields.size() == 2 && tokenMatches(fields[0])) {
                // Token gate passed (constant-time). Now a strict 64-hex id check —
                // no shell/file/RPC injection can ride the payload into a refresh.
                static const QRegularExpression hex64("^[0-9a-f]{64}$");
                const QString cand = QString::fromLatin1(fields[1]);
                if (hex64.match(cand).hasMatch()) {
                    id  = cand;
                    ack = "OK\n";   // a validated notify grants ONLY an ack, no data
                }
            }

            finish(c, ack);
            // Emit AFTER teardown so the downstream (debounced) refresh can't re-enter
            // this connection. Only on a validated notify.
            if (!id.isEmpty())
                emit notified(id);
        });
    }
}

void NotifyServer::finish(QLocalSocket* c, const QByteArray& ack) {
    if (!c)
        return;
    // Stop our handlers firing again for this socket (readyRead / reaper timeout),
    // so finish() runs at most once per connection regardless of event ordering.
    disconnect(c, nullptr, this, nullptr);

    if (!ack.isEmpty() && c->state() == QLocalSocket::ConnectedState) {
        c->write(ack);
        c->flush();
    }
    if (c->state() != QLocalSocket::UnconnectedState)
        c->disconnectFromServer();
    c->deleteLater();
}

bool NotifyServer::tokenMatches(const QByteArray& candidate) const {
    const QByteArray& tok = sessionTokenBytes;   // cached; avoids a per-call alloc
    const int n = tok.size();
    // The token LENGTH (64) is fixed and public — only its VALUE is secret. Start
    // `diff` non-zero on a length mismatch, then compare the full token length with
    // NO early-out so the time taken never depends on where (or whether) the first
    // differing byte is. `candidate.size()` is attacker-controlled (not secret), so
    // branching on it leaks nothing about the token value.
    int diff = (candidate.size() == n) ? 0 : 1;
    for (int i = 0; i < n; ++i) {
        const char cc = (i < candidate.size()) ? candidate.at(i) : char(0);
        diff |= (cc ^ tok.at(i));
    }
    return diff == 0;
}
