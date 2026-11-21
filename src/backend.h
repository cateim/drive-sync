#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "blockeddeletes.h"
#include "snapshot.h"
#include "synclog.h"
#include "syncconfig.h"

// Where the Backend looks. Tests point every path at a temp directory and
// systemctl at a fake script.
struct BackendPaths {
    QString log;       // drive-sync's rclone log
    QString drive;     // the mirrored folder, where conflict copies land
    QString stateDir;  // drive-sync's state folder, which holds the resync pause mark
    QString proc;      // /proc, to check that the pid in the pause mark is drive-sync.sh
    QString systemctl;
    QString find;
    QString appState;  // DriveSync's own state folder (conflict count cache); empty writes nothing
    QString config;    // DriveSync's config; empty when there is none
    QString self;      // this program, for the commands the window runs
    bool managed = false;  // DriveSync runs drive-sync through its own units
    // The config's interval: "late" is three of them. Last, so the paths
    // listed in order above keep their places.
    int intervalSeconds = kDefaultIntervalMinutes * 60;
    // The account these paths are (accounts.h): its id ("account1"...;
    // empty is the view with no account) and its units. Their defaults are
    // the plain units, so paths listed in order above mean legacy mode.
    QString id{};
    QString serviceUnit = "drive-sync.service";
    QString timerUnit = "drive-sync.timer";
    QString fixUnit = "drive-sync-fix.service";
    QString configRoot{};  // ~/.config/drive-sync
    QString stateRoot{};   // ~/.local/state/drive-sync-app
    bool accountsMode = false;  // the accounts/<id>/ layout and the template units
    // What the window, the bar and the cards call the account (accountLabel,
    // set by the resolver over the whole set); empty in paths built by hand.
    QString label{};

    // The state folder shared by every account: the takeover lock and the
    // move's journal live there. Paths with no state root use appState.
    QString globalState() const { return stateRoot.isEmpty() ? appState : stateRoot; }

    // The first account resolveAccounts() finds, or the view with no account.
    static BackendPaths defaults();
};

// What a loaded config changes in the paths: its log, its folder, its
// interval, and managed.
void applySyncConfig(BackendPaths *paths, const SyncConfig &config);

// Why an account's config does not load (the headline configError), or
// empty: always empty with no account (no id) or no config file at all.
QString accountConfigError(const BackendPaths &paths);

// Looks for an old drive-sync next to accounts (spec 12): given the plain
// drive-sync.service and .timer as systemctl show describes them, by Id, it
// returns what it found and sets *hold when an account on folder must skip
// its runs. main builds it from the real home (oldDriveSyncCheck in
// takeover.h); tests and the harness give none, so a reader never looks at
// this machine's units by itself. Asked in accounts mode only.
using OldDriveSyncCheck = std::function<QStringList(const QHash<QString, QHash<QString, QString>> &plainUnits,
                                                    const QString &folder, bool *hold)>;
// The units it needs in a reader's one systemctl show call.
QStringList plainDriveSyncUnits();

