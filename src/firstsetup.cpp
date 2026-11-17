#include "firstsetup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "controlcommands.h"
#include "filterrules.h"
#include "fixcommand.h"
#include "fixstate.h"
#include "selectivesync.h"
#include "settingscommand.h"
#include "signin.h"
#include "snapshot.h"

namespace {

constexpr int kToolTimeoutMs = 60 * 1000;
const QStringList kTimers{"drive-sync.timer", "drive-sync-watchdog.timer"};

// The folder as bisync will see it: absolute, with no "." or ".." and no
// trailing slash. The listings are named after this string, so it must not
// change between runs.
QString cleanFolder(const QString &folder) { return QDir::cleanPath(folder); }

// "rclone v1.75.2" (or "v1.76.0-DEV") on the first line of "rclone version".
QString rcloneVersionRefusal(const QString &rclone) {
    QByteArray output;
    QString error;
    if (!runProcess(rclone, {"version"}, kToolTimeoutMs, &output, &error))
        return QString("cannot run rclone: %1").arg(error);
    const QString first = QString::fromUtf8(output).section('\n', 0, 0).trimmed();
    const QRegularExpressionMatch match = QRegularExpression(R"(^rclone v(\d+)\.(\d+)\S*)").match(first);
    if (!match.hasMatch())
        return QString("cannot tell rclone's version from \"%1\"").arg(first.left(80));
    const int major = match.captured(1).toInt();
    const int minor = match.captured(2).toInt();
    if (major > kMinRcloneMajor || (major == kMinRcloneMajor && minor >= kMinRcloneMinor))
        return QString();
    return QString("rclone %1 is too old: DriveSync needs %2.%3 or newer")
        .arg(match.captured(0).mid(7))
        .arg(kMinRcloneMajor)
        .arg(kMinRcloneMinor);
}

QString remoteRefusal(const QString &rclone) {
    QByteArray output;
    QString error;
    if (!runProcess(rclone, {"listremotes"}, kToolTimeoutMs, &output, &error))
        return QString("rclone listremotes failed: %1").arg(error);
    if (QString::fromUtf8(output).split('\n').contains(QString(kNewRemoteName) + ":"))
        return QString();
    return QString("rclone has no %1 remote yet; sign in to Google first (drive-sync signin)").arg(kNewRemoteName);
}

// What this run made, so a failure before the fix starts takes it all back.
struct Written {
    QStringList files;
    QStringList folders;  // newest last; removed only while empty
};

void rollBack(const Written &written) {
    for (const QString &file : written.files)
        QFile::remove(file);
    for (auto it = written.folders.crbegin(); it != written.folders.crend(); ++it)
        QDir().rmdir(*it);  // never recursive: a folder that holds anything stays
}

// mkpath, remembering every level it creates.
bool makeFolder(const QString &path, Written *written) {
    QStringList missing;
    for (QString level = path; !level.isEmpty() && !QFileInfo::exists(level); level = QFileInfo(level).path()) {
        missing.prepend(level);
        if (level == QFileInfo(level).path())
            break;
    }
    if (!QDir().mkpath(path))
        return false;
    written->folders << missing;
    return true;
}

bool writeNewFile(const QString &path, const QByteArray &data, QString *error) {
    QFile file(path);
    // NewOnly: never over a file that appeared since the refusal check.
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = QString("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    // QFile buffers: a full disk shows up at flush, not at write. A file cut
    // short is removed, so no half of it is ever taken for the filters.
    const bool written = file.write(data) == data.size() && file.flush();
    if (!written) {
        *error = QString("cannot write %1: %2").arg(path, file.errorString());
        file.close();
        QFile::remove(path);
        return false;
    }
    file.close();
    return true;
}

// The mark of what this setup wrote: cancelling its fix before any base
// undoes exactly these, and nothing the user had before.
bool writeSetupMark(const QString &appState, const QString &config, const QString &filters, const Written &written) {
    QSaveFile file(firstSetupMarkPath(appState));
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1},
                                                      {"config", config},
                                                      {"filters", filters},
                                                      {"createdFolders", QJsonArray::fromStringList(written.folders)}})
                                .toJson();
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

