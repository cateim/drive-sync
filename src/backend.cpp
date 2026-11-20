#include "backend.h"

#include "accounts.h"
#include "conflictcache.h"
#include "fixcommand.h"
#include "syncconfig.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>

namespace {

// Three missed runs: 45 minutes at the default 15-minute interval (spec 4).
// A fixed 45 would show "late" between normal runs at a 30 or 60 minute one.
constexpr int kStaleIntervals = 3;

QVariantMap countsToMap(const SyncCounts &counts) {
    return {{"added", counts.added}, {"modified", counts.modified}, {"deleted", counts.deleted}};
}

int total(const SyncCounts &counts) { return counts.added + counts.modified + counts.deleted; }

}  // namespace

BackendPaths BackendPaths::defaults() {
    const AccountRoots roots = AccountRoots::defaults();
    const AccountSet set = resolveAccounts(roots);
    return set.accounts.isEmpty() ? noAccountPaths(roots) : set.accounts.first();
}

QString accountConfigError(const BackendPaths &paths) {
    // A config that is gone is not an error of the config: a takeover undone
    // removes it, and the view with no account has none.
    if (paths.id.isEmpty() || !QFileInfo::exists(paths.config))
        return {};
    QString error;
    return loadSyncConfig(paths.config, &error) ? QString() : error;
}

QStringList plainDriveSyncUnits() { return {"drive-sync.service", "drive-sync.timer"}; }

void applySyncConfig(BackendPaths *paths, const SyncConfig &config) {
    paths->log = config.logFile;
    paths->drive = config.localPath;
    paths->managed = true;
    paths->intervalSeconds = config.intervalMinutes * 60;
}

Backend::Backend(const BackendPaths &paths, QObject *parent) : QObject(parent), m_paths(paths) {
    m_configError = accountConfigError(m_paths);
    // rclone appends a line at a time; one read after the burst is enough.
    m_logDebounce.setSingleShot(true);
    m_logDebounce.setInterval(1000);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_logDebounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_logDebounce.start(); });
    connect(&m_logDebounce, &QTimer::timeout, this, &Backend::readLog);

    // A step of the fix moves on: show it now, and ask systemd whether its
    // unit took the step, so a confirm never reads as a dead fix.
    m_stateDebounce.setSingleShot(true);
    m_stateDebounce.setInterval(100);
    connect(&m_stateWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_stateDebounce.start(); });
    connect(&m_stateDebounce, &QTimer::timeout, this, [this] {
        querySystemd();
        recompute();
    });

    // QSaveFile replaces the config by a rename, which the folder's watch
    // sees and a watch on the file alone would lose.
    m_configDebounce.setSingleShot(true);
    m_configDebounce.setInterval(300);
    connect(&m_configWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_configDebounce.start(); });
    connect(&m_configDebounce, &QTimer::timeout, this, &Backend::reloadConfig);
    m_moveWait.setSingleShot(true);
    m_moveWait.setInterval(1000);
    connect(&m_moveWait, &QTimer::timeout, this, &Backend::reloadConfig);

    // systemd has no change signal worth a D-Bus subscription here; polling
    // also moves the relative times and the stale check along.
    m_poll.setInterval(30 * 1000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        querySystemd();
        recompute();
    });
    m_poll.start();

    // Conflict copies appear during runs and go away when the user deals
    // with them in the file manager, which no log line reports.
    m_conflictPoll.setInterval(2 * 60 * 1000);
    connect(&m_conflictPoll, &QTimer::timeout, this, &Backend::scanConflicts);
    m_conflictPoll.start();

    m_messageTimer.setSingleShot(true);
    m_messageTimer.setInterval(8000);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] { setMessage({}); });

    // deletesHeld follows the snapshot (managed, running), the fix and the stop.
    for (auto signal : {&Backend::changed, &Backend::fixChanged, &Backend::blockedChanged})
        connect(this, signal, this, &Backend::deletesHeldChanged);

    refresh();
}

Backend::~Backend() {
    // A systemctl or find still running at quit is stopped without its
    // callback, which would touch this half-destroyed object.
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect();
        process->kill();
        process->waitForFinished(500);
    }
}

void Backend::refresh() {
    watchAppState();
    watchConfig();
    readLog();
    querySystemd();
    scanConflicts();
}

