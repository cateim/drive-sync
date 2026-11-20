#include "notifier.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include "accountmove.h"
#include "accounts.h"
#include "blockeddeletes.h"
#include "clockformat.h"
#include "controlcommands.h"
#include "machineguard.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kToolTimeoutMs = 10000;
constexpr int kRepeatSeconds = 3600;
constexpr int kPauseReminderSeconds = 86400;

QString tr(const char *text) { return QCoreApplication::translate("Notifier", text); }
QString tr(const char *text, int n) { return QCoreApplication::translate("Notifier", text, nullptr, n); }

// One open problem: when it began, when it was last shown, the card's id,
// and what the card was about (a guard stop's counts), so another stop of
// the same problem is told apart from the same one again.
struct Problem {
    QDateTime since;
    QDateTime last;
    int id = 0;
    QString key;
};

Problem problemFrom(const QJsonObject &o) {
    Problem p;
    auto when = [](qint64 s) { return s > 0 ? QDateTime::fromSecsSinceEpoch(s, QTimeZone::UTC) : QDateTime(); };
    p.since = when(o.value("since").toInteger());
    p.last = when(o.value("last").toInteger());
    p.id = qMax(0, o.value("id").toInt());
    p.key = o.value("key").toString();
    return p;
}

QJsonObject problemJson(const Problem &p) {
    auto seconds = [](const QDateTime &t) { return t.isValid() ? t.toSecsSinceEpoch() : 0; };
    return {{"since", seconds(p.since)}, {"last", seconds(p.last)}, {"id", p.id}, {"key", p.key}};
}

QJsonObject readJsonFile(const QString &file) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(in.readAll()).object();
}

void writeJsonFile(const QString &file, const QJsonObject &object) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    out.commit();
}

Problem readProblem(const QString &file) { return problemFrom(readJsonFile(file)); }

void writeProblem(const QString &file, const Problem &p) { writeJsonFile(file, problemJson(p)); }

QString at(const NotifierPaths &tools, const QDateTime &when) {
    if (tools.timeFormat.isEmpty())
        return tools.locale.toString(when.toLocalTime(), QLocale::ShortFormat);
    return tools.locale.toString(when.toLocalTime(),
                                 tools.locale.dateFormat(QLocale::ShortFormat) + " " + tools.timeFormat);
}

// The card's id, or 0 when there is no graphical session to show it.
int notify(const NotifierPaths &tools, const QString &urgency, const QString &icon, const QString &title,
           const QString &body) {
    QByteArray output;
    runProcess(tools.notifySend, {"-p", "-a", "drive-sync", "-u", urgency, "-i", icon, title, body}, kToolTimeoutMs,
               &output, nullptr);
    return QString::fromUtf8(output).trimmed().toInt();
}

// CloseNotification only removes a card that has not shown yet: the Omarchy
// shell keeps visible ones, so the shell's own command closes them by title.
void close(const NotifierPaths &tools, int id, const QStringList &titles) {
    if (id > 0)
        runProcess(tools.gdbus,
                   {"call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path",
                    "/org/freedesktop/Notifications", "--method", "org.freedesktop.Notifications.CloseNotification",
                    QString::number(id)},
                   kToolTimeoutMs, nullptr, nullptr);
    for (const QString &title : titles)
        runProcess(tools.omarchy, {"notification", "dismiss", title}, kToolTimeoutMs, nullptr, nullptr);
}

// An ongoing problem: shown the first time, then every hour. A key other
// than the last card's (another guard stop's list), or news, shows at once.
// False when no card was due, so there is nothing to write back.
bool showProblem(const NotifierPaths &tools, Problem &p, const QString &firstTitle, const QString &reminderTitle,
                 const QString &text, const QString &footer, const QDateTime &now, const QString &urgency,
                 const QString &key, bool news) {
    QString title;
    if (!p.since.isValid()) {
        p.since = now;
        title = firstTitle;
    } else if (news || (!key.isEmpty() && key != p.key)) {
        title = firstTitle;
    } else if (!p.last.isValid() || p.last.secsTo(now) >= kRepeatSeconds) {
        // No card ever reached the screen (last unset): try the first one again.
        title = p.last.isValid() ? reminderTitle : firstTitle;
    } else {
        return false;
    }
    // Also the first time: an old card of the same problem does not stay stacked.
    close(tools, p.id, {firstTitle, reminderTitle});
    const int id = notify(tools, urgency, urgency == "critical" ? "dialog-warning" : "dialog-information", title,
                          text + "\n" + footer.arg(at(tools, p.since)));
    p.last = id > 0 ? now : QDateTime();
    p.id = id;
    // A card about something else keeps the last guard stop's key, or the
    // same stop afterwards would look new.
    if (!key.isEmpty())
        p.key = key;
    return true;
}

