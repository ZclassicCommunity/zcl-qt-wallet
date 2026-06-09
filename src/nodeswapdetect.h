#ifndef NODESWAPDETECT_H
#define NODESWAPDETECT_H

// ============================================================================
// IDIOTPROOF NODE-SWAP — pure, header-only service detection.
//
// Extracted into its own tiny header (only <QString>/<QStringList>) so the L0 unit
// suite can compile + test it WITHOUT linking connection.cpp (which pulls libsodium,
// the RPC class, the daemon lifecycle, etc.). Both the product (connection.cpp) and
// the test include this single definition, so there is exactly one parser.
//
// C++14: no std::optional / std::string_view. `inline` so multiple TUs may include it.
// ============================================================================

#include <QString>
#include <QStringList>
#include <QLatin1Char>
#include <QDir>

// SERVICE DETECTION. Given the raw bytes of /proc/<pid>/cgroup and the resolved
// /proc/<pid>/exe target, decide whether the foreign node is a SYSTEM SERVICE that
// systemd would auto-restart (so a graceful "stop" would just respawn and the swap must
// NOT fight it). A node is classified a service when ANY of:
//   * the cgroup line names "system.slice" (systemd system manager), OR
//   * the cgroup line names a "....service" unit (systemd-managed unit), OR
//   * the exe lives under /usr/bin or /usr/sbin (a packaged/system install; /usr/local is
//     EXCLUDED as the FHS user-install prefix — a hand-built node there is not, by itself, a
//     service).
// Best-effort unit name (the trailing "<name>.service" component) is returned via outUnit;
// it falls back to "zclassicd" when a service is detected but no unit name could be parsed.
// EMPTY outUnit + return false == a user-session process (safe to graceful-stop + swap).
inline bool classifyForeignServiceFromProc(const QString& cgroupContents,
                                           const QString& exePath,
                                           QString* outUnit) {
    if (outUnit)
        outUnit->clear();

    const bool inSystemSlice  = cgroupContents.contains(QStringLiteral("system.slice"));
    bool       hasServiceUnit = false;

    // SERVICE-UNIT detection from the cgroup, with a critical exclusion: a node launched in
    // the USER session (e.g. the tray wallet's own daemon) lives under a path like
    //   /user.slice/user-1000.slice/user@1000.service/app.slice/<name>.scope
    // whose ONLY ".service" component is the session manager "user@<uid>.service" and whose
    // actual leaf is a ".scope". That is NOT a systemd SYSTEM service and MUST stay
    // graceful-stoppable (else the swap would wrongly route a stoppable node to the
    // "run systemctl stop" guidance — a dead-end). So a ".service" only counts as a real
    // service unit when it is the cgroup LEAF and is not the "user@*.service" session manager.
    for (const QString& line : cgroupContents.split(QLatin1Char('\n'))) {
        QString path = line.trimmed();
        if (path.isEmpty())
            continue;
        // The leaf is the last '/'-separated component (the process's own cgroup unit/scope).
        int lastSlash = path.lastIndexOf(QLatin1Char('/'));
        QString leaf = (lastSlash >= 0) ? path.mid(lastSlash + 1) : path;
        if (!leaf.endsWith(QStringLiteral(".service")))
            continue;                                   // leaf is a .scope/.slice -> not a service unit
        if (leaf.startsWith(QStringLiteral("user@")))
            continue;                                   // user-session manager, not the app's unit
        // A genuine service unit at the cgroup leaf (system OR a `systemctl --user` service,
        // both of which auto-restart) — surface it as the unit to stop.
        if (outUnit) *outUnit = leaf;
        hasServiceUnit = true;
        break;
    }
    // NOTE: a system.slice node with an opaque (non-".service") leaf is still classified a
    // service via `inSystemSlice` below; its unit then falls back to the default name.

    // A node under /usr/bin or /usr/sbin is, by convention, a packaged/system-managed daemon
    // (apt/.deb) rather than a user's tray-wallet daemon (cache/home path) or a self-built node.
    // We deliberately EXCLUDE /usr/local/* — the FHS user-install prefix — so a hand-compiled
    // /usr/local/bin node is NOT misclassified as a service and routed to the systemctl-stop
    // dead-end. If such a node really is a service, the cgroup checks above catch it; and the
    // swap's rebind-poll fallback catches any auto-restart service this heuristic misses.
    const bool packaged = exePath.startsWith(QStringLiteral("/usr/bin/")) ||
                          exePath.startsWith(QStringLiteral("/usr/sbin/"));

    const bool isService = inSystemSlice || hasServiceUnit || packaged;
    if (isService && outUnit && outUnit->isEmpty())
        *outUnit = QStringLiteral("zclassicd");   // sensible default unit name
    return isService;
}

// The exact command we tell the user to run for a detected service (honest, copy-pasteable).
inline QString nodeSwapServiceStopCommand(const QString& unit) {
    QString u = unit.isEmpty() ? QStringLiteral("zclassicd") : unit;
    return QStringLiteral("sudo systemctl stop ") + u;
}

