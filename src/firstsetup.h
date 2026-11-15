#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "syncconfig.h"
#include "takeover.h"

// "drive-sync setup --folder <path>": first setup when no drive-sync exists
// (spec section 6). It writes the default filters and the config, then
// starts the fix, whose dry run, confirmation, base run, questions and normal
// run are the rest of the setup, and turns the timers on. The fix's mark is
// written before the timers start, so no tick runs before the base.

// The generic rules a new user starts with; editing them later needs F.
QByteArray defaultFilters();

// What a new user runs: remote kNewRemoteName, the given folder, the work dir
// under DriveSync's own state (never ~/.cache), the usual log, and the flags
// the maintainer's sync proved.
SyncConfig newUserConfig(const QString &folder, const QString &home);

// The oldest rclone whose bisync DriveSync relies on.
inline constexpr int kMinRcloneMajor = 1;
inline constexpr int kMinRcloneMinor = 66;

// Why setup cannot start, or empty. Asks rclone its version and its remotes.
QString setupRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                     const QString &rclone);

// "drive-sync setup --folder <path> [--json]".
int runSetupCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                    QTextStream &out, QTextStream &err);
