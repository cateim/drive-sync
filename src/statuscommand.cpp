#include "statuscommand.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

#include "accounts.h"
#include "conflictcache.h"
#include "synclog.h"

namespace {

// A cold find over a large Drive can take a while; it only runs when the
// cached count is older than kConflictCacheSeconds.
constexpr int kFindTimeoutMs = 30000;

// Runs a program and waits. A timeout kills it, so a stuck child never
// outlives the command.
bool runAndWait(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output) {
    if (program.isEmpty())
        return false;
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs))
        return false;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

int countConflicts(const BackendPaths &paths, const QDateTime &now) {
    const QString cacheFile = paths.appState.isEmpty() ? QString() : paths.appState + "/conflicts.json";
    if (!cacheFile.isEmpty()) {
        const ConflictCount cached = readConflictCache(cacheFile);
        if (conflictCacheFresh(cached, now))
            return cached.count;
    }
    QByteArray output;
    const bool ok = runAndWait(paths.find, conflictFindArguments(paths.drive), kFindTimeoutMs, &output);
    // find exits 1 when one folder can't be read but still lists the rest,
    // so only an empty answer from a failed run is an error.
    int count = 0;
    for (const QByteArray &line : output.split('\n'))
        if (line.count('\t') >= 2)
            ++count;
    QString error;
    if (paths.find.isEmpty())
        error = "noFind";
    else if (!ok && output.isEmpty())
        error = "scanFailed";
    // Never during the move to account1: the cache would land at a place it
    // is leaving (the move deletes the old cache instead of moving it).
    if (!cacheFile.isEmpty() && !moveInProgress(paths.globalState()))
        writeConflictCache(cacheFile, {count, error, now});
    return count;
}

// What one account's own files say: the log, the folder, the conflict
// count, the pause, the fix and its config. systemd is asked for every
// account at once by gatherAccounts.
SnapshotSources readAccountFiles(const BackendPaths &paths, const QDateTime &now) {
    SnapshotSources src;
    src.now = now;
    src.configError = accountConfigError(paths);

    bool found = false;
    bool truncated = false;
    const QString text = SyncLog::readRecent(paths.log, kTailBytes, &found, &truncated);
    src.logFound = found;
    src.runs = SyncLog::parse(text, kStateRuns, truncated);

    src.resyncRunning = Backend::resyncRunning(paths.stateDir, paths.proc);
    src.driveFound = QFileInfo(paths.drive).isDir();
    src.conflicts = src.driveFound ? countConflicts(paths, now) : 0;
    src.managed = paths.managed;
    src.intervalSeconds = paths.intervalSeconds;
    src.moving = moveInProgress(paths.globalState());
    if (!paths.appState.isEmpty()) {
        src.pause = readPause(pauseFilePath(paths.appState));
        src.fixPresent = fixPresent(paths.appState);
        if (src.fixPresent)
            src.fix = readFix(paths.appState);
    }
    return src;
}

// The tone's rank for worstAccount: anything unknown counts as an error,
// as toneFor makes an unknown status one.
int toneRank(const QString &tone) {
    if (tone == "ok")
        return 0;
    if (tone == "busy")
        return 1;
    if (tone == "warning")
        return 2;
    return 3;
}

// One line of "status" as text: the headline and what it leaves out.
// What a block or an entry calls the account: its label, else its folder's
// base name (paths built by hand have no label).
QString displayLabel(const BackendPaths &account) {
    return account.label.isEmpty() ? QFileInfo(QDir::cleanPath(account.drive)).fileName() : account.label;
}

QString statusLine(const Snapshot &snapshot) {
    QString line = snapshot.summary.status;
    if (!snapshot.summary.warnings.isEmpty())
        line += " (warnings: " + snapshot.summary.warnings.join(", ") + ")";
    return line;
}

}  // namespace

