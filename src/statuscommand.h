#pragma once

#include <QDateTime>
#include <QStringList>
#include <QTextStream>

#include "accounts.h"
#include "backend.h"
#include "snapshot.h"

// "drive-sync status": the state of drive-sync without a window, for the bar
// widget and scripts. It reads the same sources as the window, but waits for
// each answer instead of watching, and decides with the same buildSnapshot().

// Each account's log, conflict count, pause, fix and config, and systemd
// asked once for all of them: one show call with every account's units and
// one list-timers with every timer. A systemctl that fails or takes longer
// than timeoutMs counts as not working, and an account whose three units are
// not all in the answer is unavailable on its own. nextRuns, when given, gets
// each account's next elapse, or an invalid time. With oldDriveSync and an
// account in accounts mode, the plain units ride along in that same show
// call, and each such account learns of an old drive-sync next to it.
QList<SnapshotSources> gatherAccounts(const QList<BackendPaths> &accounts, const QDateTime &now, int timeoutMs,
                                      QList<QDateTime> *nextRuns, const OldDriveSyncCheck &oldDriveSync = {});
// gatherAccounts for one account.
SnapshotSources gatherSources(const BackendPaths &paths, const QDateTime &now, int timeoutMs, QDateTime *nextRun);

// The account that needs the user most: an error, then a warning, then a
// run under way, then ok; the first one on a tie. -1 with none.
int worstAccount(const QList<Snapshot> &snapshots);

// arguments are the ones after "status". Returns the exit code: 0 with the
// state printed, 1 with the usage on err for anything it does not know.
// --json prints version 2 of the only or the worst account. As text, one
// account prints its line as it always did, and several print
// "<id>: <line>" each. Each notice is one line on err, never on out.
// oldDriveSync is what gatherAccounts takes; main gives the real home's.
int runStatusCommand(const QStringList &arguments, const QList<BackendPaths> &accounts,
                     const QList<AccountNotice> &notices, QTextStream &out, QTextStream &err, int timeoutMs = 5000,
                     const OldDriveSyncCheck &oldDriveSync = {});

QString usageText();