QString setUp(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
              const QStringList &skipped, const std::optional<QByteArray> &imported) {
    if (const QString refusal = setupRefusal(paths, takeover, folder, takeover.rclone); !refusal.isEmpty())
        return refusal;
    // Filters the user brought from an earlier sync (the setup dialog's
    // import): rclone must read every rule before anything is written, or the
    // first dry run would fail with a config already in place.
    if (imported) {
        if (const QString refusal = filtersRefusal(takeover.rclone, *imported); !refusal.isEmpty())
            return refusal;
    }
    // The cloud folders the user left out in the setup dialog: the first
    // dry run already leaves them in the cloud.
    QString skipError;
    const std::optional<QByteArray> filters = withSkippedFolders(imported.value_or(defaultFilters()), skipped, &skipError);
    if (!filters)
        return skipError;
    // newUserConfig's paths, taken from the ones given: in production they
    // are the same, and tests keep every file inside their temp dir.
    SyncConfig config = newUserConfig(cleanFolder(folder), QString());
    config.workDir = paths.appState + "/bisync";
    config.filtersFile = takeover.appConfigDir + "/filters.txt";
    config.logFile = paths.log;

    Written written;
    QString error;
    if (!makeFolder(config.localPath, &written))
        error = QString("cannot create %1").arg(config.localPath);
    else if (!makeFolder(config.workDir, &written))
        error = QString("cannot create %1").arg(config.workDir);
    else if (!makeFolder(takeover.appConfigDir, &written) || !makeFolder(QFileInfo(paths.config).path(), &written))
        error = QString("cannot create %1").arg(takeover.appConfigDir);
    else if (writeNewFile(config.filtersFile, *filters, &error)) {
        written.files << config.filtersFile;
        if (saveSyncConfig(paths.config, config, &error)) {
            written.files << paths.config;
            if (!writeSetupMark(paths.appState, paths.config, config.filtersFile, written)) {
                error = QString("cannot write %1").arg(firstSetupMarkPath(paths.appState));
            } else {
                written.files << firstSetupMarkPath(paths.appState);
                BackendPaths managed = paths;
                managed.managed = true;
                managed.log = config.logFile;
                managed.drive = config.localPath;
                error = startFix(managed);
            }
        }
    }
    if (!error.isEmpty()) {
        rollBack(written);
        return error;
    }
    // From here the fix owns the state: its mark holds every tick, so the
    // timers can start, and a failure leaves everything as it is.
    if (!runProcess(paths.systemctl, QStringList{"--user", "enable", "--now"} + kTimers, kToolTimeoutMs, nullptr, &error))
        return QString("the setup started, but systemd did not turn on %1: %2; turn them on with: "
                       "systemctl --user enable --now %3")
            .arg(kTimers.join(" and "), error, kTimers.join(' '));
    return QString();
}

}  // namespace


SyncConfig newUserConfig(const QString &folder, const QString &home) {
    SyncConfig config;
    config.remote = QString(kNewRemoteName) + ":";
    config.localPath = folder;
    config.workDir = home + "/.local/state/drive-sync-app/bisync";
    config.filtersFile = home + "/.config/drive-sync/filters.txt";
    config.logFile = home + "/.local/state/drive-sync.log";
    config.rcloneFlags = newUserFlags();
    return config;
}

QString resolvedFolder(const QString &path) {
    QString existing = QDir::cleanPath(path);
    QString rest;
    while (!QFileInfo::exists(existing)) {
        rest = rest.isEmpty() ? QFileInfo(existing).fileName() : QFileInfo(existing).fileName() + "/" + rest;
        const QString up = QFileInfo(existing).path();
        if (up == existing)
            break;
        existing = up;
    }
    QString real = QFileInfo(existing).canonicalFilePath();
    if (real.isEmpty())
        real = existing;
    return rest.isEmpty() ? real : QDir::cleanPath(real + "/" + rest);
}

