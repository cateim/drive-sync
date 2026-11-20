#include "accountmove.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "controlcommands.h"
#include "fixstate.h"
#include "machineguard.h"
#include "settingscommand.h"
#include "syncconfig.h"

namespace {

constexpr int kSystemctlTimeoutMs = 15000;

// The state entries of one account (spec 12, "Where an account's files
// live"), in the order they move. conflicts.json is not here: it is a cache,
// deleted instead of moved.
const QStringList &stateEntries() {
    static const QStringList entries{"paused.json",         "fix.json",          "fix.lock",
                                     "fix-answers.json",    "fix-dry-run.log",   "first-setup.json",
                                     "settings-change.json", "allow-deletes.json", "allowed-dry-run.log",
                                     "allowed-run.json",    "allowed-backup",    "unconfirmed-deletes.json",
                                     "notify-run.json",     "notify-watchdog.json", "identity-hold.json"};
    return entries;
}

using PathMap = QList<QPair<QString, QString>>;

// Whether anything sits at path, a dangling link included: a rename onto it
// would fail just the same.
bool present(const QString &path) {
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0;
}

// A rename that never overwrites: a file already at the target means the
// state is not what the plan saw, and replacing it would lose it.
bool renameNoReplace(const QString &from, const QString &to) {
    return ::renameat2(AT_FDCWD, QFile::encodeName(from).constData(), AT_FDCWD, QFile::encodeName(to).constData(),
                       RENAME_NOREPLACE)
           == 0;
}

bool isFixLock(const QString &path) { return QFileInfo(path).fileName() == "fix.lock"; }

// Whether another process holds the FixLock at appState, without creating
// the file or its folder: the check must touch nothing.
bool fixLockHeld(const QString &appState) {
    const int fd = ::open(QFile::encodeName(appState + "/fix.lock").constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    ::close(fd);
    return held;
}

// The FixLock an orphan journal's rollback holds, as the move holds its
// own: on fix.lock wherever it is now (back at the new place, else at the
// old one), opened without creating anything, since a file or folder the
// rollback did not expect would make it stuck. The renames keep the inode,
// and with it the lock. With no fix.lock at either place there is nothing a
// writer could hold: any writer that makes one finds the journal and
// refuses.
class OrphanFixLock {
public:
    explicit OrphanFixLock(const AccountRoots &roots) {
        const QString moved = newAccountPaths(roots, "account1").appState + "/fix.lock";
        const QString legacy = roots.stateRoot + "/fix.lock";
        m_path = present(moved) ? moved : legacy;
        m_fd = ::open(QFile::encodeName(m_path).constData(), O_RDONLY | O_CLOEXEC);
        if (m_fd < 0)
            return;
        m_held = ::flock(m_fd, LOCK_EX | LOCK_NB) == 0;
    }
    ~OrphanFixLock() {
        if (m_fd >= 0)
            ::close(m_fd);  // closing the descriptor releases the flock
    }
    OrphanFixLock(const OrphanFixLock &) = delete;
    OrphanFixLock &operator=(const OrphanFixLock &) = delete;
    bool held() const { return m_fd < 0 || m_held; }
    QString path() const { return m_path; }

private:
    QString m_path;
    int m_fd = -1;
    bool m_held = false;
};

bool busy(const QString &state) {
    return state == "active" || state == "activating" || state == "deactivating" || state == "reloading";
}

bool systemctl(const TakeoverPaths &tools, const QStringList &arguments, QString *error) {
    return runProcess(tools.systemctl, QStringList{"--user"} + arguments, kSystemctlTimeoutMs, nullptr, error);
}

// One systemctl show of units, mapped by Id; nullopt with *error when it
// fails or leaves one of them out.
std::optional<QHash<QString, QHash<QString, QString>>> showUnits(const TakeoverPaths &tools, const QStringList &units,
                                                                 const QString &properties, QString *error) {
    QByteArray output;
    if (!runProcess(tools.systemctl, QStringList{"--user", "show", "-p", properties} + units, kSystemctlTimeoutMs,
                    &output, error)) {
        *error = "systemctl show failed: " + *error;
        return std::nullopt;
    }
    const auto byId = Backend::parseShowById(output);
    for (const QString &unit : units)
        if (!byId.contains(unit)) {
            *error = QString("systemctl show did not describe %1").arg(unit);
            return std::nullopt;
        }
    return byId;
}

// The first of units that is running or about to, as "<unit> is <state>".
QString busyUnit(const TakeoverPaths &tools, const QStringList &units) {
    QString error;
    const auto byId = showUnits(tools, units, "Id,ActiveState", &error);
    if (!byId)
        return error;
    for (const QString &unit : units)
        if (busy(byId->value(unit).value("ActiveState")))
            return QString("%1 is %2; the move waits for it to end")
                .arg(unit, byId->value(unit).value("ActiveState"));
    return QString();
}

// ---- The paths inside the moved JSON files ----

// value, or where the map moves it: a key of the map itself or a path inside
// one. Exact pairs only, never a common prefix, so <stateRoot>/bisync (the
// work dir a first setup names, which stays) is never touched.
QString mapped(const QString &value, const PathMap &map, bool *changed) {
    for (const auto &[from, to] : map)
        if (value == from || value.startsWith(from + "/")) {
            *changed = true;
            return to + value.mid(from.size());
        }
    return value;
}

QJsonValue mapped(const QJsonValue &value, const PathMap &map, bool *changed) {
    if (value.isString())
        return mapped(value.toString(), map, changed);
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue &item : value.toArray())
            array.append(mapped(item, map, changed));
        return array;
    }
    if (value.isObject()) {
        QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            it.value() = mapped(it.value(), map, changed);
        return object;
    }
    return value;
}

