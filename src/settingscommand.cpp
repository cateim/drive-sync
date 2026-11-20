#include "settingscommand.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include "accounts.h"
#include "blockeddeletes.h"
#include "controlcommands.h"
#include "firstsetup.h"
#include "fixcommand.h"
#include "fixstate.h"
#include "selectivesync.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kSystemctlTimeoutMs = 15000;
constexpr int kRcloneCheckTimeoutMs = 30000;

const QString kNotManaged = "settings need DriveSync to run drive-sync; take it over first";
const QString kUsage = "usage: drive-sync settings [--interval N | --max-delete N | --folder PATH | --filters FILE|- | "
                       "--skip FOLDER... | --skip-none] [--json]";

// What a settings call answers: the error, and whether a fix now holds the
// runs (its dry run waits for the user in the window).
struct Answer {
    QString error;
    bool fixStarted = false;
};

int answerChange(bool json, const Answer &answer, QTextStream &out, QTextStream &err) {
    if (!json)
        return commandAnswer(false, answer.error, out, err);
    out << QJsonDocument(QJsonObject{{"ok", answer.error.isEmpty()},
                                     {"error", answer.error},
                                     {"fixStarted", answer.fixStarted}})
               .toJson(QJsonDocument::Compact)
        << "\n";
    out.flush();
    return answer.error.isEmpty() ? 0 : 1;
}

QByteArray readAll(const QString &path, bool *ok = nullptr) {
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    if (ok)
        *ok = opened;
    return opened ? file.readAll() : QByteArray();
}

bool writeAtomically(const QString &path, const QByteArray &data, QString *error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit())
        return true;
    *error = QString("cannot write %1").arg(path);
    return false;
}

// A whole number in [low, high], or nothing.
std::optional<int> wholeNumber(const QString &text, int low, int high) {
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < low || value > high)
        return std::nullopt;
    return value;
}

// The newest run that reached bisync stopped at the max-delete guard.
bool guardStopInLog(const SyncConfig &config) {
    bool truncated = false;
    const QString text = SyncLog::readRecent(config.logFile, kTailBytes, nullptr, &truncated);
    return guardStopHolds(SyncLog::parse(text, kStateRuns, truncated));
}

QJsonObject showSettings(const SyncConfig &config) {
    const QByteArray filters = readAll(config.filtersFile);
    QString skipError;
    const std::optional<QStringList> skipped = skippedFolders(filters, &skipError);
    return QJsonObject{{"ok", true},
                       {"error", QString()},
                       {"folder", config.localPath},
                       {"remote", config.remote},
                       {"intervalMinutes", config.intervalMinutes},
                       {"maxDelete", configuredMaxDelete(config.rcloneFlags)},
                       {"filtersFile", config.filtersFile},
                       {"filters", QString::fromUtf8(filters)},
                       {"skipped", QJsonArray::fromStringList(skipped.value_or(QStringList()))},
                       {"skipError", skipped ? QString() : skipError}};
}

QString setMaxDelete(const BackendPaths &paths, SyncConfig config, const QString &value) {
    const std::optional<int> percent = wholeNumber(value, 1, kMaxSettableMaxDelete);
    if (!percent)
        return QString("the max-delete guard is a whole percent from 1 to %1, not \"%2\"")
            .arg(kMaxSettableMaxDelete)
            .arg(value);
    // Spec 5b: past a stop, deletions go through only after the user saw the
    // list and typed its count. A looser guard would let them through unseen.
    if (*percent > configuredMaxDelete(config.rcloneFlags) && guardStopInLog(config))
        return "the last run stopped at the max-delete guard; review those deletions with D first, then raise the guard";
    config.rcloneFlags = withMaxDelete(config.rcloneFlags, *percent);
    QString error;
    saveSyncConfig(paths.config, config, &error);
    return error;
}

// systemctl --user <arguments>; its error, or empty.
QString systemctl(const BackendPaths &paths, const QStringList &arguments) {
    QString error;
    if (runProcess(paths.systemctl, QStringList{"--user"} + arguments, kSystemctlTimeoutMs, nullptr, &error))
        return QString();
    return error.isEmpty() ? QString("systemctl %1 failed").arg(arguments.join(' ')) : error;
}

