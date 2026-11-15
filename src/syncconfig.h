#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>

// The packaged timer's interval, and the range the settings accept.
inline constexpr int kDefaultIntervalMinutes = 15;
inline constexpr int kMinIntervalMinutes = 5;
inline constexpr int kMaxIntervalMinutes = 1440;

// What DriveSync runs when it owns drive-sync. A takeover copies every value
// byte for byte from the old unit: the bisync listings are named after the
// remote and local strings, so changing either one would ask for a resync.
struct SyncConfig {
    QString remote;           // "gdrive:", always Path1
    QString localPath;        // always Path2
    QString workDir;          // where bisync keeps its listings, never under ~/.cache
    QString filtersFile;      // with its ".md5" beside it
    QString logFile;
    QStringList rcloneFlags;  // every other rclone argument, in the old unit's order
    QString runOnlyWhen;      // a shell command; the run is skipped when it fails; empty runs always
    // Minutes between runs, written into a drop-in for the packaged timer
    // (drive-sync settings --interval). "Late" is three of them.
    int intervalMinutes = kDefaultIntervalMinutes;
};

// The rclone flags a new user's sync runs with (first setup); the cloud
// folder tree lists with their --drive-* ones before any config exists.
QStringList newUserFlags();

// ~/.config/drive-sync/config.json
QString defaultSyncConfigPath();
// Missing, unreadable, another version or a missing field: no config, and
// error says which. intervalMinutes came later and is optional: absent or
// not a positive whole number, it reads kDefaultIntervalMinutes.
std::optional<SyncConfig> loadSyncConfig(const QString &path, QString *error);
// The bytes saveSyncConfig writes.
QByteArray syncConfigBytes(const SyncConfig &config);
// Writes through QSaveFile, so a crash never leaves half a config behind.
bool saveSyncConfig(const QString &path, const SyncConfig &config, QString *error);
// The arguments after "rclone" for one normal run.
QStringList rcloneRunArguments(const SyncConfig &config);
// bisync keeps the md5 of the filters in "<file>.md5" and checks it on every
// run. A run that finds it missing or different stops with a critical error
// and renames the listings to "-err", and from then on only a resync brings
// the sync back (tried with rclone 1.75 on 2026-11-08). So nothing starts
// rclone unless this is true.
bool filtersUsable(const QString &filtersFile, QString *why);
// Ends every refusal of filtersUsable, so the log parser can tell a run
// refused for changed filters (which a fix repairs) from any other error.
inline constexpr char kFiltersChangedReason[] = "so bisync would lose its base";
// Ends the refusal of a run before the first base was ever made (a first
// setup whose dry run was not confirmed): no .md5 and no listings. The log
// parser gives it a cause of its own, so it never reads as edited filters.
inline constexpr char kFirstSyncPendingReason[] = "open DriveSync and press F to see its dry run";