// The account the window shows and the notices about the set (accounts.h).
struct WindowAccount;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString tone READ tone NOTIFY changed)
    Q_PROPERTY(QString causeKind READ causeKind NOTIFY changed)
    Q_PROPERTY(QString causeDetail READ causeDetail NOTIFY changed)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(QVariantMap currentRun READ currentRun NOTIFY changed)
    Q_PROPERTY(QDateTime lastSuccess READ lastSuccess NOTIFY changed)
    Q_PROPERTY(QDateTime nextRun READ nextRun NOTIFY changed)
    Q_PROPERTY(int typicalSeconds READ typicalSeconds NOTIFY changed)
    Q_PROPERTY(bool managed READ managed NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(QDateTime pausedUntil READ pausedUntil NOTIFY changed)
    Q_PROPERTY(bool canSyncNow READ canSyncNow NOTIFY changed)
    Q_PROPERTY(QVariantList runs READ runs NOTIFY runsChanged)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY conflictsChanged)
    Q_PROPERTY(QString conflictError READ conflictError NOTIFY conflictsChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // The fix (fixstate.h), read from <appState>/fix.json: its step, what the
    // dry run would copy, and the files it asks about.
    Q_PROPERTY(QString fixStep READ fixStep NOTIFY fixChanged)
    Q_PROPERTY(int fixCopies READ fixCopies NOTIFY fixChanged)
    // At most 500 of the dry run's copies, as {path, toNotebook}, the copies
    // to the notebook first; and how many of all of them go to the notebook.
    Q_PROPERTY(QVariantList fixCopyList READ fixCopyList NOTIFY fixChanged)
    Q_PROPERTY(int fixCopiesToNotebook READ fixCopiesToNotebook NOTIFY fixChanged)
    Q_PROPERTY(QVariantList fixQuestions READ fixQuestions NOTIFY fixChanged)
    Q_PROPERTY(QString fixError READ fixError NOTIFY fixChanged)
    // The base run already ran: cancelling now throws it away, and the next
    // run fails until another fix.
    Q_PROPERTY(bool fixBaseDone READ fixBaseDone NOTIFY fixChanged)
    // When the newest run stopped at the max-delete guard (spec 5b): what it
    // would delete, per side, and at most 500 of them as {path,
    // deletedInCloud}, the ones gone from the cloud first. 0 and empty
    // otherwise.
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY blockedChanged)
    Q_PROPERTY(int blockedCloudCount READ blockedCloudCount NOTIFY blockedChanged)
    Q_PROPERTY(int blockedNotebookCount READ blockedNotebookCount NOTIFY blockedChanged)
    Q_PROPERTY(QVariantList blockedList READ blockedList NOTIFY blockedChanged)
    // The newest run that reached bisync stopped at the guard, even with
    // refused runs after it (edited filters): F would bring the files back.
    Q_PROPERTY(bool guardStopHolds READ guardStopHolds NOTIFY blockedChanged)
    // BlockedDeletes.keptReason: the stop is the run the user let through,
    // which kept the usual guard. Empty otherwise.
    Q_PROPERTY(QString blockedKeptReason READ blockedKeptReason NOTIFY blockedChanged)
    // The newest run is one the user let through, and it deleted files that
    // were not in the list confirmed (UnconfirmedDeletes): per side, and the
    // folder that keeps the ones deleted on the notebook. 0 and empty otherwise.
    Q_PROPERTY(int unconfirmedCloudCount READ unconfirmedCloudCount NOTIFY blockedChanged)
    Q_PROPERTY(int unconfirmedNotebookCount READ unconfirmedNotebookCount NOTIFY blockedChanged)
    Q_PROPERTY(QString unconfirmedBackupDir READ unconfirmedBackupDir NOTIFY blockedChanged)
    // D, its banner and its dialog: a guard stop that DriveSync can let one
    // run through, with no run under way. A fix holds every run, except one
    // whose final step failed with that stop still the newest run, which D
    // sends back to that step; "drive-sync deletes --allow" follows the same
    // rule (fixWaitsForDeletes).
    Q_PROPERTY(bool deletesHeld READ deletesHeld NOTIFY deletesHeldChanged)
    // Changes when the window adopts another account (a first setup, a
    // takeover, the move to account1).
    Q_PROPERTY(QString logPath READ logPath NOTIFY changed)
    // The config's folder: it changes with a folder change in Settings.
    Q_PROPERTY(QString drivePath READ drivePath NOTIFY changed)
    // The account's units, for the lines that tell the user what to run.
    Q_PROPERTY(QString serviceUnit READ serviceUnit NOTIFY changed)
    Q_PROPERTY(QString timerUnit READ timerUnit NOTIFY changed)
    Q_PROPERTY(QString fixUnit READ fixUnit NOTIFY changed)
    // What the resolver says about the set of accounts, as {kind, path}:
    // "staleConfig" (an old config.json that is ignored) and "moveStuck"
    // (the move to accounts stopped halfway; path is its journal).
    Q_PROPERTY(QVariantList notices READ notices NOTIFY changed)
    // The account shown ("account1"...; empty is the view with no account):
    // every command the window runs names it.
    Q_PROPERTY(QString accountId READ accountId NOTIFY changed)

