#include "controlcommands.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <unistd.h>

#include "accounts.h"
#include "fixstate.h"
#include "listingdiff.h"
#include "machineguard.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kCommandTimeoutMs = 15000;

// Answers the caller once, as JSON or as a line on err.
int answer(bool json, const QString &error, QTextStream &out, QTextStream &err) {
    if (json) {
        out << QJsonDocument(QJsonObject{{"ok", error.isEmpty()}, {"error", error}}).toJson(QJsonDocument::Compact)
            << "\n";
        out.flush();
    } else if (!error.isEmpty()) {
        err << "drive-sync: " << error << "\n";
        err.flush();
    }
    return error.isEmpty() ? 0 : 1;
}

// Splits off "--json"; any other argument left in rest is for the caller.
bool takeJson(QStringList *arguments) { return arguments->removeAll("--json") > 0; }

bool runProgram(const QString &program, const QStringList &arguments, QString *error) {
    return runProcess(program, arguments, kCommandTimeoutMs, nullptr, error);
}

QStringList withLogFile(QStringList arguments, const QString &file) {
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == "--log-file" && i + 1 < arguments.size())
            arguments[++i] = file;
        else if (arguments.at(i).startsWith("--log-file="))
            arguments[i] = "--log-file=" + file;
    }
    return arguments;
}


// One account's answer to sync, pause or resume: empty error is ok.
struct AccountAnswer {
    QString id;
    QString error;
};

// Answers sync, pause or resume once, over every account they acted on. The
// JSON keeps "ok" and "error" at the top (the window and the bar read those;
// the error is the first account's that failed) and lists each account.
// Without JSON each refusal is a line, named by its account when there are
// several.
int answerAccounts(bool json, const QList<AccountAnswer> &answers, QTextStream &out, QTextStream &err) {
    QString firstError;
    QJsonArray list;
    for (const AccountAnswer &a : answers) {
        list.append(QJsonObject{{"id", a.id}, {"ok", a.error.isEmpty()}, {"error", a.error}});
        if (firstError.isEmpty())
            firstError = a.error;
    }
    if (json) {
        out << QJsonDocument(QJsonObject{{"ok", firstError.isEmpty()}, {"error", firstError}, {"accounts", list}})
                   .toJson(QJsonDocument::Compact)
            << "\n";
        out.flush();
    } else {
        for (const AccountAnswer &a : answers)
            if (!a.error.isEmpty())
                err << "drive-sync: " << (answers.size() > 1 ? a.id + ": " : QString()) << a.error << "\n";
        err.flush();
    }
    return firstError.isEmpty() ? 0 : 1;
}

}  // namespace

QString startOneRun(const BackendPaths &paths) {
    QString error;
    // --no-block: the run takes minutes; the state shows it once it starts.
    runProgram(paths.systemctl, {"--user", "start", "--no-block", paths.serviceUnit}, &error);
    return error;
}

int commandAnswer(bool json, const QString &error, QTextStream &out, QTextStream &err) {
    return answer(json, error, out, err);
}

QString takeoverLockPath(const QString &appState) { return appState + "/takeover-running"; }

bool takeoverLockHeld(const QString &appState, const QString &proc) {
    QFile lock(takeoverLockPath(appState));
    if (!lock.open(QIODevice::ReadOnly))
        return false;
    bool ok = false;
    const qint64 pid = lock.readAll().trimmed().toLongLong(&ok);
    QFile cmdline(QString("%1/%2/cmdline").arg(proc).arg(pid));
    if (!ok || pid <= 0 || !cmdline.open(QIODevice::ReadOnly))
        return false;
    // The program is the first argument, matched by its exact name: "drive-sync"
    // is also a piece of drive-sync.sh and of the units' command lines, so a
    // substring match would take a pid reused by them for the app. A window
    // left open across a rename still runs under its old name and may hold
    // the lock.
    const QString program = QFileInfo(QString::fromLocal8Bit(cmdline.readAll().split('\0').value(0))).fileName();
    return program == "drive-sync" || program == "drivesync" || program == "syncwatch";
}