QList<SnapshotSources> gatherAccounts(const QList<BackendPaths> &accounts, const QDateTime &now, int timeoutMs,
                                      QList<QDateTime> *nextRuns, const OldDriveSyncCheck &oldDriveSync) {
    QStringList units;
    QStringList timers;
    bool accountsMode = false;
    for (const BackendPaths &paths : accounts) {
        units << accountUnits(paths);
        timers << paths.timerUnit;
        accountsMode = accountsMode || paths.accountsMode;
    }
    // An old drive-sync next to accounts: the plain units in the same call.
    const bool lookForOld = accountsMode && oldDriveSync;
    if (lookForOld)
        units << plainDriveSyncUnits();
    // Every account's units in one call, and one list-timers: the bar polls
    // this, and a call per account would multiply its cost.
    const QString systemctl = accounts.isEmpty() ? QString() : accounts.first().systemctl;
    QByteArray show;
    const bool showOk = !accounts.isEmpty() && runAndWait(systemctl, systemdShowArguments(units), timeoutMs, &show);
    const auto byId = Backend::parseShowById(show);

    QList<SnapshotSources> sources;
    bool anyWorks = false;
    for (const BackendPaths &paths : accounts) {
        SnapshotSources src = readAccountFiles(paths, now);
        // Each of the account's three units must answer, found by its Id; a
        // missing one makes this account alone unavailable.
        src.systemctlWorks = showOk;
        for (const QString &unit : accountUnits(paths))
            src.systemctlWorks = src.systemctlWorks && byId.contains(unit);
        if (src.systemctlWorks) {
            src.service = byId.value(paths.serviceUnit);
            src.timer = byId.value(paths.timerUnit);
            src.fixUnitActive = fixUnitBusy(byId.value(paths.fixUnit));
            src.fixUnitResult = byId.value(paths.fixUnit).value("Result");
            anyWorks = true;
        }
        if (lookForOld && paths.accountsMode)
            src.oldDriveSync = oldDriveSync(byId, paths.drive, &src.oldDriveSyncHold);
        sources << src;
    }

    if (nextRuns) {
        nextRuns->clear();
        QByteArray json;
        const bool timersOk = anyWorks && runAndWait(systemctl, listTimersArguments(timers), timeoutMs, &json);
        for (qsizetype i = 0; i < accounts.size(); ++i)
            *nextRuns << (timersOk && sources.at(i).systemctlWorks
                              ? Backend::parseNextElapse(json, accounts.at(i).timerUnit)
                              : QDateTime());
    }
    return sources;
}

SnapshotSources gatherSources(const BackendPaths &paths, const QDateTime &now, int timeoutMs, QDateTime *nextRun) {
    QList<QDateTime> nextRuns;
    const QList<SnapshotSources> sources = gatherAccounts({paths}, now, timeoutMs, nextRun ? &nextRuns : nullptr);
    if (nextRun)
        *nextRun = nextRuns.value(0);
    return sources.first();
}

int worstAccount(const QList<Snapshot> &snapshots) {
    int worst = snapshots.isEmpty() ? -1 : 0;
    for (int i = 1; i < snapshots.size(); ++i)
        if (toneRank(snapshots.at(i).tone) > toneRank(snapshots.at(worst).tone))
            worst = i;
    return worst;
}

QJsonObject statusListToJson(const QList<Snapshot> &snapshots, const QList<BackendPaths> &accounts,
                             const QList<QDateTime> &nextRuns, const QDateTime &now) {
    // The view with no account stands for an old drive-sync that was not
    // taken over; with no old log either there is nothing at all to list.
    const bool nothing = snapshots.isEmpty()
                         || (snapshots.size() == 1 && accounts.value(0).id.isEmpty()
                             && snapshots.first().summary.status == "noLog");
    QJsonArray list;
    bool canSyncNow = false;
    if (!nothing) {
        for (qsizetype i = 0; i < snapshots.size(); ++i) {
            list.append(accountToJson(snapshots.at(i), accounts.value(i), nextRuns.value(i)));
            canSyncNow = canSyncNow || snapshots.at(i).canSyncNow;
        }
    }
    const int worst = nothing ? -1 : worstAccount(snapshots);
    return {{"version", 3},
            {"now", now.isValid() ? now.toSecsSinceEpoch() : 0},
            {"tone", worst < 0 ? QString("error") : snapshots.at(worst).tone},
            {"worst", worst < 0 ? QString() : accounts.value(worst).id},
            {"canSyncNow", canSyncNow},
            {"accounts", list}};
}