void Backend::watchLog() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    // The folder (~/.local/state) changes all the time for other programs,
    // so it is watched only until the log shows up there. A replaced log
    // fires fileChanged, and the next read re-arms the watch.
    if (QFileInfo::exists(m_paths.log)) {
        m_watcher.addPath(m_paths.log);
        return;
    }
    const QString folder = QFileInfo(m_paths.log).absolutePath();
    if (QFileInfo::exists(folder))
        m_watcher.addPath(folder);
}

void Backend::watchAppState() {
    // The folder appears with the first thing DriveSync writes there.
    if (!m_paths.appState.isEmpty() && QFileInfo(m_paths.appState).isDir()
        && !m_stateWatcher.directories().contains(m_paths.appState))
        m_stateWatcher.addPath(m_paths.appState);
}

void Backend::watchConfig() {
    // The folder appears with a takeover or a first setup. The config root
    // too, where accounts/ appears or goes and the old config.json leaves:
    // that is how a first setup, a takeover or the move reaches a window
    // that shows another account, or none.
    const QString folder = m_paths.config.isEmpty() ? QString() : QFileInfo(m_paths.config).absolutePath();
    for (const QString &watched : {folder, m_paths.configRoot})
        if (!watched.isEmpty() && QFileInfo(watched).isDir() && !m_configWatcher.directories().contains(watched)
            && m_configWatcher.addPath(watched) && m_resolve)
            // A folder that just appeared (a setup or a takeover run from a
            // terminal made ~/.config/drive-sync) may already hold what the
            // watch would have reported: ask the resolver once.
            m_configDebounce.start();
}

void Backend::readLog() {
    bool found = false;
    bool truncated = false;
    const QString text = SyncLog::readRecent(m_paths.log, kTailBytes, &found, &truncated);
    m_logFound = found;
    m_parsed = SyncLog::parse(text, kStateRuns, truncated);
    watchLog();
    updateBlocked(text);

    // A run that just started or ended changes what systemd says, and an
    // ended run may have left conflict copies behind.
    QDateTime newestFinished;
    for (auto it = m_parsed.crbegin(); it != m_parsed.crend(); ++it)
        if (it->outcome != "open") {
            newestFinished = it->start;
            break;
        }
    const bool openNow = !m_parsed.isEmpty() && m_parsed.last().outcome == "open";
    const bool wasOpen = !m_currentRun.isEmpty();
    if (newestFinished != m_lastFinishedStart || openNow != wasOpen) {
        m_lastFinishedStart = newestFinished;
        querySystemd();
        scanConflicts();
    }
    recompute();
}

void Backend::run(const QString &program, const QStringList &arguments,
                  std::function<void(bool, const QByteArray &)> done) {
    if (program.isEmpty()) {
        done(false, {});
        return;
    }
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0, process->readAllStandardOutput());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        done(false, {});
        process->deleteLater();
    });
    // Nothing here should take seconds; a hung call must not pile up.
    QTimer::singleShot(10 * 1000, process, [process] { process->kill(); });
    process->start(program, arguments);
}

void Backend::setOldDriveSyncCheck(const OldDriveSyncCheck &check) {
    m_oldDriveSyncCheck = check;
    querySystemd();
}