bool takeTakeoverLock(const QString &stateRoot, QString *error) {
    const QString path = takeoverLockPath(stateRoot);
    QDir().mkpath(stateRoot);
    // The check, the removal of a stale file and the new file are one step:
    // two takers that both saw the same stale file would otherwise both
    // remove it and write their own. An flock on the state root itself
    // serialises them and leaves no file behind; the pid file stays the
    // lock every reader (and an older drive-sync) checks.
    const int folder = ::open(QFile::encodeName(stateRoot).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder < 0 || ::flock(folder, LOCK_EX) != 0) {
        if (folder >= 0)
            ::close(folder);
        *error = "cannot take the takeover lock " + path;
        return false;
    }
    const auto unlockFolder = qScopeGuard([folder] { ::close(folder); });
    if (QFileInfo::exists(path)) {
        if (takeoverLockHeld(stateRoot, "/proc")) {
            *error = "another takeover, undo or move to accounts is running";
            return false;
        }
        QFile::remove(path);
    }
    QFile lock(path);
    if (!lock.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || lock.write(QByteArray::number(QCoreApplication::applicationPid()) + "\n") <= 0) {
        *error = "cannot take the takeover lock " + path;
        return false;
    }
    return true;
}

void releaseTakeoverLock(const QString &stateRoot) {
    QFile lock(takeoverLockPath(stateRoot));
    if (lock.open(QIODevice::ReadOnly) && lock.readAll().trimmed() == QByteArray::number(QCoreApplication::applicationPid())) {
        lock.close();
        lock.remove();
    }
}

bool runProcess(const QString &program, const QStringList &arguments, int timeoutMs, QByteArray *output,
                QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    if (program.isEmpty()) {
        *error = "a program it needs was not found";
        return false;
    }
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        *error = QString("%1 did not answer").arg(program);
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return true;
    *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (error->isEmpty())
        *error = QString("%1 exited with %2").arg(program).arg(process.exitCode());
    return false;
}

namespace {

QString syncOne(const BackendPaths &paths) {
    if (takeoverLockHeld(paths.globalState(), paths.proc))
        return "a takeover is running; sync when it ends";
    // run-condition would skip the run anyway: say so instead of "ok".
    if (fixPresent(paths.appState))
        return "a fix is under way, and it holds every run until it ends";
    // A pause that ended is cleared by the next run itself, so it does not stop one.
    const PauseMark mark = readPause(pauseFilePath(paths.appState));
    if (paths.managed && mark.present && !pauseEnded(mark, QDateTime::currentDateTimeUtc()))
        return "drive-sync is paused; run drive-sync resume first";
    return startOneRun(paths);
}

QString pauseOne(const BackendPaths &paths, const QDateTime &now, const QDateTime &until, bool *stale) {
    if (!paths.managed)
        return "pausing needs DriveSync to run drive-sync; take it over first";
    // The mark is the account's state: never written at a place the move to
    // account1 has just left.
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty())
        return lock.refusal();
    if ((*stale = lock.stale()))
        return QString();
    const QString file = pauseFilePath(paths.appState);
    PauseMark mark = readPause(file);
    if (!mark.present || !mark.since.isValid())
        mark.since = now;  // a new end keeps when the pause began
    mark.present = true;
    mark.until = until;
    if (!writePause(file, mark))
        return QString("cannot write %1").arg(file);
    return QString();
}

QString resumeOne(const BackendPaths &paths, bool *stale) {
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty())
        return lock.refusal();
    if ((*stale = lock.stale()))
        return QString();
    const QString file = pauseFilePath(paths.appState);
    if (!clearPause(file))
        return QString("cannot remove %1").arg(file);
    // The pause is gone; a fix still holds the runs, and its end brings them back.
    if (fixPresent(paths.appState))
        return QString();
    return startOneRun(paths);
}

}  // namespace

