#pragma once

#include <QString>

#include <optional>

#include "backend.h"
#include "syncconfig.h"

// The runOnlyWhen check guards the machine (the right home mounted, say),
// not one account (spec 12, "The machine guard"). In legacy mode it is the
// config's runOnlyWhen, as before accounts. In accounts mode, and with no
// account at all, it is <configRoot>/machine.json:
// {"version": 1, "runOnlyWhen": "<shell command>"}. A missing file or an
// empty command is no guard. Each command checks it once.

// <configRoot>/machine.json.
QString machineGuardPath(const QString &configRoot);

// The command in file: "" for a missing file or an empty command; nullopt,
// with *error, for a file that is not a version 1 object with text in
// runOnlyWhen.
std::optional<QString> readMachineGuard(const QString &file, QString *error);

// Writes {"version": 1, "runOnlyWhen": command} to file at once (QSaveFile).
bool writeMachineGuard(const QString &file, const QString &command, QString *error);

// The command that guards paths' account: config.runOnlyWhen in legacy mode,
// else the file. An unreadable file gives "" and *error naming it:
// "<file> cannot be read: <why>".
QString machineGuardCommand(const BackendPaths &paths, const SyncConfig &config, QString *error);

// Runs command with sh -c, as systemd reads an ExecCondition: 0 passes, 1 says
// no (any exit but 0 and 255), 255 for a crash, a timeout or exit 255. An
// empty command passes.
int runMachineGuard(const QString &command);

// Why a command that would touch the folder must not run on this machine:
// empty, "the runOnlyWhen check says no (<command>), so this is not the home
// drive-sync runs on", or the file that cannot be read.
QString machineGuardRefusal(const BackendPaths &paths, const SyncConfig &config);