// showProblem over a file of its own. With account, the file is in that
// account's state folder, and the write takes its lock after the card: the
// move to accounts may have taken the file meanwhile (or be taking it), and
// one written at the old place would make its rollback stuck, so the
// bookkeeping then waits for the next round.
void reportProblem(const NotifierPaths &tools, const QString &file, const QString &firstTitle,
                   const QString &reminderTitle, const QString &text, const QString &footer, const QDateTime &now,
                   const QString &urgency = "critical", const QString &key = QString(), bool news = false,
                   const BackendPaths *account = nullptr) {
    Problem p = readProblem(file);
    if (!showProblem(tools, p, firstTitle, reminderTitle, text, footer, now, urgency, key, news))
        return;
    if (!account) {
        writeProblem(file, p);
        return;
    }
    const AccountWriteLock lock(*account);
    if (lock.refusal().isEmpty() && !lock.stale())
        writeProblem(file, p);
}

// The cards about every account (an old config left beside accounts/, a
// move that stopped halfway, an old drive-sync), kept by kind in
// <stateRoot>/notify-accounts.json, which no move or rollback touches.
QString accountsCardsPath(const QString &stateRoot) { return stateRoot + "/notify-accounts.json"; }

// A problem about every account, repeated every hour like any other.
void reportAccountsProblem(const NotifierPaths &tools, const QString &stateRoot, const QString &kind,
                           const QString &firstTitle, const QString &reminderTitle, const QString &text,
                           const QDateTime &now, const QString &urgency) {
    const QString file = accountsCardsPath(stateRoot);
    QJsonObject cards = readJsonFile(file);
    Problem p = problemFrom(cards.value(kind).toObject());
    if (!showProblem(tools, p, firstTitle, reminderTitle, text, tr("Noticed at %1."), now, urgency, QString(), false))
        return;
    cards.insert(kind, problemJson(p));
    writeJsonFile(file, cards);
}

// Such a problem is gone (fixed by hand): its card goes, with nothing said.
void dropAccountsProblem(const NotifierPaths &tools, const QString &stateRoot, const QString &kind,
                         const QStringList &titles) {
    const QString file = accountsCardsPath(stateRoot);
    QJsonObject cards = readJsonFile(file);
    if (!cards.contains(kind))
        return;
    close(tools, problemFrom(cards.value(kind).toObject()).id, titles);
    cards.remove(kind);
    writeJsonFile(file, cards);
}

// An old drive-sync back next to the accounts: one card for each list of
// what was found, never an hourly one, since nothing DriveSync does removes
// it. The list is kept in <stateRoot>/notify-accounts.json, beside the other
// cards about every account, and dropped once nothing is found, so a later
// return gets its card again.
void reportOldDriveSync(const NotifierPaths &tools, const QString &stateRoot, QStringList found) {
    const QString file = accountsCardsPath(stateRoot);
    QJsonObject cards = readJsonFile(file);
    found.sort();
    const QString key = found.join('\n');
    if (key == cards.value("oldDriveSync").toString())
        return;
    if (found.isEmpty()) {
        cards.remove("oldDriveSync");
    } else {
        const int id = notify(tools, "critical", "dialog-warning", tr("Old drive-sync found"),
                              tr("An old drive-sync is back next to DriveSync: %1. Remove it by hand; the account on "
                                 "its folder does not sync until then.")
                                  .arg(found.join(", ")));
        // No graphical session: the next watchdog tries again.
        if (id <= 0)
            return;
        cards.insert("oldDriveSync", key);
    }
    writeJsonFile(file, cards);
}