QString folderRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                      const QStringList &alsoOwn) {
    if (folder.isEmpty() || QDir::isRelativePath(folder))
        return QString("the folder must be an absolute path, not \"%1\"").arg(folder);
    const QString clean = cleanFolder(folder);
    // The path as written and the one it leads to: a link to the home or to
    // DriveSync's own folders must not pass for another folder.
    const QStringList names{clean, resolvedFolder(clean)};
    const QString home = QDir::cleanPath(QDir::homePath());
    const QStringList homes{home, resolvedFolder(home)};
    auto inside = [](const QString &path, const QString &folder) {
        return path.startsWith(folder.endsWith('/') ? folder : folder + "/");
    };
    // The home itself, or a folder that holds it, would sync every config
    // and cache of the session to the cloud.
    for (const QString &name : names)
        for (const QString &h : homes)
            if (name == h || inside(h, name))
                return QString("%1 holds the whole home folder; pick a folder inside it, such as ~/Drive").arg(clean);
    // A folder that holds DriveSync's or rclone's own files, or sits inside
    // them, would sync its own state, the token and the log it writes while
    // syncing; ~/.config and ~/.local/state hold every program's.
    const QString rcloneConfig = qEnvironmentVariable("RCLONE_CONFIG");
    const QStringList own = QStringList{paths.appState,
                                        paths.appState + "/bisync",
                                        QFileInfo(paths.log).absolutePath(),
                                        QFileInfo(paths.config).absolutePath(),
                                        takeover.appConfigDir,
                                        rcloneConfig.isEmpty() ? home + "/.config/rclone"
                                                               : QFileInfo(rcloneConfig).absolutePath(),
                                        home + "/.config",
                                        home + "/.local/state"}
                            + alsoOwn;
    for (const QString &path : own) {
        for (const QString &other : {QDir::cleanPath(path), resolvedFolder(path)})
            for (const QString &name : names)
                if (name == other || inside(name, other) || inside(other, name))
                    return QString("%1 holds or sits inside %2, where DriveSync, rclone or other programs keep their "
                                   "own files; pick a folder of its own, such as ~/Drive")
                        .arg(clean, QDir::cleanPath(path));
    }
    const QFileInfo info(clean);
    if (info.exists() && !info.isDir())
        return QString("%1 is not a folder").arg(clean);
    return QString();
}

QString setupRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                     const QString &rclone) {
    if (const QString refusal = folderRefusal(paths, takeover, folder); !refusal.isEmpty())
        return refusal;
    if (QFile::exists(paths.config))
        return QString("DriveSync already runs drive-sync (%1 exists)").arg(paths.config);
    if (fixPresent(paths.appState))
        return "a fix is under way; open DriveSync to follow it";
    if (takeoverLockHeld(paths.appState, paths.proc))
        return "a takeover is running";
    const TakeoverPlan plan = detectTakeover(takeover, Snapshot());
    if (plan.state == "found")
        return "an older drive-sync is set up here; take it over instead (T in DriveSync, or drive-sync takeover "
               "--apply), which keeps its listings";
    if (plan.state != "none")
        return QString("DriveSync already runs drive-sync (%1)").arg(plan.state);
    const QString filters = takeover.appConfigDir + "/filters.txt";
    if (QFile::exists(filters))
        return QString("%1 exists already, and setup never overwrites a filters file").arg(filters);
    if (const QString refusal = rcloneVersionRefusal(rclone); !refusal.isEmpty())
        return refusal;
    return remoteRefusal(rclone);
}

int runSetupCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                    QTextStream &out, QTextStream &err) {
    // Option by option, values as they are: a cloud folder named "--json"
    // is a folder.
    bool json = false;
    bool valid = true;
    QString folder, filtersFile;
    QStringList skipped;
    for (qsizetype i = 0; i < arguments.size() && valid; ++i) {
        const QString &argument = arguments.at(i);
        if (argument == "--json") {
            json = true;
        } else if ((argument == "--folder" && folder.isNull()) || argument == "--skip"
                   || (argument == "--filters" && filtersFile.isNull())) {
            valid = i + 1 < arguments.size();
            if (!valid)
                break;
            if (argument == "--folder")
                folder = arguments.at(++i);
            else if (argument == "--filters")
                filtersFile = arguments.at(++i);
            else
                skipped << arguments.at(++i);
        } else {
            valid = false;
        }
    }
    if (!valid || folder.isNull()) {
        err << "usage: drive-sync setup --folder <path> [--skip <cloud folder>]... [--filters <file>|-] [--json]\n";
        return 1;
    }
    // "-" is standard input: the window hands the imported rules over that
    // way, so no copy of them is left in a temporary file.
    std::optional<QByteArray> imported;
    if (!filtersFile.isNull()) {
        QFile in;
        const bool opened = filtersFile == "-" ? in.open(stdin, QIODevice::ReadOnly)
                                               : (in.setFileName(filtersFile), in.open(QIODevice::ReadOnly));
        if (!opened)
            return commandAnswer(json, QString("cannot read %1: %2").arg(filtersFile, in.errorString()), out, err);
        imported = in.readAll();
    }
    // One setup, fix command or unit step at a time: they all write fix.json.
    const FixLock lock(paths.appState);
    return commandAnswer(json, setUp(paths, takeover, folder, skipped, imported), out, err);
}
