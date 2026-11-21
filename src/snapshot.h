#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "fixstate.h"
#include "pausestate.h"
#include "synclog.h"

// One state of drive-sync, decided in one place. The window, the
// "drive-sync status" command and the bar widget all show what
// buildSnapshot() decides, so they can never disagree.

// A normal run writes about 30 KB to the log, a resync a few MB; the tail
// keeps dozens of normal runs without reading the whole history every time.
constexpr qint64 kTailBytes = 3 * 1024 * 1024;
// Runs the window lists.
constexpr int kMaxRuns = 40;
// Runs the state is decided from: every run in the tail. Forty runs are ten
// hours at the 15-minute interval, and a longer outage must still know when
// it began and when the last success was.
constexpr int kStateRuns = 1000000;
// rclone renews its lock and prints stats every minute, so an open run with
// nothing newer than this is no longer running.
constexpr int kAliveSeconds = 150;
// "drive-sync fix" writes the next step and then asks systemd to start the
// unit: for this long an inactive unit is still starting, not a dead fix.
constexpr int kFixStartGraceSeconds = 60;

// Everything the headline is decided from, gathered in one plain struct so
// the rules can be tested without systemd or a log.
struct SyncInputs {
    bool systemctlWorks = true;
    QString configError;     // why the account's config does not load; empty when it does
    bool logFound = true;
    bool driveFound = true;
    QString serviceLoad;     // the account's service LoadState
    QString serviceActive;   // ActiveState
    QString serviceResult;   // Result of its latest run
    QDateTime serviceStart;  // ExecMainStartTimestamp of that run
    QDateTime serviceSkip;   // InactiveEnterTimestamp: a skip has no main start
    QString timerActive;     // the account's timer ActiveState
    QString timerFileState;  // UnitFileState: "enabled" starts it on boot
    bool resyncRunning = false;
    bool paused = false;     // a pause mark holds the runs
    bool running = false;    // a run is writing to the log right now
    QString lastOutcome;     // newest finished run in the log
    QDateTime lastStart;
    QString lastCauseKind;
    QString lastCauseDetail;
    QDateTime lastSuccess;
    QDateTime now;
    int conflicts = 0;
    QString fixStep;          // the fix's step (fixstate.h), empty when no fix
    bool fixInterrupted = false;  // a working step with the fix unit gone
    QString fixError;         // why the fix stopped
    QString fixUnitResult;    // the fix unit's Result, for an interrupted step
    int intervalSeconds = 15 * 60;  // the timer's interval: "late" is three of them
    QStringList oldDriveSync;       // an old drive-sync found next to accounts (findOldDriveSync)
    bool oldDriveSyncHold = false;  // and it syncs this account's folder: run-condition skips
};

struct SyncSummary {
    QString status;  // see Backend::summarize
    QString causeKind;
    QString causeDetail;
    QStringList warnings;
};

// What a snapshot is built from: the parsed log and the raw answers of
// systemd, before any rule is applied.
struct SnapshotSources {
    QList<SyncRun> runs;              // SyncLog::parse output, oldest first
    QString configError;              // accountConfigError(): the account's config does not load
    bool logFound = false;
    bool systemctlWorks = true;
    QHash<QString, QString> service;  // parseShowById block of the account's service
    QHash<QString, QString> timer;    // parseShowById block of the account's timer
    bool resyncRunning = false;
    bool driveFound = true;
    int conflicts = 0;
    bool managed = false;             // DriveSync's own units run drive-sync
    PauseMark pause;
    FixState fix;                     // readFix, when fixPresent
    bool fixPresent = false;
    bool fixUnitActive = false;       // the fix unit is starting or running a step
    QString fixUnitResult;            // its Result: "success", "signal", "exit-code"...
    QDateTime now;                    // UTC
    int intervalSeconds = 15 * 60;    // the config's interval
    QStringList oldDriveSync;         // an old drive-sync found next to accounts, empty in legacy mode
    bool oldDriveSyncHold = false;    // and it syncs this account's folder
    bool moving = false;              // migration.json exists: run-condition skips every tick
};

struct Snapshot {
    SyncSummary summary;
    QString tone;                     // toneFor(summary.status)
    bool running = false;
    bool resync = false;
    bool canSyncNow = false;
    QVariantMap currentRun;           // Backend::runToMap(open run, "running"), empty when idle
    QVariantList runs;                // newest first, without the open run while running
    QDateTime lastSuccess;
    QDateTime lastRunStart;
    QDateTime lastRunEnd;
    QString lastRunOutcome;           // "success" | "failed" | "interrupted" | ""
    QDateTime failingSince;           // first run of the current failure, only when failed
    int typicalSeconds = 0;           // median of the last 10 successful non-resync runs
    int conflicts = 0;
    bool managed = false;
    bool paused = false;
    QDateTime pausedUntil;            // invalid while paused means until the user resumes
};

struct BackendPaths;

Snapshot buildSnapshot(const SnapshotSources &src);
// Version 2 of the "drive-sync status --json" contract (version 1 plus
// managed, paused and pausedUntil). Every key is always
// present; times are Unix seconds, 0 when unknown. The 5.1 bar widget reads
// it, so its bytes never change.
QJsonObject snapshotToJson(const Snapshot &s, const QDateTime &nextRun, const QDateTime &now);
// One account of version 3 ("drive-sync status --json --all"): the 18 keys
// version 2 has besides version and now, plus id, label (the account's, else
// its folder's base name) and folder (the clean path of its synced folder).
QJsonObject accountToJson(const Snapshot &s, const BackendPaths &account, const QDateTime &nextRun);
// "ok" | "busy" | "warning" | "error": how urgent a headline is.
QString toneFor(const QString &status);
// The two systemctl calls every reader makes, so they cannot drift apart.
// The show call describes the units given (accountUnits: each account's
// service, timer and fix unit), with Id first, so each answer is mapped by
// its Id (Backend::parseShowById), never by its position.
QStringList systemdShowArguments(const QStringList &units);
// list-timers for the timers given; each entry carries its "unit".
QStringList listTimersArguments(const QStringList &timers);
// The fix unit's show block says it is starting or running a step.
bool fixUnitBusy(const QHash<QString, QString> &unit);
// "systemctl show --timestamp=unix" prints "@1794076098", or nothing.
QDateTime fromUnixStamp(const QString &value);