// A problem that ended: its card goes, and a normal one says since when it
// lasted. With account, the file goes under that account's lock, for the
// reason reportProblem gives.
void reportSolved(const NotifierPaths &tools, const QString &file, const QString &title, const QString &text,
                  const QStringList &problemTitles, const BackendPaths *account = nullptr) {
    const Problem p = readProblem(file);
    if (!p.since.isValid())
        return;
    close(tools, p.id, problemTitles);
    notify(tools, "normal", "dialog-information", title, text.arg(at(tools, p.since)));
    if (!account) {
        QFile::remove(file);
        return;
    }
    const AccountWriteLock lock(*account);
    if (lock.refusal().isEmpty() && !lock.stale())
        QFile::remove(file);
}

QString causeSentence(const QString &kind, const QString &detail) {
    if (kind == "lostListings")
        return tr("bisync lost its listings, the base of both sides. To recover, use Fix… in DriveSync.");
    if (kind == "outOfSync")
        return tr("The listings of the two sides don't match, so bisync dropped its base. "
                  "To recover, use Fix… in DriveSync.");
    if (kind == "filtersChanged")
        return tr("The filters changed since the last base, so every run waits. "
                  "Open DriveSync and use Fix… to rebuild the base with them.");
    if (kind == "firstSyncPending")
        return tr("The first sync was never confirmed, so nothing syncs yet. Open DriveSync and use Fix… to see "
                  "its dry run.");
    if (kind == "signIn")
        return tr("Google no longer accepts DriveSync's sign-in, so nothing syncs. Open DriveSync and sign in again.");
    return detail;
}

// One line on what broke in the run that just ended. Only runs that started
// with it count, as in drive-sync-aviso; the log parser already turns an
// error before "Synching Path1" (rclone that did not start, or DriveSync's
// refusal) into a failed run of its own.
QString cause(const QString &serviceResult, const BackendPaths &paths, QString *kind, QString *stopKey, bool *news) {
    if (serviceResult == "timeout")
        return tr("The run went past its time limit (TimeoutStartSec) and was stopped.");
    QByteArray stamp;
    runProcess(paths.systemctl,
               {"--user", "show", "--timestamp=unix", "-p", "ExecMainStartTimestamp", "--value", paths.serviceUnit},
               kToolTimeoutMs, &stamp, nullptr);
    const QDateTime started = fromUnixStamp(QString::fromUtf8(stamp).trimmed());
    bool truncated = false;
    const QString text = SyncLog::readRecent(paths.log, 256 * 1024, nullptr, &truncated);
    // Every record, so a stop behind many refused ticks is still seen.
    const QList<SyncRun> runs = SyncLog::parse(text, kStateRuns, truncated);
    // The log keeps whole seconds; systemd's start can be up to a second later.
    if (!runs.isEmpty() && (!started.isValid() || runs.last().start >= started.addSecs(-2))
        && runs.last().outcome == "failed") {
        *kind = runs.last().causeKind;
        // Spec 5b: the guard stopped it before anything was deleted. Every
        // deletion counts, both sides, as the window lists them.
        if (*kind == "filtersChanged" && guardStopHolds(runs))
            return tr("The filters changed after a run stopped at the max-delete guard, and a fix would bring those "
                      "files back. Undo the filters edit first: the next run lists the deletions for you to review.")
                .left(200);
        if (*kind == "maxDelete")
            if (const auto blocked = blockedDeletes(text, 0)) {
                // Each tick stops again on the same list: only another list,
                // or the run the user let through, is a new stop.
                *stopKey = QString("%1:%2").arg(blocked->cloudCount).arg(blocked->notebookCount);
                *news = !blocked->keptReason.isEmpty();
                if (*news)
                    return tr("The run you let through kept the usual guard and stopped again: it would delete %Ln "
                              "files. Open DriveSync and use Review… to see them.",
                              blocked->count)
                        .left(200);
                return tr("A run would delete %Ln files, more than the guard allows, so it stopped and deleted "
                          "nothing. Open DriveSync and use Review… to see them.",
                          blocked->count)
                    .left(200);
            }
        const QString sentence = causeSentence(runs.last().causeKind, runs.last().causeDetail);
        if (!sentence.isEmpty())
            return sentence.left(200);
    }
    return tr("No detail in the log (result: %1).").arg(serviceResult.isEmpty() ? "?" : serviceResult);
}