void Backend::querySystemd() {
    if (m_systemdPending)
        return;
    m_systemdPending = true;
    const BackendPaths paths = m_paths;
    // In accounts mode the plain units ride along, for an old drive-sync
    // next to the accounts (read when the answer comes, so a check given
    // after this call is still used).
    const QStringList plain = paths.accountsMode ? plainDriveSyncUnits() : QStringList();
    // An answer about the units of an account the window no longer shows
    // (it adopted another meanwhile) is dropped and asked again: it would
    // describe the plain units after the move to account1, say.
    const auto stale = [this](const BackendPaths &asked) {
        if (accountUnits(asked) == accountUnits(m_paths) && asked.accountsMode == m_paths.accountsMode)
            return false;
        m_systemdPending = false;
        querySystemd();
        return true;
    };
    run(m_paths.systemctl, systemdShowArguments(accountUnits(paths) + plain),
        [this, paths, stale](bool ok, const QByteArray &output) {
            if (stale(paths))
                return;
            const auto units = parseShowById(output);
            m_oldDriveSync.clear();
            m_oldDriveSyncHold = false;
            if (paths.accountsMode && m_oldDriveSyncCheck)
                m_oldDriveSync = m_oldDriveSyncCheck(units, paths.drive, &m_oldDriveSyncHold);
            // Each of the three must answer: a unit missing from the answer
            // is a systemd that cannot be read, not an idle one.
            m_systemctlWorks = ok;
            for (const QString &unit : accountUnits(paths))
                m_systemctlWorks = m_systemctlWorks && units.contains(unit);
            m_service = m_systemctlWorks ? units.value(paths.serviceUnit) : QHash<QString, QString>();
            m_timer = m_systemctlWorks ? units.value(paths.timerUnit) : QHash<QString, QString>();
            m_fixUnit = m_systemctlWorks ? units.value(paths.fixUnit) : QHash<QString, QString>();
            run(m_paths.systemctl, listTimersArguments({paths.timerUnit}),
                [this, paths, stale](bool ok, const QByteArray &output) {
                    if (stale(paths))
                        return;
                    m_systemdPending = false;
                    m_nextRun = ok ? parseNextElapse(output, paths.timerUnit) : QDateTime();
                    recompute();
                });
        });
}

void Backend::scanConflicts() {
    if (m_scanPending)
        return;
    if (!QFileInfo(m_paths.drive).isDir()) {
        const bool changedNow = !m_conflicts.isEmpty() || m_conflictError != "noDrive";
        m_conflicts.clear();
        m_conflictError = "noDrive";
        if (changedNow)
            emit conflictsChanged();
        recompute();
        return;
    }
    m_scanPending = true;
    const QString drive = m_paths.drive;
    run(m_paths.find, conflictFindArguments(m_paths.drive),
        [this, drive](bool ok, const QByteArray &output) {
            m_scanPending = false;
            // The window followed another account while find ran: its
            // copies are not this account's.
            if (drive != m_paths.drive) {
                scanConflicts();
                return;
            }
            // find exits 1 when one folder can't be read but still lists the
            // rest, so only an empty answer from a failed run is an error.
            QList<QPair<qint64, QVariantMap>> found;
            QString error;
            if (m_paths.find.isEmpty())
                error = "noFind";
            else if (!ok && output.isEmpty())
                error = "scanFailed";
            for (const QByteArray &line : output.split('\n')) {
                const QList<QByteArray> fields = line.split('\t');
                if (fields.size() < 3)
                    continue;
                const QString path = QString::fromUtf8(line.mid(fields.at(0).size() + fields.at(1).size() + 2));
                const qint64 ms = qint64(fields.at(0).toDouble() * 1000.0);
                found.append({ms, QVariantMap{{"path", path},
                                              {"name", QFileInfo(path).fileName()},
                                              {"folder", QFileInfo(path).path() == "." ? QString() : QFileInfo(path).path()},
                                              {"modified", QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)},
                                              {"size", fields.at(1).toLongLong()}}});
            }
            // Newest first.
            std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            QVariantList conflicts;
            for (const auto &entry : found)
                conflicts.append(entry.second);
            // Never during the move to account1: it is leaving this place.
            if (!m_paths.appState.isEmpty() && !moveInProgress(m_paths.globalState()))
                writeConflictCache(m_paths.appState + "/conflicts.json",
                                   {int(conflicts.size()), error, QDateTime::currentDateTimeUtc()});
            if (conflicts != m_conflicts || error != m_conflictError) {
                m_conflicts = conflicts;
                m_conflictError = error;
                emit conflictsChanged();
            }
            recompute();
        });
}

QVariantMap Backend::runToMap(const SyncRun &run, const QString &outcome) {
    QVariantList changes;
    for (const auto &change : run.changes)
        changes.append(QVariantMap{{"side", change.side}, {"kind", change.kind}, {"path", change.path}});
    int count = total(run.cloud) + total(run.notebook);
    if (count == 0)
        count = int(run.changes.size()) + run.changesOmitted;  // A resync prints no per-side summary.
    return {{"start", run.start},
            {"end", run.end.isValid() ? QVariant(run.end) : QVariant()},
            {"seconds", run.end.isValid() ? run.start.secsTo(run.end) : -1},
            {"outcome", outcome},
            {"resync", run.resync},
            {"phase", run.phase},
            {"listed", run.listed},
            {"cloud", countsToMap(run.cloud)},
            {"notebook", countsToMap(run.notebook)},
            {"changeCount", count},
            {"changes", changes},
            {"changesOmitted", run.changesOmitted},
            {"errors", run.errors},
            {"causeKind", run.causeKind},
            {"causeDetail", run.causeDetail}};
}

