#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "snapshot.h"
#include "syncconfig.h"

// Taking over a drive-sync made by drive-sync.sh: its units, scripts and
// filters move aside, and DriveSync's packaged units run the same rclone
// command. Nothing that names the bisync listings changes (remote, folder,
// work dir), and the filters move with their md5, so no resync is needed.

struct TakeoverPaths {
    QString userUnits;     // ~/.config/systemd/user
    QString binDir;        // ~/.local/bin
    QString oldStateDir;   // ~/.local/state/drive-sync
    QString appConfigDir;  // ~/.config/drive-sync
    QString backupRoot;    // ~/.local/share/drive-sync/taken-over
    QString systemctl;
    QString rclone;
    int serviceWaitMs = 20 * 60 * 1000;           // how long to wait for a run to end
    QString packagedUnits = "/usr/lib/systemd/user";
    QString appState;                             // holds the takeover lock and the pause mark
    QString systemUserUnits = "/etc/systemd/user";  // drop-ins there apply to user units too

    static TakeoverPaths defaults();
};

struct TakeoverPlan {
    QString state;           // "none" | "found" | "managed"
    SyncConfig config;       // what the takeover would write
    QString oldFiltersFile;  // the filters the old unit reads
    QStringList found;       // every old file that moves, for the window to show
    // Machine keys, translated in the window: "running", "lastRunNotSuccess",
    // "noListings", "filtersChanged", "remoteMissing", "unreadableUnit",
    // "unknownDirective:<key>", "differentValue:<key>", "targetExists",
    // "dropIn", "pathInsideMoved".
    QStringList blockers;

    bool canApply() const { return state == "found" && blockers.isEmpty(); }
};

// Reads the old setup and says what a takeover would do, or why it cannot.
// Anything in the old units that DriveSync cannot express blocks it, so
// nothing is ever dropped silently.
TakeoverPlan detectTakeover(const TakeoverPaths &paths, const Snapshot &snapshot);

// Copies the filters and their md5 to a new path, refusing to overwrite, and
// checks that the copy is one bisync accepts.
bool copyFiltersWithMd5(const QString &from, const QString &to, QString *error);

// Takes over as account1 of the accounts layout (step 5.1), in this order,
// undoing every step done so far when one fails: note how the old timers
// were, stop them, wait for a run to end, copy the filters with their md5 to
// <appConfigDir>/accounts/account1/filters.txt, write the old ExecCondition
// to machine.json (unless the same one is there), write the config beside
// the filters (runOnlyWhen empty), disable the old timers, move the
// old units, scripts and state into <backupRoot>/<stamp>/ (manifest first),
// reload systemd, check that account1's packaged instance units are the ones
// systemd uses and carry no drop-in, wait for a run again, move the old
// filters last, enable drive-sync@account1.timer and the watchdog's. The
// rollback also removes the folders it made while they are empty. The old
// filters stay in place until no unit can read
// them any more, so a run started by hand halfway never finds them missing.
// A lock file makes "drive-sync sync" refuse while this runs. When a file
// cannot be moved back, the rollback stops there and leaves every timer off,
// because starting the old ones without their filters would cost a resync.
bool applyTakeover(const TakeoverPaths &paths, const TakeoverPlan &plan, const QDateTime &now, QString *error);

// Goes back to the newest takeover made before accounts (the single config
// of legacy mode). Refused, touching nothing, once <appConfigDir> holds
// accounts/ or disconnected/ (going back is then done by hand, following the
// backup's manifest.json), and when the config cannot be read (the folder
// check needs it). Also refused while a run is going, when the
// filters changed since, or when anything already sits where an old file goes
// back. The packaged timers stop before the old ones return, so there is
// never a second scheduler; if a file cannot go back, the ones already back
// return to the backup and the packaged timers start again. Clears a pause,
// which the old units would not honour.
bool undoTakeover(const TakeoverPaths &paths, QString *error);

// The packaged units must be the ones systemd uses: one systemctl show of
// units, mapped by Id; each must be loaded from a file under packagedUnits
// (a template instance: the template's file), and every drop-in on it must
// be allowedDropIn (empty allows none). Empty, or the reason.
QString checkPackagedUnits(const TakeoverPaths &paths, const QStringList &units, const QString &allowedDropIn);

// Every entry in a drop-in folder, under userUnits or systemUserUnits, that
// reaches account <id>'s units: the templates', the dashed prefixes'
// (drive-sync-.service.d, drive-.service.d, drive-.timer.d), the plain units'
// and the instances' of <id>. DriveSync's own interval files in userUnits
// (the plain timer's and <id>'s instance's) are not foreign.
QStringList foreignDropIns(const TakeoverPaths &paths, const QString &id);

// An old drive-sync next to accounts (spec 12): a DIY restore, or a
// takeover backup put back by hand, can bring drive-sync.sh's units back
// beside drive-sync@account1, on the same folder. found lists what exists:
// <userUnits>/drive-sync.service, .timer, drive-sync-vigia.service, .timer,
// <binDir>/drive-sync.sh, drive-sync-aviso, the filters file the old unit's
// ExecStart names, and a plain drive-sync.service or .timer that systemd
// loads from outside packagedUnits (plainUnits, by Id). folder is what that
// ExecStart syncs, empty when it cannot be read.
struct OldDriveSync {
    QStringList found;
    QString folder;
};
OldDriveSync findOldDriveSync(const TakeoverPaths &paths, const QHash<QString, QHash<QString, QString>> &plainUnits);

// Whether the account on accountFolder must skip its runs: the old unit's
// folder is it, holds it or sits in it (foldersOverlap). An old
// drive-sync.service whose folder cannot be read holds every account, since
// two schedulers on one folder are the worse mistake; scripts, filters, the
// old watchdog or a timer alone hold nothing.
bool oldDriveSyncHolds(const OldDriveSync &old, const QString &accountFolder);

// The check readers take (backend.h) over these paths, and, for a command
// with no show call of its own (run-condition, the watchdog), one
// systemctl show of the plain units, then findOldDriveSync.
OldDriveSyncCheck oldDriveSyncCheck(const TakeoverPaths &paths);
OldDriveSync readOldDriveSync(const TakeoverPaths &paths);

// "drive-sync takeover --check | --apply | --undo [--json]".
int runTakeoverCommand(const QStringList &arguments, const TakeoverPaths &paths, const BackendPaths &backend,
                       QTextStream &out, QTextStream &err);