// The text of a JSON string's content, as QJsonDocument writes it.
QByteArray jsonEscaped(const QString &text) {
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return array.mid(2, array.size() - 4);
}

// Points the paths in a JSON file at their new place, as rewriteLegacyPaths
// does for the old app names. The file keeps its layout (compact or
// indented): the strings are replaced in its text, and that text is kept
// only when it reads back as exactly the rewritten document; otherwise the
// document is written again, indented. Keeping the bytes lets a rollback
// give back the very file it found. A file that names no moved path, or is
// not JSON, is left alone. Empty, or the reason.
QString rewritePaths(const QString &path, const PathMap &map) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString("cannot read %1").arg(path);
    const QByteArray original = file.readAll();
    file.close();
    const QJsonDocument document = QJsonDocument::fromJson(original);
    if (!document.isObject() && !document.isArray())
        return QString();
    bool changed = false;
    const QJsonValue value =
        mapped(document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array()), map, &changed);
    if (!changed)
        return QString();
    const QJsonDocument expected = value.isObject() ? QJsonDocument(value.toObject()) : QJsonDocument(value.toArray());
    QByteArray text = original;
    for (const auto &[from, to] : map) {
        text.replace('"' + jsonEscaped(from) + '"', '"' + jsonEscaped(to) + '"');
        text.replace('"' + jsonEscaped(from) + '/', '"' + jsonEscaped(to) + '/');
    }
    const QByteArray data = QJsonDocument::fromJson(text) == expected ? text : expected.toJson(QJsonDocument::Indented);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
        return QString("cannot write %1").arg(path);
    return QString();
}

PathMap inverted(const PathMap &map) {
    PathMap inverse;
    for (const auto &[from, to] : map)
        inverse.append({to, from});
    return inverse;
}

// ---- The journal ----

struct Journal {
    PathMap moves;
    QStringList created;  // folders and files this move made, in order
    bool plainTimerEnabled = false;
    bool plainTimerActive = false;
    bool stuck = false;
    QString stuckReason;
};