void Backend::recompute() {
    // The state folder may appear after the window opened (the first
    // conflict cache on a new machine): watch it from then on. So may the
    // config root, made by a setup or a takeover run from a terminal.
    watchAppState();
    watchConfig();
    SnapshotSources src;
    src.runs = m_parsed;
    src.logFound = m_logFound;
    src.systemctlWorks = m_systemctlWorks && !m_paths.systemctl.isEmpty();
    src.service = m_service;
    src.timer = m_timer;
    src.resyncRunning = resyncRunning(m_paths.stateDir, m_paths.proc);
    src.driveFound = m_conflictError != "noDrive";
    src.conflicts = int(m_conflicts.size());
    src.managed = m_paths.managed;
    src.intervalSeconds = m_paths.intervalSeconds;
    src.configError = m_configError;
    src.oldDriveSync = m_oldDriveSync;
    src.oldDriveSyncHold = m_oldDriveSyncHold;
    src.moving = moveInProgress(m_paths.globalState());
    if (!m_paths.appState.isEmpty()) {
        src.pause = readPause(pauseFilePath(m_paths.appState));
        src.fixPresent = fixPresent(m_paths.appState);
        if (src.fixPresent)
            src.fix = readFix(m_paths.appState);
    }
    src.fixUnitActive = fixUnitBusy(m_fixUnit);
    src.fixUnitResult = m_fixUnit.value("Result");
    updateFix(src.fix);
    // UTC: comparing local times makes Qt look the zone up every time (see
    // toTime in synclog.cpp), and the parsed times carry a fixed offset.
    src.now = QDateTime::currentDateTimeUtc();
    const Snapshot snapshot = buildSnapshot(src);

    // While a run is going the poll is quicker, so the end shows up soon.
    m_poll.setInterval(snapshot.running || snapshot.resync ? 5000 : 30000);

    if (snapshot.runs != m_runs) {
        m_runs = snapshot.runs;
        emit runsChanged();
    }
    m_snapshot = snapshot;
    m_summary = snapshot.summary;
    m_currentRun = snapshot.currentRun;
    m_lastSuccess = snapshot.lastSuccess;
    m_typicalSeconds = snapshot.typicalSeconds;
    // Emitted every time so relative times ("8 min ago") move along.
    emit changed();
}