QString showValue(const BackendPaths &paths, const QStringList &arguments) {
    QByteArray output;
    runProcess(paths.systemctl, arguments, kToolTimeoutMs, &output, nullptr);
    return QString::fromUtf8(output).trimmed();
}

// What keeps every run from happening; empty when all is well.
QString timerProblem(const BackendPaths &paths) {
    const QString load = showValue(paths, {"--user", "show", "-p", "LoadState", "--value", paths.serviceUnit});
    if (load != "loaded")
        return tr("%1 does not load (LoadState=%2). See: systemctl --user status %1")
            .arg(paths.serviceUnit, load.isEmpty() ? "?" : load);
    // drive-sync.sh stops the timer on purpose during its resync.
    if (Backend::resyncRunning(paths.stateDir, paths.proc))
        return {};
    const QString active = showValue(paths, {"--user", "is-active", paths.timerUnit});
    if (active != "active")
        return tr("%1 is %2: no run happens. To turn it back on: systemctl --user enable --now %1")
            .arg(paths.timerUnit, active.isEmpty() ? "?" : active);
    const QString enabled = showValue(paths, {"--user", "is-enabled", paths.timerUnit});
    if (enabled != "enabled")
        return tr("%1 is %2: it runs now, but will not start on the next boot. To fix: systemctl --user enable %1")
            .arg(paths.timerUnit, enabled.isEmpty() ? "?" : enabled);
    return {};
}

// A fix waiting for the user, or stopped, holds every run: a card every
// hour, counted from when its step began, so a forgotten fix cannot become
// the silent outage this app exists to prevent.
void remindOfFix(const BackendPaths &paths, const NotifierPaths &tools, const QDateTime &now) {
    if (!fixPresent(paths.appState))
        return;
    const FixState state = readFix(paths.appState);
    bool stopped = state.step == "failed";
    if (fixWorking(state.step)) {
        const qint64 age = state.stepStarted.isValid() ? state.stepStarted.secsTo(now) : kFixStartGraceSeconds;
        QByteArray output;
        const bool asked = runProcess(paths.systemctl,
                                      {"--user", "show", "-p", "ActiveState", "--value", paths.fixUnit},
                                      kToolTimeoutMs, &output, nullptr);
        const QString active = QString::fromUtf8(output).trimmed();
        // Starting, at work, or systemd did not say: no "stopped" card on a guess.
        if ((age >= 0 && age < kFixStartGraceSeconds) || !asked || active.isEmpty()
            || fixUnitBusy({{"ActiveState", active}}))
            return;
        stopped = true;  // the unit left the step halfway
    }
    const QDateTime since = state.stepStarted.isValid() ? state.stepStarted : now;
    const QDateTime last = state.lastReminder.isValid() ? state.lastReminder : since;
    if (last.secsTo(now) < kRepeatSeconds)
        return;
    notifyFixNeedsYou(tools, state, stopped);
    // notify-send can take seconds: a step that moved on in the meantime wins,
    // and the lock keeps a confirm from landing between this read and write.
    // While the move to account1 runs, or once it moved the fix, the
    // bookkeeping waits for the next round.
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty() || lock.stale())
        return;
    FixState current = readFix(paths.appState);
    if (current.step != state.step || current.stepStarted != state.stepStarted)
        return;
    current.lastReminder = now;
    writeFix(paths.appState, current);
}

void remindOfPause(const BackendPaths &paths, const NotifierPaths &tools, const QDateTime &now) {
    const QString file = pauseFilePath(paths.appState);
    PauseMark mark = readPause(file);
    if (!mark.present || mark.until.isValid())
        return;  // a pause with an end ends by itself
    const QDateTime since = mark.since.isValid() ? mark.since : now;
    const QDateTime last = mark.lastReminder.isValid() ? mark.lastReminder : since;
    if (last.secsTo(now) < kPauseReminderSeconds)
        return;
    notify(tools, "normal", "dialog-information", tr("DriveSync is still paused"),
           tr("Drive has been paused since %1. Resume it in DriveSync or with: drive-sync resume")
               .arg(at(tools, since)));
    // notify-send can take seconds: a resume or a new pause in the meantime
    // wins. The write takes the account's lock, after the card: the move to
    // account1 may have taken the mark meanwhile, and the bookkeeping waits.
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty() || lock.stale())
        return;
    const PauseMark current = readPause(file);
    if (!current.present || current.since != mark.since || current.until != mark.until)
        return;
    mark.lastReminder = now;
    writePause(file, mark);
}

}  // namespace

