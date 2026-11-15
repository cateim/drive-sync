#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// What drive-sync's log says about each run. The log is plain rclone bisync
// output ("2026/11/07 15:28:19 INFO  : message", in local time), with
// multi-line stats and settings blocks whose continuation lines carry no
// timestamp.

// One file bisync found changed on one side during a run.
// An rclone error that says Google refused the sign-in itself (an expired or
// revoked grant, a client it no longer knows), never a network failure.
bool isTokenError(const QString &error);

struct SyncChange {
    QString side;  // "cloud" (bisync's Path1), "notebook" (Path2) or "both"
    QString kind;  // "new", "modified", "deleted" or "conflict"
    QString path;  // relative to ~/Drive
};

// The "Path1: 9 changes: 9 new, 0 modified, 0 deleted" summary of one side.
struct SyncCounts {
    int added = 0;
    int modified = 0;
    int deleted = 0;
};

struct SyncRun {
    QDateTime start;
    QDateTime end;          // invalid while the run is open or after it was cut off
    QDateTime lastSeen;     // the run's newest log line; rclone writes one at least every minute
    QString outcome;        // "success", "failed", "interrupted" or "open"
    bool resync = false;
    QString phase;          // "listing", "comparing", "applying" or "updating"
    long long listed = -1;  // files listed so far, from the latest stats block
    SyncCounts cloud;
    SyncCounts notebook;
    QList<SyncChange> changes;
    int changesOmitted = 0;
    QStringList errors;
    QString causeKind;      // failed runs only: "lostListings", "outOfSync", "filtersChanged", "firstSyncPending", "maxDelete", "signIn", "error" or "none"
    QString causeDetail;    // the error line behind "error"
};

namespace SyncLog {

// The runs in the text, oldest first, keeping the newest maxRuns. When the
// text starts in the middle of the log, pass startsMidway so the tail of a
// run whose start was cut off is skipped instead of read as a run of its own.
QList<SyncRun> parse(const QString &text, int maxRuns = 40, bool startsMidway = false);

// The last maxBytes of the file, from the first whole line. found is false
// when the file can't be read; truncated says whether the start was cut.
QString readTail(const QString &path, qint64 maxBytes, bool *found = nullptr, bool *truncated = nullptr);

// readTail, reaching further back (at most 64 MB) when the tail was cut and
// holds no "Synching Path1" line, so the newest run is whole. A run that
// --max-delete stopped writes one line per file it would delete: on a wiped
// ~/Drive that is more than the tail, and the run would go unseen. Each step
// back reads only the bytes before what it already has.
QString readRecent(const QString &path, qint64 maxBytes, bool *found = nullptr, bool *truncated = nullptr);

// Where the newest run in the text starts: the start of its "Synching
// Path1" line, or -1. Only a whole log line counts, so a file named like
// one ("- Path2  File was deleted  - a: Synching Path1 b") is never taken
// for a run's start.
qsizetype newestRunStart(QStringView text);

}