// The headline, most urgent first. Statuses: "unavailable", "configError", "notLoaded",
// "fixing", "fixWaiting", "fixFailed", "running", "resync", "paused", "noLog", "failed", "interrupted", "skipped",
// "timerOff", "stale", "noDrive", "noRuns", "conflicts", "ok". Warnings carry what the
// headline leaves out: "timerOff", "timerDisabled", "conflicts", "oldDriveSync".
SyncSummary Backend::summarize(const SyncInputs &in) {
    SyncSummary s;
    const bool timerOff = in.timerActive != "active";
    // A newer run in the log than the one systemd remembers means the log
    // knows better (drive-sync.sh resync runs rclone outside the service).
    const bool logIsNewer = in.lastStart.isValid() && in.serviceStart.isValid()
                            && in.lastStart > in.serviceStart.addSecs(60);
    const bool systemdFailed = !in.serviceResult.isEmpty() && in.serviceResult != "success"
                               && in.serviceResult != "exec-condition" && !logIsNewer;
    const bool stale = in.lastSuccess.isValid() && in.lastSuccess.secsTo(in.now) > qint64(kStaleIntervals) * in.intervalSeconds;

    if (!in.systemctlWorks) {
        s.status = "unavailable";
    } else if (!in.configError.isEmpty()) {
        // An account whose config does not load runs nothing DriveSync can
        // describe: say so, with the loader's reason, before anything else.
        s.status = "configError";
        s.causeDetail = in.configError;
    } else if (in.serviceLoad != "loaded") {
        s.status = "notLoaded";
        s.causeDetail = in.serviceLoad;
    } else if (!in.fixStep.isEmpty()) {
        // A fix holds every run, and its base run writes to the log like one.
        if (in.fixStep == "failed" || in.fixInterrupted) {
            s.status = "fixFailed";
            s.causeKind = in.fixInterrupted ? "fixInterrupted" : "fix";
            // A unit that failed on its own says how; a kill (logout,
            // shutdown) is "signal", which the window words itself.
            const bool unitFailed = !in.fixUnitResult.isEmpty() && in.fixUnitResult != "success"
                                    && in.fixUnitResult != "signal";
            s.causeDetail = !in.fixInterrupted ? in.fixError
                            : unitFailed       ? in.fixUnitResult
                                               : QString("the fix stopped halfway");
            // Google refused the token: sign in again (I), then retry (F);
            // rclone's own advice needs a terminal.
            if (!in.fixInterrupted && isTokenError(in.fixError)) {
                s.causeKind = "signIn";
                s.causeDetail.clear();
            }
        } else {
            s.status = fixWaiting(in.fixStep) ? "fixWaiting" : "fixing";
        }
    } else if (in.running) {
        s.status = in.resyncRunning ? "resync" : "running";
    } else if (in.resyncRunning) {
        s.status = "resync";
    } else if (in.paused) {
        // Deliberate: no failure, "skipped" or "late" while the user holds the runs.
        s.status = "paused";
    } else if (!in.logFound) {
        s.status = "noLog";
    } else if (systemdFailed) {
        s.status = "failed";
        if (in.serviceResult == "timeout") {
            s.causeKind = "timeout";
        } else if (in.lastOutcome == "failed") {
            s.causeKind = in.lastCauseKind;
            s.causeDetail = in.lastCauseDetail;
        } else {
            s.causeKind = "systemd";
            s.causeDetail = in.serviceResult;
        }
    } else if (in.lastOutcome == "failed") {
        s.status = "failed";
        s.causeKind = in.lastCauseKind;
        s.causeDetail = in.lastCauseDetail;
    } else if (in.lastOutcome == "interrupted") {
        s.status = "interrupted";
    } else if (in.serviceResult == "exec-condition"
               && !(in.serviceSkip.isValid() && in.lastStart.isValid() && in.lastStart > in.serviceSkip)) {
        // A run in the log after the skip (the fix's final run, which runs
        // outside the unit) is newer news than the skip.
        s.status = "skipped";
        // run-condition holds this account for an old drive-sync on its
        // folder: the detail names what was found, to remove by hand.
        if (in.oldDriveSyncHold) {
            s.causeKind = "oldDriveSync";
            s.causeDetail = in.oldDriveSync.join(", ");
        }
    } else if (timerOff) {
        s.status = "timerOff";
    } else if (stale) {
        s.status = "stale";
    } else if (!in.driveFound) {
        s.status = "noDrive";
    } else if (!in.lastStart.isValid()) {
        // An empty log (new, or just rotated) proves no sync: "stale" needs a
        // last success to count from, so without this it stayed green.
        s.status = "noRuns";
    } else if (in.conflicts > 0) {
        s.status = "conflicts";
    } else {
        s.status = "ok";
    }

    // drive-sync.sh stops the timer on purpose during a resync.
    const bool headlineCoversTimer = s.status == "timerOff" || s.status == "unavailable" || s.status == "notLoaded";
    if (timerOff && !in.resyncRunning && !headlineCoversTimer)
        s.warnings << "timerOff";
    if (!timerOff && in.timerFileState != "enabled" && in.systemctlWorks)
        s.warnings << "timerDisabled";
    if (in.conflicts > 0 && s.status != "conflicts")
        s.warnings << "conflicts";
    if (!in.oldDriveSync.isEmpty())
        s.warnings << "oldDriveSync";
    return s;
}

QList<QHash<QString, QString>> Backend::parseShow(const QByteArray &output) {
    QList<QHash<QString, QString>> units;
    QHash<QString, QString> current;
    for (const QByteArray &raw : output.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) {
            if (!current.isEmpty())
                units.append(current);
            current.clear();
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (equals > 0)
            current.insert(line.left(equals), line.mid(equals + 1));
    }
    if (!current.isEmpty())
        units.append(current);
    return units;
}