bool writeJournal(const QString &path, const Journal &journal, QString *error) {
    QJsonArray moves;
    for (const auto &[from, to] : journal.moves)
        moves.append(QJsonObject{{"from", from}, {"to", to}});
    const QJsonObject object{{"version", 1},
                             {"account", "account1"},
                             {"moves", moves},
                             {"created", QJsonArray::fromStringList(journal.created)},
                             {"plainTimerEnabled", journal.plainTimerEnabled},
                             {"plainTimerActive", journal.plainTimerActive},
                             {"stuck", journal.stuck},
                             {"stuckReason", journal.stuckReason}};
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit())
        return true;
    *error = QString("cannot write %1").arg(path);
    return false;
}

std::optional<Journal> readJournal(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("cannot read %1").arg(path);
        return std::nullopt;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("version").toInt() != 1 || !object.value("moves").isArray()) {
        *error = QString("%1 is not a journal DriveSync can read").arg(path);
        return std::nullopt;
    }
    Journal journal;
    for (const QJsonValue &move : object.value("moves").toArray()) {
        const QString from = move.toObject().value("from").toString();
        const QString to = move.toObject().value("to").toString();
        if (from.isEmpty() || to.isEmpty()) {
            *error = QString("%1 lists a move with no path").arg(path);
            return std::nullopt;
        }
        journal.moves.append({from, to});
    }
    for (const QJsonValue &created : object.value("created").toArray())
        journal.created << created.toString();
    journal.plainTimerEnabled = object.value("plainTimerEnabled").toBool();
    journal.plainTimerActive = object.value("plainTimerActive").toBool();
    journal.stuck = object.value("stuck").toBool();
    journal.stuckReason = object.value("stuckReason").toString();
    return journal;
}

// ---- The plain timer a rollback could not put back ----

// <stateRoot>/plain-timer.json: what the plain timer was before a move whose
// rollback could not turn it back on. The journal goes all the same (legacy
// mode, timer off, which the watchdog reports), and the next move would
// otherwise read the timer as off and leave the instance timer off too.
// It stays in the state root, outside the moved entries, until a move or a
// rollback puts the timer back as it records.
QString plainTimerRecordPath(const QString &stateRoot) { return stateRoot + "/plain-timer.json"; }

void readPlainTimerRecord(const QString &stateRoot, bool *enabled, bool *active) {
    QFile file(plainTimerRecordPath(stateRoot));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    *enabled = *enabled || object.value("enabled").toBool();
    *active = *active || object.value("active").toBool();
}

void writePlainTimerRecord(const QString &stateRoot, bool enabled, bool active) {
    const QByteArray data =
        QJsonDocument(QJsonObject{{"version", 1}, {"enabled", enabled}, {"active", active}}).toJson(QJsonDocument::Indented);
    QSaveFile out(plainTimerRecordPath(stateRoot));
    if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size())
        out.commit();
}

// ---- The rollback ----

// Whether the instance timer may still fire: systemd refuses to disable a
// unit it cannot find, and such a unit runs nothing.
bool instanceTimerMayRun(const TakeoverPaths &tools, const QString &timer) {
    QString error;
    const auto byId = showUnits(tools, {timer}, "Id,LoadState,ActiveState,UnitFileState", &error);
    if (!byId)
        return true;
    const QHash<QString, QString> &unit = byId->value(timer);
    return unit.value("LoadState") != "not-found"
           && (unit.value("UnitFileState").startsWith("enabled") || busy(unit.value("ActiveState")));
}

// Stops the rollback where it is: the journal stays, marked stuck with the
// reason, and no timer is started, because a half-moved state must not run.
MoveOutcome stuckAt(const QString &journalPath, Journal journal, const QString &reason) {
    journal.stuck = true;
    journal.stuckReason = reason;
    QString ignored;
    writeJournal(journalPath, journal, &ignored);
    return {"stuck", reason};
}