int runStatusCommand(const QStringList &arguments, const QList<BackendPaths> &accounts,
                     const QList<AccountNotice> &notices, QTextStream &out, QTextStream &err, int timeoutMs,
                     const OldDriveSyncCheck &oldDriveSync) {
    bool json = false;
    bool all = false;
    bool usage = false;
    for (const QString &argument : arguments) {
        bool *flag = argument == "--json" ? &json : argument == "--all" ? &all : nullptr;
        if (!flag || *flag)
            usage = true;
        else
            *flag = true;
    }
    if (usage || (all && !json)) {
        err << usageText();
        err.flush();
        return 1;
    }

    // On stderr, so the bar, which reads stdout, never sees them.
    for (const AccountNotice &notice : notices) {
        if (notice.kind == "staleConfig")
            err << "drive-sync: " << notice.path
                << " is ignored: DriveSync keeps each account's config under accounts/\n";
        else if (notice.kind == "moveStuck")
            err << "drive-sync: the move to accounts stopped halfway; clear what stuckReason in " << notice.path
                << " names, then run: drive-sync accounts --migrate\n";
    }
    err.flush();

    // main always passes at least the view with no account; an empty list
    // reads as a drive-sync nobody can ask about.
    const QList<BackendPaths> read = accounts.isEmpty() ? QList<BackendPaths>{BackendPaths()} : accounts;
    // UTC for the same reason as Backend::recompute: local-time comparisons
    // make Qt resolve the zone on every call.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<QDateTime> nextRuns;
    QList<Snapshot> snapshots;
    for (const SnapshotSources &src : gatherAccounts(read, now, timeoutMs, json ? &nextRuns : nullptr, oldDriveSync))
        snapshots << buildSnapshot(src);
    if (json && all) {
        out << QJsonDocument(statusListToJson(snapshots, read, nextRuns, now)).toJson(QJsonDocument::Compact) << "\n";
    } else if (json) {
        // Version 2 has room for one account: the one that needs the user most.
        const int worst = worstAccount(snapshots);
        out << QJsonDocument(snapshotToJson(snapshots.at(worst), nextRuns.value(worst), now))
                   .toJson(QJsonDocument::Compact)
            << "\n";
    } else if (snapshots.size() == 1) {
        out << statusLine(snapshots.first()) << "\n";
    } else {
        for (qsizetype i = 0; i < snapshots.size(); ++i)
            out << displayLabel(read.at(i)) << " (" << read.at(i).id << ")\n  " << statusLine(snapshots.at(i)) << "\n";
    }
    out.flush();
    return 0;
}

QString usageText() {
    return "usage: drive-sync [open [--account <id>]]\n"
           "       drive-sync status [--json [--all]]\n"
           "       drive-sync sync [--json]\n"
           "       drive-sync pause [--minutes N | --until-tomorrow] [--json]\n"
           "       drive-sync resume [--json]\n"
           "       drive-sync signin [--abandon] [--json]\n"
           "       drive-sync setup --folder <path> [--skip <cloud folder>]... [--json]\n"
           "       drive-sync folders [--remote <remote:>] [--json] [[--] <cloud folder>]\n"
           "       drive-sync settings [--interval N | --max-delete N | --folder <path> | --filters <file> |\n"
           "                           --skip <cloud folder>... | --skip-none] [--json]\n"
           "       drive-sync takeover --check | --apply | --undo [--json]\n"
           "       drive-sync fix --start | --confirm | --answers <file.json> | --cancel | --retry [--json]\n"
           "       drive-sync deletes --list | --allow <count> [--json]\n"
           "       drive-sync accounts [--migrate] [--json]\n"
           "       drive-sync run-condition | run | notify | watchdog | fix-run    (for the units)\n"
           "       drive-sync --help\n"
           "Every command but setup, takeover, watchdog and accounts takes --account <id> (account1...) as its first\n"
           "two arguments. Without it, status, sync, pause and resume act on every account, and the others on\n"
           "the only one.\n";
}
