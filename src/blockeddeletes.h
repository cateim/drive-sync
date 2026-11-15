#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <optional>

#include "synclog.h"

struct BackendPaths;  // backend.h, which includes this header
struct FixState;

// When --max-delete stops a run (spec section 5b), the run deleted nothing
// and its own log lines say what it would have deleted: one "File was
// deleted" line per file, both sides, then "Safety abort: too many deletes
// (>5%, 11 of 21) on Path2 ...". bisync checks each side against that
// side's count in the last listing, Path1 first (rclone 1.75.2).

struct BlockedDelete {
    QString path;                 // relative to ~/Drive
    bool deletedInCloud = false;  // Path1: gone from the cloud, so it goes from the notebook (to --backup-dir2)
};

struct BlockedDeletes {
    QDateTime runStart;  // the run that stopped (log time, as SyncRun.start)
    int count = 0;       // every deletion of that run, both sides
    int cloudCount = 0, notebookCount = 0;
    int guardCount = 0, guardTotal = 0;  // "11 of 21" from the abort line
    QList<BlockedDelete> list;           // at most limit, the ones that go from the notebook first
    // Why the run that took a confirmation still had the usual guard, from
    // the note drive-sync run wrote just before it: "dropped",
    // "unconfirmed", "stillStopped", "noRun" or "failed". Empty for a stop
    // with no such note right before it.
    QString keptReason;
};

// The deletion lines of the newest run in the text (from its "Synching
// Path1" line on), whatever it ended with, at any level: a dry run writes
// them as NOTICE. Fills count, cloudCount, notebookCount and list.
BlockedDeletes deletionsInNewestRun(const QString &logText, int limit);

// The newest run that reached bisync stopped at the max-delete guard. Runs
// under way and records that never reached bisync (DriveSync's refusals,
// rclone that did not start: no phase) are looked past, so edited filters
// after a stop do not open the way to a fix that would bring the deleted
// files back (spec 5b).
bool guardStopHolds(const QList<SyncRun> &runs);

// A fix holds every run, so a guard stop can be let through during one only
// when the fix's final step (answers, then the run) failed and the newest
// run that reached bisync is still that stop: the confirmation sends the fix
// back to that step. Decided from fix.json and the log, never from the
// fix's error text, which a later failure of the answers replaces.
bool fixWaitsForDeletes(const FixState &fix, bool guardStopHolds);

// The newest run in the log text, when it stopped at the max-delete guard.
// A run after it, even one DriveSync refused or one under way, holds nothing
// back.
std::optional<BlockedDeletes> blockedDeletes(const QString &logText, int limit);

// What the user confirmed, kept for one run in <appState>/allow-deletes.json
// ("drive-sync deletes --allow"). The run that uses it raises --max-delete
// just enough for these deletions, never to --force.
struct AllowDeletes {
    int count = 0;
    int cloudCount = 0, notebookCount = 0;  // per side, as in BlockedDeletes
    int total = 0;                          // the guard's total, "11 of 21"
    QDateTime at;                           // UTC
    QDateTime runStart;                     // the stopped run's start (BlockedDeletes.runStart)
};

QString allowDeletesPath(const QString &appState);
bool writeAllowDeletes(const QString &appState, const AllowDeletes &allow);
// Read and removed in one go, so it serves one run. A mark older than 30
// minutes (its run never started: a pause set in between) or one that
// cannot be read is removed, and nothing is returned.
std::optional<AllowDeletes> takeAllowDeletes(const QString &appState, const QDateTime &now);

// The smallest --max-delete that lets largestSide deletions out of total
// through: ceil(100 * largestSide / total), as rclone 1.75.2 compares the
// real percentage. Never below configured, never above 100.
int maxDeletePercent(int largestSide, int total, int configured);
// The arguments with "--max-delete N" or "--max-delete=N" set to percent,
// or the flag added at the end.
QStringList withMaxDelete(const QStringList &arguments, int percent);
// The --max-delete in the arguments; bisync's default, 50, when there is none.
int configuredMaxDelete(const QStringList &arguments);

// The run that got the raised guard, kept in <appState>/allowed-run.json
// until the run has ended: drive-sync run execs rclone, so the check after
// it is made by "drive-sync notify" (ExecStopPost), or by the fix's final
// step. confirmed holds "1:<path>" for each file gone from the cloud and
// "2:<path>" for each one gone from the notebook, as the user saw them.
struct ConfirmedRun {
    QDateTime at;       // UTC, just before rclone started
    QString backupDir;  // the run's --backup-dir2: what it deletes or overwrites on the notebook
    QStringList confirmed;
};
bool writeConfirmedRun(const QString &appState, const ConfirmedRun &run);

// Deletions of the run let through that were not in the confirmed list
// (sides as in BlockedDeletes), saved in <appState>/unconfirmed-deletes.json
// for the window, with the run's start.
struct UnconfirmedDeletes {
    QDateTime runStart;
    int cloudCount = 0, notebookCount = 0;
    QString backupDir;
};
// Takes allowed-run.json and compares the newest run's own "File was
// deleted" lines with the list. Any extra gives one NOTICE line in the log
// (never ERROR: after "Bisync successful" the parser would read an error as
// a failed run of its own) and the window's file. The run checked is the
// first that reached bisync after the record, even with runs after it.
// Nothing when there was none, when it started more than 5 minutes after the
// record (its own run never started), or when it stopped before "Applying
// changes" (then it only listed what it would delete).
UnconfirmedDeletes checkConfirmedRun(const QString &appState, const QString &logFile);
std::optional<UnconfirmedDeletes> readUnconfirmedDeletes(const QString &appState);

// "drive-sync deletes --list | --allow <count> [--json]". --list prints what
// the newest run would have deleted (JSON: count, cloudCount,
// notebookCount, guardCount, guardTotal, runStart, and list, at most 500
// {path, deletedInCloud}, the ones gone from the cloud first). --allow takes
// the number of files in that list, both sides, writes the one-run mark and
// starts drive-sync.service; for a fix whose final run stopped at the guard,
// it sends the fix back to that run instead. Refused while paused, during a
// takeover or another fix, and when DriveSync does not run drive-sync.
int runDeletesCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err);