public:
    explicit Backend(const BackendPaths &paths = BackendPaths::defaults(), QObject *parent = nullptr);
    ~Backend() override;

    QString status() const { return m_summary.status; }
    // "ok" | "busy" | "warning" | "error", the same grouping the bar shows.
    QString tone() const { return m_snapshot.tone; }
    QString causeKind() const { return m_summary.causeKind; }
    QString causeDetail() const { return m_summary.causeDetail; }
    QStringList warnings() const { return m_summary.warnings; }
    QVariantMap currentRun() const { return m_currentRun; }
    QDateTime lastSuccess() const { return m_lastSuccess; }
    QDateTime nextRun() const { return m_nextRun; }
    int typicalSeconds() const { return m_typicalSeconds; }
    bool managed() const { return m_snapshot.managed; }
    bool paused() const { return m_snapshot.paused; }
    QDateTime pausedUntil() const { return m_snapshot.pausedUntil; }
    bool canSyncNow() const { return m_snapshot.canSyncNow; }
    QVariantList runs() const { return m_runs; }
    QVariantList conflicts() const { return m_conflicts; }
    QString conflictError() const { return m_conflictError; }
    QString message() const { return m_message; }
    QString fixStep() const { return m_fix.step; }
    int fixCopies() const { return m_fix.copies; }
    QVariantList fixCopyList() const { return m_fixCopyList; }
    int fixCopiesToNotebook() const { return m_fixCopiesToNotebook; }
    QVariantList fixQuestions() const;
    QString fixError() const { return m_fix.error; }
    bool fixBaseDone() const {
        return m_fix.step == "questions" || m_fix.step == "finishing"
               || (m_fix.step == "failed" && m_fix.failedStep == "finishing");
    }
    int blockedCount() const { return m_blocked.count; }
    int blockedCloudCount() const { return m_blocked.cloudCount; }
    int blockedNotebookCount() const { return m_blocked.notebookCount; }
    QVariantList blockedList() const { return m_blockedList; }
    bool guardStopHolds() const { return m_guardStopHolds; }
    QString blockedKeptReason() const { return m_blocked.keptReason; }
    int unconfirmedCloudCount() const { return m_unconfirmed.cloudCount; }
    int unconfirmedNotebookCount() const { return m_unconfirmed.notebookCount; }
    QString unconfirmedBackupDir() const { return m_unconfirmed.backupDir; }
    bool deletesHeld() const;
    QString logPath() const { return m_paths.log; }
    QString drivePath() const { return m_paths.drive; }
    QString serviceUnit() const { return m_paths.serviceUnit; }
    QString timerUnit() const { return m_paths.timerUnit; }
    QString fixUnit() const { return m_paths.fixUnit; }
    QVariantList notices() const { return m_notices; }
    QString accountId() const { return m_paths.id; }
    const BackendPaths &paths() const { return m_paths; }

    // Gives the window the resolver: reloadConfig asks it first, and when it
    // names another account (another id, config, state folder or unit), the
    // window drops its watches on the old one and follows the new one. So
    // a first setup, a takeover or the move to account1 needs no restart.
    // Without one, reloadConfig only reads the config again.
    void setResolver(std::function<WindowAccount()> resolve);

    // Gives the window the check for an old drive-sync next to accounts;
    // without it the window never looks for one.
    void setOldDriveSyncCheck(const OldDriveSyncCheck &check);

    // Reads the log, asks systemd and looks for conflict copies again.
    Q_INVOKABLE void refresh();
    // Opens the file manager on a conflict copy, given relative to the drive.
    Q_INVOKABLE void showInFileManager(const QString &relativePath);
    // ~/Drive itself, as the bar's O opens it.
    Q_INVOKABLE void openDriveFolder();
    // The same commands the bar and the terminal use; a refusal or a failure
    // shows as the message.
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void pauseFor(int minutes);
    Q_INVOKABLE void pauseUntilTomorrow();
    Q_INVOKABLE void pauseUntilResumed();
    Q_INVOKABLE void resume();
    // "drive-sync fix", step by step. answerFix takes "cloud" or "notebook"
    // for each question, in the order of fixQuestions, and hands them over
    // through <appState>/fix-answers.json. By position, not by name: a file
    // called "__proto__" or "constructor" would break a JavaScript object.
    Q_INVOKABLE void startFix();
    Q_INVOKABLE void confirmFix();
    Q_INVOKABLE void answerFix(const QStringList &answers);
    Q_INVOKABLE void cancelFix();
    Q_INVOKABLE void retryFix();
    // "drive-sync deletes --allow <count>": count is what the user typed,
    // checked again by the command against the list in the log.
    Q_INVOKABLE void allowDeletes(int count);
    // Reads the config again after a takeover or an undo, so the window
    // follows the log and the controls it now owns without a restart.
    Q_INVOKABLE void reloadConfig();
    // "drive-sync accounts --migrate --json" in a child, then reloadConfig.
    // Silent: a move that defers says nothing, and one that got stuck shows
    // as the notice "moveStuck".
    Q_INVOKABLE void startMove();

    static SyncSummary summarize(const SyncInputs &in);
    // "systemctl show" output for several units: one property map per unit.
    static QList<QHash<QString, QString>> parseShow(const QByteArray &output);
    // The same, keyed by each block's Id; a block with no Id is dropped.
    static QHash<QString, QHash<QString, QString>> parseShowById(const QByteArray &output);
    // The next elapse of unit in "systemctl list-timers -o json".
    static QDateTime parseNextElapse(const QByteArray &json, const QString &unit);
    // True when the pid in drive-sync's pause mark is still drive-sync.sh.
    static bool resyncRunning(const QString &stateDir, const QString &proc);
    static QVariantMap runToMap(const SyncRun &run, const QString &outcome);