// Undoes a move from its journal, for a failure in this run or for a crash
// in an earlier one: the same steps either way, using only the journal's
// paths. Back to legacy mode it puts the plain timer back as the journal
// recorded it and removes the journal; when it cannot get back there, it
// stops (stuckAt).
MoveOutcome rollBack(const AccountRoots &roots, const TakeoverPaths &tools, const QString &journalPath,
                     const QString &why) {
    const BackendPaths target = newAccountPaths(roots, "account1");
    QString error;
    const std::optional<Journal> read = readJournal(journalPath, &error);
    if (!read) {
        systemctl(tools, {"disable", "--now", target.timerUnit}, &error);
        return {"stuck", error};
    }
    Journal journal = *read;
    // The instance timer goes off first: from here on only the plain one may
    // come back, so there is never a second scheduler.
    if (!systemctl(tools, {"disable", "--now", target.timerUnit}, &error) && instanceTimerMayRun(tools, target.timerUnit))
        return stuckAt(journalPath, journal, QString("cannot turn off %1: %2").arg(target.timerUnit, error));
    for (auto it = journal.moves.crbegin(); it != journal.moves.crend(); ++it) {
        const QString &from = it->first;
        const QString &to = it->second;
        const bool atTarget = present(to);
        const bool atSource = present(from);
        if (!atTarget && atSource)
            continue;  // never moved
        if (!atTarget)
            return stuckAt(journalPath, journal, QString("%1 is neither at %2 nor at %3").arg(QFileInfo(from).fileName(), from, to));
        if (atSource) {
            // fix.lock carries no data: one a writer made again at the old
            // place is the same file, empty.
            if (!isFixLock(from) || !QFile::remove(from))
                return stuckAt(journalPath, journal,
                               QString("%1 and %2 both exist, and DriveSync cannot tell which one to keep").arg(from, to));
        }
        QDir().mkpath(QFileInfo(from).absolutePath());
        if (!renameNoReplace(to, from))
            return stuckAt(journalPath, journal, QString("cannot move %1 back to %2").arg(to, from));
    }
    const PathMap inverse = inverted(journal.moves);
    for (const auto &[from, to] : journal.moves) {
        if (!from.endsWith(".json") || !QFileInfo(from).isFile())
            continue;
        if (const QString problem = rewritePaths(from, inverse); !problem.isEmpty())
            return stuckAt(journalPath, journal, problem);
    }
    for (auto it = journal.created.crbegin(); it != journal.created.crend(); ++it) {
        if (QFileInfo(*it).isDir())
            QDir().rmdir(*it);  // never recursive: a folder that holds anything stays
        else
            QFile::remove(*it);
    }
    if (readAccountLayout(roots.configRoot).mode != "legacy")
        return stuckAt(journalPath, journal,
                       QString("%1 is still there, so DriveSync cannot go back to the single config")
                           .arg(roots.configRoot + "/accounts"));
    // A failed reload only means systemd keeps the drop-ins it read last,
    // which are these again; it never stops the plain timer coming back.
    systemctl(tools, {"daemon-reload"}, &error);
    QStringList problems;
    const BackendPaths plain = noAccountPaths(roots);
    if (journal.plainTimerEnabled && !systemctl(tools, {"enable", plain.timerUnit}, &error))
        problems << error;
    if (journal.plainTimerActive && !systemctl(tools, {"start", plain.timerUnit}, &error))
        problems << error;
    // Back in legacy mode even when the timer would not come back: the
    // watchdog says the timer is off, and a journal would hold every tick.
    // What the timer was is kept for the next move.
    if (problems.isEmpty())
        QFile::remove(plainTimerRecordPath(roots.stateRoot));
    else
        writePlainTimerRecord(roots.stateRoot, journal.plainTimerEnabled, journal.plainTimerActive);
    if (!QFile::remove(journalPath))
        return stuckAt(journalPath, journal, QString("cannot remove %1").arg(journalPath));
    QString reason = why.isEmpty() ? QString("the move stopped halfway earlier and was undone") : why;
    if (!problems.isEmpty())
        reason += QString("; %1 could not be put back: %2").arg(plain.timerUnit, problems.join("; "));
    return {"rolledBack", reason};
}

}  // namespace

QList<QPair<QString, QString>> plannedMoves(const AccountRoots &roots, const BackendPaths &legacy) {
    const BackendPaths target = newAccountPaths(roots, "account1");
    PathMap moves;
    for (const QString &entry : stateEntries())
        if (present(legacy.appState + "/" + entry))
            moves.append({legacy.appState + "/" + entry, target.appState + "/" + entry});
    const QString dropIn = intervalDropInPath(roots.userUnits, legacy.timerUnit);
    if (present(dropIn))
        moves.append({dropIn, intervalDropInPath(roots.userUnits, target.timerUnit)});
    moves.append({legacy.config, target.config});
    return moves;
}

