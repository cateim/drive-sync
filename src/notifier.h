#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QStringList>

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
    // <stateRoot>/notify-bare-titles.done, given only while the resolver
    // lists exactly one account; empty: the bare 5.1 titles are never
    // dismissed (dismissBareTitlesOnce).
    QString bareTitlesMark{};

    static NotifierPaths defaults(const QLocale &locale);
};

// A per-account card's title names its account: "<title> (<label>)", the
// label being the account's, else its folder's base name, else none (the
// bare title). The label sits between fixed text on both sides because
// "omarchy notification dismiss" closes every card whose title contains its
// argument: a label before the text ("My Drive stopped syncing") would let
// one account's recovery close another's card. The cards about every
// account (an old drive-sync, the move, a stale config, the config gone)
// keep their bare titles.
QString accountCardTitle(const QString &title, const BackendPaths &account);

// The 14 per-account titles as 5.1 showed them, with no label, translated.
QStringList accountCardBareTitles();

// Before the first labelled card: the 5.1 cards on screen go once. Every
// labelled title contains its bare title, so this runs only while one
// account exists (tools.bareTitlesMark set) and never while the move's
// journal is in globalState. The mark is written first, and the titles are
// dismissed only when that write succeeded: a dismissal repeated on every
// card would close the account's own labelled card. Nothing when the mark
// path is empty or the mark exists.
void dismissBareTitlesOnce(const NotifierPaths &tools, const QString &globalState);

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
void notifyFixNeedsYou(const BackendPaths &paths, const NotifierPaths &tools, const FixState &state, bool stopped);

// A fix ended with a normal run that worked: the "stopped" card goes, and a
// normal one says Drive syncs again.
void notifyFixEnded(const BackendPaths &paths, const NotifierPaths &tools);

// A run let through deleted files that were not in the confirmed list
// (checkConfirmedRun): a critical card that says how many and where they
// went. Nothing when there were none.
void notifyUnconfirmedDeletes(const BackendPaths &paths, const NotifierPaths &tools, const UnconfirmedDeletes &extra);