signals:
    void changed();
    void runsChanged();
    void conflictsChanged();
    void messageChanged();
    void fixChanged();
    void blockedChanged();
    void deletesHeldChanged();

private:
    void readLog();
    void querySystemd();
    void scanConflicts();
    void recompute();
    void watchLog();
    void watchAppState();
    void watchConfig();
    void adopt(const BackendPaths &paths);
    void updateFix(const FixState &fix);
    void updateBlocked(const QString &logText);
    void setMessage(const QString &message);
    void control(const QStringList &arguments);
    void run(const QString &program, const QStringList &arguments,
             std::function<void(bool ok, const QByteArray &output)> done);

    BackendPaths m_paths;
    QFileSystemWatcher m_watcher;
    // DriveSync's own state folder, where the fix writes its step. Watched
    // apart from the log, so a step change does not parse the log again.
    QFileSystemWatcher m_stateWatcher;
    QTimer m_stateDebounce;
    // The config's folder (~/.config/drive-sync, quiet): a settings change
    // made anywhere, the window or a terminal, reaches the window as it
    // reaches "drive-sync status", which reads the config on every call.
    QFileSystemWatcher m_configWatcher;
    QTimer m_configDebounce;
    // Halfway through a live move the resolver can see no account at all
    // (accounts/account1 made, the config not in it yet): the window keeps
    // what it shows and asks again each second until the journal is gone.
    QTimer m_moveWait;
    std::function<WindowAccount()> m_resolve;
    QVariantList m_notices;
    QTimer m_logDebounce;
    QTimer m_poll;
    QTimer m_conflictPoll;
    QTimer m_messageTimer;

    QList<SyncRun> m_parsed;
    bool m_logFound = false;
    bool m_systemctlWorks = true;
    bool m_systemdPending = false;
    bool m_scanPending = false;
    QHash<QString, QString> m_service;
    QHash<QString, QString> m_timer;
    QHash<QString, QString> m_fixUnit;
    OldDriveSyncCheck m_oldDriveSyncCheck;
    QStringList m_oldDriveSync;
    bool m_oldDriveSyncHold = false;
    QDateTime m_lastFinishedStart;
    // Read when the window opens and when the config changes on disk, not on
    // every recompute: it parses the config.
    QString m_configError;

    Snapshot m_snapshot;
    SyncSummary m_summary;
    QVariantMap m_currentRun;
    QVariantList m_runs;
    QVariantList m_conflicts;
    QString m_conflictError;
    QDateTime m_lastSuccess;
    QDateTime m_nextRun;
    int m_typicalSeconds = 0;
    QString m_message;
    FixState m_fix;
    QVariantList m_fixCopyList;
    int m_fixCopiesToNotebook = 0;
    BlockedDeletes m_blocked;
    QVariantList m_blockedList;
    bool m_guardStopHolds = false;
    UnconfirmedDeletes m_unconfirmed;
};
