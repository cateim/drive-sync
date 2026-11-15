#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// A fix rebuilds the bisync base of both sides while ~/Drive stays in use.
// It is a state machine kept in <appState>/fix.json: "drive-sync fix" moves
// it on the user's word, and drive-sync-fix.service ("drive-sync fix-run")
// does the long steps. While the file exists, run-condition skips every
// timer tick, so nothing else starts rclone in between.

// A file newer or only in the cloud after the base run, which waits for
// the user: spec section 5, step 5.
struct FixQuestion {
    QString path;
    QString kind;  // "onlyCloud" | "differs"
    qint64 cloudSize = -1, notebookSize = -1, cloudTime = 0, notebookTime = 0;  // Unix seconds
    QString answer;  // "" | "cloud" | "notebook"
};

struct FixState {
    QString step;        // "preparing" | "confirm" | "base" | "questions" | "finishing" | "failed"; "" when no fix
    QString failedStep;  // the step to retry from when step == "failed"
    QDateTime started, stepStarted, lastReminder;  // UTC
    int copies = 0;      // from the dry run
    QString error;       // one line for the window and the card
    QList<FixQuestion> questions;
    QStringList touched;  // notebook-newer files touched after the base run
};

QString fixStatePath(const QString &appState);     // <appState>/fix.json
QString fixDryRunLogPath(const QString &appState);  // <appState>/fix-dry-run.log
bool fixPresent(const QString &appState);
// <appState>/first-setup.json: what "drive-sync setup" wrote (the config, the
// filters, the folders it made), so cancelling its fix before any base undoes
// exactly that. The base run removes it.
QString firstSetupMarkPath(const QString &appState);
// <appState>/settings-change.json: a change made in Settings (folder,
// filters, cloud folders) that goes through a fix. It holds the old and the
// new bytes (previousConfig/newConfig, previousFilters/newFilters, base64,
// only for what changes), "config" (the config file it applies to), "fix"
// (the fix's start, Unix seconds: it serves that fix only) and "applied".
// The fix's preparing step writes the new bytes once no run is under way;
// cancelling before the base puts the old ones back, so the old base still
// matches; the base run removes it.
QString settingsChangeMarkPath(const QString &appState);
// No file: an empty step. A file that cannot be read is a failed fix with
// that error: holding the timer is the safer mistake, and
// "drive-sync fix --cancel" clears it.
FixState readFix(const QString &appState);
bool writeFix(const QString &appState, const FixState &state);
// Also removes the dry-run log and the window's answers file. True also when
// there was nothing to clear.
bool clearFix(const QString &appState);
// Serializes the writers of fix.json (the command, the unit and the
// watchdog's reminder): each read, change and write happens under an
// exclusive flock on <appState>/fix.lock, held until this goes out of scope.
// Never hold one across a long step, and never take a second one in the
// same process: flock would wait for the first.
class FixLock {
public:
    explicit FixLock(const QString &appState);
    ~FixLock();
    FixLock(const FixLock &) = delete;
    FixLock &operator=(const FixLock &) = delete;
    bool held() const { return m_fd >= 0; }

private:
    int m_fd = -1;
};

// The steps that wait for the user, and the ones the unit works through.
bool fixWaiting(const QString &step);
bool fixWorking(const QString &step);