QString setInterval(const BackendPaths &paths, const TakeoverPaths &takeover, SyncConfig config, const QString &value) {
    const std::optional<int> minutes = wholeNumber(value, kMinIntervalMinutes, kMaxIntervalMinutes);
    if (!minutes)
        return QString("the interval is a whole number of minutes from %1 to %2, not \"%3\"")
            .arg(kMinIntervalMinutes)
            .arg(kMaxIntervalMinutes)
            .arg(value);
    config.intervalMinutes = *minutes;
    QString error;
    if (!saveSyncConfig(paths.config, config, &error))
        return error;
    const QString dropIn = intervalDropInPath(takeover.userUnits, paths.timerUnit);
    if (*minutes == kDefaultIntervalMinutes) {
        // The packaged value needs no drop-in, and none keeps DropInPaths
        // empty, as a takeover expects.
        if (QFile::exists(dropIn) && !QFile::remove(dropIn))
            return QString("cannot remove %1").arg(dropIn);
        QDir().rmdir(QFileInfo(dropIn).absolutePath());  // only when empty
    } else if (!writeAtomically(dropIn, intervalDropIn(*minutes), &error)) {
        return error;
    }
    // The config and the drop-in stay when systemd says no: the next
    // daemon-reload (a login, a package upgrade) applies them.
    if (const QString problem = systemctl(paths, {"daemon-reload"}); !problem.isEmpty())
        return QString("the interval is saved, but systemd did not reload: %1").arg(problem);
    if (const QString problem = systemctl(paths, {"try-restart", paths.timerUnit}); !problem.isEmpty())
        return QString("the interval is saved, but %1 did not restart: %2").arg(paths.timerUnit, problem);
    return QString();
}

}  // namespace

// rclone's own reading of the filters, on an empty folder: a rule it cannot
// parse would stop every run, and the fix's dry run would fail on it after
// the old filters were already replaced.
QString filtersRefusal(const QString &rclone, const QByteArray &filters) {
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return "cannot make a temporary folder to check the filters";
    const QString file = scratch.filePath("filters.txt");
    QString error;
    if (!writeAtomically(file, filters, &error))
        return error;
    QDir().mkpath(scratch.filePath("empty"));
    if (runProcess(rclone, {"lsf", scratch.filePath("empty"), "--filter-from", file}, kRcloneCheckTimeoutMs, nullptr,
                   &error))
        return QString();
    // "CRITICAL: Failed to initialise global options: failed to reload
    // "filter" options: malformed rule ..." (rclone 1.75.2): the part after
    // the last "options: " names the rule.
    QString why = error.section('\n', -1).trimmed();
    if (const qsizetype at = why.lastIndexOf("options: "); at >= 0)
        why = why.mid(at + 9);
    return QString("rclone cannot read these filters: %1").arg(why.left(200));
}