NotifierPaths NotifierPaths::defaults(const QLocale &locale) {
    return {QStandardPaths::findExecutable("notify-send"), QStandardPaths::findExecutable("gdbus"),
            QStandardPaths::findExecutable("omarchy"), locale, currentClockTimeFormat()};
}

int runNotifyCommand(const QString &serviceResult, const BackendPaths &paths, const NotifierPaths &tools,
                     const QDateTime &now) {
    const QString file = paths.appState + "/notify-run.json";
    const QString stopped = tr("Drive stopped syncing");
    const QString stillStopped = tr("Drive is still stopped");
    const QString filters = tr("Drive filters changed");
    const QString stillFilters = tr("Drive filters still changed");
    const QString firstSync = tr("Drive's first sync waits for you");
    const QString stillFirstSync = tr("Drive's first sync still waits for you");
    if (serviceResult == "exec-condition")
        return 0;  // skipped on purpose: paused, a fix under way, or runOnlyWhen said no
    // drive-sync run became rclone, so the run let through is checked here,
    // after it, whatever it ended with.
    notifyUnconfirmedDeletes(tools, checkConfirmedRun(paths.appState, paths.log));
    if (serviceResult == "success") {
        reportSolved(tools, file, tr("Drive is syncing again"), tr("It had stopped at %1."),
                     {stopped, stillStopped, filters, stillFilters, firstSync, stillFirstSync});
        return 0;
    }
    QString kind, stopKey;
    bool news = false;
    const QString text = cause(serviceResult, paths, &kind, &stopKey, &news);
    // Changed filters are the user's own edit, waiting for F: news, not an alarm.
    if (kind == "filtersChanged")
        reportProblem(tools, file, filters, stillFilters, text, tr("Since %1."), now, "normal");
    // A first sync nobody confirmed waits for the user's F, like an edit.
    else if (kind == "firstSyncPending")
        reportProblem(tools, file, firstSync, stillFirstSync, text, tr("Since %1."), now, "normal");
    else
        reportProblem(tools, file, stopped, stillStopped, text, tr("Stopped since %1. Log: %2").arg("%1", paths.log),
                      now, "critical", stopKey, news);
    return 0;
}