QHash<QString, QHash<QString, QString>> Backend::parseShowById(const QByteArray &output) {
    QHash<QString, QHash<QString, QString>> byId;
    for (const auto &unit : parseShow(output))
        if (!unit.value("Id").isEmpty())
            byId.insert(unit.value("Id"), unit);
    return byId;
}

QDateTime Backend::parseNextElapse(const QByteArray &json, const QString &unit) {
    for (const QJsonValue &entry : QJsonDocument::fromJson(json).array()) {
        const QJsonObject timer = entry.toObject();
        if (timer.value("unit").toString() != unit)
            continue;
        // Microseconds since the epoch; 0 or null when nothing is scheduled.
        const double next = timer.value("next").toDouble(0);
        return next > 0 ? QDateTime::fromMSecsSinceEpoch(qint64(next / 1000.0), QTimeZone::UTC) : QDateTime();
    }
    return {};
}

bool Backend::resyncRunning(const QString &stateDir, const QString &proc) {
    // An account in the accounts layout has no drive-sync.sh state folder:
    // an empty one would read "/pausado" at the root.
    if (stateDir.isEmpty())
        return false;
    QFile mark(stateDir + "/pausado");
    if (!mark.open(QIODevice::ReadOnly))
        return false;
    const QString pid = QString::fromUtf8(mark.readLine()).trimmed();
    bool ok = false;
    pid.toLongLong(&ok);
    if (!ok)
        return false;
    // A mark left by a drive-sync.sh that already died means the timer was
    // left off, not that a resync is going.
    QFile cmdline(proc + "/" + pid + "/cmdline");
    return cmdline.open(QIODevice::ReadOnly) && cmdline.readAll().contains("drive-sync.sh");
}

void Backend::setResolver(std::function<WindowAccount()> resolve) {
    m_resolve = std::move(resolve);
    reloadConfig();
}

namespace {

bool sameAccount(const BackendPaths &a, const BackendPaths &b) {
    return a.id == b.id && a.config == b.config && a.appState == b.appState && accountUnits(a) == accountUnits(b);
}

bool hasNotice(const QList<AccountNotice> &notices, const QString &kind) {
    return std::any_of(notices.cbegin(), notices.cend(), [&](const AccountNotice &n) { return n.kind == kind; });
}

}  // namespace

void Backend::reloadConfig() {
    if (m_resolve) {
        const WindowAccount next = m_resolve();
        // A move under way (its journal, and no notice that it stopped) can
        // be caught between two renames: keep what is shown until it ends.
        if (moveInProgress(next.paths.globalState()) && !hasNotice(next.notices, "moveStuck")) {
            m_moveWait.start();
            return;
        }
        QVariantList notices;
        for (const AccountNotice &notice : next.notices)
            notices.append(QVariantMap{{"kind", notice.kind}, {"path", notice.path}});
        if (notices != m_notices) {
            m_notices = notices;
            emit changed();
        }
        if (!sameAccount(next.paths, m_paths)) {
            adopt(next.paths);
            return;
        }
    }
    if (m_paths.config.isEmpty())
        return;
    const auto config = loadSyncConfig(m_paths.config, nullptr);
    const QString log = config ? config->logFile : m_paths.log;
    m_configError = accountConfigError(m_paths);
    // An account whose config broke stays managed, as the resolver lists
    // it; one whose config is gone (a takeover undone) is not.
    m_paths.managed = config.has_value() || !m_configError.isEmpty();
    // Without a config the packaged timer's interval is the one that runs.
    m_paths.intervalSeconds = (config ? config->intervalMinutes : kDefaultIntervalMinutes) * 60;
    if (config)
        m_paths.drive = config->localPath;
    if (log != m_paths.log) {
        m_paths.log = log;
        watchLog();
        readLog();
    }
    refresh();
}

void Backend::adopt(const BackendPaths &paths) {
    // Every watch named the old account's files: its log, its state folder
    // (the whole state root in legacy mode) and its config's folder.
    for (QFileSystemWatcher *watcher : {&m_watcher, &m_stateWatcher, &m_configWatcher}) {
        const QStringList watched = watcher->files() + watcher->directories();
        if (!watched.isEmpty())
            watcher->removePaths(watched);
    }
    m_paths = paths;
    m_configError = accountConfigError(m_paths);
    refresh();
}