namespace {

// A change bisync must rebuild its base for (spec 8): the old and the new
// bytes go into the pending mark, bound to the fix it starts, and the fix's
// preparing step writes the new ones once no run is under way
// (applySettingsChange in fixcommand.cpp). Nothing of the user's config or
// filters is written here. Bytes equal to the current ones start nothing:
// the .md5 covers every byte, and so does the base.
Answer changeThroughFix(const BackendPaths &paths, const SyncConfig &current, const std::optional<SyncConfig> &config,
                        const std::optional<QByteArray> &filters, const QStringList &createdFolders = {}) {
    bool configRead = false, filtersRead = false;
    const QByteArray oldConfig = readAll(paths.config, &configRead);
    const QByteArray oldFilters = readAll(current.filtersFile, &filtersRead);
    if (!configRead)
        return {QString("cannot read %1").arg(paths.config)};
    if (filters && !filtersRead)
        return {QString("cannot read %1").arg(current.filtersFile)};
    const bool filtersChange = filters && *filters != oldFilters;
    const bool configChange = config.has_value();
    if (!filtersChange && !configChange)
        return {};

    QJsonObject mark{{"version", 1},
                     {"config", paths.config},
                     {"filtersFile", current.filtersFile},
                     {"createdFolders", QJsonArray::fromStringList(createdFolders)},
                     {"applied", false}};
    if (configChange) {
        mark.insert("previousConfig", QString::fromLatin1(oldConfig.toBase64()));
        mark.insert("newConfig", QString::fromLatin1(syncConfigBytes(*config).toBase64()));
    }
    if (filtersChange) {
        mark.insert("previousFilters", QString::fromLatin1(oldFilters.toBase64()));
        mark.insert("newFilters", QString::fromLatin1(filters->toBase64()));
    }
    const QString markPath = settingsChangeMarkPath(paths.appState);

    // The mark goes with a unit that did not start; the folders made for it too.
    auto undo = [&] {
        QFile::remove(markPath);
        for (auto it = createdFolders.crbegin(); it != createdFolders.crend(); ++it)
            QDir().rmdir(*it);  // never recursive
    };
    auto apply = [&]() -> QString {
        // The fix's own start, so the mark serves this fix and no later one.
        mark.insert("fix", readFix(paths.appState).started.toSecsSinceEpoch());
        QString error;
        if (!writeAtomically(markPath, QJsonDocument(mark).toJson(), &error)) {
            undo();
            return error;
        }
        return QString();
    };
    const QString error = startFix(paths, apply, undo);
    if (!error.isEmpty()) {
        // startFix refused before apply ran: the folders made for it go.
        for (auto it = createdFolders.crbegin(); it != createdFolders.crend(); ++it)
            QDir().rmdir(*it);
        return {error};
    }
    return {QString(), true};
}

Answer setFilters(const BackendPaths &paths, const TakeoverPaths &takeover, const SyncConfig &config,
                  const QByteArray &filters) {
    if (filters == readAll(config.filtersFile))
        return {};
    if (const QString refusal = filtersRefusal(takeover.rclone, filters); !refusal.isEmpty())
        return {refusal};
    return changeThroughFix(paths, config, std::nullopt, filters);
}

// mkpath, returning every level it created, outermost first.
bool makeFolder(const QString &path, QStringList *created) {
    QStringList missing;
    for (QString level = path; !level.isEmpty() && !QFileInfo::exists(level); level = QFileInfo(level).path()) {
        missing.prepend(level);
        if (level == QFileInfo(level).path())
            break;
    }
    if (!QDir().mkpath(path))
        return false;
    *created = missing;
    return true;
}

bool inside(const QString &path, const QString &folder) { return path.startsWith(folder + "/"); }

// A new local folder means listings named after another path: a new base,
// built by the fix from the cloud and whatever the new folder holds. The old
// folder stays as it is, outside the sync.
Answer setFolder(const BackendPaths &paths, const TakeoverPaths &takeover, const SyncConfig &config,
                 const QString &folder) {
    if (const QString refusal = folderRefusal(paths, takeover, folder, {config.workDir}); !refusal.isEmpty())
        return {refusal};
    const QString clean = QDir::cleanPath(folder);
    const QString current = QDir::cleanPath(config.localPath);
    if (clean == current)
        return {};
    // The same folder under another name (a link) would change the listings'
    // name and rebuild the base for nothing.
    const QString real = resolvedFolder(clean);
    const QString currentReal = resolvedFolder(current);
    if (real == currentReal)
        return {QString("%1 is %2 under another name").arg(clean, current)};
    // Inside the old folder it would hold a copy of part of the cloud within
    // a folder the user still sees as the old sync; holding it, the old copy
    // would go up as a subfolder of the new one.
    for (const QString &name : {clean, real})
        for (const QString &old : {current, currentReal})
            if (inside(name, old) || inside(old, name))
                return {QString("%1 and %2 are one inside the other; pick a folder outside %2").arg(clean, current)};
    QStringList created;
    if (!makeFolder(clean, &created))
        return {QString("cannot create %1").arg(clean)};
    SyncConfig moved = config;
    moved.localPath = clean;
    return changeThroughFix(paths, config, moved, std::nullopt, created);
}

// Selective sync: the folders left out become the block at the top of the
// filters, a filters change like any other.
Answer setSkipped(const BackendPaths &paths, const SyncConfig &config, const QStringList &folders) {
    bool ok = false;
    const QByteArray filters = readAll(config.filtersFile, &ok);
    if (!ok)
        return {QString("cannot read %1").arg(config.filtersFile)};
    QString error;
    const std::optional<QByteArray> changed = withSkippedFolders(filters, folders, &error);
    if (!changed)
        return {error};
    if (*changed == filters)
        return {};
    return changeThroughFix(paths, config, std::nullopt, *changed);
}

}  // namespace

QString intervalDropInPath(const QString &userUnits, const QString &timerUnit) {
    return userUnits + "/" + timerUnit + ".d/drive-sync-interval.conf";
}

