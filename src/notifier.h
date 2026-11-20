#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

#include "accounts.h"
#include "backend.h"
#include "syncconfig.h"
#include "takeover.h"

// The desktop notifications of drive-sync, ported from drive-sync-aviso:
// a critical card when a run fails, a reminder every hour while it keeps
// failing, and a normal card saying since when once a run works again. The
// watchdog covers what no run can report, because then no run happens: a
// timer that is off or not enabled, or a unit that does not load. It also
// reminds once a day of a pause with no end, so a forgotten pause cannot
// become the silent outage this app exists to prevent.
//
// Exists because on 2026-11-06 the bisync failed 26 times in a row over six
// hours with nothing on screen.

struct NotifierPaths {
    QString notifySend;
    QString gdbus;    // closes a card by id
    QString omarchy;  // "omarchy notification dismiss" closes a card already shown, by title
    QLocale locale;   // for the dates and the AM/PM text
    QString timeFormat;  // the bar clock's hours (clockformat.h); empty: the locale's own

    static NotifierPaths defaults(const QLocale &locale);
};

// "drive-sync notify", the ExecStopPost of drive-sync.service, with systemd's
// $SERVICE_RESULT. Always returns 0: a notification that cannot be shown
// must not turn a good run into a failed unit.
int runNotifyCommand(const QString &serviceResult, const BackendPaths &paths, const NotifierPaths &tools,
                     const QDateTime &now);

// "drive-sync watchdog", from drive-sync-watchdog.timer every hour. First
// the move to account1 (accountmove.h), with the rollback of one that
// stopped halfway; then, once for the machine, the runOnlyWhen guard, which
// silences it when it says no; then each account the resolver lists: a
// config that cannot be read or units that keep every run from happening (a
// card every hour, kept in the account's notify-watchdog.json), and the
// reminders of a pause with no end and of a fix that waits. While the
// move's journal exists no account is checked. Last, the cards about every
// account, kept in <stateRoot>/notify-accounts.json: an ignored old config
// and a stuck move (every hour), and an old drive-sync next to accounts
// (one card per list of what was found, through takeover's tools).
int runWatchdogCommand(const AccountRoots &roots, const TakeoverPaths &takeover, const NotifierPaths &tools,
                       const QDateTime &now);

// A fix that waits for the user (stopped false) or stopped: the card the unit
// sends at once, and the watchdog repeats every hour.
void notifyFixNeedsYou(const NotifierPaths &tools, const FixState &state, bool stopped);

// A fix ended with a normal run that worked: the "stopped" card goes, and a
// normal one says Drive syncs again.
void notifyFixEnded(const BackendPaths &paths, const NotifierPaths &tools);

// A run let through deleted files that were not in the confirmed list
// (checkConfirmedRun): a critical card that says how many and where they
// went. Nothing when there were none.
void notifyUnconfirmedDeletes(const NotifierPaths &tools, const UnconfirmedDeletes &extra);