QString moveBlocker(const AccountRoots &roots, const TakeoverPaths &tools) {
    const AccountSet set = resolveAccounts(roots);
    if (set.mode != "legacy" || set.accounts.isEmpty())
        return "DriveSync already keeps each account under accounts/";
    const BackendPaths &legacy = set.accounts.first();
    const QString journal = migrationJournalPath(roots.stateRoot);
    if (present(journal))
        return QString("%1 is there: the next move rolls it back first").arg(journal);
    QString error;
    const auto config = loadSyncConfig(legacy.config, &error);
    if (!config)
        return error;
    // The guard keeps the move from acting in the wrong home.
    if (const QString refusal = machineGuardRefusal(legacy, *config); !refusal.isEmpty())
        return refusal;
    if (takeoverLockHeld(roots.stateRoot, roots.base.proc))
        return "another takeover, undo or move to accounts is running";
    if (fixLockHeld(legacy.appState))
        return QString("%1 is held: a fix step or a change is being written").arg(legacy.appState + "/fix.lock");
    for (const auto &[from, to] : plannedMoves(roots, legacy))
        if (!isFixLock(to) && present(to))
            return QString("%1 already exists").arg(to);
    if (present(machineGuardPath(roots.configRoot)))
        return QString("%1 already exists").arg(machineGuardPath(roots.configRoot));

    const BackendPaths target = newAccountPaths(roots, "account1");
    if (const QString units = checkPackagedUnits(tools, accountUnits(target), QString()); !units.isEmpty())
        return units;
    if (const QStringList foreign = foreignDropIns(tools, target.id); !foreign.isEmpty())
        return "a drop-in would reach account1's units: " + foreign.join(", ");
    const QStringList plain = accountUnits(legacy);
    const auto byId = showUnits(tools, plain, "Id,LoadState,ActiveState,UnitFileState,FragmentPath", &error);
    if (!byId)
        return error;
    if (const OldDriveSync old = findOldDriveSync(tools, *byId); !old.found.isEmpty())
        return "an old drive-sync is here: " + old.found.join(", ") + "; remove it by hand";
    for (const QString &unit : {legacy.serviceUnit, legacy.fixUnit})
        if (busy(byId->value(unit).value("ActiveState")))
            return QString("%1 is %2; the move waits for it to end").arg(unit, byId->value(unit).value("ActiveState"));
    return QString();
}