QByteArray intervalDropIn(int minutes) {
    return QString("# Written by DriveSync (drive-sync settings --interval). The empty\n"
                   "# OnUnitActiveSec= resets every timer of the packaged unit, so OnBootSec\n"
                   "# repeats the packaged value.\n"
                   "[Timer]\n"
                   "OnUnitActiveSec=\n"
                   "OnBootSec=5min\n"
                   "OnUnitActiveSec=%1min\n")
        .arg(minutes)
        .toUtf8();
}

int runSettingsCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                       QTextStream &out, QTextStream &err, QIODevice *input) {
    // Option by option: a value is taken as it is, so a folder named
    // "--json" or "--skip-none" is a folder.
    bool json = false;
    bool bad = false;
    QString action, value;
    QStringList skipped;
    static const QStringList withValue{"--interval", "--max-delete", "--folder", "--filters"};
    for (qsizetype i = 0; i < arguments.size() && !bad; ++i) {
        const QString &argument = arguments.at(i);
        if (argument == "--json") {
            json = true;
        } else if (withValue.contains(argument) || argument == "--skip") {
            const bool another = !action.isEmpty() && !(argument == "--skip" && action == "--skip");
            bad = another || i + 1 >= arguments.size();
            if (!bad) {
                action = argument;
                value = arguments.at(++i);
                if (argument == "--skip")
                    skipped << value;
            }
        } else if (argument == "--skip-none" && action.isEmpty()) {
            action = argument;
        } else {
            bad = true;
        }
    }
    if (bad)
        return answerChange(json, {kUsage}, out, err);

    QString error;
    if (action.isEmpty()) {
        const auto config = paths.managed ? loadSyncConfig(paths.config, &error) : std::nullopt;
        if (!config)
            return answerChange(json, {paths.managed ? error : kNotManaged}, out, err);
        if (json) {
            out << QJsonDocument(showSettings(*config)).toJson(QJsonDocument::Compact) << "\n";
        } else {
            const QJsonObject shown = showSettings(*config);
            for (const char *key : {"folder", "remote", "intervalMinutes", "maxDelete", "filtersFile"})
                out << key << ": " << shown.value(key).toVariant().toString() << "\n";
        }
        out.flush();
        return 0;
    }

    // One writer at a time: the fix, setup and the unit take this lock too,
    // and a change must not land between a fix's read and write. The config
    // is read under it, so a change starts from the config as it is now.
    // None while the move to account1 runs, and none over paths it changed.
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty())
        return answerChange(json, {lock.refusal()}, out, err);
    if (lock.stale())
        return kAccountPathsMoved;
    if (!paths.managed)
        return answerChange(json, {kNotManaged}, out, err);
    auto config = loadSyncConfig(paths.config, &error);
    if (!config)
        return answerChange(json, {error}, out, err);
    // In accounts mode the check is the machine's (machine.json), and one
    // left in an account's config would only mislead: every save drops it.
    if (paths.accountsMode)
        config->runOnlyWhen.clear();
    if (fixPresent(paths.appState))
        return answerChange(json, {"a fix is under way; change the settings when it ends"}, out, err);
    if (takeoverLockHeld(paths.globalState(), paths.proc))
        return answerChange(json, {"a takeover is running; change the settings when it ends"}, out, err);
    if (action == "--max-delete")
        return answerChange(json, {setMaxDelete(paths, *config, value)}, out, err);
    if (action == "--skip" || action == "--skip-none")
        return answerChange(json, setSkipped(paths, *config, skipped), out, err);
    if (action == "--folder")
        return answerChange(json, setFolder(paths, takeover, *config, value), out, err);
    if (action == "--filters") {
        // "-": the bytes come on stdin, so the window's child never depends
        // on a file of the window's that may go before it reads it.
        bool ok = false;
        QByteArray filters;
        if (value == "-") {
            QFile standardInput;
            QIODevice *source = input;
            if (!source && standardInput.open(stdin, QIODevice::ReadOnly))
                source = &standardInput;
            ok = source && source->isOpen();
            if (ok)
                filters = source->readAll();
        } else {
            filters = readAll(value, &ok);
        }
        if (!ok)
            return answerChange(json, {QString("cannot read the filters from %1").arg(value == "-" ? "stdin" : value)}, out,
                                err);
        return answerChange(json, setFilters(paths, takeover, *config, filters), out, err);
    }
    return answerChange(json, {setInterval(paths, takeover, *config, value)}, out, err);
}