int runSyncCommand(const QStringList &arguments, const QList<BackendPaths> &accounts, QTextStream &out,
                   QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    if (!rest.isEmpty())
        return answer(json, "usage: drive-sync sync [--json]", out, err);
    QList<AccountAnswer> answers;
    for (const BackendPaths &paths : accounts)
        answers << AccountAnswer{paths.id, syncOne(paths)};
    return answerAccounts(json, answers, out, err);
}

int runPauseCommand(const QStringList &arguments, const QList<BackendPaths> &accounts, const QDateTime &now,
                    QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    const QString usage = "usage: drive-sync pause [--minutes N | --until-tomorrow] [--json]";
    QDateTime until;
    if (rest.size() == 2 && rest.at(0) == "--minutes") {
        bool ok = false;
        const int minutes = rest.at(1).toInt(&ok);
        if (!ok || minutes <= 0)
            return answer(json, usage, out, err);
        until = now.addSecs(qint64(minutes) * 60);
    } else if (rest == QStringList{"--until-tomorrow"}) {
        until = nextMorning(now.toLocalTime());
    } else if (!rest.isEmpty()) {
        return answer(json, usage, out, err);
    }
    QList<AccountAnswer> answers;
    for (const BackendPaths &paths : accounts) {
        bool stale = false;
        answers << AccountAnswer{paths.id, pauseOne(paths, now, until, &stale)};
        if (stale)
            return kAccountPathsMoved;  // nothing printed: main runs it again
    }
    return answerAccounts(json, answers, out, err);
}

int runResumeCommand(const QStringList &arguments, const QList<BackendPaths> &accounts, QTextStream &out,
                     QTextStream &err) {
    QStringList rest = arguments;
    const bool json = takeJson(&rest);
    if (!rest.isEmpty())
        return answer(json, "usage: drive-sync resume [--json]", out, err);
    QList<AccountAnswer> answers;
    for (const BackendPaths &paths : accounts) {
        // With several accounts, resume resumes the paused ones (spec 12,
        // "Commands"): an account that was not paused gets no extra run. One
        // account keeps what resume always did.
        if (accounts.size() > 1 && !readPause(pauseFilePath(paths.appState)).present) {
            answers << AccountAnswer{paths.id, QString()};
            continue;
        }
        bool stale = false;
        answers << AccountAnswer{paths.id, resumeOne(paths, &stale)};
        if (stale)
            return kAccountPathsMoved;  // nothing printed: main runs it again
    }
    return answerAccounts(json, answers, out, err);
}

int runConditionCommand(const BackendPaths &paths, const SyncConfig &config, const QDateTime &now,
                        bool oldDriveSyncHold) {
    // The move to account1 renames the config and the state under the
    // units: no run until it ends or rolls back.
    if (moveInProgress(paths.globalState()))
        return 1;
    // A fix holds every run from its first step to its end: its own unit
    // runs rclone, and a normal run in between would break the new base.
    if (fixPresent(paths.appState))
        return 1;
    // An old drive-sync syncs this folder too (accounts mode only: before the
    // move its units are what a takeover moves aside). Two schedulers on one
    // folder would break its base, so this one waits until it is removed.
    if (paths.accountsMode && oldDriveSyncHold)
        return 1;
    // The machine's runOnlyWhen, as systemd reads an ExecCondition: a crash
    // or 255 is a failure, any other non-zero exit a skip. A machine.json it
    // cannot read holds too, as a failure.
    QString guardError;
    const QString guard = machineGuardCommand(paths, config, &guardError);
    if (!guardError.isEmpty())
        return 255;
    if (const int verdict = runMachineGuard(guard); verdict != 0)
        return verdict;
    const QString file = pauseFilePath(paths.appState);
    const PauseMark mark = readPause(file);
    if (!mark.present)
        return 0;
    if (pauseEnded(mark, now))
        // 1 would read as a deliberate skip, forever: say it failed instead.
        return clearPause(file) ? 0 : 255;
    return 1;
}

