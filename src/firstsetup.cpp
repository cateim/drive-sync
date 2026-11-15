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
#include "fixcommand.h"
#include "fixstate.h"
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

QString setUp(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder) {
    if (const QString refusal = setupRefusal(paths, takeover, folder, takeover.rclone); !refusal.isEmpty())
        return refusal;
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
    else if (writeNewFile(config.filtersFile, defaultFilters(), &error)) {
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

QByteArray defaultFilters() {
    return "# What DriveSync does not sync between your folder and Google Drive.\n"
           "# rclone bisync filter rules: \"- pattern\" leaves it out. A rule with no \"/\"\n"
           "# at the start applies in every folder.\n"
           "# bisync checks this file on every run: after editing it, open DriveSync and\n"
           "# press F, which rebuilds the base with the new rules. Runs wait until then.\n"
           "\n"
           "# ---- Git: the history lives in the remote repository ----\n"
           "- .git/**\n"
           "- .git\n"
           "\n"
           "# ---- Dependencies and environments, rebuilt with one command ----\n"
           "- node_modules/**\n"
           "- .venv/**\n"
           "- __pycache__/**\n"
           "- *.pyc\n"
           "\n"
           "# ---- Google Docs shortcuts: the document lives in the cloud ----\n"
           "- *.gdoc\n"
           "- *.gsheet\n"
           "- *.gslides\n"
           "- *.gdraw\n"
           "- *.gform\n"
           "- *.gscript\n"
           "- *.gsite\n"
           "- *.gmap\n"
           "- *.gjam\n"
           "- *.glink\n"
           "- *.gvid\n"
           "\n"
           "# ---- Windows, macOS, office suites and Drive for desktop ----\n"
           "- desktop.ini\n"
           "- Thumbs.db\n"
           "- .DS_Store\n"
           "- .~lock.*#\n"
           "- .tmp.drivedownload/**\n"
           "- .tmp.driveupload/**\n";
}

SyncConfig newUserConfig(const QString &folder, const QString &home) {
    SyncConfig config;
    config.remote = QString(kNewRemoteName) + ":";
    config.localPath = folder;
    config.workDir = home + "/.local/state/drive-sync-app/bisync";
    config.filtersFile = home + "/.config/drive-sync/filters.txt";
    config.logFile = home + "/.local/state/drive-sync.log";
    // The maintainer's sync proved these over months; --fast-list is added
    // by rcloneRunArguments, and the fix leaves the routine ones out of the
    // base run.
    config.rcloneFlags = {"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                          "--max-lock",         "2m",                     "--max-delete", "5",
                          "--conflict-resolve", "newer",                  "--log-level",  "INFO",
                          "--fast-list"};
    return config;
}

QString setupRefusal(const BackendPaths &paths, const TakeoverPaths &takeover, const QString &folder,
                     const QString &rclone) {
    if (folder.isEmpty() || QDir::isRelativePath(folder))
        return QString("the folder must be an absolute path, not \"%1\"").arg(folder);
    const QString clean = cleanFolder(folder);
    const QString home = QDir::cleanPath(QDir::homePath());
    // The home itself, or a folder that holds it, would sync every config
    // and cache of the session to the cloud.
    if (clean == home || home.startsWith(clean.endsWith('/') ? clean : clean + "/"))
        return QString("%1 holds the whole home folder; pick a folder inside it, such as ~/Drive").arg(clean);
    // A folder that holds DriveSync's or rclone's own files, or sits inside
    // them, would sync its own state, the token and the log it writes while
    // syncing; ~/.config and ~/.local/state hold every program's.
    const QString rcloneConfig = qEnvironmentVariable("RCLONE_CONFIG");
    const QStringList own = {paths.appState,
                             paths.appState + "/bisync",
                             QFileInfo(paths.log).absolutePath(),
                             QFileInfo(paths.config).absolutePath(),
                             takeover.appConfigDir,
                             rcloneConfig.isEmpty() ? home + "/.config/rclone" : QFileInfo(rcloneConfig).absolutePath(),
                             home + "/.config",
                             home + "/.local/state"};
    for (const QString &path : own) {
        const QString other = QDir::cleanPath(path);
        if (clean == other || clean.startsWith(other + "/") || other.startsWith(clean + "/"))
            return QString("%1 holds or sits inside %2, where DriveSync, rclone or other programs keep their own "
                           "files; pick a folder of its own, such as ~/Drive")
                .arg(clean, other);
    }
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
    const QFileInfo info(clean);
    if (info.exists() && !info.isDir())
        return QString("%1 is not a folder").arg(clean);
    if (const QString refusal = rcloneVersionRefusal(rclone); !refusal.isEmpty())
        return refusal;
    return remoteRefusal(rclone);
}

int runSetupCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                    QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    if (rest.size() != 2 || rest.first() != "--folder") {
        err << "usage: drive-sync setup --folder <path> [--json]\n";
        return 1;
    }
    // One setup, fix command or unit step at a time: they all write fix.json.
    const FixLock lock(paths.appState);
    return commandAnswer(json, setUp(paths, takeover, rest.at(1)), out, err);
}