void Backend::startMove() {
    if (m_paths.self.isEmpty())
        return;
    // Never killed: no timeout, no parent that kills it at quit (~Backend
    // stops its children). A move stopped by a signal leaves its journal for
    // the next trigger to roll back, which can be an hour away. Nothing is
    // read from it either, so a window that quits leaves no pipe to break.
    auto *process = new QProcess;
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        reloadConfig();
    });
    connect(process, &QProcess::errorOccurred, this, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process->start(m_paths.self, {"accounts", "--migrate", "--json"});
}

namespace {

// The paths the confirm dialog lists; the count says how many there are in all.
constexpr int kShownCopies = 500;

bool sameFix(const FixState &a, const FixState &b) {
    if (a.step != b.step || a.stepStarted != b.stepStarted || a.error != b.error || a.copies != b.copies
        || a.questions.size() != b.questions.size())
        return false;
    for (qsizetype i = 0; i < a.questions.size(); ++i)
        if (a.questions.at(i).path != b.questions.at(i).path || a.questions.at(i).answer != b.questions.at(i).answer)
            return false;
    return true;
}

QDateTime fromUnixSeconds(qint64 seconds) {
    return seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC) : QDateTime();
}

}  // namespace

void Backend::updateFix(const FixState &fix) {
    if (sameFix(fix, m_fix))
        return;
    // The dry-run log can hold thousands of copies: read once per confirm.
    if (fix.step != "confirm") {
        m_fixCopyList.clear();
        m_fixCopiesToNotebook = 0;
    } else if (m_fix.step != "confirm" || m_fix.stepStarted != fix.stepStarted || m_fixCopyList.isEmpty()) {
        QFile log(fixDryRunLogPath(m_paths.appState));
        int total = 0;
        m_fixCopiesToNotebook = 0;
        m_fixCopyList.clear();
        if (log.open(QIODevice::ReadOnly))
            for (const DryRunCopy &copy : dryRunCopies(log.readAll(), kShownCopies, &total, &m_fixCopiesToNotebook))
                m_fixCopyList.append(QVariantMap{{"path", copy.path}, {"toNotebook", copy.toNotebook}});
    }
    m_fix = fix;
    emit fixChanged();
}

void Backend::updateBlocked(const QString &logText) {
    if (const bool holds = ::guardStopHolds(m_parsed); holds != m_guardStopHolds) {
        m_guardStopHolds = holds;
        emit blockedChanged();
    }
    // Deletions nobody confirmed stay in the window while their run is the
    // newest one that reached bisync; the card and the log keep them after.
    UnconfirmedDeletes unconfirmed;
    if (const auto extra = readUnconfirmedDeletes(m_paths.appState))
        for (auto it = m_parsed.crbegin(); it != m_parsed.crend(); ++it)
            if (!it->phase.isEmpty()) {
                if (it->start.toSecsSinceEpoch() == extra->runStart.toSecsSinceEpoch())
                    unconfirmed = *extra;
                break;
            }
    if (unconfirmed.cloudCount != m_unconfirmed.cloudCount || unconfirmed.notebookCount != m_unconfirmed.notebookCount
        || unconfirmed.backupDir != m_unconfirmed.backupDir) {
        m_unconfirmed = unconfirmed;
        emit blockedChanged();
    }
    const bool guardStop = !m_parsed.isEmpty() && m_parsed.last().outcome == "failed"
                           && m_parsed.last().causeKind == "maxDelete";
    // A stopped run never changes: read its list once.
    if (guardStop && m_blocked.count > 0 && m_blocked.runStart == m_parsed.last().start)
        return;
    const std::optional<BlockedDeletes> blocked = guardStop ? blockedDeletes(logText, kShownCopies) : std::nullopt;
    if (!blocked && m_blocked.count == 0)
        return;
    m_blocked = blocked.value_or(BlockedDeletes());
    m_blockedList.clear();
    for (const BlockedDelete &d : m_blocked.list)
        m_blockedList.append(QVariantMap{{"path", d.path}, {"deletedInCloud", d.deletedInCloud}});
    emit blockedChanged();
}

bool Backend::deletesHeld() const {
    // The headline is "failed" or "fixFailed" only while nothing runs: a
    // run under way (the confirmed one's dry run writes no line to the main
    // log yet) takes D away.
    const QString headline = status();
    return managed() && m_blocked.count > 0 && (headline == "failed" || headline == "fixFailed")
           && (m_fix.step.isEmpty() || fixWaitsForDeletes(m_fix, m_guardStopHolds));
}