namespace {

// One account's watch: its config, the machine guard's file when it cannot
// be read, its units (timerProblem), then the reminders of its pause and its
// fix. The bookkeeping is in the account's own state folder.
void watchAccount(const BackendPaths &paths, const NotifierPaths &tools, const QDateTime &now,
                  const QString &guardError) {
    QString configError;
    if (loadSyncConfig(paths.config, &configError))
        configError.clear();
    const QString file = paths.appState + "/notify-watchdog.json";
    const QString noSync = tr("Drive will not sync");
    const QString stillNoSync = tr("Drive is still not syncing");
    const QString problem = !configError.isEmpty()
        ? tr("DriveSync's config cannot be read, so no run happens: %1").arg(configError)
        : !guardError.isEmpty() ? tr("No run happens: %1").arg(guardError) : timerProblem(paths);
    if (!problem.isEmpty())
        reportProblem(tools, file, noSync, stillNoSync, problem, tr("Noticed at %1."), now, "critical", QString(),
                      false, &paths);
    else
        reportSolved(tools, file, tr("Drive timer is back"), tr("The problem was noticed at %1."), {noSync, stillNoSync},
                     &paths);
    remindOfPause(paths, tools, now);
    remindOfFix(paths, tools, now);
}

// The resolver's notices, each a card about every account. While the move's
// journal is there, a config at the old place is only a step of that move,
// so only the stuck move is said and the other card is left as it is.
void reportAccountNotices(const NotifierPaths &tools, const AccountRoots &roots, const QList<AccountNotice> &notices,
                          bool moving, const MoveOutcome &move, const QDateTime &now) {
    const auto noticeOf = [&](const QString &kind) {
        for (const AccountNotice &notice : notices)
            if (notice.kind == kind)
                return notice.path;
        return QString();
    };
    const QString stuckTitle = tr("DriveSync's move to accounts stopped");
    const QString stillStuckTitle = tr("DriveSync's move to accounts is still stopped");
    if (const QString journal = noticeOf("moveStuck"); !journal.isEmpty()) {
        // drive-sync accounts --migrate runs the rollback from the journal
        // again, so once its cause is cleared it puts everything back.
        QString text = tr("DriveSync's move to accounts stopped halfway, and every drive-sync timer is off. "
                          "Clear what stuckReason in %1 names, then run: drive-sync accounts --migrate")
                           .arg(journal);
        if (const QString reason = migrationStuckReason(journal); !reason.isEmpty())
            text += "\n" + tr("stuckReason: %1").arg(reason.left(200));
        reportAccountsProblem(tools, roots.stateRoot, "moveStuck", stuckTitle, stillStuckTitle, text, now, "critical");
    } else {
        dropAccountsProblem(tools, roots.stateRoot, "moveStuck", {stuckTitle, stillStuckTitle});
    }
    // A move undone (here, or by the window's own try) is tried again every
    // hour; without a card it would fail and be undone again in silence.
    // Deferred is no news either way: nothing was tried.
    const QString undoneTitle = tr("DriveSync's move to accounts was undone");
    const QString stillUndoneTitle = tr("DriveSync's move to accounts was undone again");
    if (move.result == "rolledBack")
        reportAccountsProblem(tools, roots.stateRoot, "moveRolledBack", undoneTitle, stillUndoneTitle,
                              tr("DriveSync tried to move to accounts and put everything back as it was: %1. It tries "
                                 "again every hour.")
                                  .arg(move.reason.left(300)),
                              now, "normal");
    else if (move.result == "moved" || move.result == "notNeeded")
        dropAccountsProblem(tools, roots.stateRoot, "moveRolledBack", {undoneTitle, stillUndoneTitle});
    if (moving)
        return;
    const QString staleTitle = tr("DriveSync ignores an old config");
    const QString stillStaleTitle = tr("DriveSync still ignores an old config");
    if (const QString config = noticeOf("staleConfig"); !config.isEmpty())
        reportAccountsProblem(tools, roots.stateRoot, "staleConfig", staleTitle, stillStaleTitle,
                              tr("DriveSync ignores %1: each account's config now lives under %2. Remove it by hand.")
                                  .arg(config, roots.configRoot + "/accounts"),
                              now, "normal");
    else
        dropAccountsProblem(tools, roots.stateRoot, "staleConfig", {staleTitle, stillStaleTitle});
}

// The single config gone while DriveSync's plain timer still runs (spec
// 12 left it a silent skip): what keeps every run from happening, or empty.
// The timer counts only as DriveSync's own unit, enabled or active: a new
// machine has it off, and after an undone takeover the old user units of
// the same name are drive-sync.sh's, which never read this config.
QString configGoneProblem(const AccountRoots &roots, const TakeoverPaths &takeover) {
    if (!singleConfigGone(roots.configRoot))
        return {};
    const QString timer = noAccountPaths(roots).timerUnit;
    QByteArray output;
    if (!runProcess(takeover.systemctl,
                    {"--user", "show", "-p", "Id,UnitFileState,ActiveState,FragmentPath", timer}, kToolTimeoutMs,
                    &output, nullptr))
        return {};
    const QHash<QString, QString> unit = Backend::parseShowById(output).value(timer);
    if (!unit.value("FragmentPath").startsWith(takeover.packagedUnits + "/"))
        return {};
    if (unit.value("UnitFileState") != "enabled" && unit.value("ActiveState") != "active")
        return {};
    QString error;
    loadSyncConfig(roots.configRoot + "/config.json", &error);
    return tr("DriveSync's config cannot be read, so no run happens: %1").arg(error);
}

// Its card, about every account, like the other cards about them.
void reportConfigGone(const NotifierPaths &tools, const AccountRoots &roots, const QString &problem,
                      const QDateTime &now) {
    const QString title = tr("DriveSync's config is gone");
    const QString stillTitle = tr("DriveSync's config is still gone");
    if (!problem.isEmpty())
        reportAccountsProblem(tools, roots.stateRoot, "configGone", title, stillTitle, problem, now, "critical");
    else
        dropAccountsProblem(tools, roots.stateRoot, "configGone", {title, stillTitle});
}

}  // namespace