MoveOutcome moveToAccounts(const AccountRoots &roots, const TakeoverPaths &tools) {
    const QString journalPath = migrationJournalPath(roots.stateRoot);
    // Checked against the roots' proc before the lock is taken: a journal a
    // live drive-sync is working on is its move, never one to roll back.
    if (takeoverLockHeld(roots.stateRoot, roots.base.proc))
        return {"deferred", "another takeover, undo or move to accounts is running"};
    QString error;
    // A journal nobody works on is a move that stopped halfway. It is undone
    // before anything else, in either mode: a crash after config.json moved
    // left accounts mode, where every tick skips while the journal is there.
    if (present(journalPath)) {
        if (!takeTakeoverLock(roots.stateRoot, &error))
            return {"deferred", error};
        const auto unlock = qScopeGuard([&] { releaseTakeoverLock(roots.stateRoot); });
        const OrphanFixLock fixLock(roots);
        if (!fixLock.held())
            return {"deferred", QString("%1 is held: a fix step or a change is being written").arg(fixLock.path())};
        return rollBack(roots, tools, journalPath, QString());
    }
    if (readAccountLayout(roots.configRoot).mode != "legacy")
        return {"notNeeded", "DriveSync already keeps each account under accounts/"};

    // Step 1: everything checked, nothing touched.
    if (const QString blocker = moveBlocker(roots, tools); !blocker.isEmpty())
        return {"deferred", blocker};
    if (!takeTakeoverLock(roots.stateRoot, &error))
        return {"deferred", error};
    const auto unlock = qScopeGuard([&] { releaseTakeoverLock(roots.stateRoot); });
    const AccountSet set = resolveAccounts(roots);
    if (set.mode != "legacy" || set.accounts.isEmpty())
        return {"deferred", "the single config changed while the move started"};
    const BackendPaths legacy = set.accounts.first();
    // Held to the end: a writer that waits on it finds the journal, or the
    // moved paths, once it gets it.
    const FixLock fixLock(legacy.appState, false);
    if (!fixLock.held())
        return {"deferred", QString("%1 is held: a fix step or a change is being written").arg(legacy.appState + "/fix.lock")};
    const auto config = loadSyncConfig(legacy.config, &error);
    if (!config)
        return {"deferred", error};
    const BackendPaths target = newAccountPaths(roots, "account1");
    Journal journal;
    // Planned with the lock held, so fix.lock is one of the moves.
    journal.moves = plannedMoves(roots, legacy);
    const auto plain = showUnits(tools, {legacy.timerUnit}, "Id,ActiveState,UnitFileState", &error);
    if (!plain)
        return {"deferred", error};
    journal.plainTimerEnabled = plain->value(legacy.timerUnit).value("UnitFileState") == "enabled";
    journal.plainTimerActive = plain->value(legacy.timerUnit).value("ActiveState") == "active";
    // A rollback that could not turn the plain timer back on kept what it was.
    readPlainTimerRecord(roots.stateRoot, &journal.plainTimerEnabled, &journal.plainTimerActive);

    // Step 2: the journal. From here every run-condition skips, and any
    // failure, or a crash, is undone from it.
    if (!writeJournal(journalPath, journal, &error))
        return {"deferred", error};
    const auto fail = [&](const QString &why) { return rollBack(roots, tools, journalPath, why); };
    const auto create = [&](const QString &path) {
        if (present(path))
            return true;
        journal.created << path;
        return writeJournal(journalPath, journal, &error) && QDir().mkdir(path);
    };

    // Step 3: the plain timer off (the watchdog's is left alone).
    if (!systemctl(tools, {"disable", "--now", legacy.timerUnit}, &error))
        return fail(QString("cannot turn off %1: %2").arg(legacy.timerUnit, error));
    // Step 4: a tick may have fired between step 1 and step 3.
    if (const QString running = busyUnit(tools, {legacy.serviceUnit, legacy.fixUnit}); !running.isEmpty()) {
        const MoveOutcome undone = fail(running);
        return {undone.result == "rolledBack" ? QString("deferred") : undone.result, undone.reason};
    }

    // Step 5: the state files, the drop-in, machine.json, then config.json.
    if (!create(roots.stateRoot + "/accounts") || !create(target.appState))
        return fail(QString("cannot make %1").arg(target.appState));
    QString movedDropIn;
    for (const auto &[from, to] : journal.moves) {
        if (from == legacy.config)
            continue;
        if (from.startsWith(legacy.appState + "/")) {
            // An empty fix.lock left at the target by a writer that waited
            // there during an earlier rollback carries nothing.
            if (isFixLock(to) && present(to))
                QFile::remove(to);
        } else {
            if (!create(QFileInfo(to).absolutePath()))
                return fail(QString("cannot make %1").arg(QFileInfo(to).absolutePath()));
            movedDropIn = to;
        }
        if (!renameNoReplace(from, to))
            return fail(QString("cannot move %1 to %2").arg(from, to));
    }
    if (!movedDropIn.isEmpty())
        QDir().rmdir(QFileInfo(intervalDropInPath(roots.userUnits, legacy.timerUnit)).absolutePath());
    for (const auto &[from, to] : journal.moves) {
        if (from == legacy.config || !to.endsWith(".json") || !QFileInfo(to).isFile())
            continue;
        if (const QString problem = rewritePaths(to, journal.moves); !problem.isEmpty())
            return fail(problem);
    }
    if (!config->runOnlyWhen.isEmpty()) {
        const QString machine = machineGuardPath(roots.configRoot);
        journal.created << machine;
        if (!writeJournal(journalPath, journal, &error) || !writeMachineGuard(machine, config->runOnlyWhen, &error))
            return fail(error);
    }
    const QString accountConfigDir = QFileInfo(target.config).absolutePath();
    if (!create(roots.configRoot + "/accounts") || !create(accountConfigDir))
        return fail(QString("cannot make %1").arg(accountConfigDir));
    if (!renameNoReplace(legacy.config, target.config))
        return fail(QString("cannot move %1 to %2").arg(legacy.config, target.config));

    // Step 6: systemd reads the moved drop-in, and runs the packaged units.
    if (!systemctl(tools, {"daemon-reload"}, &error))
        return fail("systemctl daemon-reload failed: " + error);
    if (const QString units = checkPackagedUnits(tools, accountUnits(target), movedDropIn); !units.isEmpty())
        return fail(units);
    // Step 7: the instance timer as the plain one was.
    if (journal.plainTimerEnabled && !systemctl(tools, {"enable", target.timerUnit}, &error))
        return fail(QString("cannot enable %1: %2").arg(target.timerUnit, error));
    if (journal.plainTimerActive && !systemctl(tools, {"start", target.timerUnit}, &error))
        return fail(QString("cannot start %1: %2").arg(target.timerUnit, error));

    // Step 8. The old conflict cache goes only now, once the move holds, so
    // a rollback keeps it; nothing reads that place in accounts mode.
    QFile::remove(legacy.appState + "/conflicts.json");
    if (!QFile::remove(journalPath))
        return fail(QString("cannot remove %1").arg(journalPath));
    QFile::remove(plainTimerRecordPath(roots.stateRoot));
    return {"moved", QString()};
}