// ============================================================================
// DATADIR-PIN SAFETY (safety point #4). A foreign node may have been launched with a
// command-line `-datadir=/Y` that DIFFERS from the datadir we resolved out of the conf
// the GUI reads. If we then started our embedded node with no datadir pin it would adopt
// the conf/default datadir = a DIFFERENT wallet.dat, and the user would face an
// apparently-empty wallet (their funds untouched but invisible — forbidden by the mandate).
//
// Pure, header-only so the L0 suite can test it without linking connection.cpp. Both the
// product (startNodeSwap / startEmbeddedZClassicd) and the test call THIS one parser.
// C++14: no std::optional; an empty QString is the "no -datadir on the cmdline" sentinel.

// Pull the LAST `-datadir=<value>` out of a space-joined daemon command line (last-wins is
// the bitcoin/zcash arg convention). Returns the verbatim value, or EMPTY if none is present.
// `-datadir <value>` (space-separated) is NOT a form zclassicd accepts, so only `=` is parsed.
inline QString foreignDataDirFromCmdline(const QString& cmdline) {
    QString out;   // empty == no -datadir on the cmdline
    // Plain split (no SkipEmptyParts enum, which differs between Qt 5.12/5.15): empty tokens
    // simply never match the "-datadir=" prefix, so they are harmless to scan.
    for (const QString& tok : cmdline.split(QLatin1Char(' '))) {
        if (tok.startsWith(QStringLiteral("-datadir="))) {
            QString v = tok.mid(QStringLiteral("-datadir=").size());
            if (!v.isEmpty())
                out = v;   // keep scanning: last -datadir wins
        }
    }
    return out;
}

// The decision: does the foreign node's command-line datadir let us SAFELY adopt the
// identical datadir on the embedded launch, or must we REFUSE the in-app swap because the
// embedded node would otherwise run on a DIFFERENT wallet.dat?
//
//  * foreignDataDir EMPTY  -> the foreign node used the conf/default datadir, exactly what
//    our embedded node would resolve too. Nothing to pin; allow the swap.  (refuse=false, pin="")
//  * foreignDataDir present, confDataDir EMPTY -> we couldn't resolve a conf datadir, so adopt
//    the foreign one explicitly (it is the authoritative wallet location).  (refuse=false, pin=foreign)
//  * foreignDataDir == confDataDir (path-cleaned) -> adopt it explicitly so the launch is
//    unambiguous even if the conf later changes.                            (refuse=false, pin=foreign)
//  * foreignDataDir != confDataDir -> DIVERGENT. Do NOT silently launch on the wrong datadir;
//    refuse and route the caller to honest guidance.                        (refuse=true,  pin="")
//
// Paths are compared with QDir::cleanPath (no CWD-relative resolution, so the decision is
// deterministic and side-effect-free for the L0 test). Foreign `-datadir` values are
// absolute in every supported daemon usage, so cleanPath equality is exact.
struct NodeSwapDataDirPlan {
    bool    refuse  = false;   // true == the divergent-datadir guard fired; do NOT swap in-app
    QString pin;               // -datadir to pin onto the embedded launch (empty == pin nothing)
};

inline NodeSwapDataDirPlan nodeSwapDataDirPlan(const QString& foreignDataDir,
                                               const QString& confDataDir) {
    NodeSwapDataDirPlan plan;
    if (foreignDataDir.isEmpty())
        return plan;                                   // refuse=false, pin="" : nothing to pin

    if (confDataDir.isEmpty()) {
        plan.pin = foreignDataDir;                     // adopt the only datadir we know
        return plan;
    }

    const QString f = QDir::cleanPath(foreignDataDir);
    const QString c = QDir::cleanPath(confDataDir);
    if (f == c) {
        plan.pin = foreignDataDir;                     // same datadir: pin it explicitly
        return plan;
    }

    plan.refuse = true;                                // DIVERGENT: refuse, do not guess
    return plan;
}

// DATADIR-VERIFY gate (safety point #4, the NO-PIDFILE case). When the foreign node could NOT
// be identified — no readable pidfile, so foreignPid<=0 (cross-user, restricted /proc, or it
// runs on a datadir we never looked in) — we also could not read its actual `-datadir`. The
// embedded node would then fall back to the conf/default datadir. That is only safe if the
// foreign node is actually running on THAT datadir; if it isn't, launching the embedded node
// there would serve a DIFFERENT (likely empty) wallet.dat = the user's funds go invisible.
//
// So: refuse the in-app swap when we couldn't identify the node AND the conf/default datadir
// shows no sign of a live chain (confDatadirHasChain == false). A node we COULD identify
// (foreignPid>0) carries its `-datadir` on the cmdline and is handled by nodeSwapDataDirPlan()
// above, so this never fires for it (incl. bob's pidfile case).
//
// Pure + header-only so the L0 suite tests the DECISION without any filesystem/daemon. The
// caller computes confDatadirHasChain (e.g. blocksDirSizeBytes(confDir) >= 0).
inline bool nodeSwapRefuseUnverifiedDatadir(qint64 foreignPid, bool confDatadirHasChain) {
    return foreignPid <= 0 && !confDatadirHasChain;
}

#endif // NODESWAPDETECT_H