QStringList runCommandPlan(const SyncConfig &config, QString *refusal, const std::optional<AllowDeletes> &allow) {
    if (!filtersUsable(config.filtersFile, refusal)) {
        // No .md5 and no listings: no base was ever built (a first setup whose
        // dry run nobody confirmed), which is not an edit of the filters.
        if (!QFile::exists(config.filtersFile + ".md5")
            && currentListings(config.workDir, bisyncSessionName(config.remote, config.localPath)).first.isEmpty())
            *refusal = QString("the first sync of %1 has not been confirmed yet, so nothing syncs; %2")
                           .arg(config.localPath, kFirstSyncPendingReason);
        return {};
    }
    const QStringList arguments = rcloneRunArguments(config);
    if (!allow)
        return arguments;
    // bisync checks each side against that side's own count in the last
    // listing (Path1 is the cloud), so the side that needs the most sets it.
    const int configured = configuredMaxDelete(arguments);
    const QPair<QString, QString> listings =
        currentListings(config.workDir, bisyncSessionName(config.remote, config.localPath));
    QString error;
    const auto cloud = listings.first.isEmpty() ? std::nullopt : readListing(listings.first, &error);
    const auto notebook = listings.second.isEmpty() ? std::nullopt : readListing(listings.second, &error);
    if (cloud && notebook) {
        int percent = configured;
        if (allow->cloudCount > 0)
            percent = qMax(percent, maxDeletePercent(allow->cloudCount, int(cloud->size()), configured));
        if (allow->notebookCount > 0)
            percent = qMax(percent, maxDeletePercent(allow->notebookCount, int(notebook->size()), configured));
        return withMaxDelete(arguments, percent);
    }
    // No listings to count (the run would fail on them anyway): the abort
    // line's total, for the side with more deletions.
    const int largestSide = qMax(allow->cloudCount, allow->notebookCount);
    return withMaxDelete(arguments, maxDeletePercent(largestSide, allow->total, configured));
}

QString allowedRunLogLine(const AllowDeletes &allow, int percent) {
    return QString("DriveSync raised --max-delete to %1% for this run: %2 deletions confirmed")
        .arg(QString::number(percent), QString::number(allow.count));
}

void appendToLog(const QString &logFile, const QString &level, const QString &message) {
    QFile log(logFile);
    if (log.open(QIODevice::Append))
        log.write(QString("%1 %2: %3\n")
                      .arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), message)
                      .toUtf8());
}