int runAccountsCommand(const QStringList &arguments, const AccountRoots &roots, const TakeoverPaths &tools,
                       QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    const bool migrate = rest.removeAll("--migrate") > 0;
    if (!rest.isEmpty())
        return commandAnswer(json, "usage: drive-sync accounts [--migrate] [--json]", out, err);
    std::optional<MoveOutcome> move;
    if (migrate)
        move = moveToAccounts(roots, tools);
    const bool failed = move && move->result != "moved" && move->result != "notNeeded";
    const AccountSet set = resolveAccounts(roots);
    const QString blocker = moveBlocker(roots, tools);

    if (json) {
        QJsonArray accounts;
        for (const BackendPaths &account : set.accounts)
            accounts.append(QJsonObject{{"id", account.id},
                                        {"label", accountLabel(account, set.accounts, QDir::homePath())},
                                        {"folder", account.drive},
                                        {"configError", accountConfigError(account)}});
        QJsonArray notices;
        for (const AccountNotice &notice : set.notices)
            notices.append(QJsonObject{{"kind", notice.kind}, {"path", notice.path}});
        QJsonObject object{{"ok", !failed},
                           {"error", failed ? move->reason : QString()},
                           {"mode", set.mode},
                           {"accounts", accounts},
                           {"moveBlocker", blocker},
                           {"notices", notices}};
        if (move)
            object.insert("move", QJsonObject{{"result", move->result}, {"reason", move->reason}});
        out << QJsonDocument(object).toJson(QJsonDocument::Compact) << "\n";
        out.flush();
        return failed ? 1 : 0;
    }
    out << "mode: " << set.mode << "\n";
    for (const BackendPaths &account : set.accounts)
        out << account.id << "  " << accountLabel(account, set.accounts, QDir::homePath()) << "  " << account.drive
            << "\n";
    for (const AccountNotice &notice : set.notices)
        out << notice.kind << ": " << notice.path << "\n";
    if (move)
        out << "move: " << move->result << (move->reason.isEmpty() ? QString() : ": " + move->reason) << "\n";
    else if (set.mode == "legacy")
        out << "move: " << (blocker.isEmpty() ? QString("ready") : blocker) << "\n";
    out.flush();
    return failed ? 1 : 0;
}