int runWatchdogCommand(const AccountRoots &roots, const TakeoverPaths &takeover, const NotifierPaths &tools,
                       const QDateTime &now) {
    // The move to account1 first, or the rollback of one that stopped
    // halfway: what it changes is what the checks below look at. Deferred
    // is no problem; the next round tries again. A move it had to undo gets
    // a card (reportAccountNotices).
    const MoveOutcome move = moveToAccounts(roots, takeover);
    const AccountSet set = resolveAccounts(roots);
    // Where the runs are skipped on purpose (another home mounted, say), the
    // watchdog has nothing to say either. The check is the machine's, so it
    // runs once: legacy mode reads it from the only config, accounts mode
    // from machine.json. A machine.json it cannot read holds every run,
    // which is a problem to report, not a skip.
    const BackendPaths guardPaths = set.accounts.isEmpty() ? noAccountPaths(roots) : set.accounts.first();
    QString guardError;
    const QString guard =
        machineGuardCommand(guardPaths, loadSyncConfig(guardPaths.config, nullptr).value_or(SyncConfig()), &guardError);
    if (guardError.isEmpty() && runMachineGuard(guard) != 0)
        return 0;
    // While the journal is there (a move another drive-sync is making, or
    // one that stopped halfway), no account is checked and none of their
    // files is written: the timers are off on purpose, and a file written at
    // a place the move empties would make its rollback stuck.
    const bool moving = moveInProgress(roots.stateRoot);
    if (!moving) {
        for (const BackendPaths &account : set.accounts)
            watchAccount(account, tools, now, guardError);
        // Next to accounts only: with none, an old drive-sync is what a
        // takeover offers to move aside.
        if (set.mode == "accounts" && !set.accounts.isEmpty())
            reportOldDriveSync(tools, roots.stateRoot, readOldDriveSync(takeover).found);
        // With no account at all, nothing above looks at the plain timer.
        reportConfigGone(tools, roots, set.accounts.isEmpty() ? configGoneProblem(roots, takeover) : QString(), now);
    }
    reportAccountNotices(tools, roots, set.notices, moving, move, now);
    return 0;
}

void notifyFixNeedsYou(const NotifierPaths &tools, const FixState &state, bool stopped) {
    const QString waiting = tr("The Drive fix is waiting for you");
    const QString halted = tr("The Drive fix stopped");
    close(tools, 0, {waiting, halted});
    if (stopped)
        notify(tools, "critical", "dialog-warning", halted,
               (state.error.isEmpty()          ? tr("It stopped halfway.")
                : isTokenError(state.error) ? tr("Google no longer accepts DriveSync's sign-in. Open DriveSync, sign in again and retry the fix.")
                                               : state.error.left(200))
                   + "\n"
                   + tr("Drive does not sync until it ends. Open DriveSync to retry or cancel it."));
    else
        notify(tools, "normal", "dialog-information", waiting,
               tr("Drive does not sync until you answer. Open DriveSync and use Fix… to answer."));
}

void notifyFixEnded(const BackendPaths &paths, const NotifierPaths &tools) {
    const QString file = paths.appState + "/notify-run.json";
    close(tools, readProblem(file).id,
          {tr("Drive stopped syncing"), tr("Drive is still stopped"), tr("Drive filters changed"),
           tr("Drive filters still changed")});
    QFile::remove(file);
    notify(tools, "normal", "dialog-information", tr("Drive is syncing again"),
           tr("The fix rebuilt the base of both sides."));
}

void notifyUnconfirmedDeletes(const NotifierPaths &tools, const UnconfirmedDeletes &extra) {
    const int count = extra.cloudCount + extra.notebookCount;
    if (count == 0)
        return;
    QString text = tr("The run you let through deleted %Ln files that were not in the list you confirmed.", count);
    if (extra.cloudCount > 0)
        text += " " + tr("The ones deleted on the notebook are kept in %1.").arg(extra.backupDir);
    if (extra.notebookCount > 0)
        text += " " + tr("The ones deleted in the cloud are in the Drive trash.");
    notify(tools, "critical", "dialog-warning", tr("Drive deleted files you did not confirm"), text);
}