QStringList runPlanTakingMark(const SyncConfig &config, const QString &appState, const QString &rclone,
                              const QDateTime &now, QString *refusal, QString *note) {
    note->clear();
    const std::optional<AllowDeletes> allow = takeAllowDeletes(appState, now);
    const QStringList normal = runCommandPlan(config, refusal);
    if (!allow || normal.isEmpty())
        return normal;
    const QString kept = QString("DriveSync kept --max-delete at %1% for this run: ").arg(configuredMaxDelete(normal));
    // The confirmation is for the files of one stopped run: that run must
    // still be the newest one (a mark written while the confirmed run was
    // already under way would otherwise serve a later run), and every
    // deletion the dry run finds must be in its list.
    const std::optional<BlockedDeletes> stopped =
        blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), std::numeric_limits<int>::max());
    if (!stopped || !allow->runStart.isValid()
        || stopped->runStart.toSecsSinceEpoch() != allow->runStart.toSecsSinceEpoch()) {
        *note = kept + "a confirmation of deletions was dropped, since the run it was for is no longer the last one";
        return normal;
    }
    // What the run deletes or overwrites on the notebook goes to a folder of
    // DriveSync's state instead (rclone 1.75.2 bisync takes --backup-dir2 on
    // local paths); what it deletes in the cloud goes to the Drive trash.
    const QString backupDir =
        appState + "/allowed-backup/" + stopped->runStart.toLocalTime().toString("yyyyMMdd-HHmmss");
    const QStringList raised =
        runCommandPlan(config, refusal, allow) + QStringList{"--backup-dir2", backupDir};
    const int percent = configuredMaxDelete(raised);
    const QString dryLog = appState + "/allowed-dry-run.log";
    QFile::remove(dryLog);
    QString error;
    // No time limit: the unit's TimeoutStartSec bounds the run, dry part included.
    const bool dryRunOk = runProcess(rclone, withLogFile(raised, dryLog) + QStringList{"--dry-run"}, -1, nullptr, &error);
    QFile dry(dryLog);
    const QString dryText = dry.open(QIODevice::ReadOnly) ? QString::fromUtf8(dry.readAll()) : QString();
    // A dry run that never reached bisync found nothing, which proves nothing.
    const bool dryRunRan = SyncLog::newestRunStart(dryText) >= 0;
    const BlockedDeletes found = deletionsInNewestRun(dryText, std::numeric_limits<int>::max());
    QSet<QString> confirmed;
    for (const BlockedDelete &d : stopped->list)
        confirmed.insert(QString(d.deletedInCloud ? "1:" : "2:") + d.path);
    int unconfirmed = 0;
    for (const BlockedDelete &d : found.list)
        if (!confirmed.contains(QString(d.deletedInCloud ? "1:" : "2:") + d.path))
            ++unconfirmed;
    if (dryRunOk && dryRunRan && unconfirmed == 0) {
        // Kept for the check after the run: the stopped run is no longer
        // the newest one by then, and its list may be out of the tail.
        // Now, after the dry run: the run starts right after this.
        ConfirmedRun run;
        run.at = QDateTime::currentDateTimeUtc();
        run.backupDir = backupDir;
        run.confirmed = QStringList(confirmed.cbegin(), confirmed.cend());
        if (writeConfirmedRun(appState, run)) {
            *note = allowedRunLogLine(*allow, percent);
            return raised;
        }
        *note = kept + QString("its record for the check after the run could not be written in %1").arg(appState);
        return normal;
    }
    if (dryRunOk && !dryRunRan)
        *note = kept + "its dry run with the raised guard wrote no run to its log";
    else if (dryRunOk)
        *note = kept
                + QString("a dry run finds %1 deletion%2 not in the list that was confirmed (gone from the cloud: %3, "
                          "gone from the notebook: %4)")
                      .arg(QString::number(unconfirmed), unconfirmed == 1 ? QString() : QString("s"),
                           QString::number(found.cloudCount), QString::number(found.notebookCount));
    else if (dryText.contains("Safety abort: too many deletes"))
        *note = kept
                + QString("its dry run with the raised guard still stopped, so more files would go than the %1 confirmed")
                      .arg(allow->count);
    else
        *note = kept + QString("its dry run with the raised guard failed (%1)").arg(error.left(200));
    return normal;
}

int runRunCommand(const BackendPaths &paths, QTextStream &err) {
    QString error;
    const auto config = loadSyncConfig(paths.config, &error);
    const QString rclone = QStandardPaths::findExecutable("rclone");
    QStringList arguments;
    QString note;
    if (config && rclone.isEmpty())
        error = "rclone was not found";
    else if (config)
        arguments = runPlanTakingMark(*config, paths.appState, rclone, QDateTime::currentDateTimeUtc(), &error, &note);
    if (arguments.isEmpty()) {
        err << "drive-sync: did not start rclone: " << error << "\n";
        err.flush();
        // In the log too, in rclone's format, so the window and the
        // notification show why this run failed.
        appendToLog(config ? config->logFile : paths.log, "CRITICAL", "DriveSync did not start rclone: " + error);
        return 1;
    }
    if (!note.isEmpty())
        appendToLog(config->logFile, "INFO", note);

    QList<QByteArray> storage{QFile::encodeName(rclone)};
    for (const QString &argument : arguments)
        storage << argument.toUtf8();
    QList<char *> argv;
    for (QByteArray &item : storage)
        argv << item.data();
    argv << nullptr;
    execv(argv.first(), argv.data());
    err << "drive-sync: could not start " << rclone << "\n";
    err.flush();
    return 1;
}