void Backend::allowDeletes(int count) { control({"deletes", "--allow", QString::number(count)}); }

QVariantList Backend::fixQuestions() const {
    QVariantList list;
    for (const FixQuestion &q : m_fix.questions)
        list.append(QVariantMap{{"path", q.path},
                                {"kind", q.kind},
                                {"cloudSize", q.cloudSize},
                                {"notebookSize", q.notebookSize},
                                {"cloudTime", fromUnixSeconds(q.cloudTime)},
                                {"notebookTime", fromUnixSeconds(q.notebookTime)},
                                {"answer", q.answer}});
    return list;
}

void Backend::startFix() { control({"fix", "--start"}); }
void Backend::confirmFix() { control({"fix", "--confirm"}); }
void Backend::cancelFix() { control({"fix", "--cancel"}); }
void Backend::retryFix() { control({"fix", "--retry"}); }

void Backend::answerFix(const QStringList &answers) {
    if (answers.size() != m_fix.questions.size()) {
        setMessage(tr("The answers do not match the questions; open them again."));
        return;
    }
    QJsonObject byPath;
    for (qsizetype i = 0; i < answers.size(); ++i)
        byPath.insert(m_fix.questions.at(i).path, answers.at(i));
    // The answers are the account's state: none written while the move to
    // account1 takes it to its new place.
    if (moveInProgress(m_paths.globalState())) {
        setMessage(tr("DriveSync is moving to accounts; try again in a minute."));
        return;
    }
    const QString file = m_paths.appState + "/fix-answers.json";
    QSaveFile out(file);
    if (!QDir().mkpath(m_paths.appState) || !out.open(QIODevice::WriteOnly)
        || out.write(QJsonDocument(byPath).toJson()) < 0 || !out.commit()) {
        setMessage(tr("Cannot write %1.").arg(file));
        return;
    }
    control({"fix", "--answers", file});
}

void Backend::syncNow() { control({"sync"}); }
void Backend::pauseFor(int minutes) { control({"pause", "--minutes", QString::number(minutes)}); }
void Backend::pauseUntilTomorrow() { control({"pause", "--until-tomorrow"}); }
void Backend::pauseUntilResumed() { control({"pause"}); }
void Backend::resume() { control({"resume"}); }

void Backend::control(const QStringList &arguments) {
    run(m_paths.self, arguments + QStringList{"--json"}, [this](bool, const QByteArray &output) {
        const QJsonObject answer = QJsonDocument::fromJson(output).object();
        if (!answer.value("ok").toBool()) {
            const QString error = answer.value("error").toString();
            setMessage(error.isEmpty() ? tr("DriveSync could not do that.") : error);
        }
        refresh();
    });
}

void Backend::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    if (!message.isEmpty())
        m_messageTimer.start();
    emit messageChanged();
}

void Backend::openDriveFolder() {
    const QString drive = QDir(m_paths.drive).absolutePath();
    if (!QFileInfo(drive).isDir()) {
        setMessage(tr("%1 isn't there.").arg(drive));
        return;
    }
    // The running file manager over D-Bus, as for a conflict copy; without
    // it, whatever opens folders.
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1", "ShowFolders");
    call << QStringList{QUrl::fromLocalFile(drive).toString()} << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, drive] {
        watcher->deleteLater();
        if (!watcher->isError())
            return;
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(drive)))
            setMessage(tr("No file manager answered for %1.").arg(drive));
    });
}

void Backend::showInFileManager(const QString &relativePath) {
    const QString absolute = QDir(m_paths.drive).filePath(relativePath);
    if (!QFileInfo::exists(absolute)) {
        setMessage(tr("%1 is gone; the list is up to date now.").arg(relativePath));
        scanConflicts();
        return;
    }
    // FileManager1 opens the folder with the file selected (Nautilus on
    // Omarchy). Without it, opening the folder is the next best thing.
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1", "ShowItems");
    call << QStringList{QUrl::fromLocalFile(absolute).toString()} << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, absolute] {
        watcher->deleteLater();
        if (!watcher->isError())
            return;
        const QString folder = QFileInfo(absolute).absolutePath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))
            setMessage(tr("No file manager answered for %1.").arg(folder));
    });
}
