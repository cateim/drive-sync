#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "accounts.h"
#include "backend.h"
#include "filterrules.h"
#include "syncconfig.h"
#include "takeover.h"

// "drive-sync setup --folder <path>": first setup when no drive-sync exists
// (spec section 6). It writes the default filters and the config, then
// starts the fix, whose dry run, confirmation, base run, questions and normal
// run are the rest of the setup, and turns the timers on. The fix's mark is
// written before the timers start, so no tick runs before the base.


// What a new user runs: remote kNewRemoteName, the given folder, the work dir
// under DriveSync's own state (never ~/.cache), the usual log, and the flags
// the maintainer's sync proved.
SyncConfig newUserConfig(const QString &folder, const QString &home);

// Why a folder cannot be the synced one, or empty: it must be absolute, not
// the home or a folder holding it, not one where DriveSync, rclone or other
// programs keep their own files (~/.config, ~/.local/state), and not a file.
// First setup and a folder change in Settings both ask it, the second with
// the config's work dir in alsoOwn. Each check runs on the path as written
// and on the one it leads to (resolvedFolder), so a link does not pass.
QString folderRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                      const QStringList &alsoOwn = {});
// The path with its existing part resolved (links, ".."), the rest as written.
QString resolvedFolder(const QString &path);
// Whether two folders are the same, or one holds the other, compared as
// folderRefusal compares them: cleaned, as written and with links resolved.
// An empty path overlaps nothing.
bool foldersOverlap(const QString &a, const QString &b);

// The oldest rclone whose bisync DriveSync relies on.
inline constexpr int kMinRcloneMajor = 1;
inline constexpr int kMinRcloneMinor = 66;

// Why setup cannot start, or empty. Asks rclone its version and its remotes.
QString setupRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                     const QString &rclone);

// The account first setup writes, from the one the dispatcher chose: with
// no account listed (chosen has no id), account1 of the accounts layout
// (newAccountPaths), whose files do not exist yet; otherwise chosen, which
// setup then refuses.
BackendPaths setupAccountPaths(const AccountRoots &roots, const BackendPaths &chosen);

// "drive-sync setup --folder <path> [--skip <cloud folder>]... [--json]":
// each --skip is a cloud folder left out from the start (selectivesync.h).
int runSetupCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                    QTextStream &out, QTextStream &err);
