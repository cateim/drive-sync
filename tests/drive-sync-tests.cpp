#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>
#include <algorithm>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "accountlist.h"
#include "accountmove.h"
#include "accounts.h"
#include "backend.h"
#include "blockeddeletes.h"
#include "clockformat.h"
#include "conflictcache.h"
#include "controlcommands.h"
#include "legacymigration.h"
#include "fixcommand.h"
#include "firstsetup.h"
#include "filtereditor.h"
#include "filterrules.h"
#include "fixstate.h"
#include "listingdiff.h"
#include "localtree.h"
#include "machineguard.h"
#include "notifier.h"
#include "oauthclient.h"
#include "pausestate.h"
#include "pluginsetup.h"
#include "settingscommand.h"
#include "selectivesync.h"
#include "settingscontroller.h"
#include "foldertree.h"
#include "setupcontroller.h"
#include "signin.h"
#include "snapshot.h"
#include "statuscommand.h"
#include "syncconfig.h"
#include "synclog.h"
#include "takeover.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

namespace {

// One line the way rclone writes it: "2026/11/07 15:28:19 INFO  : message".
QString line(const QDateTime &when, const QString &level, const QString &message) {
    return QString("%1 %2: %3\n").arg(when.toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), message);
}

QString change(const QString &tag, const QString &what, const QString &path) {
    return QString("- %1%2 - %3").arg(tag.leftJustified(18), what.leftJustified(43), path);
}

// A normal run that copied one file each way, as drive-sync logs it.
QString successfulRun(const QDateTime &start, int seconds) {
    QString log;
    log += line(start, "INFO", "Bisyncing with Comparison Settings: ");
    log += "{\n\t\"Modtime\": true,\n}\n";
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "INFO", "Building Path1 and Path2 listings");
    log += line(start.addSecs(60), "INFO", "");
    log += "Transferred:   \t          0 B / 0 B, -, 0 B/s, ETA -\nChecks:             14560 / 14560, 100%, Listed 16680\nElapsed time:        59.9s\n\n";
    log += line(start.addSecs(seconds - 20), "INFO", "Path1 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path1", "File changed: size (larger), time (newer)", "Notes/todo.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path1:    1 changes:    0 new,    1 modified,    0 deleted");
    log += line(start.addSecs(seconds - 20), "INFO", "Path2 checking for diffs");
    log += line(start.addSecs(seconds - 20), "INFO", change("Path2", "File is new", "GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 20), "INFO", "Path2:    1 changes:    1 new,    0 modified,    0 deleted");
    log += line(start.addSecs(seconds - 19), "INFO", "Applying changes");
    log += line(start.addSecs(seconds - 19), "INFO", change("Path2", "Queue copy to Path1", "gdrive{x}:/GitHub/app/README.md"));
    log += line(start.addSecs(seconds - 2), "INFO", "Updating listings");
    log += line(start.addSecs(seconds), "INFO", "Bisync successful");
    return log;
}

// A run that --max-delete stopped, as rclone 1.75.2 writes it (tried
// 2026-11-12): one "File was deleted" line per file on each side, then the
// abort for the first side over 5% of total, Path1 checked first.
QString abortedRun(const QDateTime &start, const QStringList &cloudDeleted, const QStringList &notebookDeleted,
                   int total) {
    QString log;
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "INFO", "Building Path1 and Path2 listings");
    log += line(start, "INFO", "Path1 checking for diffs");
    for (const QString &path : cloudDeleted)
        log += line(start, "INFO", change("Path1", "File was deleted", path));
    log += line(start, "INFO", QString("Path1:    %1 changes:    0 new,    0 modified,    %1 deleted").arg(cloudDeleted.size()));
    log += line(start, "INFO", "Path2 checking for diffs");
    for (const QString &path : notebookDeleted)
        log += line(start, "INFO", change("Path2", "File was deleted", path));
    log += line(start, "INFO",
                QString("Path2:    %1 changes:    0 new,    0 modified,    %1 deleted").arg(notebookDeleted.size()));
    const bool cloudSide = cloudDeleted.size() * 100 > total * 5;
    log += line(start, "ERROR",
                QString(R"(Safety abort: too many deletes (>5%, %1 of %2) on %3 "%4". Run with --force if desired.)")
                    .arg(cloudSide ? cloudDeleted.size() : notebookDeleted.size())
                    .arg(total)
                    .arg(cloudSide ? "Path1" : "Path2", cloudSide ? "gdrive{x}:/" : "/home/someone/Drive/"));
    log += line(start, "NOTICE", "Bisync aborted. Please try again.");
    log += line(start, "NOTICE", "Failed to bisync: too many deletes");
    return log;
}

QStringList numberedFiles(const QString &pattern, int count) {
    QStringList files;
    for (int i = 1; i <= count; ++i)
        files << pattern.arg(i);
    return files;
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A home the way an earlier name of the app (SyncWatch, then drivesync) left it
// after a takeover: config with its filters and .md5, a pause mark, and one
// backup whose manifest names its own folder.
struct LegacyHome {
    QString home;
    QByteArray filters = "- .git/**\n- node_modules/**\n";
    QString name = "syncwatch";
};

bool makeLegacyHome(LegacyHome &legacy) {
    const QString config = legacy.home + "/.config/" + legacy.name;
    const QString backup = legacy.home + "/.local/share/" + legacy.name + "/taken-over/20261110-224919";
    if (!QDir().mkpath(config) || !QDir().mkpath(legacy.home + "/.local/state/" + legacy.name)
        || !QDir().mkpath(backup + "/bin"))
        return false;
    SyncConfig sync;
    sync.remote = "gdrive:";
    sync.localPath = legacy.home + "/Drive";
    sync.workDir = legacy.home + "/.local/state/rclone/bisync";
    sync.filtersFile = config + "/filters.txt";
    sync.logFile = legacy.home + "/.local/state/drive-sync.log";
    sync.rcloneFlags = {"--resilient"};
    QString error;
    const QJsonObject manifest{
        {"version", 1},
        {"moved", QJsonArray{QJsonObject{{"from", legacy.home + "/.local/bin/drive-sync.sh"},
                                         {"to", backup + "/bin/drive-sync.sh"}}}},
        {"undone", false}};
    return saveSyncConfig(config + "/config.json", sync, &error) && write(sync.filtersFile, legacy.filters)
           && write(sync.filtersFile + ".md5",
                    QCryptographicHash::hash(legacy.filters, QCryptographicHash::Md5).toHex())
           && write(legacy.home + "/.local/state/" + legacy.name + "/paused.json", "{}")
           && write(backup + "/bin/drive-sync.sh", "#!/bin/sh\n")
           && write(backup + "/manifest.json", QJsonDocument(manifest).toJson());
}

// A systemctl that answers like systemd with the service idle and the timer
// on: one show block per unit asked, starting with its Id, so readers that
// map the answers by Id find them; list-timers names each timer asked.
QString fakeSystemctl(const QString &dir, const QDateTime &serviceStart, const QString &serviceActive = "inactive",
                      const QString &fixActive = "inactive") {
    const QString path = dir + "/systemctl";
    const QString script = QString(R"sh(#!/bin/sh
case "$*" in
  *list-timers*)
    out=''
    for a in "$@"; do
      case "$a" in *.timer) [ -n "$out" ] && out="$out,"; out="$out"'{"next":%1,"unit":"'"$a"'"}' ;; esac
    done
    echo "[$out]" ;;
  *)
    first=1
    for a in "$@"; do
      case "$a" in
        drive-sync-fix*.service) block='LoadState=loaded\nActiveState=%4\nUnitFileState=static\nResult=success\n' ;;
        *.service) block='LoadState=loaded\nActiveState=%2\nUnitFileState=static\nResult=success\nExecMainStartTimestamp=@%3\n' ;;
        *.timer) block='LoadState=loaded\nActiveState=active\nUnitFileState=enabled\nResult=success\n' ;;
        *) continue ;;
      esac
      [ $first = 1 ] || printf '\n'
      first=0
      printf "Id=%s\n$block" "$a"
    done ;;
esac
)sh")
        .arg(QDateTime::currentDateTime().addSecs(420).toMSecsSinceEpoch() * 1000)
        .arg(serviceActive)
        .arg(serviceStart.toSecsSinceEpoch())
        .arg(fixActive);
    write(path, script.toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

SyncInputs healthy() {
    SyncInputs in;
    in.serviceLoad = "loaded";
    in.serviceActive = "inactive";
    in.serviceResult = "success";
    in.timerActive = "active";
    in.timerFileState = "enabled";
    in.now = QDateTime::currentDateTime();
    in.serviceStart = in.now.addSecs(-600);
    in.lastStart = in.serviceStart;
    in.lastOutcome = "success";
    in.lastSuccess = in.now.addSecs(-120);
    return in;
}

// A run that stopped on lost listings, as drive-sync logs it.
QString failedRun(const QDateTime &start) {
    QString log;
    log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
    log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
    log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
    log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");
    return log;
}

// A fake omarchy and omarchy-shell that append every call to calls.log, and
// answer "plugin list --json" with list.json, all inside dir.
PluginPaths fakeOmarchy(const QString &dir, const QByteArray &list) {
    write(dir + "/list.json", list);
    const QString omarchy = QString(
        "#!/bin/sh\n"
        "echo \"$*\" >> '%1/calls.log'\n"
        "case \"$*\" in\n"
        "  'plugin list --json') cat '%1/list.json' ;;\n"
        "esac\n").arg(dir);
    write(dir + "/omarchy", omarchy.toUtf8());
    write(dir + "/omarchy-shell", QString("#!/bin/sh\necho \"shell-cmd $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    for (const QString &name : {QString("omarchy"), QString("omarchy-shell")})
        QFile::setPermissions(dir + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir(dir).mkpath("shipped");
    return {dir + "/shipped", dir + "/plugins", dir + "/omarchy", dir + "/omarchy-shell"};
}

// A live process whose argv[0] is dir/<name>, which is what the takeover lock
// check reads. It sleeps; the caller kills it.
bool startNamedHolder(QProcess &holder, const QString &dir, const QString &name) {
    const QString program = dir + "/" + name;
    const QString sleepTool = QStandardPaths::findExecutable("sleep");
    if (sleepTool.isEmpty() || !QFile::link(sleepTool, program))
        return false;
    holder.start(program, {"30"});
    if (!holder.waitForStarted())
        return false;
    // QProcess can report the start before the child's exec shows in /proc,
    // and the lock check reads argv[0] there: wait until it does.
    QDeadlineTimer deadline(5000);
    while (!deadline.hasExpired()) {
        QFile cmdline(QString("/proc/%1/cmdline").arg(holder.processId()));
        if (cmdline.open(QIODevice::ReadOnly)
            && QFileInfo(QString::fromLocal8Bit(cmdline.readAll().split('\0').value(0))).fileName() == name)
            return true;
        QThread::msleep(5);
    }
    return false;
}

QStringList calls(const QString &dir) {
    QFile file(dir + "/calls.log");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

// What buildSnapshot sees with the service idle, the timer on and enabled,
// and the log already parsed into runs.
SnapshotSources idleSources(const QString &log, const QDateTime &now) {
    SnapshotSources src;
    src.runs = SyncLog::parse(log);
    src.logFound = true;
    src.service = {{"LoadState", "loaded"}, {"ActiveState", "inactive"}, {"Result", "success"},
                   {"ExecMainStartTimestamp", QString("@%1").arg(now.addSecs(-100).toSecsSinceEpoch())}};
    src.timer = {{"ActiveState", "active"}, {"UnitFileState", "enabled"}};
    src.now = now;
    return src;
}

// The ExecStart of the unit drive-sync.sh writes, with the home made up.
constexpr char kOldExecStart[] =
    "/usr/bin/rclone bisync gdrive: /home/someone/Drive --workdir /home/someone/.local/state/rclone/bisync "
    "--filters-file /home/someone/.config/rclone/gdrive-filtros.txt --drive-skip-gdocs --drive-skip-shortcuts "
    "--resilient --recover --max-lock 2m --max-delete 5 --conflict-resolve newer "
    "--log-file /home/someone/.local/state/drive-sync.log --log-level INFO";

// What a takeover of that unit writes.
SyncConfig oldUnitConfig() {
    SyncConfig config;
    config.remote = "gdrive:";
    config.localPath = "/home/someone/Drive";
    config.workDir = "/home/someone/.local/state/rclone/bisync";
    config.filtersFile = "/home/someone/.config/drive-sync/filters.txt";
    config.logFile = "/home/someone/.local/state/drive-sync.log";
    config.rcloneFlags = {"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                          "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                          "--log-level", "INFO"};
    config.runOnlyWhen = "findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'";
    return config;
}

// A systemctl that appends every call to calls.log and exits with exitCode,
// printing "denied" on stderr when it fails.
QString recordingSystemctl(const QString &dir, int exitCode = 0) {
    const QString path = dir + "/systemctl";
    write(path, QString("#!/bin/sh\necho \"systemctl $*\" >> '%1/calls.log'\n"
                        "[ %2 -eq 0 ] || echo denied >&2\nexit %2\n").arg(dir).arg(exitCode).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Paths for the control commands, everything inside dir.
BackendPaths controlPaths(const QString &dir, bool managed, int systemctlExit = 0) {
    BackendPaths paths{dir + "/drive-sync.log", dir + "/Drive", dir + "/state", dir + "/proc",
                       recordingSystemctl(dir, systemctlExit), QString(), dir + "/app"};
    paths.managed = managed;
    return paths;
}

// The watchdog's roots over the paths a test made before accounts, inside
// the folder that holds paths.appState: the config at config/config.json
// (legacy) or config/accounts/account1/config.json (accounts), the state
// root at paths.appState, and the old drive-sync looked for in that folder
// with the test's systemctl. A null configText saves config; any other is
// written as it is.
struct WatchdogHome {
    AccountRoots roots;
    TakeoverPaths tools;
};

WatchdogHome watchdogHome(const BackendPaths &paths, const SyncConfig &config, bool accounts = false,
                          const QByteArray &configText = QByteArray()) {
    const QString dir = QFileInfo(paths.appState).path();
    WatchdogHome h;
    h.roots.configRoot = dir + "/config";
    h.roots.stateRoot = paths.appState;
    h.roots.userUnits = dir + "/units";
    h.roots.base = paths;
    h.tools.userUnits = h.roots.userUnits;
    h.tools.binDir = dir + "/bin";
    h.tools.oldStateDir = dir + "/state";
    h.tools.appConfigDir = h.roots.configRoot;
    h.tools.backupRoot = dir + "/backup";
    h.tools.systemctl = paths.systemctl;
    h.tools.appState = h.roots.stateRoot;
    h.tools.systemUserUnits = dir + "/etc";
    const QString file = h.roots.configRoot + (accounts ? "/accounts/account1/config.json" : "/config.json");
    QDir().mkpath(QFileInfo(file).path());
    if (configText.isNull())
        saveSyncConfig(file, config, nullptr);
    else
        write(file, configText);
    return h;
}

// "drive-sync watchdog" over watchdogHome(paths, config, accounts).
int watchdogOver(const BackendPaths &paths, const SyncConfig &config, const NotifierPaths &tools,
                 const QDateTime &now, bool accounts = false) {
    const WatchdogHome h = watchdogHome(paths, config, accounts);
    return runWatchdogCommand(h.roots, h.tools, tools, now);
}

// notify-send, gdbus and omarchy that append each call to calls.log. The
// notify-send prints the next id from ids.txt, or nothing when it is "none"
// (no graphical session).
NotifierPaths fakeNotifier(const QString &dir) {
    write(dir + "/ids.txt", "41\n");
    const QString notify = QString(
        "#!/bin/sh\n"
        "echo \"notify-send $*\" >> '%1/calls.log'\n"
        "id=$(cat '%1/ids.txt')\n"
        "[ \"$id\" = none ] && exit 0\n"
        "echo $((id + 1)) > '%1/ids.txt'\n"
        "echo $id\n").arg(dir);
    write(dir + "/notify-send", notify.toUtf8());
    write(dir + "/gdbus", QString("#!/bin/sh\necho \"gdbus $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    write(dir + "/omarchy", QString("#!/bin/sh\necho \"omarchy $*\" >> '%1/calls.log'\n").arg(dir).toUtf8());
    for (const QString &name : {QString("notify-send"), QString("gdbus"), QString("omarchy")})
        QFile::setPermissions(dir + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return {dir + "/notify-send", dir + "/gdbus", dir + "/omarchy", QLocale::c()};
}

// A systemctl for the fix. drive-sync.service answers each line of
// service-active in turn, then "inactive"; drive-sync-fix.service answers
// fix-active, or "inactive". A start records whether fix.json existed at
// that moment and exits with start-exit, or 0.
QString fixSystemctl(const QString &dir, const QString &appState) {
    const QString path = dir + "/systemctl";
    write(path, QString(R"sh(#!/bin/sh
echo "systemctl $*" >> '%1/calls.log'
case "$*" in
  *" start "*)
    if [ -f '%2/fix.json' ]; then echo 'mark present' >> '%1/calls.log'; fi
    exit "$(cat '%1/start-exit' 2>/dev/null || echo 0)" ;;
  *drive-sync-fix*) cat '%1/fix-active' 2>/dev/null || echo inactive ;;
  *drive-sync.service*|*drive-sync@*.service*)
    if [ -s '%1/service-active' ]; then head -n 1 '%1/service-active'; sed -i 1d '%1/service-active'; else echo inactive; fi ;;
esac
)sh").arg(dir, appState).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// An rclone for the fix that records each call in calls.log. A resync dry
// run logs two copies, a normal dry run the lines in dir/normal-dry; a base run copies the listings in dir/canned to the work dir
// and writes the filters' .md5, as --resync does. lock-once makes the next
// base run fail on a lock; base-fails and normal-fails make those runs
// fail; deletefile exits with deletefile-exit, or 0.
QString fakeFixRclone(const QString &dir) {
    const QString path = dir + "/rclone";
    write(path, QString(R"sh(#!/bin/sh
echo "rclone $*" >> '%1/calls.log'
log=; work=; filters=; prev=
for a in "$@"; do
  case "$prev" in
    --log-file) log=$a ;;
    --workdir) work=$a ;;
    --filters-file) filters=$a ;;
  esac
  prev=$a
done
case " $* " in
  *" --dry-run "*)
    case " $* " in
      *" --resync "*) printf '2026/11/11 12:00:01 NOTICE: a.txt: Skipped copy as --dry-run is set (size 3)\n2026/11/11 12:00:01 NOTICE: b c.txt: Skipped copy as --dry-run is set (size 4)\n' >> "$log" ;;
      *) cat '%1/normal-dry' >> "$log" 2>/dev/null ;;
    esac
    exit 0 ;;
  *" --resync "*)
    if [ -f '%1/lock-once' ]; then
      rm '%1/lock-once'
      echo '2026/11/11 12:00:00 ERROR : Bisync critical error: prior lock file found: /w/x.lck' >> "$log"
      exit 2
    fi
    if [ -f '%1/base-fails' ]; then
      echo '2026/11/11 12:00:00 ERROR : Bisync critical error: boom' >> "$log"
      echo '2026/11/11 12:00:00 ERROR : Bisync aborted. Must run --resync to recover.' >> "$log"
      exit 2
    fi
    echo '2026/11/11 12:00:00 INFO  : Bisync successful' >> "$log"
    cp '%1/canned/'*.lst "$work/"
    md5sum "$filters" | cut -d' ' -f1 > "$filters.md5"
    exit 0 ;;
  *" bisync "*)
    if [ -f '%1/normal-fails' ]; then
      echo '2026/11/11 12:00:00 ERROR : Bisync critical error: normal broke' >> "$log"
      if [ -f '%1/normal-loses-listings' ]; then
        for f in "$work"/*.lst; do mv "$f" "$f-err"; done
      fi
      exit 2
    fi
    exit 0 ;;
  *" deletefile "*)
    exit "$(cat '%1/deletefile-exit' 2>/dev/null || echo 0)" ;;
esac
exit 0
)sh").arg(dir).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// One file line of a bisync listing.
QByteArray listingLine(const QString &name, qint64 size, const QDateTime &when) {
    return QString("-  %1 - - %2.000000000+0000 \"%3\"\n")
        .arg(size)
        .arg(when.toUTC().toString("yyyy-MM-dd'T'HH:mm:ss"), name)
        .toUtf8();
}

// The listings the fake base run leaves behind.
bool cannedListings(const QString &dir, const QByteArray &cloud, const QByteArray &notebook) {
    QDir().mkpath(dir + "/canned");
    const QString session = dir + "/canned/" + bisyncSessionName("gdrive:", dir + "/Drive");
    return write(session + ".path1.lst", "# bisync listing v1\n" + cloud)
           && write(session + ".path2.lst", "# bisync listing v1\n" + notebook);
}

// A drive-sync run by DriveSync inside dir, with the fix's fake rclone and
// systemctl, a notifier that records its cards, and waits recorded.
struct FixSetup {
    BackendPaths paths;
    SyncConfig config;
    FixTools tools;
    NotifierPaths notify;
    std::shared_ptr<QList<int>> waits = std::make_shared<QList<int>>();
};

FixSetup makeFixSetup(const QString &dir) {
    FixSetup s;
    QDir().mkpath(dir + "/Drive");
    QDir().mkpath(dir + "/work");
    s.config.remote = "gdrive:";
    s.config.localPath = dir + "/Drive";
    s.config.workDir = dir + "/work";
    s.config.filtersFile = dir + "/filters.txt";
    s.config.logFile = dir + "/drive-sync.log";
    s.config.rcloneFlags = {"--drive-skip-gdocs", "--resilient", "--max-delete", "5"};
    write(s.config.filtersFile, "- .git/**\n");
    QString error;
    saveSyncConfig(dir + "/config.json", s.config, &error);
    s.paths = controlPaths(dir, true);
    s.paths.log = s.config.logFile;
    s.paths.drive = s.config.localPath;
    s.paths.config = dir + "/config.json";
    s.paths.systemctl = fixSystemctl(dir, s.paths.appState);
    s.tools.rclone = fakeFixRclone(dir);
    const auto waits = s.waits;
    s.tools.wait = [waits](int seconds) { *waits << seconds; };
    s.notify = fakeNotifier(dir);
    return s;
}

// Runs "drive-sync fix <arguments> --json" and returns its error, empty when ok.
QString fixCommand(const FixSetup &s, const QStringList &arguments) {
    QString output;
    QString errors;
    QTextStream out(&output);
    QTextStream err(&errors);
    runFixCommand(arguments + QStringList{"--json"}, s.paths, out, err);
    return QJsonDocument::fromJson(output.toUtf8()).object().value("error").toString("no JSON answer");
}

// Runs "drive-sync deletes <arguments> --json" and returns its answer.
QJsonObject deletesCommand(const BackendPaths &paths, const QStringList &arguments) {
    QString output;
    QString errors;
    QTextStream out(&output);
    QTextStream err(&errors);
    runDeletesCommand(arguments + QStringList{"--json"}, paths, out, err);
    return QJsonDocument::fromJson(output.toUtf8()).object();
}

FixState fixAt(const QString &step) {
    FixState state;
    state.step = step;
    state.started = QDateTime::currentDateTimeUtc().addSecs(-600);
    state.stepStarted = state.started;
    return state;
}

FixQuestion question(const QString &path, const QString &kind, const QString &answer = {}) {
    FixQuestion q;
    q.path = path;
    q.kind = kind;
    q.answer = answer;
    return q;
}

// The listings a base run left in the work dir, as finishing finds them.
bool baseListings(const FixSetup &s) {
    const QByteArray body = "# bisync listing v1\n" + listingLine("a.txt", 1, QDateTime::currentDateTimeUtc().addSecs(-3600));
    const QString session = s.config.workDir + "/" + bisyncSessionName(s.config.remote, s.config.localPath);
    return write(session + ".path1.lst", body) && write(session + ".path2.lst", body);
}

// The modification time of a file, in Unix seconds.
qint64 mtimeOf(const QString &path) { return QFileInfo(path).lastModified().toSecsSinceEpoch(); }

bool setOldMtime(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadWrite)
           && file.setFileTime(QDateTime(QDate(2020, 1, 1), QTime(12, 0), QTimeZone::UTC),
                               QFileDevice::FileModificationTime);
}

// A systemctl for the watchdog: LoadState, is-active and is-enabled answers.
QString watchdogSystemctl(const QString &dir, const QString &load, const QString &active, const QString &enabled,
                          const QString &fixActive = "inactive") {
    const QString path = dir + "/systemctl";
    // fixActive "fail": systemctl cannot answer about drive-sync-fix.service.
    write(path, QString("#!/bin/sh\n"
                        "case \"$*\" in\n"
                        "  *drive-sync-fix*) [ %4 = fail ] && exit 1; echo %4 ;;\n"
                        "  *LoadState*) echo %1 ;;\n"
                        "  *is-active*) echo %2; [ %2 = active ] ;;\n"
                        "  *is-enabled*) echo %3; [ %3 = enabled ] ;;\n"
                        "esac\n").arg(load, active, enabled, fixActive).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QStringList notifications(const QString &dir) {
    QStringList sent;
    for (const QString &call : calls(dir))
        if (call.startsWith("notify-send"))
            sent << call;
    return sent;
}

// A drive-sync set up the way drive-sync.sh does it, inside root/home, with
// a systemctl and an rclone that record their calls in root/calls.log.
struct OldSetup {
    QString home;
    TakeoverPaths paths;
    QString filters;
};

QString oldServiceText(const QString &home) {
    return QString("[Unit]\nDescription=rclone bisync de ~/Drive com o Google Drive\n\n[Service]\nType=oneshot\n"
                   "# Rodada travada vira falha.\nTimeoutStartSec=2h\n"
                   "ExecCondition=/usr/bin/sh -c \"findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'\"\n"
                   "ExecStart=/usr/bin/rclone bisync gdrive: %1/Drive --workdir %1/.local/state/rclone/bisync "
                   "--filters-file %1/.config/rclone/gdrive-filtros.txt --drive-skip-gdocs --drive-skip-shortcuts "
                   "--resilient --recover --max-lock 2m --max-delete 5 --conflict-resolve newer "
                   "--log-file %1/.local/state/drive-sync.log --log-level INFO\n"
                   "ExecStopPost=-%1/.local/bin/drive-sync-aviso\nNice=10\nIOSchedulingClass=idle\n").arg(home);
}

OldSetup makeOldSetup(const QString &root) {
    OldSetup o;
    o.home = root + "/home";
    QDir d;
    for (const char *folder : {"/.config/systemd/user", "/.local/bin", "/.config/rclone",
                               "/.local/state/rclone/bisync", "/.local/state/drive-sync", "/Drive"})
        d.mkpath(o.home + folder);
    const QString units = o.home + "/.config/systemd/user/";
    write(units + "drive-sync.service", oldServiceText(o.home).toUtf8());
    write(units + "drive-sync.timer", "[Unit]\nDescription=a cada 15 min\n\n[Timer]\nOnBootSec=5min\n"
                                      "OnUnitActiveSec=15min\n\n[Install]\nWantedBy=timers.target\n");
    write(units + "drive-sync-vigia.service", "[Service]\nType=oneshot\nExecStart=/x/drive-sync-aviso --vigia\n");
    write(units + "drive-sync-vigia.timer", "[Timer]\nOnStartupSec=10min\nOnUnitActiveSec=1h\n");
    write(o.home + "/.local/bin/drive-sync.sh", "#!/usr/bin/env bash\necho old\n");
    write(o.home + "/.local/bin/drive-sync-aviso", "#!/usr/bin/env bash\necho old\n");
    o.filters = o.home + "/.config/rclone/gdrive-filtros.txt";
    const QByteArray text = "# Gerado pelo drive-sync.sh.\n- .git/**\n";
    write(o.filters, text);
    write(o.filters + ".md5", QCryptographicHash::hash(text, QCryptographicHash::Md5).toHex());
    const QString session = o.home + "/.local/state/rclone/bisync/" + bisyncSessionName("gdrive:", o.home + "/Drive");
    write(session + ".path1.lst", "# bisync listing v1\n");
    write(session + ".path2.lst", "# bisync listing v1\n");
    write(o.home + "/.local/state/drive-sync/aviso", "0 0 0\n");
    write(o.home + "/.local/state/drive-sync.log", "");

    const QString rclone = root + "/rclone";
    write(rclone, QString("#!/bin/sh\necho \"rclone $*\" >> '%1/calls.log'\n"
                          "[ \"$1\" = listremotes ] && echo gdrive:\nexit 0\n").arg(root).toUtf8());
    QFile::setPermissions(rclone, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    o.paths = {o.home + "/.config/systemd/user", o.home + "/.local/bin", o.home + "/.local/state/drive-sync",
               o.home + "/.config/drive-sync", o.home + "/.local/share/drive-sync/taken-over",
               recordingSystemctl(root), rclone};
    o.paths.appState = o.home + "/.local/state/drive-sync-app";
    o.paths.systemUserUnits = root + "/etc-systemd-user";
    return o;
}

// The state a takeover wants: last run a success, nothing running.
Snapshot settledSnapshot() {
    Snapshot s;
    s.summary.status = "ok";
    s.lastRunOutcome = "success";
    return s;
}

// A systemctl for a takeover: it records every call, fails the failAt-th call
// (0 never fails), says the service is in serviceState and that each unit
// "show" asks about loads from fragmentDir (an instance from its template).
QString takeoverSystemctl(const QString &root, int failAt, const QString &serviceState = "inactive",
                          const QString &fragmentDir = "/usr/lib/systemd/user", const QString &onShow = ":") {
    const QString path = root + "/systemctl";
    write(path, QString("#!/bin/sh\n"
                        "echo \"systemctl $*\" >> '%1/calls.log'\n"
                        "n=$(grep -c '^systemctl' '%1/calls.log')\n"
                        "[ \"$n\" -eq %2 ] && { echo refused >&2; exit 1; }\n"
                        "case \"$*\" in\n"
                        "  *is-active\\ drive-sync.service*) echo %3; [ %3 = active ] ;;\n"
                        "  *is-active*) echo active ;;\n"
                        "  *is-enabled*) echo enabled ;;\n"
                        "  *FragmentPath*) %5\n"
                        "    first=1\n"
                        "    for u in \"$@\"; do\n"
                        "      case $u in *.service|*.timer)\n"
                        "        [ $first = 1 ] || echo; first=0\n"
                        "        t=$(echo \"$u\" | sed 's/@[^.]*\\./@./')\n"
                        "        printf 'Id=%s\\nLoadState=loaded\\nFragmentPath=%4/%s\\nDropInPaths=\\n' \"$u\" \"$t\" ;;\n"
                        "      esac\n"
                        "    done ;;\n"
                        "esac\n").arg(root).arg(failAt).arg(serviceState, fragmentDir, onShow).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// After a takeover, what one made before accounts (step 5.1) left instead:
// config.json and the filters with their .md5 at the top of the config
// folder, the old ExecCondition in the config's runOnlyWhen, no machine.json
// and no accounts/. A takeover now writes account1, which "takeover --undo"
// refuses, so the undo tests start from this legacy home.
bool asLegacyTakeover(const OldSetup &old) {
    const QString root = old.paths.appConfigDir;
    const QString account = root + "/accounts/account1";
    std::optional<SyncConfig> config = loadSyncConfig(account + "/config.json", nullptr);
    if (!config)
        return false;
    config->runOnlyWhen = readMachineGuard(machineGuardPath(root), nullptr).value_or(QString());
    config->filtersFile = root + "/filters.txt";
    return QFile::rename(account + "/filters.txt", root + "/filters.txt")
           && QFile::rename(account + "/filters.txt.md5", root + "/filters.txt.md5")
           && saveSyncConfig(root + "/config.json", *config, nullptr) && QFile::remove(account + "/config.json")
           && (!QFile::exists(machineGuardPath(root)) || QFile::remove(machineGuardPath(root)))
           && QDir().rmdir(account) && QDir().rmdir(root + "/accounts");
}

// Every file under root with its contents, so two moments can be compared.
QMap<QString, QByteArray> filesUnder(const QString &root, const QStringList &skip = {}) {
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = QDir(root).relativeFilePath(path);
        if (std::any_of(skip.cbegin(), skip.cend(), [&](const QString &s) { return relative.startsWith(s); }))
            continue;
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        files.insert(relative, file.readAll());
    }
    return files;
}

// Only the systemctl calls in calls.log (detection also asks rclone).
QStringList systemctlCalls(const QString &dir) {
    QStringList found;
    for (const QString &call : calls(dir))
        if (call.startsWith("systemctl"))
            found << call;
    return found;
}

// A drive-sync that appends its arguments to calls.log and answers with
// answer.json for "takeover", or fails "sync" when told to.
QString fakeDriveSync(const QString &dir, const QByteArray &takeoverAnswer, bool syncFails = false) {
    write(dir + "/answer.json", takeoverAnswer);
    const QString path = dir + "/drive-sync";
    write(path, QString("#!/bin/sh\n"
                        "echo \"drive-sync $*\" >> '%1/calls.log'\n"
                        "case \"$1\" in\n"
                        "  takeover) cat '%1/answer.json' ;;\n"
                        "  sync) %2 ;;\n"
                        "  *) echo '{\"ok\":true,\"error\":\"\"}' ;;\n"
                        "esac\n")
                    .arg(dir, syncFails ? "echo '{\"ok\":false,\"error\":\"denied\"}'; exit 1"
                                        : "echo '{\"ok\":true,\"error\":\"\"}'")
                    .toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// The client the sign-in tests hand to the command: made up, so the shipped
// one (when this build has it) never reaches a fake rclone's call log.
OAuthClient fakeClient() {
    return {"fake-client.apps.googleusercontent.com", "fake-secret"};
}

// An rclone for "drive-sync signin" and "drive-sync setup" that writes each
// call to rclone-calls.log, one line, arguments separated by tabs. Files in
// dir steer it: remotes (what listremotes prints), dump (config dump),
// version (the version line), create-exit, update-exit, lsjson-exit,
// lsjson-fails-once, create-hangs (create waits until killed), create-notice
// (printed on stderr first), create-stderr (printed on a failed create).
// The file after config_template_file is copied to page-seen.html, and its
// mode to page-mode, while the call runs.
QString fakeSignInRclone(const QString &dir) {
    const QString path = dir + "/rclone";
    write(path, QString(R"sh(#!/bin/sh
for a in "$@"; do printf '%s\t' "$a"; done >> '%1/rclone-calls.log'
echo >> '%1/rclone-calls.log'
prev=
for a in "$@"; do
  if [ "$prev" = config_template_file ]; then cp "$a" '%1/page-seen.html'; stat -c %a "$a" > '%1/page-mode'; fi
  prev=$a
done
case "$1 $2" in
  "listremotes "*) cat '%1/remotes' 2>/dev/null ;;
  "version "*) cat '%1/version' 2>/dev/null || echo 'rclone v1.75.2' ;;
  "config create")
    [ -f '%1/create-notice' ] && cat '%1/create-notice' >&2
    [ -f '%1/create-hangs' ] && exec sleep 5
    code=$(cat '%1/create-exit' 2>/dev/null || echo 0)
    [ "$code" = 0 ] || cat '%1/create-stderr' >&2 2>/dev/null
    exit "$code" ;;
  "config update") exit "$(cat '%1/update-exit' 2>/dev/null || echo 0)" ;;
  "config dump") cat '%1/dump' 2>/dev/null || echo '{}' ;;
  "config delete") exit 0 ;;
  "lsf "*)
    if [ -f '%1/lsf-refuses' ]; then
      echo 'CRITICAL: Failed to initialise global options: failed to reload "filter" options: malformed rule "Photos"' >&2
      exit 1
    fi
    exit 0 ;;
  "lsjson "*)
    echo '[]'
    if [ -f '%1/lsjson-fails-once' ]; then rm '%1/lsjson-fails-once'; exit 1; fi
    exit "$(cat '%1/lsjson-exit' 2>/dev/null || echo 0)" ;;
esac
exit 0
)sh").arg(dir).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// The calls the fake rclone recorded, each as its arguments.
QList<QStringList> rcloneCalls(const QString &dir) {
    QList<QStringList> calls;
    QFile file(dir + "/rclone-calls.log");
    if (!file.open(QIODevice::ReadOnly))
        return calls;
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (line.isEmpty())
            continue;
        QStringList arguments = QString::fromUtf8(line).split('\t');
        arguments.removeLast();  // every argument ends with a tab
        calls << arguments;
    }
    return calls;
}

struct CommandResult {
    int code = -1;
    QString out;
    QString err;
};

CommandResult signInCommand(const BackendPaths &paths, const SignInTools &tools, const QStringList &arguments) {
    CommandResult result;
    QTextStream out(&result.out);
    QTextStream err(&result.err);
    result.code = runSignInCommand(arguments, paths, tools, out, err);
    out.flush();
    err.flush();
    return result;
}

SignInTools signInTools(const QString &dir) {
    SignInTools tools;
    tools.rclone = fakeSignInRclone(dir);
    tools.client = fakeClient();
    return tools;
}

// The page a recorded rclone call was given, or an empty string.
QString pageArgument(const QStringList &call) {
    const qsizetype at = call.indexOf("config_template_file");
    return at < 0 ? QString() : call.value(at + 1);
}

// The temporary pages signin leaves in the temp folder, by name.
QStringList leftoverPages() {
    return QDir(QDir::tempPath()).entryList({"drive-sync-signin-*"}, QDir::Files | QDir::Hidden);
}

// One HTTP/1.0 GET of target on rclone's sign-in port, as the browser makes
// it when Google sends it back; the body of the answer, or nothing.
QByteArray getFromRclonePort(const QByteArray &target) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return {};
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(53682);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const QByteArray request = "GET " + target + " HTTP/1.0\r\nHost: 127.0.0.1:53682\r\n\r\n";
        if (::send(fd, request.constData(), request.size(), 0) != request.size()) {
            ::close(fd);
            return {};
        }
        QByteArray answer;
        char buffer[4096];
        ssize_t got;
        while ((got = ::recv(fd, buffer, sizeof buffer, 0)) > 0)
            answer.append(buffer, got);
        ::close(fd);
        const qsizetype body = answer.indexOf("\r\n\r\n");
        return body < 0 ? QByteArray() : answer.mid(body + 4);
    }
    return {};
}

// "drive-sync signin" against the real rclone: a scratch rclone.conf
// (confText), Google's token address pointed at a closed local port so no
// test reaches Google, and a thread that plays the browser coming back with
// query after the state, once the sandbox's xdg-open was asked to open
// rclone's link. The page rclone served comes back with the command's answer.
struct ServedSignIn {
    CommandResult result;
    QByteArray page;
};

ServedSignIn signInThroughRclone(const QString &dir, const BackendPaths &paths, SignInTools tools,
                                 const QByteArray &confText, const QByteArray &query) {
    const QByteArray oldConfig = qgetenv("RCLONE_CONFIG");
    const QString opened = QFileInfo(QString::fromLocal8Bit(oldConfig)).absolutePath() + "/opened.log";
    write(dir + "/rclone.conf", confText);
    qputenv("RCLONE_CONFIG", (dir + "/rclone.conf").toLocal8Bit());
    qputenv("RCLONE_DRIVE_TOKEN_URL", "http://127.0.0.1:9/token");
    const auto restore = qScopeGuard([&oldConfig] {
        qputenv("RCLONE_CONFIG", oldConfig);
        qunsetenv("RCLONE_DRIVE_TOKEN_URL");
    });
    const qsizetype openedBefore = readBytes(opened).count('\n');
    ServedSignIn served;
    std::atomic<bool> finished{false};
    std::thread browser([&] {
        static const QRegularExpression state("state=([A-Za-z0-9_-]+)");
        while (!finished) {
            const QList<QByteArray> lines = readBytes(opened).split('\n');
            if (lines.size() - 1 > openedBefore) {
                const QRegularExpressionMatch match = state.match(QString::fromUtf8(lines.at(openedBefore)));
                if (match.hasMatch())
                    served.page = getFromRclonePort("/?state=" + match.captured(1).toUtf8() + "&" + query);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    tools.timeoutMs = 15000;
    served.result = signInCommand(paths, tools, {});
    finished = true;
    browser.join();
    return served;
}


// A systemctl for first setup: records each call in calls.log; an enable
// records "mark preparing" when fix.json already holds that step, and exits
// with enable-exit; a start exits with start-exit; show answers inactive.
QString setupSystemctl(const QString &dir, const QString &appState) {
    const QString path = dir + "/systemctl";
    write(path, QString(R"sh(#!/bin/sh
echo "systemctl $*" >> '%1/calls.log'
case "$*" in
  *" enable "*)
    if grep -q preparing '%2/fix.json' 2>/dev/null; then echo 'mark preparing' >> '%1/calls.log'; fi
    code=$(cat '%1/enable-exit' 2>/dev/null || echo 0)
    [ "$code" = 0 ] || echo 'Failed to enable unit: denied' >&2
    exit "$code" ;;
  *" start "*) exit "$(cat '%1/start-exit' 2>/dev/null || echo 0)" ;;
  *" show "*) echo inactive ;;
esac
exit 0
)sh").arg(dir, appState).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Everything first setup reads and writes, inside dir: no config, no old
// drive-sync, rclone that knows the drive-sync remote.
struct SetupFixture {
    BackendPaths paths;
    TakeoverPaths takeover;
    QString folder;
};

SetupFixture makeSetupFixture(const QString &dir) {
    SetupFixture s;
    s.paths = controlPaths(dir, false);
    s.paths.systemctl = setupSystemctl(dir, s.paths.appState);
    s.takeover.userUnits = dir + "/units";
    s.takeover.binDir = dir + "/bin";
    s.takeover.oldStateDir = dir + "/state";
    s.takeover.appConfigDir = dir + "/config";
    s.takeover.backupRoot = dir + "/backup";
    s.takeover.systemctl = s.paths.systemctl;
    s.takeover.rclone = fakeSignInRclone(dir);
    s.takeover.packagedUnits = dir + "/packaged";
    s.takeover.appState = s.paths.appState;
    s.takeover.systemUserUnits = dir + "/etc";
    s.paths.config = s.takeover.appConfigDir + "/config.json";
    // The log in a folder of its own, as ~/.local/state is: setup refuses a
    // folder inside the log's.
    QDir().mkpath(dir + "/logs");
    s.paths.log = dir + "/logs/drive-sync.log";
    write(dir + "/remotes", "drive-sync:\n");
    s.folder = dir + "/Drive";
    return s;
}

CommandResult setupCommand(const SetupFixture &s, const QString &folder) {
    CommandResult result;
    QTextStream out(&result.out);
    QTextStream err(&result.err);
    result.code = runSetupCommand({"--folder", folder, "--json"}, s.paths, s.takeover, out, err);
    out.flush();
    err.flush();
    return result;
}

QString setupError(const CommandResult &r) {
    return QJsonDocument::fromJson(r.out.toUtf8()).object().value("error").toString("no JSON answer");
}


// A drive-sync for SetupController: records each call in calls.log.
// "signin --json" prints signin-link first when it exists; then it prints
// signin-answer once signin-release exists (at once when there is no
// signin-link), or, with no answer, starts two sleeping processes in its own
// group, rclone (grandchild.pid) and a browser (browser.pid), and waits.
// Like the real command, a SIGTERM stops only its rclone and logs "cleaned".
// "setup" prints setup-answer. The call is logged as made; "--account <id>"
// right after the command is then dropped, so "signin --account account1
// --json" waits as "signin --json" does.
QString fakeSetupDrivesync(const QString &dir) {
    const QString path = dir + "/drive-sync";
    write(path, QString(R"sh(#!/bin/sh
echo "$*" >> '%1/calls.log'
if [ "$2" = --account ] && [ $# -ge 3 ]; then c=$1; shift 3; set -- "$c" "$@"; fi
case "$*" in
  "signin --json")
    if [ -f '%1/signin-link' ]; then
      cat '%1/signin-link'
      while [ ! -f '%1/signin-release' ]; do sleep 0.05; done
    fi
    if [ -f '%1/signin-answer' ]; then cat '%1/signin-answer'; exit 0; fi
    sleep 300 &
    rc=$!
    sleep 300 &
    echo $! > '%1/browser.pid'
    echo $rc > '%1/grandchild.pid'
    echo $$ > '%1/child.pid'
    trap 'kill $rc; echo cleaned >> %1/calls.log; exit 1' TERM INT
    wait $rc ;;
  "setup "*)
    case "$*" in *"--filters -"*) cat > '%1/setup-stdin' ;; esac
    cat '%1/setup-answer' ;;
esac
exit 0
)sh").arg(dir).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Whether the real FixBanner.qml offers F over a stand-in backend with this
// status and cause: what the window shows.
bool fixBannerOffers(const QString &status, const QString &causeKind) {
    QQmlEngine engine;
    QQmlPropertyMap backend, theme;
    backend.insert("status", status);
    backend.insert("causeKind", causeKind);
    backend.insert("managed", true);
    backend.insert("fixStep", "");
    backend.insert("guardStopHolds", false);
    theme.insert("radius", 8);
    theme.insert("accent", QColor("#7aa2f7"));
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("theme", &theme);
    QQmlComponent component(&engine);
    component.setData(QString("import QtQuick\nimport \"%1\"\nFixBanner {}\n")
                          .arg(QUrl::fromLocalFile(QString(SRCDIR) + "/../src").toString())
                          .toUtf8(),
                      QUrl());
    std::unique_ptr<QObject> banner(component.create());
    return banner && banner->property("visible").toBool();
}

// Gone, or a zombie nobody reaped yet: either way it runs no more.
bool processGone(qint64 pid) {
    QFile stat(QString("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly))
        return true;
    const QByteArray text = stat.readAll();
    return text.mid(text.lastIndexOf(')') + 2, 1) == "Z";
}

// Step 4b: what "drive-sync settings" reads and writes, inside dir. A
// drive-sync DriveSync runs (makeFixSetup), its unit folder, and the fix's
// fake rclone and systemctl.
struct SettingsFixture {
    FixSetup fix;
    TakeoverPaths takeover;
};

SettingsFixture makeSettingsFixture(const QString &dir) {
    SettingsFixture s;
    s.fix = makeFixSetup(dir);
    // The config and the log in folders of their own, as ~/.config/drive-sync
    // and ~/.local/state are: a new local folder may not sit inside them.
    QDir().mkpath(dir + "/conf");
    QDir().mkpath(dir + "/logs");
    QFile::remove(dir + "/config.json");
    s.fix.config.logFile = dir + "/logs/drive-sync.log";
    s.fix.paths.log = s.fix.config.logFile;
    s.fix.paths.config = dir + "/conf/config.json";
    QString error;
    saveSyncConfig(s.fix.paths.config, s.fix.config, &error);
    s.takeover.userUnits = dir + "/units";
    s.takeover.binDir = dir + "/bin";
    s.takeover.oldStateDir = dir + "/state";
    s.takeover.appConfigDir = dir + "/conf";
    s.takeover.backupRoot = dir + "/backup";
    s.takeover.systemctl = s.fix.paths.systemctl;
    s.takeover.rclone = s.fix.tools.rclone;
    s.takeover.packagedUnits = dir + "/packaged";
    s.takeover.appState = s.fix.paths.appState;
    s.takeover.systemUserUnits = dir + "/etc";
    return s;
}

// Runs "drive-sync settings <arguments> --json": its exit code and answer.
QPair<int, QJsonObject> settingsCommand(const SettingsFixture &s, const QStringList &arguments,
                                        QIODevice *input = nullptr) {
    QString output;
    QString errors;
    QTextStream out(&output);
    QTextStream err(&errors);
    const int code = runSettingsCommand(arguments + QStringList{"--json"}, s.fix.paths, s.takeover, out, err, input);
    out.flush();
    return {code, QJsonDocument::fromJson(output.toUtf8()).object()};
}

// An rclone for "drive-sync folders": records each call in rclone-calls.log
// (arguments separated by tabs), prints lsjson.json and exits with
// lsjson-exit, or 0.
QString fakeFoldersRclone(const QString &dir) {
    const QString path = dir + "/rclone";
    write(path, QString(R"sh(#!/bin/sh
for a in "$@"; do printf '%s\t' "$a"; done >> '%1/rclone-calls.log'
echo >> '%1/rclone-calls.log'
cat '%1/lsjson.json' 2>/dev/null
exit "$(cat '%1/lsjson-exit' 2>/dev/null || echo 0)"
)sh").arg(dir).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Runs "drive-sync folders <arguments> --json": its exit code and answer.
QPair<int, QJsonObject> foldersCommand(const BackendPaths &paths, const QString &rclone, const QStringList &arguments) {
    QString output;
    QString errors;
    QTextStream out(&output);
    QTextStream err(&errors);
    const int code = runFoldersCommand(QStringList{"--json"} + arguments, paths, rclone, out, err);
    out.flush();
    return {code, QJsonDocument::fromJson(output.toUtf8()).object()};
}

// A drive-sync for the Settings view: records each call in calls.log, then
// drops "--account <id>" right after the command, as the window passes it.
// "settings --json" prints settings.json; a change copies the filters file
// it was given to filters-sent.txt and prints change-answer.json; "folders"
// prints folders-root.json, or folders-<PATH>.json for a level below.
QString fakeSettingsDrivesync(const QString &dir) {
    const QString path = dir + "/drive-sync";
    write(path, QString(R"sh(#!/bin/sh
echo "$*" >> '%1/calls.log'
if [ "$2" = --account ] && [ $# -ge 3 ]; then c=$1; shift 3; set -- "$c" "$@"; fi
case "$1" in
  settings)
    if [ "$2" = --json ]; then cat '%1/settings.json'; exit 0; fi
    if [ "$2" = --filters ]; then
      if [ "$3" = - ]; then cat > '%1/filters-sent.txt'; else cp "$3" '%1/filters-sent.txt'; fi
    fi
    cat '%1/change-answer.json' ;;
  folders)
    case " $* " in
      *" -- "*) for last in "$@"; do :; done; cat "%1/folders-$last.json" ;;
      *) cat '%1/folders-root.json' ;;
    esac ;;
esac
exit 0
)sh").arg(dir).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Moves the fix to a step and keeps its start, as the unit does: a
// settings-change mark serves the fix with that start only.
bool moveFix(const QString &appState, const QString &step, const QString &failedStep = {}) {
    FixState state = readFix(appState);
    state.step = step;
    state.failedStep = failedStep;
    return writeFix(appState, state);
}

// Runs the fix unit from "preparing" to the dry run's "confirm", with the
// config on disk, as drive-sync-fix.service does.
bool runToConfirm(const SettingsFixture &s) {
    QString error;
    const auto config = loadSyncConfig(s.fix.paths.config, &error);
    return config && runFixUnit(s.fix.paths, *config, s.fix.tools, s.fix.notify) == 0
           && readFix(s.fix.paths.appState).step == "confirm";
}

// The resolver's roots inside dir: config/, app/ (the state root), units/,
// and a base with the old log, Drive, drive-sync.sh's state, a fake proc and
// a systemctl that records its calls.
AccountRoots accountRoots(const QString &dir) {
    AccountRoots roots;
    roots.configRoot = dir + "/config";
    roots.stateRoot = dir + "/app";
    roots.userUnits = dir + "/units";
    roots.base = BackendPaths{dir + "/drive-sync.log", dir + "/Drive", dir + "/state", dir + "/proc",
                              recordingSystemctl(dir), QString(), roots.stateRoot};
    return roots;
}

// A valid config at path, syncing localPath and logging to logFile.
bool writeAccountConfig(const QString &path, const QString &localPath, const QString &logFile,
                        int intervalMinutes = kDefaultIntervalMinutes) {
    SyncConfig config;
    config.remote = "gdrive:";
    config.localPath = localPath;
    config.workDir = QFileInfo(path).path() + "/bisync";
    config.filtersFile = QFileInfo(path).path() + "/filters.txt";
    config.logFile = logFile;
    config.intervalMinutes = intervalMinutes;
    QString error;
    return QDir().mkpath(QFileInfo(path).path()) && saveSyncConfig(path, config, &error);
}

// paths with account <id>'s instance units, as the resolver names them in
// accounts mode; every other path stays.
BackendPaths withAccountUnits(BackendPaths paths, const QString &id) {
    const BackendPaths account = newAccountPaths(AccountRoots(), id);
    paths.id = id;
    paths.serviceUnit = account.serviceUnit;
    paths.timerUnit = account.timerUnit;
    paths.fixUnit = account.fixUnit;
    paths.accountsMode = true;
    return paths;
}

// Account <id> in the accounts layout under roots, with its own folder in
// dir/<id>/Drive and a log in dir/<id>/drive-sync.log holding logText. A
// null config text writes a valid config; any other is written as it is.
bool makeStatusAccount(const AccountRoots &roots, const QString &dir, const QString &id, const QString &logText,
                       const QByteArray &configText = QByteArray()) {
    const QString folder = dir + "/" + id;
    const QString config = roots.configRoot + "/accounts/" + id + "/config.json";
    if (!QDir().mkpath(folder + "/Drive") || !write(folder + "/drive-sync.log", logText.toUtf8()))
        return false;
    if (!configText.isNull())
        return QDir().mkpath(QFileInfo(config).path()) && write(config, configText);
    return writeAccountConfig(config, folder + "/Drive", folder + "/drive-sync.log");
}

// As makeStatusAccount, with the folder at dir/<folderName> and the log at
// dir/<id>.log, so two accounts can get the labels Drive and Drive2.
bool makeLabelledAccount(const AccountRoots &roots, const QString &dir, const QString &id, const QString &folderName,
                         const QString &logText, const QByteArray &configText = QByteArray()) {
    const QString folder = dir + "/" + folderName;
    const QString log = dir + "/" + id + ".log";
    const QString config = roots.configRoot + "/accounts/" + id + "/config.json";
    if (!QDir().mkpath(folder) || !write(log, logText.toUtf8()))
        return false;
    if (!configText.isNull())
        return QDir().mkpath(QFileInfo(config).path()) && write(config, configText);
    return writeAccountConfig(config, folder, log);
}

// The window's accounts over roots (Task 8 of step 5.2): each Backend made
// as main makes one, minus the old drive-sync check, which tests never give.
std::unique_ptr<AccountList> accountListOver(const AccountRoots &roots) {
    return std::make_unique<AccountList>([roots] { return windowAccounts(roots); },
                                         [](const BackendPaths &paths) { return new Backend(paths); });
}

// The Backend the list holds for id, or null.
Backend *backendOf(const AccountList &list, const QString &id) {
    for (Backend *backend : list.backends())
        if (backend->accountId() == id)
            return backend;
    return nullptr;
}

struct StatusAnswer {
    int code = -1;
    QString out;
    QString err;
};

// "drive-sync status <arguments>" over accounts, with notices.
StatusAnswer statusOf(const QStringList &arguments, const QList<BackendPaths> &accounts,
                      const QList<AccountNotice> &notices = {}) {
    StatusAnswer answer;
    QTextStream out(&answer.out);
    QTextStream err(&answer.err);
    answer.code = runStatusCommand(arguments, accounts, notices, out, err);
    return answer;
}

// A run that started a few seconds ago and is still listing.
QString openRun(const QDateTime &start) {
    return line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")")
           + line(start, "INFO", "Building Path1 and Path2 listings");
}

// A fake /proc entry for pid whose program is name.
bool fakeProcess(const QString &proc, int pid, const QString &name) {
    const QString folder = QString("%1/%2").arg(proc).arg(pid);
    return QDir().mkpath(folder) && write(folder + "/cmdline", QByteArray("/usr/bin/") + name.toUtf8() + '\0' + "status");
}

// Step 5.1, the move: a systemctl that records every call in calls.log and
// keeps each unit's properties as files in dir/sd/<unit>/<property>, which
// the test edits. "show" answers by Id, one block per unit asked, and
// "--value" prints the values alone; a unit with no LoadState file is
// not-found, and DropInPaths lists the .conf files in <userUnits>/<unit>.d.
// enable, disable (with --now), start and stop change the files. A file
// dir/fail-<verb> fails that verb once; dir/on-<verb>.sh runs first.
QString moveSystemctl(const QString &dir, const QString &userUnits) {
    const QString path = dir + "/systemctl";
    write(path, QString(R"sh(#!/bin/sh
D='%1'
U='%2'
echo "systemctl $*" >> "$D/calls.log"
[ "$1" = --user ] && shift
verb=$1
shift
prop() { cat "$D/sd/$1/$2" 2>/dev/null; }
setprop() { mkdir -p "$D/sd/$1"; printf %s "$3" > "$D/sd/$1/$2"; }
[ -f "$D/on-$verb.sh" ] && . "$D/on-$verb.sh"
if [ -f "$D/fail-$verb" ]; then rm -f "$D/fail-$verb"; echo refused >&2; exit 1; fi
case $verb in
show)
  props=
  value=
  units=
  while [ $# -gt 0 ]; do
    case $1 in
      -p) props=$2; shift 2 ;;
      --value) value=1; shift ;;
      --*) shift ;;
      *) units="$units $1"; shift ;;
    esac
  done
  first=1
  for u in $units; do
    [ -n "$value" ] || [ $first = 1 ] || echo
    first=0
    for p in $(echo "$props" | tr , ' '); do
      case $p in
        Id) v=$u ;;
        LoadState) v=$(prop "$u" LoadState); [ -n "$v" ] || v=not-found ;;
        ActiveState) v=$(prop "$u" ActiveState); [ -n "$v" ] || v=inactive ;;
        DropInPaths) v=$(ls -d "$U/$u.d/"*.conf 2>/dev/null | tr '\n' ' ' | sed 's/ $//') ;;
        *) v=$(prop "$u" "$p") ;;
      esac
      if [ -n "$value" ]; then echo "$v"; else echo "$p=$v"; fi
    done
  done ;;
is-active)
  v=$(prop "$1" ActiveState); [ -n "$v" ] || v=inactive
  echo "$v"; [ "$v" = active ]; exit ;;
is-enabled)
  v=$(prop "$1" UnitFileState); [ -n "$v" ] || v=disabled
  echo "$v"; [ "$v" = enabled ]; exit ;;
enable|disable|start|stop)
  now=
  for u in "$@"; do
    case $u in
      --now) now=1 ;;
      --*) ;;
      *) case $verb in
           enable) setprop "$u" UnitFileState enabled; [ -z "$now" ] || setprop "$u" ActiveState active ;;
           disable) setprop "$u" UnitFileState disabled; [ -z "$now" ] || setprop "$u" ActiveState inactive ;;
           start) setprop "$u" ActiveState active ;;
           stop) setprop "$u" ActiveState inactive ;;
         esac ;;
    esac
  done ;;
esac
exit 0
)sh").arg(dir, userUnits).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// Sets one property of a unit for moveSystemctl.
bool setUnitProperty(const QString &dir, const QString &unit, const QString &property, const QString &value) {
    return QDir().mkpath(dir + "/sd/" + unit) && write(dir + "/sd/" + unit + "/" + property, value.toUtf8());
}

QString unitProperty(const QString &dir, const QString &unit, const QString &property) {
    return QString::fromUtf8(readBytes(dir + "/sd/" + unit + "/" + property));
}

// A machine in legacy mode, ready to move, inside dir: the config at
// config/config.json (folder dir/Drive, work dir app/bisync as a first setup
// makes it, filters config/filters.txt, log dir/drive-sync.log), the state
// root app/ with an empty fix.lock, and moveSystemctl answering that the
// three account1 instances load from the package and that the plain timer
// is enabled and active. This process shows as drive-sync in the fake
// /proc, as the installed program does, so a check that mistook the
// move's own takeover lock for another's would fail here too.
struct MoveHome {
    QString dir;
    AccountRoots roots;
    TakeoverPaths tools;
    QString config;  // the legacy config file
};

MoveHome makeMoveHome(const QString &dir, const QString &runOnlyWhen = "true") {
    MoveHome h;
    h.dir = dir;
    h.roots = accountRoots(dir);
    const QString systemctl = moveSystemctl(dir, h.roots.userUnits);
    h.roots.base.systemctl = systemctl;
    h.tools.userUnits = h.roots.userUnits;
    h.tools.binDir = dir + "/bin";
    h.tools.oldStateDir = dir + "/state";
    h.tools.appConfigDir = h.roots.configRoot;
    h.tools.backupRoot = dir + "/backup";
    h.tools.systemctl = systemctl;
    h.tools.appState = h.roots.stateRoot;
    h.tools.systemUserUnits = dir + "/etc";
    h.config = h.roots.configRoot + "/config.json";
    QDir().mkpath(dir + "/Drive");
    QDir().mkpath(h.roots.stateRoot + "/bisync");
    SyncConfig config;
    config.remote = "gdrive:";
    config.localPath = dir + "/Drive";
    config.workDir = h.roots.stateRoot + "/bisync";
    config.filtersFile = h.roots.configRoot + "/filters.txt";
    config.logFile = dir + "/drive-sync.log";
    config.runOnlyWhen = runOnlyWhen;
    QString error;
    QDir().mkpath(h.roots.configRoot);
    saveSyncConfig(h.config, config, &error);
    write(config.filtersFile, "- .git/**\n");
    write(h.roots.stateRoot + "/fix.lock", "");
    const QString packaged = "/usr/lib/systemd/user/";
    const QList<QPair<QString, QString>> units{{"drive-sync@account1.service", "drive-sync@.service"},
                                               {"drive-sync@account1.timer", "drive-sync@.timer"},
                                               {"drive-sync-fix@account1.service", "drive-sync-fix@.service"},
                                               {"drive-sync.service", "drive-sync.service"},
                                               {"drive-sync.timer", "drive-sync.timer"},
                                               {"drive-sync-fix.service", "drive-sync-fix.service"}};
    for (const auto &[unit, file] : units) {
        setUnitProperty(dir, unit, "LoadState", "loaded");
        setUnitProperty(dir, unit, "FragmentPath", packaged + file);
        setUnitProperty(dir, unit, "ActiveState", "inactive");
        setUnitProperty(dir, unit, "UnitFileState", unit.endsWith(".timer") ? "disabled" : "static");
    }
    setUnitProperty(dir, "drive-sync.timer", "ActiveState", "active");
    setUnitProperty(dir, "drive-sync.timer", "UnitFileState", "enabled");
    fakeProcess(h.roots.base.proc, int(QCoreApplication::applicationPid()), "drive-sync");
    return h;
}

// Every state entry the move carries, in the legacy state root, with the
// paths a real one names: JSON in both formats DriveSync writes (compact and
// indented), so a rollback can be checked byte for byte.
bool fillLegacyState(const MoveHome &h) {
    const QString root = h.roots.stateRoot;
    const QString backup = root + "/allowed-backup/20261119-101010";
    const QJsonObject setup{{"config", h.config},
                            {"filters", h.roots.configRoot + "/filters.txt"},
                            {"createdFolders", QJsonArray{root + "/bisync", h.dir + "/Drive"}}};
    const QJsonObject settings{{"config", h.config},
                               {"filtersFile", h.roots.configRoot + "/filters.txt"},
                               {"createdFolders", QJsonArray{root + "/bisync"}},
                               {"fix", 1794067200},
                               {"applied", false}};
    const QByteArray compactRun = QJsonDocument(QJsonObject{{"at", 1794067200}, {"backupDir", backup},
                                                            {"confirmed", QJsonArray{"2:a.txt"}}})
                                      .toJson(QJsonDocument::Compact);
    return write(root + "/paused.json", R"({"since":1794067200})") && write(root + "/fix.json", "{\n}\n")
           && write(root + "/fix-answers.json", "{}") && write(root + "/fix-dry-run.log", "dry run\n")
           && write(root + "/first-setup.json", QJsonDocument(setup).toJson())
           && write(root + "/settings-change.json", QJsonDocument(settings).toJson())
           && write(root + "/allow-deletes.json", "{}") && write(root + "/allowed-dry-run.log", "allowed\n")
           && write(root + "/allowed-run.json", compactRun) && QDir().mkpath(backup)
           && write(backup + "/a.txt", "kept\n")
           && write(root + "/unconfirmed-deletes.json",
                    QJsonDocument(QJsonObject{{"backupDir", backup}}).toJson(QJsonDocument::Compact))
           && write(root + "/notify-run.json", "{}") && write(root + "/notify-watchdog.json", "{}")
           && write(root + "/identity-hold.json", "{}") && write(root + "/conflicts.json", "{}")
           && write(root + "/bar-declined", "") && write(root + "/bisync/session.path1.lst", "# bisync listing v1\n")
           && QDir().mkpath(h.roots.userUnits + "/drive-sync.timer.d")
           && write(h.roots.userUnits + "/drive-sync.timer.d/drive-sync-interval.conf",
                    "[Timer]\nOnUnitActiveSec=\nOnBootSec=5min\nOnUnitActiveSec=30min\n")
           && QDir().mkpath(h.tools.backupRoot + "/20261101-090000")
           && write(h.tools.backupRoot + "/20261101-090000/manifest.json", "{\"localPath\": \"x\"}\n");
}

// Every file of the home but the fake's own bookkeeping.
QMap<QString, QByteArray> moveHomeFiles(const MoveHome &h) {
    return filesUnder(h.dir, {"calls.log", "sd/", "proc/", "systemctl"});
}

// The systemctl calls that change something, in order.
QStringList changingCalls(const QString &dir) {
    QStringList found;
    for (const QString &call : calls(dir))
        if (call.startsWith("systemctl") && !call.startsWith("systemctl --user show"))
            found << call;
    return found;
}

QJsonObject readJsonObject(const QString &path) { return QJsonDocument::fromJson(readBytes(path)).object(); }

// A move that stopped after the first k of its planned renames: the journal
// as the move writes it (every folder it would make listed as made), the
// plain timer already off, the renames done by hand. With instanceOn, the
// instance timer is on too, as just before the journal goes.
bool crashedMove(const MoveHome &h, int k, bool instanceOn = false) {
    const auto moves = plannedMoves(h.roots, resolveAccounts(h.roots).accounts.first());
    QJsonArray list;
    for (const auto &[from, to] : moves)
        list.append(QJsonObject{{"from", from}, {"to", to}});
    const QStringList folders{h.roots.stateRoot + "/accounts", h.roots.stateRoot + "/accounts/account1",
                              h.roots.userUnits + "/drive-sync@account1.timer.d", h.roots.configRoot + "/accounts",
                              h.roots.configRoot + "/accounts/account1"};
    if (!write(migrationJournalPath(h.roots.stateRoot),
               QJsonDocument(QJsonObject{{"version", 1}, {"account", "account1"}, {"moves", list},
                                         {"created", QJsonArray::fromStringList(folders)},
                                         {"plainTimerEnabled", true}, {"plainTimerActive", true},
                                         {"stuck", false}, {"stuckReason", ""}})
                   .toJson()))
        return false;
    setUnitProperty(h.dir, "drive-sync.timer", "UnitFileState", "disabled");
    setUnitProperty(h.dir, "drive-sync.timer", "ActiveState", "inactive");
    for (int i = 0; i < k && i < moves.size(); ++i)
        if (!QDir().mkpath(QFileInfo(moves.at(i).second).absolutePath())
            || !QDir().rename(moves.at(i).first, moves.at(i).second))
            return false;
    if (instanceOn) {
        setUnitProperty(h.dir, "drive-sync@account1.timer", "UnitFileState", "enabled");
        setUnitProperty(h.dir, "drive-sync@account1.timer", "ActiveState", "active");
    }
    return true;
}

// makeMoveHome's machine in accounts mode: no single config, and each of ids
// an account of its own (folder dir/<id>/Drive) whose service loads and
// whose instance timer is enabled and active.
MoveHome makeAccountsHome(const QString &dir, const QStringList &ids) {
    MoveHome h = makeMoveHome(dir);
    QFile::remove(h.config);
    for (const QString &id : ids) {
        makeStatusAccount(h.roots, dir, id, QString());
        const BackendPaths account = newAccountPaths(h.roots, id);
        setUnitProperty(dir, account.serviceUnit, "LoadState", "loaded");
        setUnitProperty(dir, account.timerUnit, "LoadState", "loaded");
        setUnitProperty(dir, account.timerUnit, "UnitFileState", "enabled");
        setUnitProperty(dir, account.timerUnit, "ActiveState", "active");
    }
    return h;
}

// A move that stopped halfway and cannot be undone: the journal, every
// state file moved, and then a pause written at the old place too, with
// other bytes, so no rollback can tell which mark is right.
bool stuckMove(const MoveHome &h) {
    const auto moves = plannedMoves(h.roots, resolveAccounts(h.roots).accounts.first());
    QJsonArray list;
    for (const auto &[from, to] : moves)
        list.append(QJsonObject{{"from", from}, {"to", to}});
    if (!write(migrationJournalPath(h.roots.stateRoot),
               QJsonDocument(QJsonObject{{"version", 1}, {"account", "account1"}, {"moves", list},
                                         {"created", QJsonArray{}}, {"plainTimerEnabled", true},
                                         {"plainTimerActive", true}, {"stuck", false}, {"stuckReason", ""}})
                   .toJson()))
        return false;
    setUnitProperty(h.dir, "drive-sync.timer", "UnitFileState", "disabled");
    setUnitProperty(h.dir, "drive-sync.timer", "ActiveState", "inactive");
    if (!QDir().mkpath(h.roots.stateRoot + "/accounts/account1"))
        return false;
    for (const auto &[from, to] : moves)
        if (from.startsWith(h.roots.stateRoot + "/") && !QDir().rename(from, to))
            return false;
    return write(h.roots.stateRoot + "/paused.json", R"({"since":1794070000})");
}

// First setup over the resolver's roots inside dir, with nothing configured:
// setupSystemctl over account1's state folder, an rclone that knows the
// drive-sync remote, and the folder dir/Drive.
struct AccountSetup {
    AccountRoots roots;
    TakeoverPaths takeover;
    QString folder;
};

AccountSetup makeAccountSetup(const QString &dir) {
    AccountSetup s;
    s.roots = accountRoots(dir);
    const QString systemctl = setupSystemctl(dir, newAccountPaths(s.roots, "account1").appState);
    s.roots.base.systemctl = systemctl;
    s.takeover.userUnits = s.roots.userUnits;
    s.takeover.binDir = dir + "/bin";
    s.takeover.oldStateDir = dir + "/state";
    s.takeover.appConfigDir = s.roots.configRoot;
    s.takeover.backupRoot = dir + "/backup";
    s.takeover.systemctl = systemctl;
    s.takeover.rclone = fakeSignInRclone(dir);
    s.takeover.packagedUnits = dir + "/packaged";
    s.takeover.appState = s.roots.stateRoot;
    s.takeover.systemUserUnits = dir + "/etc";
    write(dir + "/remotes", "drive-sync:\n");
    s.folder = dir + "/Drive";
    return s;
}

// "drive-sync setup --folder <folder> --json" as main runs it: resolved,
// chosen, then setupAccountPaths; *runs counts the tries.
CommandResult setupThroughMain(const AccountSetup &s, int *runs = nullptr) {
    CommandResult result;
    QTextStream out(&result.out);
    QTextStream err(&result.err);
    result.code = dispatchToAccounts(
        "setup", {"--folder", s.folder, "--json"}, resolveAccounts(s.roots), s.roots, out, err,
        [&](const QStringList &rest, const QList<BackendPaths> &accounts) {
            if (runs)
                ++*runs;
            return runSetupCommand(rest, setupAccountPaths(s.roots, accounts.first()), s.takeover, out, err);
        });
    out.flush();
    err.flush();
    return result;
}

// Every file named name under root, by path relative to root.
QStringList filesNamed(const QString &root, const QString &name) {
    QStringList found;
    for (const QString &path : filesUnder(root).keys())
        if (QFileInfo(path).fileName() == name)
            found << path;
    return found;
}

}  // namespace

class DrivesyncTests : public QObject {
    Q_OBJECT

    // Every rclone a test starts, real or through a command under test, reads
    // this scratch config and finds this xdg-open first on PATH: no test may
    // touch ~/.config/rclone/rclone.conf or open a real Google sign-in.
    QTemporaryDir m_sandbox;

private slots:
    void initTestCase()
    {
        QVERIFY(m_sandbox.isValid());
        QVERIFY(write(m_sandbox.filePath("rclone.conf"), ""));
        qputenv("RCLONE_CONFIG", m_sandbox.filePath("rclone.conf").toLocal8Bit());
        const QString opener = m_sandbox.filePath("bin/xdg-open");
        QVERIFY(QDir().mkpath(m_sandbox.filePath("bin")));
        QVERIFY(write(opener, QString("#!/bin/sh\necho \"$*\" >> '%1'\n").arg(m_sandbox.filePath("opened.log")).toUtf8()));
        QFile::setPermissions(opener, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", m_sandbox.filePath("bin").toLocal8Bit() + ":" + qgetenv("PATH"));
    }

    // The client is whole or absent: half a client would send a sign-in
    // Google rejects. Only lengths are printed, never the values.
    void shippedOAuthClientIsWholeOrMissing()
    {
        const auto client = shippedOAuthClient();
        if (!client)
            return;
        QVERIFY2(!client->id.isEmpty(), "the shipped client has no id");
        QVERIFY2(!client->secret.isEmpty(), qPrintable(QString("id of %1 characters, no secret").arg(client->id.size())));
        QVERIFY2(client->id.endsWith(".apps.googleusercontent.com"),
                 qPrintable(QString("the id (%1 characters) is not a Google client id").arg(client->id.size())));
    }

    void parsesASuccessfulRun()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(15, 28, 19));
        const auto runs = SyncLog::parse(successfulRun(start, 485));
        QCOMPARE(runs.size(), 1);
        const SyncRun &run = runs.first();
        QCOMPARE(run.outcome, QString("success"));
        QCOMPARE(run.start, start);
        QCOMPARE(run.start.secsTo(run.end), 485);
        QCOMPARE(run.cloud.modified, 1);
        QCOMPARE(run.notebook.added, 1);
        QCOMPARE(run.listed, 16680LL);
        QVERIFY(!run.resync);
        QCOMPARE(run.changes.size(), 2);  // The "Queue copy" line repeats a change.
        QCOMPARE(run.changes.at(0).side, QString("cloud"));
        QCOMPARE(run.changes.at(0).kind, QString("modified"));
        QCOMPARE(run.changes.at(0).path, QString("Notes/todo.md"));
        QCOMPARE(run.changes.at(1).side, QString("notebook"));
        QCOMPARE(run.changes.at(1).kind, QString("new"));
        QVERIFY(run.errors.isEmpty());
    }

    void readsTheCauseOfAFailedRun()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 48, 47));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
        log += "Tip: here are the filenames we were looking for. Do they exist? \n";
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += line(start.addSecs(1), "NOTICE", "Failed to bisync: bisync aborted");

        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);  // The trailing lines belong to the same run.
        QCOMPARE(runs.first().outcome, QString("failed"));
        QCOMPARE(runs.first().causeKind, QString("lostListings"));
        QCOMPARE(runs.first().errors.size(), 3);
    }

    void namesTheFirstRealErrorAsTheCause()
    {
        const QDateTime start(QDate(2026, 11, 6), QTime(20, 10, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(30), "ERROR", "-                   Size not equal in listing. Path1: 5972, Path2: 7617 - GitHub/app/x.yml");
        log += line(start.addSecs(30), "ERROR", "Bisync aborted. Must run --resync to recover.");
        const auto outOfSync = SyncLog::parse(log);
        QCOMPARE(outOfSync.first().causeKind, QString("error"));
        QVERIFY(outOfSync.first().causeDetail.startsWith("Size not equal in listing"));

        log += line(start.addSecs(31), "ERROR", "Bisync critical error: path1 and path2 are out of sync, run --resync to recover");
        QCOMPARE(SyncLog::parse(log).first().causeKind, QString("outOfSync"));
    }

    void countsAnErrorBeforeAnyRunAsAFailedRun()
    {
        const QDateTime when(QDate(2026, 11, 7), QTime(9, 0, 0));
        QString log = successfulRun(when.addSecs(-3600), 480);
        log += line(when, "CRITICAL", "Failed to create file system for \"gdrive:\": couldn't find root directory ID");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("success"));
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("error"));
        QVERIFY(runs.last().causeDetail.contains("couldn't find root directory ID"));
    }

    void marksCutOffAndOpenRuns()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(60), "INFO", "Path1 checking for diffs");
        log += line(start.addSecs(900), "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start.addSecs(960), "INFO", "Applying changes");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.first().outcome, QString("interrupted"));
        QCOMPARE(runs.last().outcome, QString("open"));
        QCOMPARE(runs.last().phase, QString("applying"));
        QCOMPARE(runs.last().lastSeen, start.addSecs(960));
    }

    void skipsARunWhoseStartWasCutOff()
    {
        const QDateTime start(QDate(2026, 11, 7), QTime(10, 0, 0));
        QString log;
        log += line(start, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
        log += successfulRun(start.addSecs(600), 480);
        QCOMPARE(SyncLog::parse(log, 40, true).size(), 1);
        QCOMPARE(SyncLog::parse(log, 40, false).size(), 2);
    }

    void spotsAResyncAndCapsItsChanges()
    {
        const QDateTime start(QDate(2026, 11, 2), QTime(9, 5, 16));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", "Copying Path2 files to Path1");
        for (int i = 0; i < 305; ++i)
            log += line(start, "INFO", change("Path2", "File is new", QString("f%1.txt").arg(i)));
        log += line(start.addSecs(9000), "INFO", "Resync updating listings");
        log += line(start.addSecs(9002), "INFO", "Bisync successful");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);
        QVERIFY(runs.first().resync);
        QCOMPARE(runs.first().changes.size(), 300);
        QCOMPARE(runs.first().changesOmitted, 5);
    }

    // rclone 1.75.2 writes no "Path1: N changes" summary in a resync, only
    // one "Copied" line per file after the line that names the direction
    // (seen in the owner's log, 2026-11-17 15:21: 193 copies, "No changes").
    void countsTheCopiesOfAResync()
    {
        const QDateTime start(QDate(2026, 11, 17), QTime(15, 21, 49));
        QString log;
        log += line(start, "INFO", R"(Synching Path1 "drive-sync{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", "Copying Path2 files to Path1");
        log += line(start, "INFO", "- Path2             Resync is copying files to                  - Path1");
        log += line(start.addSecs(30), "INFO", "Notes/new.md: Copied (new)");
        log += line(start.addSecs(31), "INFO", "Notes/a: b.md: Copied (new)");
        log += line(start.addSecs(32), "INFO", "Notes/old.md: Copied (replaced existing)");
        log += line(start.addSecs(33), "INFO", "Notes/time.md: Updated modification time in destination");
        log += line(start.addSecs(190), "INFO", "- Path1             Resync is copying files to                  - Path2");
        log += line(start.addSecs(200), "INFO", "Photos/beach.jpg: Copied (new)");
        log += line(start.addSecs(330), "INFO", "Resync updating listings");
        log += line(start.addSecs(340), "INFO", "Bisync successful");
        // A normal run copies too, but its summary lines count it already.
        QString normal = successfulRun(start.addSecs(900), 60);
        normal.insert(normal.indexOf("Updating listings") - 28,
                      line(start.addSecs(950), "INFO", "GitHub/app/README.md: Copied (new)"));
        log += normal;
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        const SyncRun &resync = runs.first();
        QVERIFY(resync.resync);
        QCOMPARE(resync.notebook.added, 2);
        QCOMPARE(resync.notebook.modified, 1);
        QCOMPARE(resync.cloud.added, 1);
        QCOMPARE(resync.cloud.modified, 0);
        QCOMPARE(resync.changes.size(), 4);
        QCOMPARE(resync.changes.at(1).path, QString("Notes/a: b.md"));
        QCOMPARE(resync.changes.at(1).side, QString("notebook"));
        QCOMPARE(resync.changes.at(2).kind, QString("modified"));
        QCOMPARE(resync.changes.at(3).side, QString("cloud"));
        QVERIFY(!runs.last().resync);
        QCOMPARE(runs.last().notebook.added, 1);
        QCOMPARE(runs.last().changes.size(), 2);
    }

    void keepsTheNewestRuns()
    {
        QString log;
        const QDateTime start(QDate(2026, 11, 7), QTime(0, 0, 0));
        for (int i = 0; i < 5; ++i)
            log += successfulRun(start.addSecs(i * 900), 480);
        const auto runs = SyncLog::parse(log, 3);
        QCOMPARE(runs.size(), 3);
        QCOMPARE(runs.last().start, start.addSecs(4 * 900));
    }

    void readsTheTailFromAWholeLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("drive-sync.log");
        QVERIFY(write(path, "first line\nsecond line\nthird line\n"));
        bool found = false, truncated = false;
        QCOMPARE(SyncLog::readTail(path, 20, &found, &truncated), QString("third line\n"));
        QVERIFY(found);
        QVERIFY(truncated);
        QCOMPARE(SyncLog::readTail(path, 1000, &found, &truncated).count('\n'), 3);
        QVERIFY(!truncated);
        SyncLog::readTail(dir.filePath("missing.log"), 1000, &found);
        QVERIFY(!found);
    }

    // A log with no run (new, or just rotated) proves nothing: not green.
    void summarizesALogWithNoRuns()
    {
        SyncInputs in = healthy();
        in.lastStart = QDateTime();
        in.lastOutcome.clear();
        in.lastSuccess = QDateTime();
        QCOMPARE(Backend::summarize(in).status, QString("noRuns"));
        QCOMPARE(toneFor("noRuns"), QString("warning"));
        in.conflicts = 2;
        QCOMPARE(Backend::summarize(in).status, QString("noRuns"));
        QCOMPARE(Backend::summarize(in).warnings, QStringList{"conflicts"});
        in.driveFound = false;  // the more urgent headline wins
        QCOMPARE(Backend::summarize(in).status, QString("noDrive"));

        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources("", t0);
        src.driveFound = true;
        const Snapshot snap = buildSnapshot(src);
        QCOMPARE(snap.summary.status, QString("noRuns"));
        QCOMPARE(snap.tone, QString("warning"));
        QVERIFY(!snap.lastSuccess.isValid());
    }

    void summarizesAHealthySync()
    {
        const auto s = Backend::summarize(healthy());
        QCOMPARE(s.status, QString("ok"));
        QVERIFY(s.warnings.isEmpty());
    }

    void putsTheWorstProblemFirst()
    {
        SyncInputs in = healthy();
        in.lastOutcome = "failed";
        in.lastCauseKind = "lostListings";
        in.conflicts = 2;
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("lostListings"));
        QCOMPARE(s.warnings, QStringList{"conflicts"});

        in.running = true;
        QCOMPARE(Backend::summarize(in).status, QString("running"));

        in = healthy();
        in.conflicts = 1;
        in.timerFileState = "disabled";
        s = Backend::summarize(in);
        QCOMPARE(s.status, QString("conflicts"));
        QCOMPARE(s.warnings, QStringList{"timerDisabled"});

        in = healthy();
        in.serviceLoad = "not-found";
        QCOMPARE(Backend::summarize(in).status, QString("notLoaded"));
    }

    void trustsSystemdOnlyForItsNewestRun()
    {
        SyncInputs in = healthy();
        in.serviceResult = "timeout";
        auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("failed"));
        QCOMPARE(s.causeKind, QString("timeout"));

        // A resync from drive-sync.sh ran after the service failed, and worked.
        in.lastStart = in.serviceStart.addSecs(3600);
        QCOMPARE(Backend::summarize(in).status, QString("ok"));
    }

    void tellsATimerLeftOffFromAResync()
    {
        SyncInputs in = healthy();
        in.timerActive = "inactive";
        QCOMPARE(Backend::summarize(in).status, QString("timerOff"));

        in.resyncRunning = true;
        const auto s = Backend::summarize(in);
        QCOMPARE(s.status, QString("resync"));
        QVERIFY(s.warnings.isEmpty());

        in.resyncRunning = false;
        in.lastOutcome = "failed";
        QCOMPARE(Backend::summarize(in).warnings, QStringList{"timerOff"});
    }

    void noticesRunsThatStopped()
    {
        SyncInputs in = healthy();
        in.lastSuccess = in.now.addSecs(-3 * 3600);
        QCOMPARE(Backend::summarize(in).status, QString("stale"));
        in.lastOutcome = "interrupted";
        QCOMPARE(Backend::summarize(in).status, QString("interrupted"));
    }

    void parsesSystemctl()
    {
        const auto units = Backend::parseShow("LoadState=loaded\nActiveState=inactive\nExecMainStartTimestamp=@1794076098\n\n"
                                              "LoadState=loaded\nActiveState=active\n");
        QCOMPARE(units.size(), 2);
        QCOMPARE(units.at(0).value("ExecMainStartTimestamp"), QString("@1794076098"));
        QCOMPARE(units.at(1).value("ActiveState"), QString("active"));

        const QDateTime next =
            Backend::parseNextElapse(R"([{"next":1794076998808087,"unit":"drive-sync.timer"}])", "drive-sync.timer");
        QCOMPARE(next.toMSecsSinceEpoch(), 1794076998808LL);
        QVERIFY(!Backend::parseNextElapse("[]", "drive-sync.timer").isValid());
        QVERIFY(!Backend::parseNextElapse(R"([{"next":0,"unit":"drive-sync.timer"}])", "drive-sync.timer").isValid());
    }

    void checksThePidInTheResyncMark()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("state");
        QDir(dir.path()).mkpath("proc/4242");
        QDir(dir.path()).mkpath("proc/4343");
        const QString state = dir.filePath("state"), proc = dir.filePath("proc");
        QVERIFY(!Backend::resyncRunning(state, proc));

        QVERIFY(write(dir.filePath("proc/4242/cmdline"), QByteArray("bash\0/home/someone/.local/bin/drive-sync.sh\0resync\0", 52)));
        QVERIFY(write(dir.filePath("proc/4343/cmdline"), QByteArray("vim\0notes.md\0", 13)));
        QVERIFY(write(state + "/pausado", "4242\n"));
        QVERIFY(Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "4343\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
        QVERIFY(write(state + "/pausado", "9999\n"));
        QVERIFY(!Backend::resyncRunning(state, proc));
    }

    void watchesARealFolder()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive/Notes");
        QDir(dir.path()).mkpath("Drive/.git");
        QDir(dir.path()).mkpath("state");
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md.conflict1"), "older"));
        QVERIFY(write(dir.filePath("Drive/.git/index.conflict1"), "never synced"));
        QVERIFY(write(dir.filePath("Drive/Notes/todo.md"), "newer"));
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));

        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find")};
        paths.appState = dir.filePath("app");
        Backend backend(paths);
        QTRY_COMPARE(backend.conflicts().size(), 1);
        // The scan leaves its count for the status command and the bar.
        QTRY_COMPARE(readConflictCache(paths.appState + "/conflicts.json").count, 1);
        QCOMPARE(backend.conflicts().first().toMap().value("path").toString(), QString("Notes/todo.md.conflict1"));
        QCOMPARE(backend.conflicts().first().toMap().value("folder").toString(), QString("Notes"));
        QTRY_COMPARE(backend.status(), QString("conflicts"));
        QCOMPARE(backend.runs().size(), 1);
        QCOMPARE(backend.typicalSeconds(), 480);
        QVERIFY(backend.nextRun() > QDateTime::currentDateTime());
        QVERIFY(backend.currentRun().isEmpty());

        // A run starts: the log knows before the systemd poll does.
        QFile log(paths.log);
        QVERIFY(log.open(QIODevice::Append));
        log.write(line(QDateTime::currentDateTime(), "INFO",
                       R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")").toUtf8());
        log.close();
        QTRY_COMPARE(backend.status(), QString("running"));
        QCOMPARE(backend.currentRun().value("phase").toString(), QString("listing"));
        QCOMPARE(backend.runs().size(), 1);
    }

    // The window colors itself from the same tone the bar reads.
    void backendExposesTone()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                                 QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QCOMPARE(backend.tone(), QString("ok"));
    }

    void reportsAMissingDriveAndLog()
    {
        QTemporaryDir dir;
        BackendPaths paths{dir.filePath("missing.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), QDateTime::currentDateTime()),
                           QStandardPaths::findExecutable("find")};
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("noLog"));
        QCOMPARE(backend.conflictError(), QString("noDrive"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Runs").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Runs": "Execuções", "Quit": "Sair"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Runs": "Rodadas"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Runs"), QString("Rodadas"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Sair"));
        QVERIFY(translator.translate("Main", "Space").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // The shipped catalogs: valid JSON, and every plural string of the
    // Portuguese one has the forms pt-BR uses.
    void shippedCatalogsParse()
    {
        for (const QString &code : {QString("en"), QString("pt-BR")}) {
            QFile file(QString(SRCDIR) + "/../src/i18n/" + code + ".json");
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            QJsonParseError error;
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll(), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(code + ": " + error.errorString()));
            QVERIFY(!catalog.isEmpty());
            for (auto it = catalog.begin(); it != catalog.end(); ++it)
                if (it.value().isObject() && code == "pt-BR")
                    QVERIFY2(it.value().toObject().contains("one") && it.value().toObject().contains("other"),
                             qPrintable(it.key()));
        }
    }

    // The tone groups every headline the way the window colors it.
    void toneGroupsEveryStatus()
    {
        for (const char *s : {"failed", "timerOff", "unavailable", "notLoaded", "noLog", "noDrive", "fixFailed"})
            QCOMPARE(toneFor(s), QString("error"));
        for (const char *s : {"interrupted", "skipped", "stale", "noRuns", "conflicts", "fixWaiting"})
            QCOMPARE(toneFor(s), QString("warning"));
        QCOMPARE(toneFor("running"), QString("busy"));
        QCOMPARE(toneFor("resync"), QString("busy"));
        QCOMPARE(toneFor("fixing"), QString("busy"));
        QCOMPARE(toneFor("ok"), QString("ok"));
        // A status nobody gave a tone is a mistake, and must not look fine.
        QCOMPARE(toneFor("somethingNew"), QString("error"));
        QCOMPARE(toneFor(""), QString("error"));
    }

    // Two failures after a success: the problem started with the first one.
    void snapshotFailingSinceIsFirstFailureAfterSuccess()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const QString log = successfulRun(t0, 480) + failedRun(t0.addSecs(900)) + failedRun(t0.addSecs(1800));
        const SnapshotSources src = idleSources(log, t0.addSecs(2000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("failed"));
        QCOMPARE(s.failingSince, src.runs.at(1).start);
    }

    // The log ends in a success but systemd says the last start failed: the
    // trouble began with that start, not with the good run before it.
    void snapshotFailingSinceForAFailureOnlySystemdSaw()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources(successfulRun(t0, 480), t0.addSecs(1000));
        const QDateTime started = t0.addSecs(900).toUTC();
        src.service.insert("Result", "exit-code");
        src.service.insert("ExecMainStartTimestamp", QString("@%1").arg(started.toSecsSinceEpoch()));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("failed"));
        QCOMPARE(s.summary.causeKind, QString("systemd"));
        QCOMPARE(s.failingSince, started);
    }

    void snapshotNoFailingSinceWhenOk()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const Snapshot s = buildSnapshot(idleSources(successfulRun(t0, 480), t0.addSecs(600)));
        QCOMPARE(s.summary.status, QString("ok"));
        QCOMPARE(s.tone, QString("ok"));
        QVERIFY(!s.failingSince.isValid());
    }

    // A manual run only makes sense with the unit loaded and nothing going.
    void snapshotCanSyncNowOnlyWhenIdle()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        SnapshotSources src = idleSources(successfulRun(t0, 480), t0.addSecs(600));
        QVERIFY(buildSnapshot(src).canSyncNow);

        SnapshotSources busy = src;
        busy.service["ActiveState"] = "activating";
        QVERIFY(!buildSnapshot(busy).canSyncNow);

        SnapshotSources resync = src;
        resync.resyncRunning = true;
        QVERIFY(!buildSnapshot(resync).canSyncNow);

        SnapshotSources noSystemctl = src;
        noSystemctl.systemctlWorks = false;
        QVERIFY(!buildSnapshot(noSystemctl).canSyncNow);

        SnapshotSources notLoaded = src;
        notLoaded.service["LoadState"] = "not-found";
        QVERIFY(!buildSnapshot(notLoaded).canSyncNow);
    }

    void snapshotLastRunIsNewestFinished()
    {
        const QDateTime t0(QDate(2026, 11, 7), QTime(12, 0, 0));
        const SnapshotSources src = idleSources(successfulRun(t0, 480) + failedRun(t0.addSecs(900)), t0.addSecs(1000));
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.lastRunStart, src.runs.at(1).start);
        QCOMPARE(s.lastRunEnd, src.runs.at(1).end);
        QCOMPARE(s.lastRunOutcome, QString("failed"));
        QCOMPARE(s.lastSuccess, src.runs.at(0).end);
    }

    // Version 1 of what "drive-sync status --json" prints: every key always
    // there, times as Unix seconds, 0 when unknown.
    void statusJsonContract()
    {
        Snapshot s;
        s.summary.status = "failed";
        s.summary.causeKind = "outOfSync";
        s.summary.warnings = {"timerDisabled"};
        s.tone = "error";
        s.lastSuccess = QDateTime(QDate(2026, 11, 7), QTime(15, 0, 0), QTimeZone::UTC);
        s.failingSince = QDateTime(QDate(2026, 11, 7), QTime(15, 30, 0), QTimeZone::UTC);
        s.conflicts = 2;
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        const QJsonObject json = snapshotToJson(s, QDateTime(), now);
        QCOMPARE(json.value("version").toInt(), 2);
        QCOMPARE(json.value("now").toInteger(), 1794067200);
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("tone").toString(), QString("error"));
        QCOMPARE(json.value("lastSuccess").toInteger(), 1794063600);
        QCOMPARE(json.value("failingSince").toInteger(), 1794065400);
        QCOMPARE(json.value("nextRun").toInteger(), 0);
        QCOMPARE(json.value("warnings").toArray(), QJsonArray{"timerDisabled"});
        QCOMPARE(json.value("conflicts").toInt(), 2);
        QVERIFY(json.value("lastRun").toObject().value("outcome").isString());
        QCOMPARE(json.value("lastRun").toObject().value("start").toInteger(), 0);
        QVERIFY(json.value("currentRun").toObject().value("phase").isString());
        QCOMPARE(json.value("managed").toBool(), false);
        QCOMPARE(json.value("paused").toBool(), false);
        QCOMPARE(json.value("pausedUntil").toInteger(), 0);
        QCOMPARE(json.keys().size(), 20);

        s.managed = true;
        s.paused = true;
        s.pausedUntil = QDateTime(QDate(2026, 11, 7), QTime(17, 0, 0), QTimeZone::UTC);
        const QJsonObject paused = snapshotToJson(s, QDateTime(), now);
        QCOMPARE(paused.value("managed").toBool(), true);
        QCOMPARE(paused.value("paused").toBool(), true);
        QCOMPARE(paused.value("pausedUntil").toInteger(), 1794070800);
    }

    void conflictCacheRoundTrip()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("state/conflicts.json");  // the folder does not exist yet
        ConflictCount c;
        c.count = 3;
        c.scannedAt = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QVERIFY(writeConflictCache(file, c));
        const ConflictCount back = readConflictCache(file);
        QCOMPARE(back.count, 3);
        QCOMPARE(back.error, QString());
        QCOMPARE(back.scannedAt, c.scannedAt);
        QVERIFY(conflictCacheFresh(back, c.scannedAt.addSecs(60)));
        QVERIFY(!conflictCacheFresh(back, c.scannedAt.addSecs(kConflictCacheSeconds)));
    }

    void conflictCacheCorruptIsStale()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("conflicts.json");
        QVERIFY(write(file, "not json"));
        const ConflictCount c = readConflictCache(file);
        QVERIFY(!c.scannedAt.isValid());
        QVERIFY(!conflictCacheFresh(c, QDateTime::currentDateTimeUtc()));
    }

    // A clock that moved back must not keep an old count alive for hours.
    void conflictCacheFromTheFutureIsStale()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        ConflictCount c;
        c.scannedAt = now.addSecs(600);
        QVERIFY(!conflictCacheFresh(c, now));
    }

    void statusCommandJson()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("version").toInt(), 2);
        QCOMPARE(json.value("status").toString(), QString("ok"));
        QVERIFY(json.value("nextRun").toInteger() > QDateTime::currentSecsSinceEpoch());
        QVERIFY(err.isEmpty());
        // The scan it had to make is left for the next call.
        QVERIFY(conflictCacheFresh(readConflictCache(paths.appState + "/conflicts.json"), QDateTime::currentDateTimeUtc()));

        // The command reads the same pause mark the window does.
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc();
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        out.clear();
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e), 0);
        const QJsonObject paused = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(paused.value("status").toString(), QString("paused"));
        QCOMPARE(paused.value("paused").toBool(), true);
        QCOMPARE(paused.value("canSyncNow").toBool(), false);
    }

    // A long outage: the last success is older than the 40 runs the window
    // lists, and the state must still say when the trouble began.
    void statusKeepsTheStartOfALongOutage()
    {
        QTemporaryDir dir;
        const QDateTime t0 = QDateTime::currentDateTime().addSecs(-46 * 900 - 600);
        QString log = successfulRun(t0, 480);
        for (int i = 1; i <= 45; ++i)
            log += failedRun(t0.addSecs(i * 900));
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), fakeSystemctl(dir.path(), t0.addSecs(45 * 900)),
                                 QStandardPaths::findExecutable("find"), dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e), 0);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("status").toString(), QString("failed"));
        QCOMPARE(json.value("failingSince").toInteger(), t0.addSecs(900).toSecsSinceEpoch());
        QCOMPARE(json.value("lastSuccess").toInteger(), t0.addSecs(480).toSecsSinceEpoch());
    }

    void statusCommandRejectsUnknownFlag()
    {
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--jsno"}, {BackendPaths{}}, {}, o, e), 1);
        QVERIFY(err.contains("usage: drive-sync"));
        QVERIFY(out.isEmpty());
    }

    // A stuck systemctl must not hang the bar: the answer is "unavailable".
    void statusCommandSystemctlTimeout()
    {
        QTemporaryDir dir;
        const QString systemctl = dir.filePath("systemctl");
        QVERIFY(write(systemctl, "#!/bin/sh\nsleep 5\n"));
        QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                                 dir.filePath("proc"), systemctl, QStandardPaths::findExecutable("find"),
                                 dir.filePath("app")};
        QString out, err;
        QTextStream o(&out), e(&err);
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e, 200), 0);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("status").toString(), QString("unavailable"));
    }

    void pluginSetupLinksWhenAbsent()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        const QFileInfo link(paths.userPlugins + "/drive-sync");
        QVERIFY(link.isSymLink());
        QCOMPARE(link.symLinkTarget(), QFileInfo(paths.shipped).absoluteFilePath());
        QVERIFY(calls(dir.path()).contains("shell-cmd shell rescanPlugins"));
    }

    // A folder the user made with the same name is theirs: never touched.
    void pluginSetupLeavesForeignFolder()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        QDir(paths.userPlugins).mkpath("drive-sync");
        QVERIFY(write(paths.userPlugins + "/drive-sync/mine.txt", "keep"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drive-sync").isSymLink());
        QFile mine(paths.userPlugins + "/drive-sync/mine.txt");
        QVERIFY(mine.open(QIODevice::ReadOnly));
        QCOMPARE(mine.readAll(), QByteArray("keep"));
        QVERIFY(calls(dir.path()).isEmpty());
    }

    // A link someone else made (a dev copy, say) is theirs too, and so is a
    // dangling one: exists() says false for it, isSymLink() does not.
    void pluginSetupLeavesForeignAndDanglingLinks()
    {
        for (const bool dangling : {false, true}) {
            QTemporaryDir dir;
            const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
            const QString elsewhere = dir.filePath(dangling ? "gone" : "dev-copy");
            if (!dangling)
                QVERIFY(QDir().mkpath(elsewhere));
            QVERIFY(QDir().mkpath(paths.userPlugins));
            const QString link = paths.userPlugins + "/drive-sync";
            QVERIFY(QFile::link(elsewhere, link));
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("blocked"));
            QCOMPARE(setup.detail(), link);
            QVERIFY(QFileInfo(link).isSymLink());
            QCOMPARE(QFileInfo(link).symLinkTarget(), elsewhere);
            QVERIFY(calls(dir.path()).isEmpty());
        }
    }

    // Drivesync's widget, renamed: the new one takes its place on the bar by
    // itself, then the old entry and its dangling link go.
    void pluginSetupTakesOverTheDrivesyncWidget()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");  // pacman removed it
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') cat '%1/list.json' ;;\n"
            "  'plugin enable drive-sync --after drivesync') echo '[{\"id\":\"drive-sync\",\"enabled\":true}]' > '%1/list.json' ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.legacyShipped, paths.userPlugins + "/drivesync"));

        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("enabled"));
        QCOMPARE(calls(dir.path()), (QStringList{"shell-cmd shell rescanPlugins", "plugin list --json",
                                                 "plugin enable drive-sync --after drivesync",
                                                 "plugin disable drivesync", "plugin list --json"}));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
        QVERIFY(QFileInfo(paths.userPlugins + "/drive-sync").isSymLink());
    }

    // The old link was there but its widget was not on the bar: the enable
    // finds no place, nothing is replaced, and the usual offer follows.
    void pluginSetupDrivesyncWidgetOffBarFallsBackToTheOffer()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') cat '%1/list.json' ;;\n"
            "  'plugin enable'*) echo 'could not find target widget drivesync' >&2; exit 1 ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(paths.legacyShipped, paths.userPlugins + "/drivesync"));

        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(!calls(dir.path()).contains("plugin disable drivesync"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
    }

    // A drivesync link that points anywhere else is not Drivesync's: never touched.
    void pluginSetupLeavesAForeignDrivesyncLink()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.legacyShipped = dir.filePath("gone/omarchy-plugin");
        QDir().mkpath(paths.userPlugins);
        QVERIFY(QFile::link(dir.filePath("mine"), paths.userPlugins + "/drivesync"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(QFileInfo(paths.userPlugins + "/drivesync").isSymLink());
        QVERIFY(!calls(dir.path()).contains("plugin enable drive-sync --after drivesync"));
    }
    void pluginSetupEnabled()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), R"([{"id":"drive-sync","enabled":true}])");
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("enabled"));
    }

    // "No" is remembered: no offer, and no link that would undo a removal.
    void pluginSetupRemembersNo()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.declined = dir.filePath("app/bar-declined");
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("offer"));
            setup.decline();
            QCOMPARE(setup.state(), QString("declined"));
        }
        QFile::remove(paths.userPlugins + "/drive-sync");  // as `omarchy plugin remove` would
        QFile::remove(dir.filePath("calls.log"));
        PluginSetup again(paths);
        again.check();
        QCOMPARE(again.state(), QString("declined"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drive-sync").isSymLink());
        QVERIFY(calls(dir.path()).isEmpty());
    }

    // omarchy failing to list its plugins at start is no banner: nobody asked
    // for anything. After B the same failure is.
    void pluginSetupListFailureNeedsAnAsk()
    {
        QTemporaryDir dir;
        const PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        const QString omarchy = QString(
            "#!/bin/sh\n"
            "echo \"$*\" >> '%1/calls.log'\n"
            "case \"$*\" in\n"
            "  'plugin list --json') [ -e '%1/list-fails' ] && { echo 'no shell' >&2; exit 1; }; cat '%1/list.json' ;;\n"
            "esac\n").arg(dir.path());
        QVERIFY(write(paths.omarchy, omarchy.toUtf8()));
        QVERIFY(write(dir.filePath("list-fails"), ""));
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("unchecked"));
            QCOMPARE(setup.detail(), QString("no shell"));
        }
        QFile::remove(dir.filePath("list-fails"));
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QVERIFY(write(dir.filePath("list-fails"), ""));
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("failed"));
        QCOMPARE(setup.detail(), QString("no shell"));
    }

    // The notice that the path is taken can be hidden for good, like the offer.
    void pluginSetupBlockedCanBeDismissed()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.declined = dir.filePath("app/bar-declined");
        QVERIFY(QDir(paths.userPlugins).mkpath("drive-sync"));
        {
            PluginSetup setup(paths);
            setup.check();
            QTRY_COMPARE(setup.state(), QString("blocked"));
            setup.decline();
            QCOMPARE(setup.state(), QString("declined"));
            QVERIFY(QFileInfo::exists(paths.declined));
        }
        PluginSetup again(paths);
        again.check();
        QCOMPARE(again.state(), QString("declined"));
        QVERIFY(QFileInfo(paths.userPlugins + "/drive-sync").isDir());  // still theirs
        QVERIFY(calls(dir.path()).isEmpty());
    }

    void pluginSetupWithoutOmarchy()
    {
        QTemporaryDir dir;
        PluginPaths paths = fakeOmarchy(dir.path(), "[]");
        paths.omarchy.clear();
        PluginSetup setup(paths);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("noOmarchy"));
        QVERIFY(!QFileInfo(paths.userPlugins + "/drive-sync").isSymLink());
    }
    void syncConfigRoundTrips()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("drive-sync/config.json");
        const SyncConfig config = oldUnitConfig();
        QString error;
        QVERIFY2(saveSyncConfig(file, config, &error), qPrintable(error));
        const auto loaded = loadSyncConfig(file, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->remote, config.remote);
        QCOMPARE(loaded->localPath, config.localPath);
        QCOMPARE(loaded->workDir, config.workDir);
        QCOMPARE(loaded->filtersFile, config.filtersFile);
        QCOMPARE(loaded->logFile, config.logFile);
        QCOMPARE(loaded->rcloneFlags, config.rcloneFlags);
        QCOMPARE(loaded->runOnlyWhen, config.runOnlyWhen);

        // Anything the app cannot trust is no config at all, with a reason.
        QVERIFY(write(dir.filePath("corrupt.json"), "{not json"));
        QVERIFY(!loadSyncConfig(dir.filePath("corrupt.json"), &error).has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(write(dir.filePath("v9.json"), R"({"version":9,"remote":"gdrive:"})"));
        QVERIFY(!loadSyncConfig(dir.filePath("v9.json"), &error).has_value());
        QVERIFY(write(dir.filePath("nolocal.json"),
                      R"({"version":1,"remote":"gdrive:","workDir":"/w","filtersFile":"/f","logFile":"/l","rcloneFlags":[]})"));
        QVERIFY(!loadSyncConfig(dir.filePath("nolocal.json"), &error).has_value());
        QVERIFY(error.contains("localPath"));
        QVERIFY(!loadSyncConfig(dir.filePath("missing.json"), &error).has_value());
    }

    void rcloneArgumentsMatchTheOldUnit()
    {
        const SyncConfig config = oldUnitConfig();
        const QStringList expected{"bisync", "gdrive:", "/home/someone/Drive",
                                   "--workdir", "/home/someone/.local/state/rclone/bisync",
                                   "--filters-file", "/home/someone/.config/drive-sync/filters.txt",
                                   "--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                                   "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                                   "--log-level", "INFO", "--fast-list",
                                   "--log-file", "/home/someone/.local/state/drive-sync.log"};
        const QStringList arguments = rcloneRunArguments(config);
        QCOMPARE(arguments, expected);

        // The same tokens the real unit passes, filters path aside, plus
        // --fast-list: no other flag invented or dropped.
        QStringList real = QProcess::splitCommand(QString(kOldExecStart)).mid(1);
        real.replace(real.indexOf("/home/someone/.config/rclone/gdrive-filtros.txt"),
                     "/home/someone/.config/drive-sync/filters.txt");
        real << "--fast-list";
        QStringList a = arguments, b = real;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        QCOMPARE(a, b);
    }

    void rcloneArgumentsAddFastListOnce()
    {
        SyncConfig config = oldUnitConfig();
        config.rcloneFlags << "--fast-list";
        QCOMPARE(rcloneRunArguments(config).count("--fast-list"), 1);

        // Turning ListR off on purpose wins over the default.
        config = oldUnitConfig();
        config.rcloneFlags << "--disable" << "ListR";
        QVERIFY(!rcloneRunArguments(config).contains("--fast-list"));
    }

    void migrationMovesLegacyFoldersKeepingEveryByte_data()
    {
        QTest::addColumn<QString>("oldName");
        QTest::newRow("syncwatch") << "syncwatch";
        QTest::newRow("drivesync") << "drivesync";
    }
    void migrationMovesLegacyFoldersKeepingEveryByte()
    {
        QFETCH(QString, oldName);
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        legacy.name = oldName;
        QVERIFY(makeLegacyHome(legacy));
        const QByteArray md5 = readBytes(dir.path() + "/.config/" + oldName + "/filters.txt.md5");

        const QStringList problems = migrateFromLegacyNames(dir.path());
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        for (const LegacyFolder &folder : legacyFolders(dir.path())) {
            QVERIFY2(!QFileInfo::exists(folder.from), qPrintable(folder.from));
            QVERIFY2(QFileInfo(folder.to).isDir(), qPrintable(folder.to));
        }
        const QString config = dir.path() + "/.config/drive-sync";
        QCOMPARE(readBytes(config + "/filters.txt"), legacy.filters);
        QCOMPARE(readBytes(config + "/filters.txt.md5"), md5);
        QVERIFY(QFile::exists(dir.path() + "/.local/state/drive-sync-app/paused.json"));

        // The config names its filters at the new place, and a run would start.
        QString error;
        const auto sync = loadSyncConfig(config + "/config.json", &error);
        QVERIFY2(sync.has_value(), qPrintable(error));
        QCOMPARE(sync->filtersFile, config + "/filters.txt");
        QCOMPARE(sync->logFile, dir.path() + "/.local/state/drive-sync.log");
        QVERIFY2(filtersUsable(sync->filtersFile, &error), qPrintable(error));

        // The manifest points an undo at the moved backup; where the file came from does not change.
        const QString backup = dir.path() + "/.local/share/drive-sync/taken-over/20261110-224919";
        const QJsonObject move = QJsonDocument::fromJson(readBytes(backup + "/manifest.json"))
                                     .object().value("moved").toArray().first().toObject();
        QCOMPARE(move.value("to").toString(), backup + "/bin/drive-sync.sh");
        QCOMPARE(move.value("from").toString(), dir.path() + "/.local/bin/drive-sync.sh");
        QVERIFY(QFile::exists(move.value("to").toString()));
    }
    void migrationRunTwiceChangesNothing()
    {
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        QVERIFY(migrateFromLegacyNames(dir.path()).isEmpty());
        const QString configFile = dir.path() + "/.config/drive-sync/config.json";
        const QByteArray once = readBytes(configFile);
        const QDateTime written = QFileInfo(configFile).lastModified();

        QVERIFY(migrateFromLegacyNames(dir.path()).isEmpty());
        QCOMPARE(readBytes(configFile), once);
        QCOMPARE(QFileInfo(configFile).lastModified(), written);

        // A home that never had SyncWatch gets no folder at all.
        QTemporaryDir fresh;
        QVERIFY(migrateFromLegacyNames(fresh.path()).isEmpty());
        QVERIFY(QDir(fresh.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void migrationMergesWhenBothFoldersExist()
    {
        // An old SyncWatch window still open writes its conflict cache to the
        // old state folder after the new one exists.
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        const QString oldState = dir.path() + "/.local/state/syncwatch";
        const QString newState = dir.path() + "/.local/state/drive-sync-app";
        QVERIFY(QDir().mkpath(newState));
        QVERIFY(write(newState + "/conflicts.json", "new"));
        QVERIFY(write(oldState + "/conflicts.json", "old"));

        const QStringList problems = migrateFromLegacyNames(dir.path());
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems.first().contains(oldState + "/conflicts.json"));
        // What the new folder lacked moved; what both had kept the new copy.
        QVERIFY(QFile::exists(newState + "/paused.json"));
        QVERIFY(!QFile::exists(oldState + "/paused.json"));
        QCOMPARE(readBytes(newState + "/conflicts.json"), QByteArray("new"));
        QCOMPARE(readBytes(oldState + "/conflicts.json"), QByteArray("old"));
        // The other folders had no new copy and moved whole.
        QVERIFY(!QFileInfo::exists(dir.path() + "/.config/syncwatch"));
        QVERIFY(QFile::exists(dir.path() + "/.config/drive-sync/filters.txt.md5"));

        // Once the old copy is gone, the next start empties and removes the old folder.
        QVERIFY(QFile::remove(oldState + "/conflicts.json"));
        QVERIFY(migrateFromLegacyNames(dir.path()).isEmpty());
        QVERIFY(!QFileInfo::exists(oldState));
    }
    // ~/.local/state/drive-sync belongs to drive-sync.sh (its resync mark) and a
    // takeover moves it whole: the app's own state must never land in it.
    void migrationKeepsTheOldScriptsStateFolderApart()
    {
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        legacy.name = "drivesync";
        QVERIFY(makeLegacyHome(legacy));
        const QString scriptState = dir.path() + "/.local/state/drive-sync";
        QVERIFY(QDir().mkpath(scriptState));
        QVERIFY(write(scriptState + "/resync.mark", "4242\n"));

        const QStringList problems = migrateFromLegacyNames(dir.path());
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        QCOMPARE(QDir(scriptState).entryList(QDir::Files), QStringList{"resync.mark"});
        QCOMPARE(readBytes(scriptState + "/resync.mark"), QByteArray("4242\n"));
        QVERIFY(QFile::exists(dir.path() + "/.local/state/drive-sync-app/paused.json"));
    }
    // A home that skipped a version has both earlier names: the newer one's
    // entries win, and what both had stays behind in the older folder.
    void migrationNewerOldNameWinsOverTheOlder()
    {
        QTemporaryDir dir;
        LegacyHome oldest{dir.path()};
        oldest.filters = "- oldest/**\n";
        QVERIFY(makeLegacyHome(oldest));
        LegacyHome newer{dir.path()};
        newer.name = "drivesync";
        newer.filters = "- newer/**\n";
        QVERIFY(makeLegacyHome(newer));

        const QStringList problems = migrateFromLegacyNames(dir.path());
        QVERIFY(!problems.isEmpty());
        QCOMPARE(readBytes(dir.path() + "/.config/drive-sync/filters.txt"), newer.filters);
        QVERIFY(QFile::exists(dir.path() + "/.config/syncwatch/filters.txt"));
        QString error;
        const auto sync = loadSyncConfig(dir.path() + "/.config/drive-sync/config.json", &error);
        QVERIFY2(sync.has_value(), qPrintable(error));
        QCOMPARE(sync->filtersFile, dir.path() + "/.config/drive-sync/filters.txt");
    }
    void halfMigratedConfigRefusesToRun()
    {
        // Folders moved but the config not rewritten (a crash in between):
        // the config still names the old filters, so no run starts until the
        // next command finishes the job.
        QTemporaryDir dir;
        LegacyHome legacy{dir.path()};
        QVERIFY(makeLegacyHome(legacy));
        QVERIFY(moveLegacyFolders(legacyFolders(dir.path())).isEmpty());
        QString error;
        const auto sync = loadSyncConfig(dir.path() + "/.config/drive-sync/config.json", &error);
        QVERIFY2(sync.has_value(), qPrintable(error));
        QCOMPARE(sync->filtersFile, dir.path() + "/.config/syncwatch/filters.txt");
        QString refusal;
        QVERIFY(runCommandPlan(*sync, &refusal).isEmpty());
        QVERIFY(!refusal.isEmpty());

        QVERIFY(migrateFromLegacyNames(dir.path()).isEmpty());
        const auto fixed = loadSyncConfig(dir.path() + "/.config/drive-sync/config.json", &error);
        QVERIFY(!runCommandPlan(*fixed, &refusal).isEmpty());
    }
    void filtersUsableRequiresMatchingMd5()
    {
        QTemporaryDir dir;
        const QString filters = dir.filePath("filters.txt");
        QString why;
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains("filters.txt"));

        const QByteArray text = "- .git/**\n- node_modules/**\n";
        QVERIFY(write(filters, text));
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains(".md5"));

        // bisync writes the hex digest alone, with no newline.
        QVERIFY(write(filters + ".md5", QCryptographicHash::hash(text, QCryptographicHash::Md5).toHex()));
        QVERIFY2(filtersUsable(filters, &why), qPrintable(why));

        QVERIFY(write(filters, text + "- .venv/**\n"));
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY(why.contains("changed"));
    }
    void pauseMarkRoundTrips()
    {
        QTemporaryDir dir;
        const QString file = pauseFilePath(dir.filePath("state"));  // the folder does not exist yet
        QCOMPARE(file, dir.filePath("state") + "/paused.json");
        QVERIFY(!readPause(file).present);

        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        mark.until = QDateTime::fromSecsSinceEpoch(1794070800, QTimeZone::UTC);
        QVERIFY(writePause(file, mark));
        PauseMark back = readPause(file);
        QVERIFY(back.present);
        QCOMPARE(back.since, mark.since);
        QCOMPARE(back.until, mark.until);
        QVERIFY(!back.lastReminder.isValid());

        mark.until = QDateTime();  // until the user resumes
        mark.lastReminder = mark.since.addSecs(86400);
        QVERIFY(writePause(file, mark));
        back = readPause(file);
        QVERIFY(!back.until.isValid());
        QCOMPARE(back.lastReminder, mark.lastReminder);

        QVERIFY(clearPause(file));
        QVERIFY(!readPause(file).present);
        QVERIFY(clearPause(file));  // nothing to clear is fine
    }

    void corruptPauseFailsSafe()
    {
        // A mark that cannot be read still holds the runs: syncing while the
        // user asked for a pause is worse than one extra "paused".
        QTemporaryDir dir;
        const QString file = dir.filePath("paused.json");
        QVERIFY(write(file, "{oops"));
        const PauseMark mark = readPause(file);
        QVERIFY(mark.present);
        QVERIFY(!mark.until.isValid());
        QVERIFY(!pauseEnded(mark, QDateTime::currentDateTimeUtc()));
    }

    void nextMorningPicksTheNext8am()
    {
        const QTimeZone zone("America/Sao_Paulo");
        const QDate day(2026, 11, 9);
        auto at = [&](const QDate &d, int h, int m) { return QDateTime(d, QTime(h, m), zone); };
        QCOMPARE(nextMorning(at(day, 7, 59)), at(day, 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 8, 0)), at(day.addDays(1), 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 23, 0)), at(day.addDays(1), 8, 0).toUTC());
        QCOMPARE(nextMorning(at(day, 8, 0)).timeSpec(), Qt::UTC);
    }

    void pauseEndedOnlyAfterItsEnd()
    {
        PauseMark mark;
        mark.present = true;
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QVERIFY(!pauseEnded(mark, now));  // no end
        mark.until = now.addSecs(60);
        QVERIFY(!pauseEnded(mark, now));
        mark.until = now;
        QVERIFY(pauseEnded(mark, now));
        QVERIFY(!pauseEnded(PauseMark(), now));  // no mark, nothing ended
    }

    void pausedBeatsLateAndFailed()
    {
        SyncInputs in = healthy();
        in.lastOutcome = "failed";
        in.lastCauseKind = "outOfSync";
        in.lastSuccess = in.now.addSecs(-3 * 3600);
        in.paused = true;
        SyncSummary s = Backend::summarize(in);
        QCOMPARE(s.status, QString("paused"));
        QVERIFY(!s.warnings.contains("stale"));
        QCOMPARE(toneFor("paused"), QString("warning"));

        in.running = true;  // a run that started before the pause still shows
        QCOMPARE(Backend::summarize(in).status, QString("running"));
    }

    void pauseEndedKeepsPausedUntilARunStarts()
    {
        // The mark stays until run-condition clears it at the next tick, so
        // between the end and that tick the state is still "paused", never
        // "skipped" from the last tick the pause held back.
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-3600), 480), now);
        src.service["Result"] = "exec-condition";
        src.managed = true;
        src.pause.present = true;
        src.pause.since = now.addSecs(-7200);
        src.pause.until = now.addSecs(-60);
        const Snapshot snap = buildSnapshot(src);
        QCOMPARE(snap.summary.status, QString("paused"));
        QVERIFY(snap.paused);
        QVERIFY(snap.managed);
        QCOMPARE(snap.pausedUntil, src.pause.until);
        QVERIFY(snap.canSyncNow);  // the run clears the ended pause itself

        src.pause = PauseMark();
        const Snapshot after = buildSnapshot(src);
        QCOMPARE(after.summary.status, QString("skipped"));
        QVERIFY(!after.paused);
        QVERIFY(after.canSyncNow);
    }

    void skipBeforeTheLastRunIsNoLongerNews()
    {
        // Seen on 2026-11-17: a tick skipped while the fix held the runs, then
        // the fix's own final run succeeded, and "skipped" stayed on until the
        // next tick. A skip has no ExecMainStartTimestamp; its time is
        // InactiveEnterTimestamp.
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-600), 50), now);
        src.service["Result"] = "exec-condition";
        src.service.remove("ExecMainStartTimestamp");
        src.service["InactiveEnterTimestamp"] = QString("@%1").arg(now.addSecs(-900).toSecsSinceEpoch());
        QCOMPARE(buildSnapshot(src).summary.status, QString("ok"));

        // A skip after that run is still the news.
        src.service["InactiveEnterTimestamp"] = QString("@%1").arg(now.addSecs(-60).toSecsSinceEpoch());
        QCOMPARE(buildSnapshot(src).summary.status, QString("skipped"));
    }
    void syncStartsTheServiceWithoutBlocking()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, {paths}, o, e), 0);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync.service"});

        out.clear();
        QCOMPARE(runSyncCommand({"--json"}, {paths}, o, e), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("ok").toBool(), true);

        // systemctl's own words reach the caller.
        QTemporaryDir failing;
        out.clear();
        QCOMPARE(runSyncCommand({"--json"}, {controlPaths(failing.path(), false, 1)}, o, e), 1);
        const QJsonObject answer = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(answer.value("ok").toBool(), false);
        QVERIFY(answer.value("error").toString().contains("denied"));
        QCOMPARE(runSyncCommand({"--bogus"}, {paths}, o, e), 1);
    }

    void syncIsRefusedWhilePaused()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, {paths}, o, e), 1);
        QVERIFY(calls(dir.path()).isEmpty());
        QVERIFY(err.contains("paused"));
    }

    void pauseIsRefusedWhenNotManaged()
    {
        // The old units never look at the mark, so a pause there would be a lie.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runPauseCommand({}, {paths}, QDateTime::currentDateTimeUtc(), o, e), 1);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);
        QVERIFY(err.contains("DriveSync"));
    }

    void pauseWritesTheEnd()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QString out, err;
        QTextStream o(&out), e(&err);
        const QString file = pauseFilePath(paths.appState);

        QCOMPARE(runPauseCommand({"--minutes", "60"}, {paths}, now, o, e), 0);
        PauseMark mark = readPause(file);
        QVERIFY(mark.present);
        QCOMPARE(mark.since, now);
        QCOMPARE(mark.until, now.addSecs(3600));

        QCOMPARE(runPauseCommand({}, {paths}, now.addSecs(60), o, e), 0);
        mark = readPause(file);
        QVERIFY(!mark.until.isValid());
        QCOMPARE(mark.since, now);  // a longer pause keeps when the pause began

        QCOMPARE(runPauseCommand({"--until-tomorrow"}, {paths}, now, o, e), 0);
        QCOMPARE(readPause(file).until, nextMorning(now.toLocalTime()));

        for (const QStringList &bad : {QStringList{"--minutes"}, QStringList{"--minutes", "0"},
                                       QStringList{"--minutes", "x"}, QStringList{"--later"}})
            QCOMPARE(runPauseCommand(bad, {paths}, now, o, e), 1);
        QVERIFY(calls(dir.path()).isEmpty());  // pausing never touches systemd
    }

    void resumeClearsTheMarkAndStartsARun()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runResumeCommand({}, {paths}, o, e), 0);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync.service"});
    }

    void conditionFollowsThePause()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QString file = pauseFilePath(paths.appState);
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);

        QCOMPARE(runConditionCommand(paths, config, now), 0);

        PauseMark mark;
        mark.present = true;
        mark.until = now.addSecs(60);
        QVERIFY(writePause(file, mark));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(readPause(file).present);

        // The first tick after the end runs, and the mark is gone with it.
        QCOMPARE(runConditionCommand(paths, config, now.addSecs(60)), 0);
        QVERIFY(!readPause(file).present);
    }

    void conditionSkipsWhenRunOnlyWhenFails()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        SyncConfig config = oldUnitConfig();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        config.runOnlyWhen = "true";
        QCOMPARE(runConditionCommand(paths, config, now), 0);
        config.runOnlyWhen = "false";
        QCOMPARE(runConditionCommand(paths, config, now), 1);

        // An ended pause is not cleared by a tick that could not run anyway.
        PauseMark mark;
        mark.present = true;
        mark.until = now.addSecs(-60);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(readPause(pauseFilePath(paths.appState)).present);
    }

    void runRefusesWithoutMatchingMd5()
    {
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        // No base at all (no listings either): the first sync was never
        // confirmed, which is no edit of the filters.
        config.workDir = dir.filePath("work");
        QString refusal;
        QVERIFY(runCommandPlan(config, &refusal).isEmpty());
        QVERIFY2(refusal.contains(kFirstSyncPendingReason), qPrintable(refusal));
        // A base whose .md5 went missing: the filters' refusal.
        const QString session = config.workDir + "/" + bisyncSessionName(config.remote, config.localPath);
        QVERIFY(QDir().mkpath(config.workDir));
        QVERIFY(write(session + ".path1.lst", "# bisync listing v1\n") && write(session + ".path2.lst", "# bisync listing v1\n"));
        refusal.clear();
        QVERIFY(runCommandPlan(config, &refusal).isEmpty());
        QVERIFY(refusal.contains("filters.txt.md5"));
        QVERIFY(refusal.contains(kFiltersChangedReason));

        QVERIFY(write(config.filtersFile + ".md5",
                      QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        refusal.clear();
        QCOMPARE(runCommandPlan(config, &refusal), rcloneRunArguments(config));
        QVERIFY(refusal.isEmpty());
    }
    void notifyIsQuietOnExecCondition()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QCOMPARE(runNotifyCommand("exec-condition", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).isEmpty());
    }

    void notifyWarnsOnceThenHourly()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY(sent.at(0).contains("-u critical"));
        QVERIFY(sent.at(0).contains("Drive stopped syncing"));

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);  // ten minutes later: nothing new

        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3660)), 0);
        sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("Drive is still stopped"));
        // The old card goes away first, by id and by title.
        const QStringList all = calls(dir.path());
        QVERIFY(all.contains("gdbus call --session --dest org.freedesktop.Notifications --object-path "
                             "/org/freedesktop/Notifications --method org.freedesktop.Notifications.CloseNotification 41"));
        // Step 5.2: the title names the account (the folder's name here).
        QVERIFY(all.contains("omarchy notification dismiss Drive stopped syncing (Drive)"));
    }

    void notifyNamesTheCause()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).at(0).contains("bisync lost its listings"));
        QVERIFY(notifications(dir.path()).at(0).contains("use Fix…"));
        QVERIFY(!notifications(dir.path()).at(0).contains("takeover --undo"));

        QTemporaryDir other;
        const BackendPaths timeoutPaths = controlPaths(other.path(), true);
        QCOMPARE(runNotifyCommand("timeout", timeoutPaths, fakeNotifier(other.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(other.path()).at(0).contains("time limit"));

        // A run DriveSync refused to start leaves its reason in the log.
        QTemporaryDir refused;
        const BackendPaths refusedPaths = controlPaths(refused.path(), true);
        QVERIFY(write(refusedPaths.log, line(QDateTime::currentDateTime(), "CRITICAL",
                                             "DriveSync did not start rclone: filters.txt.md5 is missing").toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", refusedPaths, fakeNotifier(refused.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(refused.path()).at(0).contains("filters.txt.md5 is missing"));
    }

    void notifyNamesTheGuard()
    {
        // Every deletion counts, both sides, not only the side over the guard.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, abortedRun(QDateTime::currentDateTime().addSecs(-60), {"c1"},
                                            numberedFiles("f%1.txt", 10), 21)
                                     .toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("would delete 11 files"), qPrintable(sent.at(0)));
        QVERIFY(sent.at(0).contains("use Review…"));
        QVERIFY(sent.at(0).contains("-u critical"));  // nothing syncs until it is answered
    }

    void notifyCardsANewGuardStopAtOnce()
    {
        // Every tick stops again at the guard while nobody answers: the same
        // list is the same stop, shown hourly. Another list, or the run that
        // was let through stopping again, is news and shows at once.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime t0 = QDateTime::currentDateTime().addSecs(-7200);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QString log = abortedRun(t0, {}, numberedFiles("f%1.txt", 11), 21);
        const auto stop = [&](int minutes, const QString &before, int files) {
            log += before + abortedRun(t0.addSecs(minutes * 60), {}, numberedFiles("f%1.txt", files), 21);
            QVERIFY(write(paths.log, log.toUtf8()));
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(minutes * 60)), 0);
        };
        QVERIFY(write(paths.log, log.toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        stop(15, QString(), 11);
        QCOMPARE(notifications(dir.path()).size(), 1);  // the same stop again
        stop(30, QString(), 13);
        QCOMPARE(notifications(dir.path()).size(), 2);  // two more files: a new list
        QVERIFY2(notifications(dir.path()).at(1).contains("would delete 13 files"),
                 qPrintable(notifications(dir.path()).at(1)));
        const QString kept = line(t0.addSecs(45 * 60), "INFO",
                                  "DriveSync kept --max-delete at 5% for this run: a dry run finds 1 deletion not in "
                                  "the list that was confirmed (gone from the cloud: 0, gone from the notebook: 13)");
        stop(45, kept, 13);
        QCOMPARE(notifications(dir.path()).size(), 3);  // the run let through stopped again
        QVERIFY2(notifications(dir.path()).at(2).contains("The run you let through kept the usual guard"),
                 qPrintable(notifications(dir.path()).at(2)));
        stop(60, QString(), 13);
        QCOMPARE(notifications(dir.path()).size(), 3);
    }

    void backendShowsWhyTheGuardWasKept()
    {
        // The note drive-sync run writes before rclone starts reaches the
        // window with the stop after it, never with a later one.
        const QDateTime start = QDateTime::currentDateTime().addSecs(-1800);
        const QString kept = line(start.addSecs(899), "INFO",
                                  "DriveSync kept --max-delete at 5% for this run: its dry run with the raised guard "
                                  "still stopped, so more files would go than the 11 confirmed");
        const QString log = abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21) + kept
                            + abortedRun(start.addSecs(900), {}, numberedFiles("f%1.txt", 13), 21);
        const auto blocked = blockedDeletes(log, 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->keptReason, QString("stillStopped"));
        const auto later = blockedDeletes(log + abortedRun(start.addSecs(1800), {}, numberedFiles("f%1.txt", 13), 21), 500);
        QVERIFY(later.has_value());
        QVERIFY(later->keptReason.isEmpty());
        // A file named like the note is not one.
        const QString named = abortedRun(start, {}, {"DriveSync kept --max-delete at 5% for this run: x was dropped"}, 21);
        QVERIFY(blockedDeletes(named + abortedRun(start.addSecs(900), {}, {"a"}, 21), 500)->keptReason.isEmpty());

        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        Backend backend(paths);
        QTRY_COMPARE(backend.blockedCount(), 13);
        QCOMPARE(backend.blockedKeptReason(), QString("stillStopped"));
    }

    // The card's time shows the hours of the bar clock, not the language's own.
    void notifyTimeFollowsTheBarClock()
    {
        for (const QString &format : {QString(), QString("HH:mm")}) {
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            NotifierPaths tools = fakeNotifier(dir.path());
            tools.locale = QLocale(QLocale::English, QLocale::UnitedStates);
            tools.timeFormat = format;
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, QDateTime::currentDateTimeUtc()), 0);
            // The time is on the card's second line, so the whole log is read.
            QFile log(dir.filePath("calls.log"));
            QVERIFY(log.open(QIODevice::ReadOnly));
            const QString sent = QString::fromUtf8(log.readAll());
            QVERIFY(sent.contains("Drive stopped syncing"));
            static const QRegularExpression amPm("\\d:\\d\\d.?[AP]M");
            QCOMPARE(sent.contains(amPm), format.isEmpty());
        }
    }

    void clockHoursFromAFormat()
    {
        QVERIFY(hoursInFormat("ddd d MMM HH:mm") == ClockHours::TwentyFour);
        QVERIFY(hoursInFormat("dddd h:mm:ss ap") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("h:mm AP") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("h:mm") == ClockHours::TwentyFour);  // Qt's h is 0 to 23 without AM/PM
        // Quoted text is literal: the "a" of "at" is no AM/PM marker.
        QVERIFY(hoursInFormat("'at' HH:mm") == ClockHours::TwentyFour);
        QVERIFY(hoursInFormat("'It''s' h:mm a") == ClockHours::Twelve);
        QVERIFY(hoursInFormat("d MMMM 'W'ww yyyy") == ClockHours::Unknown);
    }

    void clockHoursFromTheBar()
    {
        const QByteArray top = R"({"bar":{"position":"top","layout":{"center":[
            {"id":"omarchy.clock","format":"ddd d MMM h:mm AP","verticalFormat":"HH\nmm"}]}}})";
        QVERIFY(barClockHours({top}) == ClockHours::Twelve);
        // A side bar shows the vertical format.
        QByteArray side = top;
        side.replace("\"top\"", "\"left\"");
        QVERIFY(barClockHours({side}) == ClockHours::TwentyFour);
        // An entry without a format shows Omarchy's default, 24 hours.
        QVERIFY(barClockHours({R"({"bar":{"layout":{"right":[{"id":"omarchy.clock"}]}}})"}) == ClockHours::TwentyFour);
        // No clock on the bar, or nothing readable: the bar says nothing.
        QVERIFY(barClockHours({R"({"bar":{"layout":{"left":[{"id":"omarchy.workspaces"}]}}})"}) == ClockHours::Unknown);
        QVERIFY(barClockHours({"not json"}) == ClockHours::Unknown);
        // A user file without a layout leaves it to Omarchy's defaults, and
        // its own position still counts.
        const QByteArray defaults = R"({"bar":{"position":"top","layout":{"center":[
            {"id":"omarchy.clock","format":"h:mm AP","verticalFormat":"H mm"}]}}})";
        QVERIFY(barClockHours({R"({"idle":{}})", defaults}) == ClockHours::Twelve);
        QVERIFY(barClockHours({R"({"bar":{"position":"right"}})", defaults}) == ClockHours::TwentyFour);
    }

    void clockTimeFormatFallsBackToTheLocale()
    {
        const QLocale english(QLocale::English, QLocale::UnitedStates);
        const QLocale portuguese(QLocale::Portuguese, QLocale::Brazil);
        QCOMPARE(clockTimeFormat(ClockHours::TwentyFour, english), QString("HH:mm"));
        QCOMPARE(clockTimeFormat(ClockHours::Twelve, portuguese), QString("h:mm AP"));
        QCOMPARE(clockTimeFormat(ClockHours::Unknown, portuguese), QString("HH:mm"));
        QVERIFY(hoursInFormat(clockTimeFormat(ClockHours::Unknown, english)) == ClockHours::Twelve);
    }

    // A right click on the bar clock rewrites shell.json; the window follows.
    void clockFormatFollowsShellJson()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("omarchy")));
        const QString user = dir.filePath("omarchy/shell.json");
        QVERIFY(write(user, R"({"bar":{"layout":{"center":[{"id":"omarchy.clock","format":"HH:mm"}]}}})"));
        ClockFormat clock(ClockPaths{{user, dir.filePath("missing-defaults.json")}, QLocale::c()});
        QCOMPARE(clock.timeFormat(), QString("HH:mm"));

        QSignalSpy changed(&clock, &ClockFormat::changed);
        // The shell saves by replacing the file.
        const QString saved = dir.filePath("omarchy/shell.json.tmp");
        QVERIFY(write(saved, R"({"bar":{"layout":{"center":[{"id":"omarchy.clock","format":"h:mm AP"}]}}})"));
        QVERIFY(QFile::remove(user));
        QVERIFY(QFile::rename(saved, user));
        QTRY_COMPARE(clock.timeFormat(), QString("h:mm AP"));
        QCOMPARE(changed.count(), 1);
    }

    void notifyWithoutASessionRetries()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        QVERIFY(write(dir.filePath("ids.txt"), "none\n"));
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QVERIFY(write(dir.filePath("ids.txt"), "7\n"));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(60)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        // Still the first notice: none ever reached the screen.
        QVERIFY(sent.at(1).contains("Drive stopped syncing"));
    }

    void successClosesAndSaysSinceWhen()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(900)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("-u normal"));
        QVERIFY(sent.at(1).contains("Drive is syncing again"));
        QVERIFY(calls(dir.path()).contains("omarchy notification dismiss Drive is still stopped (Drive)"));

        // Once back, another success says nothing.
        QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
    }

    void watchdogWarnsWhenRunsCannotHappen()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const struct {
            QString load, active, enabled, expected;
        } cases[] = {{"not-found", "active", "enabled", "does not load"},
                     {"loaded", "inactive", "enabled", "drive-sync.timer is inactive"},
                     {"loaded", "active", "disabled", "next boot"}};
        for (const auto &c : cases) {
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.systemctl = watchdogSystemctl(dir.path(), c.load, c.active, c.enabled);
            QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), now), 0);
            const QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 1);
            QVERIFY2(sent.at(0).contains(c.expected), qPrintable(sent.at(0)));
            QVERIFY(sent.at(0).contains("Drive will not sync"));
        }

        // All well: silent, and a problem seen before is closed with a notice.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
        QCOMPARE(watchdogOver(paths, config, tools, now), 0);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(7200)), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY(sent.at(1).contains("Drive timer is back"));
    }

    void watchdogRespectsRunOnlyWhen()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen = "false";
        QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).isEmpty());
    }

    void watchdogRemindsDailyOnlyForAPauseWithNoEnd()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        PauseMark mark;
        mark.present = true;
        mark.since = now;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));

        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
        QVERIFY(notifications(dir.path()).isEmpty());  // a fresh pause is fine
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QVERIFY(notifications(dir.path()).at(0).contains("DriveSync is still paused"));
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(86400 + 3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);  // once a day
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(2 * 86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);

        // A pause with an end ends by itself, so it needs no reminder.
        mark.until = now.addSecs(10 * 86400);
        mark.lastReminder = QDateTime();
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(5 * 86400)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
    }
    void detectReadsTheRealUnit()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QCOMPARE(plan.state, QString("found"));
        QVERIFY2(plan.blockers.isEmpty(), qPrintable(plan.blockers.join(", ")));
        QVERIFY(plan.canApply());
        QCOMPARE(plan.config.remote, QString("gdrive:"));
        QCOMPARE(plan.config.localPath, old.home + "/Drive");
        QCOMPARE(plan.config.workDir, old.home + "/.local/state/rclone/bisync");
        QCOMPARE(plan.config.filtersFile, old.home + "/.config/drive-sync/accounts/account1/filters.txt");
        QCOMPARE(plan.config.logFile, old.home + "/.local/state/drive-sync.log");
        QCOMPARE(plan.config.rcloneFlags,
                 (QStringList{"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                              "--max-lock", "2m", "--max-delete", "5", "--conflict-resolve", "newer",
                              "--log-level", "INFO"}));
        QCOMPARE(plan.config.runOnlyWhen, QString("findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'"));
        QCOMPARE(plan.oldFiltersFile, old.filters);
        for (const QString &name : {QString("drive-sync.service"), QString("drive-sync-vigia.timer"),
                                    QString("drive-sync.sh"), QString("gdrive-filtros.txt.md5")})
            QVERIFY2(plan.found.join('\n').contains(name), qPrintable(name));
    }

    void detectIsNoneWithoutUnits()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync.service"));
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("none"));
    }

    void detectIsManagedWithAConfig()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.path());
        QVERIFY(saveSyncConfig(old.paths.appConfigDir + "/config.json", oldUnitConfig(), nullptr));
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QCOMPARE(plan.state, QString("managed"));
        QVERIFY(!plan.canApply());
    }

    void detectNamesEveryBlocker()
    {
        const auto blockersAfter = [](const std::function<void(OldSetup &, Snapshot &)> &change) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            Snapshot snapshot = settledSnapshot();
            change(old, snapshot);
            const TakeoverPlan plan = detectTakeover(old.paths, snapshot);
            return plan.blockers;
        };
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.running = true; }), QStringList{"running"});
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.resync = true; }), QStringList{"running"});
        QCOMPARE(blockersAfter([](OldSetup &, Snapshot &s) { s.lastRunOutcome = "failed"; }),
                 QStringList{"lastRunNotSuccess"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     QFile::remove(o.home + "/.local/state/rclone/bisync/" + bisyncSessionName("gdrive:", o.home + "/Drive")
                                   + ".path2.lst");
                 }),
                 QStringList{"noListings"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) { write(o.filters, "- changed/**\n"); }),
                 QStringList{"filtersChanged"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.rclone, "#!/bin/sh\necho other:\n");
                 }),
                 QStringList{"remoteMissing"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           "[Service]\nType=oneshot\nExecStart=/usr/bin/unison profile\n");
                 }),
                 QStringList{"unreadableUnit"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           (oldServiceText(o.home) + "Environment=RCLONE_CONFIG=/elsewhere\n").toUtf8());
                 }),
                 QStringList{"unknownDirective:Environment"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.service",
                           oldServiceText(o.home).replace("Nice=10", "Nice=5").toUtf8());
                 }),
                 QStringList{"differentValue:Nice"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     write(o.paths.userUnits + "/drive-sync.timer", "[Timer]\nOnBootSec=5min\nOnUnitActiveSec=30min\n");
                 }),
                 QStringList{"differentValue:OnUnitActiveSec"});
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     QDir().mkpath(o.paths.appConfigDir + "/accounts/account1");
                     write(o.paths.appConfigDir + "/accounts/account1/filters.txt", "left behind\n");
                 }),
                 QStringList{"targetExists"});
        // A machine guard that is not the old unit's own ExecCondition.
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     writeMachineGuard(machineGuardPath(o.paths.appConfigDir), "true", nullptr);
                 }),
                 QStringList{"targetExists"});
        // The same one is no conflict.
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     writeMachineGuard(machineGuardPath(o.paths.appConfigDir),
                                       "findmnt -no SOURCE /home | grep -q '^/dev/mapper/someone'", nullptr);
                 }),
                 QStringList{});
        // A drop-in that would reach account1's instance units.
        QCOMPARE(blockersAfter([](OldSetup &o, Snapshot &) {
                     QDir().mkpath(o.paths.userUnits + "/drive-sync@.service.d");
                     write(o.paths.userUnits + "/drive-sync@.service.d/x.conf", "[Service]\n");
                 }),
                 QStringList{"dropIn"});
    }
    void takeoverMovesEverythingAndWritesTheManifest()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QVERIFY(plan.canApply());
        const QDateTime now(QDate(2026, 11, 9), QTime(10, 0), QTimeZone::UTC);
        QString error;
        QVERIFY2(applyTakeover(old.paths, plan, now, &error), qPrintable(error));

        // The config and the filters with their md5, byte for byte, as account1.
        const auto config = loadSyncConfig(old.paths.appConfigDir + "/accounts/account1/config.json", &error);
        QVERIFY2(config.has_value(), qPrintable(error));
        QCOMPARE(config->filtersFile, old.paths.appConfigDir + "/accounts/account1/filters.txt");
        QVERIFY(filtersUsable(config->filtersFile, &error));
        QCOMPARE(rcloneRunArguments(*config).mid(0, 5), (QStringList{"bisync", "gdrive:", old.home + "/Drive",
                                                                     "--workdir", plan.config.workDir}));

        // Every old file is in the backup, none is left where it ran from.
        const QDir backups(old.paths.backupRoot);
        const QStringList stamps = backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(stamps.size(), 1);
        const QString backup = backups.filePath(stamps.first());
        QVERIFY(QFile::exists(backup + "/manifest.json"));
        for (const QString &original : plan.found) {
            QVERIFY2(!QFileInfo::exists(original), qPrintable(original));
        }
        QVERIFY(QFile::exists(backup + "/systemd/drive-sync.service"));
        QVERIFY(QFile::exists(backup + "/bin/drive-sync.sh"));
        QVERIFY(QFile::exists(backup + "/rclone/gdrive-filtros.txt.md5"));
        QVERIFY(QFile::exists(backup + "/state/drive-sync/aviso"));

        // The packaged units are checked before any timer starts, and the
        // old filters move only after the second wait.
        QCOMPARE(systemctlCalls(dir.path()),
                 (QStringList{"systemctl --user is-enabled drive-sync.timer",
                              "systemctl --user is-active drive-sync.timer",
                              "systemctl --user is-enabled drive-sync-vigia.timer",
                              "systemctl --user is-active drive-sync-vigia.timer",
                              "systemctl --user stop drive-sync.timer drive-sync-vigia.timer",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user disable drive-sync.timer drive-sync-vigia.timer",
                              "systemctl --user daemon-reload",
                              "systemctl --user show -p Id,LoadState,FragmentPath,DropInPaths drive-sync@account1.service "
                              "drive-sync@account1.timer drive-sync-fix@account1.service",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user enable --now drive-sync@account1.timer drive-sync-watchdog.timer"}));
        QVERIFY(!QFile::exists(takeoverLockPath(old.paths.appState)));
    }

    void takeoverRollsBackAtEveryStep()
    {
        // Each of the eleven systemctl calls fails in turn; then a move fails
        // halfway (12), the backup folder cannot be made (13), and the old
        // filters cannot move after the reload (14).
        for (int failAt = 1; failAt <= 14; ++failAt) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            const QString rcloneDir = QFileInfo(old.filters).path();
            old.paths.systemctl = takeoverSystemctl(dir.path(), failAt <= 11 ? failAt : 0, "inactive",
                                                    "/usr/lib/systemd/user",
                                                    failAt == 14 ? QString("chmod a-w '%1'").arg(rcloneDir) : ":");
            old.paths.serviceWaitMs = 300;
            const QString bin = old.paths.binDir;
            if (failAt == 12)  // the units move, then the scripts cannot leave their folder
                QFile::setPermissions(bin, QFile::ReadOwner | QFile::ExeOwner);
            if (failAt == 13) {
                // A file where the backup folder should go: the move cannot happen.
                QDir().mkpath(QFileInfo(old.paths.backupRoot).path());
                QVERIFY(write(old.paths.backupRoot, "in the way"));
            }
            const auto restore = qScopeGuard([&] {
                QFile::setPermissions(bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                QFile::setPermissions(rcloneDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            });
            const auto before = filesUnder(old.home);
            const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
            QVERIFY(plan.canApply());
            QString error;
            QVERIFY2(!applyTakeover(old.paths, plan, QDateTime::currentDateTimeUtc(), &error),
                     qPrintable(QString("step %1 did not fail").arg(failAt)));
            QVERIFY(!error.isEmpty());
            QCOMPARE(filesUnder(old.home), before);
            // An empty accounts/ left behind would be the accounts layout with no account.
            QVERIFY2(!QFileInfo::exists(old.paths.appConfigDir + "/accounts"), qPrintable(QString::number(failAt)));
            if (failAt >= 5)
                QVERIFY2(calls(dir.path()).contains("systemctl --user start drive-sync.timer drive-sync-vigia.timer"),
                         qPrintable(QString("old timers not back after step %1").arg(failAt)));
            if (failAt >= 7)
                QVERIFY2(calls(dir.path()).contains("systemctl --user enable drive-sync.timer drive-sync-vigia.timer"),
                         qPrintable(QString("old timers not enabled again after step %1").arg(failAt)));
            if (failAt == 11) {
                // One timer definition at a time: the packaged ones go, systemd
                // reloads with the old files back, and only then the old ones return.
                const QStringList made = systemctlCalls(dir.path());
                const auto at = [&](const QString &call) { return made.lastIndexOf("systemctl --user " + call); };
                QVERIFY(at("disable --now drive-sync@account1.timer drive-sync-watchdog.timer") < at("daemon-reload"));
                QVERIFY(at("daemon-reload") < at("enable drive-sync.timer drive-sync-vigia.timer"));
            }
        }
    }

    void takeoverAndUndoNeverRunAtOnce()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QDir().mkpath(old.paths.appState);
        // Another drive-sync, alive, holds the lock: apply and undo both refuse
        // and leave its lock alone.
        QProcess holder;
        QVERIFY(startNamedHolder(holder, dir.path(), "drive-sync"));
        const QByteArray holderPid = QByteArray::number(holder.processId());
        QVERIFY(write(takeoverLockPath(old.paths.appState), holderPid));
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY2(error.contains("another"), qPrintable(error));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("another"), qPrintable(error));
        QFile lock(takeoverLockPath(old.paths.appState));
        QVERIFY(lock.open(QIODevice::ReadOnly));
        QCOMPARE(lock.readAll(), holderPid);
        lock.close();
        holder.kill();
        holder.waitForFinished();

        // A lock left by a takeover that died is replaced, and released after.
        QVERIFY(write(takeoverLockPath(old.paths.appState), "999999999\n"));
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error),
                 qPrintable(error));
        QVERIFY(!QFile::exists(takeoverLockPath(old.paths.appState)));
    }

    void takeoverLeavesEveryTimerOffWhenAFileCannotGoBack()
    {
        // The packaged unit is not the one systemd uses, and at that moment
        // the old units' folder turns read-only: they cannot go back, so the
        // old timers must not start without them.
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        const QString units = old.paths.userUnits;
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "inactive", "/somewhere/else",
                                                QString("chmod a-w '%1'").arg(units));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(units, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY2(error.contains("left off"), qPrintable(error));
        const QStringList after = systemctlCalls(dir.path());
        const int show = int(after.indexOf(QRegularExpression(".*FragmentPath.*")));
        QVERIFY(show > 0);
        for (const QString &call : after.mid(show + 1)) {
            QVERIFY2(!call.contains(" start ") && !call.contains(" enable "), qPrintable(call));
        }
        // The old filters never left; DriveSync's copy and config are gone, so
        // the window does not claim to run drive-sync.
        QVERIFY(filtersUsable(old.filters, nullptr));
        QVERIFY(!QFileInfo::exists(old.paths.appConfigDir + "/accounts"));
        QVERIFY(!QFile::exists(machineGuardPath(old.paths.appConfigDir)));
        QVERIFY(readAccountLayout(old.paths.appConfigDir).ids.isEmpty());
    }

    void syncIsRefusedDuringATakeover()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QDir().mkpath(paths.appState);
        BackendPaths live = paths;
        live.proc = "/proc";
        // A live drive-sync holds the takeover lock.
        QProcess holder;
        QVERIFY(startNamedHolder(holder, dir.path(), "drive-sync"));
        QVERIFY(write(takeoverLockPath(live.appState), QByteArray::number(holder.processId())));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, {live}, o, e), 1);
        QVERIFY(err.contains("takeover"));
        QVERIFY(calls(dir.path()).isEmpty());
        holder.kill();
        holder.waitForFinished();

        // A takeover killed halfway leaves its lock behind: it does not count.
        QVERIFY(write(takeoverLockPath(live.appState), "999999999\n"));
        QCOMPARE(runSyncCommand({}, {live}, o, e), 0);
    }

    // "drive-sync" is also a piece of drive-sync.sh, drive-sync-aviso and the
    // units' command lines, so the lock holder is told by the program's exact
    // name, never by a substring of its command line.
    void takeoverLockIsHeldOnlyByTheAppByExactName()
    {
        QTemporaryDir dir;
        const QString state = dir.filePath("state");
        const QString proc = dir.filePath("proc");
        QVERIFY(QDir().mkpath(state));
        QVERIFY(write(takeoverLockPath(state), "4242\n"));
        QVERIFY(QDir().mkpath(proc + "/4242"));
        const auto argv = [](const QStringList &args) {
            QByteArray joined;
            for (const QString &arg : args)
                joined += arg.toUtf8() + '\0';
            return joined;
        };
        struct Case {
            QStringList args;
            bool held;
        };
        const QList<Case> cases{
            {{"/usr/bin/drive-sync", "takeover", "--apply"}, true},
            {{"drive-sync"}, true},
            {{"/usr/bin/drivesync"}, true},  // a window left open across the rename
            {{"/usr/bin/syncwatch"}, true},
            {{"bash", "/home/someone/.local/bin/drive-sync.sh", "resync"}, false},
            {{"/usr/bin/drive-sync-aviso"}, false},
            {{"vim", "drive-sync-notes.md"}, false},
        };
        for (const Case &c : cases) {
            QVERIFY(write(proc + "/4242/cmdline", argv(c.args)));
            QVERIFY2(takeoverLockHeld(state, proc) == c.held, qPrintable(c.args.join(' ')));
        }
    }

    void takeoverStopsWhenARunStartsDuringTheStop()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "activating");
        old.paths.serviceWaitMs = 300;
        const auto before = filesUnder(old.home);
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY(error.contains("run"));
        QCOMPARE(filesUnder(old.home), before);
    }

    void undoRestoresTheOriginalFiles()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        const QStringList skip{".config/drive-sync", ".local/share/drive-sync", ".local/state/drive-sync-app"};
        const auto before = filesUnder(old.home, skip);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        QFile::remove(dir.filePath("calls.log"));
        PauseMark pause;
        pause.present = true;
        QVERIFY(writePause(pauseFilePath(old.paths.appState), pause));
        QVERIFY2(undoTakeover(old.paths, &error), qPrintable(error));
        QVERIFY(!readPause(pauseFilePath(old.paths.appState)).present);
        QCOMPARE(filesUnder(old.home, skip), before);
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/config.json"));
        QVERIFY(!QFile::exists(old.paths.appConfigDir + "/filters.txt"));
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("found"));
        // One timer definition at a time: the packaged ones go before the old ones come back.
        QCOMPARE(systemctlCalls(dir.path()),
                 (QStringList{"systemctl --user is-active drive-sync.service",
                              "systemctl --user is-enabled drive-sync.timer",
                              "systemctl --user is-active drive-sync.timer",
                              "systemctl --user is-enabled drive-sync-watchdog.timer",
                              "systemctl --user is-active drive-sync-watchdog.timer",
                              "systemctl --user disable --now drive-sync.timer drive-sync-watchdog.timer",
                              "systemctl --user is-active drive-sync.service",
                              "systemctl --user daemon-reload",
                              "systemctl --user enable --now drive-sync.timer drive-sync-vigia.timer"}));

        // And it can be taken over again.
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()),
                               QDateTime::currentDateTimeUtc().addSecs(120), &error),
                 qPrintable(error));
    }

    void undoIsRefusedWhenFiltersChangedOrARunIsGoing()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        QVERIFY(write(old.paths.appConfigDir + "/filters.txt", "- something new/**\n"));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("filters"));
        QVERIFY(QFile::exists(old.paths.appConfigDir + "/config.json"));

        QTemporaryDir busy;
        OldSetup other = makeOldSetup(busy.path());
        other.paths.systemctl = takeoverSystemctl(busy.path(), 0);
        QVERIFY(applyTakeover(other.paths, detectTakeover(other.paths, settledSnapshot()),
                              QDateTime::currentDateTimeUtc(), &error));
        QVERIFY(asLegacyTakeover(other));
        other.paths.systemctl = takeoverSystemctl(busy.path(), 0, "active");
        QVERIFY(!undoTakeover(other.paths, &error));
        QVERIFY(error.contains("run"));
    }

    void movedFiltersKeepRealBisyncValid()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Real rclone on two local folders, with no rclone config at all.
        QTemporaryDir dir;
        QDir d(dir.path());
        for (const char *folder : {"a", "b", "work", "old", "new"})
            d.mkpath(folder);
        QVERIFY(write(dir.filePath("a/one.txt"), "one\n"));
        QVERIFY(write(dir.filePath("old/filtros.txt"), "# Gerado pelo drive-sync.sh.\n- *.skip\n"));
        auto bisync = [&](const QStringList &extra) {
            QProcess p;
            p.start(rclone, QStringList{"--config", "/dev/null", "bisync", dir.filePath("a"), dir.filePath("b"),
                                        "--workdir", dir.filePath("work")} + extra);
            p.waitForFinished(60000);
            return p.exitCode();
        };
        QCOMPARE(bisync({"--filters-file", dir.filePath("old/filtros.txt"), "--resync", "-q"}), 0);
        QVERIFY(QFile::exists(dir.filePath("old/filtros.txt.md5")));

        QString error;
        QVERIFY2(copyFiltersWithMd5(dir.filePath("old/filtros.txt"), dir.filePath("new/filters.txt"), &error),
                 qPrintable(error));
        QVERIFY(QFile::remove(dir.filePath("old/filtros.txt")));
        QVERIFY(QFile::remove(dir.filePath("old/filtros.txt.md5")));

        SyncConfig config;
        config.remote = dir.filePath("a");
        config.localPath = dir.filePath("b");
        config.workDir = dir.filePath("work");
        config.filtersFile = dir.filePath("new/filters.txt");
        config.logFile = dir.filePath("sync.log");
        config.rcloneFlags = {"--config", "/dev/null", "-q"};
        QVERIFY(write(dir.filePath("a/two.txt"), "two\n"));
        QProcess run;
        run.start(rclone, runCommandPlan(config, &error));
        run.waitForFinished(60000);
        QCOMPARE(run.exitCode(), 0);
        QVERIFY(QFile::exists(dir.filePath("b/two.txt")));
        QVERIFY(QDir(dir.filePath("work")).entryList({"*-err"}).isEmpty());
    }
    void takeoverSetupOffersOnlyWhenItCanApply()
    {
        QTemporaryDir dir;
        const QString program = fakeDriveSync(
            dir.path(), R"({"ok":true,"error":"","state":"found","found":["/a/drive-sync.service"],"blockers":[]})");
        TakeoverSetup setup(program);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        QCOMPARE(setup.found(), QStringList{"/a/drive-sync.service"});
        QCOMPARE(calls(dir.path()), QStringList{"drive-sync takeover --check --json"});

        // The takeover itself, then a fresh look, which now says managed.
        write(dir.filePath("answer.json"), R"({"ok":true,"error":"","state":"managed","found":[],"blockers":[]})");
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("managed"));
        QVERIFY(calls(dir.path()).contains("drive-sync takeover --apply --json"));

        // Nothing to take over: no offer, and accept does nothing.
        QTemporaryDir none;
        TakeoverSetup quiet(fakeDriveSync(none.path(), R"({"ok":true,"error":"","state":"none","found":[],"blockers":[]})"));
        quiet.check();
        QTRY_COMPARE(quiet.state(), QString("none"));
        quiet.accept();
        QCOMPARE(calls(none.path()), QStringList{"drive-sync takeover --check --json"});
    }

    void takeoverSetupShowsBlockersAndFailures()
    {
        QTemporaryDir dir;
        const QString program = fakeDriveSync(
            dir.path(), R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running","differentValue:Nice"]})");
        TakeoverSetup setup(program);
        setup.check();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QCOMPARE(setup.blockers(), (QStringList{"running", "differentValue:Nice"}));

        QTemporaryDir failing;
        TakeoverSetup broken(fakeDriveSync(
            failing.path(), R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":[]})"));
        broken.check();
        QTRY_COMPARE(broken.state(), QString("offer"));
        write(failing.filePath("answer.json"),
              R"({"ok":false,"error":"could not stop the old timers: denied","state":"found","found":[],"blockers":[]})");
        broken.accept();
        QTRY_COMPARE(broken.state(), QString("failed"));
        QVERIFY(broken.detail().contains("denied"));

        // An answer that is not JSON is a failure too, never an offer.
        QTemporaryDir garbage;
        TakeoverSetup confused(fakeDriveSync(garbage.path(), "not json"));
        confused.check();
        QTRY_COMPARE(confused.state(), QString("failed"));
    }

    // Seen live on 2026-11-10: a run started while the dialog was open, and
    // the confirmed takeover ended as "failed: running".
    void takeoverSetupWaitsForARunAfterTheConfirmation()
    {
        QTemporaryDir dir;
        const QByteArray offer = R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":[]})";
        const QByteArray running = R"({"ok":false,"error":"running","state":"found","found":["/a"],"blockers":["running"]})";
        TakeoverSetup setup(fakeDriveSync(dir.path(), offer));
        setup.check();
        QTRY_COMPARE(setup.state(), QString("offer"));
        write(dir.filePath("answer.json"), running);
        setup.accept();
        QTRY_COMPARE(setup.state(), QString("blocked"));
        QVERIFY(setup.pending());

        // Still running at the next look: still waiting, nothing applied.
        setup.check();
        QTRY_COMPARE(calls(dir.path()).size(), 3);
        QVERIFY(setup.pending());

        // The run ended: the next look applies without asking again.
        write(dir.filePath("answer.json"), offer);
        setup.check();
        QTRY_COMPARE(calls(dir.path()).count("drive-sync takeover --apply --json"), 2);
        QVERIFY(!setup.pending());

        // Cancelled while waiting: the next offer is only an offer.
        QTemporaryDir other;
        TakeoverSetup cancelled(fakeDriveSync(other.path(), offer));
        cancelled.check();
        QTRY_COMPARE(cancelled.state(), QString("offer"));
        write(other.filePath("answer.json"), running);
        cancelled.accept();
        QTRY_VERIFY(cancelled.pending());
        cancelled.cancelPending();
        write(other.filePath("answer.json"), offer);
        cancelled.check();
        QTRY_COMPARE(cancelled.state(), QString("offer"));
        QCOMPARE(calls(other.path()).count("drive-sync takeover --apply --json"), 1);

        // Blocked by something else than a run: the confirmation is dropped.
        QTemporaryDir third;
        TakeoverSetup dropped(fakeDriveSync(third.path(), offer));
        dropped.check();
        QTRY_COMPARE(dropped.state(), QString("offer"));
        write(third.filePath("answer.json"), running);
        dropped.accept();
        QTRY_VERIFY(dropped.pending());
        write(third.filePath("answer.json"),
              R"({"ok":true,"error":"","state":"found","found":["/a"],"blockers":["lastRunNotSuccess"]})");
        dropped.check();
        QTRY_COMPARE(dropped.blockers(), QStringList{"lastRunNotSuccess"});
        QVERIFY(!dropped.pending());
    }

    void backendControlsCallTheCommands()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDriveSync(dir.path(), "{}");
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(backend.managed());

        backend.syncNow();
        backend.pauseFor(60);
        backend.pauseUntilTomorrow();
        backend.pauseUntilResumed();
        backend.resume();
        QTRY_COMPARE(calls(dir.path()).size(), 5);
        // The five run side by side, so they finish in any order.
        QStringList made = calls(dir.path());
        made.sort();
        QCOMPARE(made, (QStringList{"drive-sync pause --json", "drive-sync pause --minutes 60 --json",
                                    "drive-sync pause --until-tomorrow --json", "drive-sync resume --json",
                                    "drive-sync sync --json"}));

        // The pause shows as soon as the mark is there.
        PauseMark mark;
        mark.present = true;
        // The mark keeps whole seconds.
        mark.since = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch(), QTimeZone::UTC);
        mark.until = mark.since.addSecs(3600);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        backend.refresh();
        QTRY_COMPARE(backend.status(), QString("paused"));
        QVERIFY(backend.paused());
        QCOMPARE(backend.pausedUntil(), mark.until);
        QVERIFY(!backend.canSyncNow());
    }

    // O with no Drive folder says so instead of opening nothing.
    void backendOpenDriveFolderSaysWhenItIsMissing()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        Backend backend(paths);
        backend.openDriveFolder();
        QCOMPARE(backend.message(), QString("%1 isn't there.").arg(dir.filePath("Drive")));
    }

    void backendSyncNowReportsAFailure()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.self = fakeDriveSync(dir.path(), "{}", true);
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        backend.syncNow();
        QTRY_VERIFY(backend.message().contains("denied"));
    }
    void undoStopsWhenAFileCannotGoBack()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        const auto taken = filesUnder(old.home);

        // Something in the way: refused before anything changes.
        QVERIFY(write(old.home + "/.local/bin/drive-sync.sh", "someone else's\n"));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("in the way"));
        QVERIFY(QFile::remove(old.home + "/.local/bin/drive-sync.sh"));
        QCOMPARE(filesUnder(old.home), taken);

        // The scripts' folder cannot take them back: what already went back
        // returns to the backup, and DriveSync's timers start again.
        QFile::remove(dir.filePath("calls.log"));
        const QString bin = old.paths.binDir;
        QFile::setPermissions(bin, QFile::ReadOwner | QFile::ExeOwner);
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("nothing was undone"), qPrintable(error));
        QCOMPARE(filesUnder(old.home), taken);
        // DriveSync's timers come back as they were: enabled, then started.
        QCOMPARE(systemctlCalls(dir.path()).mid(systemctlCalls(dir.path()).size() - 2),
                 (QStringList{"systemctl --user enable drive-sync.timer drive-sync-watchdog.timer",
                              "systemctl --user start drive-sync.timer drive-sync-watchdog.timer"}));
        QVERIFY(!systemctlCalls(dir.path()).contains("systemctl --user enable --now drive-sync.timer drive-sync-vigia.timer"));
    }

    void detectBlocksWhatSystemdReadsDifferently()
    {
        const auto blockersAfter = [](const std::function<void(OldSetup &)> &change) {
            QTemporaryDir dir;
            OldSetup old = makeOldSetup(dir.path());
            change(old);
            return detectTakeover(old.paths, settledSnapshot()).blockers;
        };
        // %h is the home for systemd, two literal characters for DriveSync.
        QVERIFY(blockersAfter([](OldSetup &o) {
                    write(o.paths.userUnits + "/drive-sync.service",
                          oldServiceText(o.home).replace(o.home + "/Drive", "%h/Drive").toUtf8());
                }).contains("unreadableUnit"));
        QVERIFY(blockersAfter([](OldSetup &o) {
                    write(o.paths.userUnits + "/drive-sync.service",
                          oldServiceText(o.home).replace("--drive-skip-gdocs", "'--drive-skip-gdocs'").toUtf8());
                }).contains("unreadableUnit"));
        QCOMPARE(blockersAfter([](OldSetup &o) { QDir().mkpath(o.paths.userUnits + "/drive-sync.service.d"); }),
                 QStringList{"dropIn"});
        QCOMPARE(blockersAfter([](OldSetup &o) { QDir().mkpath(o.paths.systemUserUnits + "/drive-sync.timer.d"); }),
                 QStringList{"dropIn"});
        QCOMPARE(blockersAfter([](OldSetup &o) { QFile::remove(o.paths.userUnits + "/drive-sync.timer"); }),
                 QStringList{"unreadableUnit"});
        QCOMPARE(blockersAfter([](OldSetup &o) {
                     write(o.paths.userUnits + "/drive-sync-vigia.service", "[Service]\nType=oneshot\nExecStart=/x/other\n");
                 }),
                 QStringList{"unknownDirective:drive-sync-vigia.service"});
        QCOMPARE(blockersAfter([](OldSetup &o) {
                     write(o.paths.userUnits + "/drive-sync-vigia.timer", "[Timer]\nOnStartupSec=10min\nOnUnitActiveSec=4h\n");
                 }),
                 QStringList{"differentValue:OnUnitActiveSec"});
    }

    void pauseOnlyCountsWhenManaged()
    {
        // After an undo the old units sync again: a leftover mark must not
        // say "paused" or hide a failure.
        const QDateTime now(QDate(2026, 11, 7), QTime(16, 0, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-3600), 480), now);
        src.pause.present = true;
        src.managed = false;
        const Snapshot snap = buildSnapshot(src);
        QVERIFY(!snap.paused);
        QVERIFY(snap.summary.status != "paused");
        QVERIFY(snap.canSyncNow);

        // An ended pause no longer stops "sync now".
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        PauseMark mark;
        mark.present = true;
        mark.until = QDateTime::currentDateTimeUtc().addSecs(-60);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, {paths}, o, e), 0);
    }

    void conditionFailsLikeSystemdOnCrashOr255()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        SyncConfig config = oldUnitConfig();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        config.runOnlyWhen = "exit 255";
        QCOMPARE(runConditionCommand(paths, config, now), 255);
        config.runOnlyWhen = "kill -9 $$";
        QCOMPARE(runConditionCommand(paths, config, now), 255);
        config.runOnlyWhen = "exit 3";
        QCOMPARE(runConditionCommand(paths, config, now), 1);
    }

    void refusedRunShowsAsAFailedRunWithItsCause()
    {
        // DriveSync's refusal is a CRITICAL line outside any run: the parser
        // makes it a failed run, so the window and the bar show its reason.
        const QDateTime start = QDateTime::currentDateTime().addSecs(-3600);
        QString log = successfulRun(start, 480);
        log += line(start.addSecs(900), "CRITICAL", "DriveSync did not start rclone: filters.txt.md5 is missing");
        const auto runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.last().outcome, QString("failed"));
        QVERIFY(runs.last().causeDetail.contains("filters.txt.md5 is missing"));
        const Snapshot snap = buildSnapshot(idleSources(log, QDateTime::currentDateTimeUtc()));
        QCOMPARE(snap.summary.status, QString("failed"));
        QVERIFY(snap.summary.causeDetail.contains("filters.txt.md5"));
    }

    void notifyNamesOnlyTheRunThatJustEnded()
    {
        // An old failed run is not the cause of this one.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-3600)).toUtf8()));
        paths.systemctl = dir.filePath("systemctl");
        QVERIFY(write(paths.systemctl, QString("#!/bin/sh\necho @%1\n")
                                           .arg(QDateTime::currentSecsSinceEpoch() - 30).toUtf8()));
        QFile::setPermissions(paths.systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(runNotifyCommand("exit-code", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        const QString sent = notifications(dir.path()).value(0);
        QVERIFY2(sent.contains("No detail"), qPrintable(sent));
        QVERIFY(!sent.contains("listings"));
    }

    void reminderDoesNotBringAPauseBack()
    {
        // A resume while notify-send shows the reminder wins.
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        NotifierPaths tools = fakeNotifier(dir.path());
        const QString mark = pauseFilePath(paths.appState);
        QVERIFY(write(tools.notifySend, QString("#!/bin/sh\nrm -f '%1'\necho 9\n").arg(mark).toUtf8()));
        PauseMark pause;
        pause.present = true;
        pause.since = QDateTime::currentDateTimeUtc().addSecs(-2 * 86400);
        QVERIFY(writePause(mark, pause));
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QCOMPARE(watchdogOver(paths, config, tools, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(!readPause(mark).present);
    }

    void watchdogReportsABrokenConfig()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        const WatchdogHome h = watchdogHome(paths, SyncConfig(), false, "not json\n");
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(notifications(dir.path()).value(0).contains("config cannot be read"));
    }

    void backendFollowsATakeoverWithoutARestart()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.config = dir.filePath("config/config.json");
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(!backend.managed());

        SyncConfig config = oldUnitConfig();
        config.logFile = dir.filePath("drive-sync.log");
        config.localPath = dir.filePath("Drive");
        QVERIFY(saveSyncConfig(paths.config, config, nullptr));
        backend.reloadConfig();
        QTRY_VERIFY(backend.managed());
    }
    void takeoverWorksWithoutTheOldWatchdog()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync-vigia.service"));
        QVERIFY(QFile::remove(old.paths.userUnits + "/drive-sync-vigia.timer"));
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error),
                 qPrintable(error));
        for (const QString &call : systemctlCalls(dir.path()))
            QVERIFY2(!call.contains("vigia"), qPrintable(call));
    }
    void detectBlocksPathsThatWouldMoveFirst()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        // The log inside the old state folder would leave with it.
        write(old.paths.userUnits + "/drive-sync.service",
              oldServiceText(old.home)
                  .replace(old.home + "/.local/state/drive-sync.log", old.paths.oldStateDir + "/rclone.log")
                  .toUtf8());
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).blockers, QStringList{"pathInsideMoved"});
    }

    void undoRefusesAfterATakeoverThatStoppedHalfway()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        const QString units = old.paths.userUnits;
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "inactive", "/somewhere/else",
                                                QString("chmod a-w '%1'").arg(units));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(units, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("by hand"), qPrintable(error));
    }

    void undoRefusedForARunChangesNothing()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        const auto taken = filesUnder(old.home);
        QFile::remove(dir.filePath("calls.log"));
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0, "active");
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY(error.contains("run"));
        QCOMPARE(filesUnder(old.home), taken);
        QCOMPARE(systemctlCalls(dir.path()), QStringList{"systemctl --user is-active drive-sync.service"});
    }

    void listingReadsFilesAndSkipsTheRest()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("gdrive_..home_someone_Drive.path1.lst");
        QVERIFY(write(path, "# bisync listing v1 from 2026-11-11 12:00:00.000000000 +0000 UTC\n"
                            "d        0 - - 2026-11-11T12:00:00.000000000+0000 \"Notes\"\n"
                            "-     1234 md5:0a1b - 2026-11-11T12:00:00.500000000+0000 \"Notes/a.txt\"\n"
                            "-        7 - - 2026-11-11T09:00:00.500000000-0300 \"b.txt\"\n"));
        QString error;
        const auto files = readListing(path, &error);
        QVERIFY2(files.has_value(), qPrintable(error));
        QCOMPARE(files->size(), 2);
        QCOMPARE(files->value("Notes/a.txt").size, 1234LL);
        QCOMPARE(files->value("b.txt").size, 7LL);
        const qint64 noon = QDateTime(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC).toSecsSinceEpoch();
        QCOMPARE(files->value("Notes/a.txt").mtimeNs, noon * 1000000000LL + 500000000LL);
        QCOMPARE(files->value("b.txt").mtimeNs, files->value("Notes/a.txt").mtimeNs);
    }

    void listingUnquotesGoEscapes()
    {
        bool ok = false;
        bool notUtf8 = true;
        QCOMPARE(unquoteGo(R"(say \"hi\" \\ done)", &ok, &notUtf8), QString(R"(say "hi" \ done)"));
        QVERIFY(ok);
        QVERIFY(!notUtf8);
        // The one escape in the maintainer's listing today: a left-to-right mark.
        QCOMPARE(unquoteGo(R"(a\u200eb)", &ok, &notUtf8), QString("a") + QChar(0x200E) + "b");
        QVERIFY(ok);
        QCOMPARE(unquoteGo(R"(\x41\101)", &ok, &notUtf8), QString("AA"));
        QVERIFY(ok);
        const char32_t grin = 0x1F600;
        QCOMPARE(unquoteGo(R"(\U0001F600)", &ok, &notUtf8), QString::fromUcs4(&grin, 1));
        QVERIFY(ok);
        QCOMPARE(unquoteGo(R"(tab\there)", &ok, &notUtf8), QString("tab\there"));
        QCOMPARE(unquoteGo(QString::fromUtf8("ação"), &ok, &notUtf8), QString::fromUtf8("ação"));
        // Bytes that are not UTF-8 one by one, but are together.
        QCOMPARE(unquoteGo(R"(\xe2\x80\x8e)", &ok, &notUtf8), QString(QChar(0x200E)));
        QVERIFY(ok);
        QVERIFY(!notUtf8);
        unquoteGo(R"(bad \q)", &ok, &notUtf8);
        QVERIFY(!ok);
        unquoteGo(R"(cut \x4)", &ok, &notUtf8);
        QVERIFY(!ok);
    }

    void listingFlagsANameThatIsNotUtf8()
    {
        bool ok = false;
        bool notUtf8 = false;
        unquoteGo(R"(bad\xffname)", &ok, &notUtf8);
        QVERIFY(ok);
        QVERIFY(notUtf8);
        QTemporaryDir dir;
        const QByteArray body = "# bisync listing v1\n"
                                "-        3 - - 2026-11-11T12:00:00.000000000+0000 \"bad\\xffname\"\n";
        QVERIFY(write(dir.filePath("a.path1.lst"), body));
        QVERIFY(write(dir.filePath("a.path2.lst"), body));
        QString error;
        const auto cloud = readListing(dir.filePath("a.path1.lst"), &error);
        const auto notebook = readListing(dir.filePath("a.path2.lst"), &error);
        QVERIFY2(cloud && notebook, qPrintable(error));
        const auto found = divergences(*cloud, *notebook);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().kind, QString("notUtf8"));
        QCOMPARE(found.first().path, QString(R"(bad\xffname)"));
    }

    void listingRejectsAMalformedLine()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.path2.lst");
        QVERIFY(write(path, "# bisync listing v1\n"
                            "-        3 - - 2026-11-11T12:00:00.000000000+0000 \"fine.txt\"\n"
                            "- not a listing line\n"));
        QString error;
        QVERIFY(!readListing(path, &error).has_value());
        QVERIFY2(error.contains(":3:"), qPrintable(error));
        QVERIFY(write(path, "-        3 - - 2026-11-11 12:00 \"bad-time.txt\"\n"));
        QVERIFY(!readListing(path, &error).has_value());
        QVERIFY2(error.contains(":1:"), qPrintable(error));
        QVERIFY(write(path, "-        3 - - 2026-11-11T12:00:00Z \"bad \\q escape\"\n"));
        QVERIFY(!readListing(path, &error).has_value());
        QVERIFY2(error.contains(":1:"), qPrintable(error));
        QVERIFY(!readListing(dir.filePath("missing.lst"), &error).has_value());
        QVERIFY(!error.isEmpty());
    }

    void divergencesFollowTheScript()
    {
        const qint64 t = 1762862400LL * 1000000000LL;
        QHash<QString, ListingEntry> cloud;
        QHash<QString, ListingEntry> notebook;
        cloud["same.txt"] = {10, t};
        notebook["same.txt"] = {10, t + 500000};  // 0.5 ms apart: equal
        cloud["unknown.txt"] = {-1, t};
        notebook["unknown.txt"] = {10, t + 900000};  // a size unknown and close times: equal
        cloud["edited.txt"] = {10, t};
        notebook["edited.txt"] = {10, t + 2000000};  // 2 ms newer on the notebook
        cloud["remote.txt"] = {10, t + 5000000};
        notebook["remote.txt"] = {10, t};  // newer in the cloud
        cloud["grown.txt"] = {10, t};
        notebook["grown.txt"] = {12, t + 100000};  // sizes differ, times close: the cloud side, asked
        cloud["edge.txt"] = {10, t};
        notebook["edge.txt"] = {10, t + 1000000};  // exactly 1 ms is not equal, and the notebook is newer
        cloud["only-cloud.txt"] = {3, t};
        notebook["only-notebook.txt"] = {4, t};
        const QList<Divergence> found = divergences(cloud, notebook);
        QStringList seen;
        for (const Divergence &d : found)
            seen << QString("%1 %2 %3").arg(d.path, d.kind, d.newer);
        QCOMPARE(seen, (QStringList{"edge.txt differs notebook", "edited.txt differs notebook",
                                    "grown.txt differs cloud", "only-cloud.txt onlyCloud cloud",
                                    "only-notebook.txt onlyNotebook notebook", "remote.txt differs cloud"}));
        QCOMPARE(found.at(2).cloud.size, 10LL);
        QCOMPARE(found.at(2).notebook.size, 12LL);
        QCOMPARE(found.at(5).cloud.mtimeNs, t + 5000000);
    }

    void currentListingsIgnoresCopies()
    {
        QTemporaryDir dir;
        const QString prefix = "gdrive_..home_someone_Drive";
        for (const QString &suffix : QStringList{".path1.lst", ".path1.lst-dry", ".path1.lst-old", ".path1.lst-new", ".path2.lst",
                                      ".path2.lst-err"})
            QVERIFY(write(dir.filePath(prefix + suffix), ""));
        const QPair<QString, QString> found = currentListings(dir.path(), prefix);
        QCOMPARE(found.first, dir.filePath(prefix + ".path1.lst"));
        QCOMPARE(found.second, dir.filePath(prefix + ".path2.lst"));
        // A stale session of other paths beside it does not get in the way.
        QVERIFY(write(dir.filePath("other.path1.lst"), ""));
        QVERIFY(write(dir.filePath("other.path2.lst"), ""));
        QCOMPARE(currentListings(dir.path(), prefix).first, dir.filePath(prefix + ".path1.lst"));
        QCOMPARE(currentListings(dir.path(), "absent"), (QPair<QString, QString>()));
        QVERIFY(QFile::remove(dir.filePath(prefix + ".path2.lst")));
        QCOMPARE(currentListings(dir.path(), prefix), (QPair<QString, QString>()));
        QCOMPARE(currentListings(dir.filePath("missing"), prefix), (QPair<QString, QString>()));
    }
    void fixStateRoundTrips()
    {
        QTemporaryDir dir;
        FixState state;
        state.step = "questions";
        state.failedStep = "base";
        state.started = QDateTime::fromSecsSinceEpoch(1762862400, QTimeZone::UTC);
        state.stepStarted = state.started.addSecs(60);
        state.lastReminder = state.started.addSecs(3600);
        state.copies = 1200;
        state.error = "rclone said no";
        FixQuestion one;
        one.path = "Notes/a.txt";
        one.kind = "differs";
        one.cloudSize = 10;
        one.notebookSize = 12;
        one.cloudTime = 1762862500;
        one.notebookTime = 1762862400;
        one.answer = "cloud";
        FixQuestion two;
        two.path = QString::fromUtf8("Odd \"name\" ‎.txt");
        two.kind = "onlyCloud";
        two.cloudSize = 3;
        two.cloudTime = 1762862600;
        state.questions = {one, two};
        state.touched = {"Work/new.md", "Work/edited.md"};
        QCOMPARE(fixStatePath(dir.path()), dir.filePath("fix.json"));
        QCOMPARE(fixDryRunLogPath(dir.path()), dir.filePath("fix-dry-run.log"));
        QVERIFY(!fixPresent(dir.path()));
        QCOMPARE(readFix(dir.path()).step, QString());
        QVERIFY(writeFix(dir.path(), state));
        QVERIFY(fixPresent(dir.path()));
        const FixState back = readFix(dir.path());
        QCOMPARE(back.step, state.step);
        QCOMPARE(back.failedStep, state.failedStep);
        QCOMPARE(back.started, state.started);
        QCOMPARE(back.stepStarted, state.stepStarted);
        QCOMPARE(back.lastReminder, state.lastReminder);
        QCOMPARE(back.copies, 1200);
        QCOMPARE(back.error, state.error);
        QCOMPARE(back.touched, state.touched);
        QCOMPARE(back.questions.size(), 2);
        for (int i = 0; i < 2; ++i) {
            const FixQuestion &want = state.questions.at(i);
            const FixQuestion &got = back.questions.at(i);
            QCOMPARE(got.path, want.path);
            QCOMPARE(got.kind, want.kind);
            QCOMPARE(got.cloudSize, want.cloudSize);
            QCOMPARE(got.notebookSize, want.notebookSize);
            QCOMPARE(got.cloudTime, want.cloudTime);
            QCOMPARE(got.notebookTime, want.notebookTime);
            QCOMPARE(got.answer, want.answer);
        }
    }

    void fixStateUnreadableIsAFailedFix()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("fix.json"), "{ not json"));
        QVERIFY(fixPresent(dir.path()));
        FixState state = readFix(dir.path());
        QCOMPARE(state.step, QString("failed"));
        QVERIFY(!state.error.isEmpty());
        QVERIFY(write(dir.filePath("fix.json"), R"({"version": 2, "step": "base"})"));
        state = readFix(dir.path());
        QCOMPARE(state.step, QString("failed"));
        QVERIFY(!state.error.isEmpty());
        QVERIFY(write(dir.filePath("fix.json"), R"({"version": 1, "step": "dancing"})"));
        QCOMPARE(readFix(dir.path()).step, QString("failed"));
    }

    void fixClearRemovesBothFiles()
    {
        QTemporaryDir dir;
        FixState state;
        state.step = "confirm";
        QVERIFY(writeFix(dir.path(), state));
        QVERIFY(write(fixDryRunLogPath(dir.path()), "a log"));
        QVERIFY(write(dir.filePath("fix-answers.json"), "{}"));
        QVERIFY(clearFix(dir.path()));
        QVERIFY(!fixPresent(dir.path()));
        QVERIFY(!QFile::exists(fixDryRunLogPath(dir.path())));
        QVERIFY(!QFile::exists(dir.filePath("fix-answers.json")));
        QVERIFY(clearFix(dir.path()));
    }

    void fixStepsAreWaitingOrWorking()
    {
        for (const QString &step : QStringList{"confirm", "questions"}) {
            QVERIFY(fixWaiting(step));
            QVERIFY(!fixWorking(step));
        }
        for (const QString &step : QStringList{"preparing", "base", "finishing"}) {
            QVERIFY(fixWorking(step));
            QVERIFY(!fixWaiting(step));
        }
        QVERIFY(!fixWaiting("failed"));
        QVERIFY(!fixWorking("failed"));
        QVERIFY(!fixWorking(""));
    }
    void fixResyncArgumentsDropTheRoutine()
    {
        const SyncConfig config = oldUnitConfig();
        const QStringList args = fixResyncArguments(config, false, "/tmp/fix.log");
        QCOMPARE(args.mid(0, 7), (QStringList{"bisync", "gdrive:", "/home/someone/Drive", "--workdir", config.workDir,
                                              "--filters-file", config.filtersFile}));
        for (const QString &kept :
             QStringList{"--drive-skip-gdocs", "--drive-skip-shortcuts", "--fast-list", "--resync", "--check-sync=false", "-v"})
            QVERIFY2(args.contains(kept), qPrintable(kept));
        QCOMPARE(args.at(args.indexOf("--resync-mode") + 1), QString("newer"));
        QCOMPARE(args.count("--max-lock"), 1);
        QCOMPARE(args.at(args.indexOf("--max-lock") + 1), QString("2m"));
        QCOMPARE(args.count("--log-file"), 1);
        QCOMPARE(args.at(args.indexOf("--log-file") + 1), QString("/tmp/fix.log"));
        for (const QString &dropped :
             QStringList{"--resilient", "--recover", "--max-delete", "5", "--conflict-resolve", "--log-level", "INFO", "--dry-run"})
            QVERIFY2(!args.contains(dropped), qPrintable(dropped));
        QVERIFY(fixResyncArguments(config, true, "/tmp/fix.log").contains("--dry-run"));

        // Flags written with "=", more output asked for, and a flag the fix keeps.
        SyncConfig other = config;
        other.rcloneFlags = {"--max-delete=5", "--check-sync=only", "-vv", "--drive-chunk-size", "64M", "--max-lock=5m",
                             "--resilient=true", "--log-file=/elsewhere.log"};
        const QStringList second = fixResyncArguments(other, false, "/tmp/fix.log");
        for (const QString &dropped : QStringList{"--max-delete=5", "--check-sync=only", "-vv", "--max-lock=5m",
                                                  "--resilient=true", "--log-file=/elsewhere.log"})
            QVERIFY2(!second.contains(dropped), qPrintable(dropped));
        QCOMPARE(second.at(second.indexOf("--drive-chunk-size") + 1), QString("64M"));
    }

    void remoteFileJoins()
    {
        QCOMPARE(remoteFile("gdrive:", "a/b.txt"), QString("gdrive:a/b.txt"));
        QCOMPARE(remoteFile("gdrive:sub", "a/b.txt"), QString("gdrive:sub/a/b.txt"));
        QCOMPARE(remoteFile("gdrive:sub/", "a b.txt"), QString("gdrive:sub/a b.txt"));
        QCOMPARE(remoteFile("gdrive:", "My Folder/a b.txt"), QString("gdrive:My Folder/a b.txt"));
    }

    void dryRunCopiesStopsAtTheLimit()
    {
        // Headings and copy lines as rclone 1.75.2 writes a resync dry run (2026-11-12).
        QByteArray log = "2026/11/11 12:00:00 INFO  : Bisyncing with Comparison Settings:\n"
                         "2026/11/11 12:00:00 NOTICE: - Path2             Resync is copying files to                  - Path1\n";
        for (int i = 0; i < 1200; ++i) {
            const QString name = i == 3   ? QString("Odd: name.txt")
                                 : i == 5 ? QString::fromUtf8("Ação.txt")
                                          : QString("Folder/file %1.txt").arg(i, 4, 10, QChar('0'));
            log += QString("2026/11/11 12:00:01 NOTICE: %1: Skipped copy as --dry-run is set (size 10)\n").arg(name).toUtf8();
            if (i % 100 == 0)
                log += "2026/11/11 12:00:01 INFO  : Path1 checking for diffs\n";
        }
        log += "2026/11/11 12:00:05 NOTICE: - Path1             Resync is copying files to                  - Path2\n";
        for (const char *name : {"Deleted/on purpose.txt", "Photos/back.jpg"})
            log += QString("2026/11/11 12:00:06 NOTICE: %1: Skipped copy as --dry-run is set (size 3)\n").arg(name).toUtf8();
        log += "2026/11/11 12:00:09 INFO  : Bisync successful\n";
        int total = 0;
        int toNotebook = 0;
        const QList<DryRunCopy> copies = dryRunCopies(log, 500, &total, &toNotebook);
        QCOMPARE(total, 1202);
        QCOMPARE(toNotebook, 2);
        QCOMPARE(copies.size(), 500);
        // Copies to the notebook come first: a file deleted there and coming back shows among them.
        QCOMPARE(copies.at(0).path, QString("Deleted/on purpose.txt"));
        QVERIFY(copies.at(0).toNotebook);
        QCOMPARE(copies.at(1).path, QString("Photos/back.jpg"));
        QCOMPARE(copies.at(2).path, QString("Folder/file 0000.txt"));
        QVERIFY(!copies.at(2).toNotebook);
        QCOMPARE(copies.at(5).path, QString("Odd: name.txt"));
        QCOMPARE(copies.at(7).path, QString::fromUtf8("Ação.txt"));
        QCOMPARE(copies.last().path, QString("Folder/file 0497.txt"));
        QVERIFY(dryRunCopies(QByteArray(), 500, &total, &toNotebook).isEmpty());
        QCOMPARE(total, 0);
        QCOMPARE(toNotebook, 0);
    }

    void fixRefusesATrashlessConfig()
    {
        SyncConfig config = oldUnitConfig();
        QVERIFY(fixConfigRefusal(config).isEmpty());
        config.rcloneFlags << "--drive-use-trash=false";
        QVERIFY2(fixConfigRefusal(config).contains("--drive-use-trash=false"), qPrintable(fixConfigRefusal(config)));
        config.rcloneFlags.last() = "--drive-use-trash=true";
        QVERIFY(fixConfigRefusal(config).isEmpty());
    }
    void fixStartWritesTheMarkBeforeTheUnit()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QCOMPARE(fixCommand(s, {"--start"}), QString());
        const QStringList seen = calls(dir.path());
        const qsizetype start = seen.indexOf("systemctl --user start --no-block drive-sync-fix.service");
        QVERIFY2(start >= 0, qPrintable(seen.join('\n')));
        QCOMPARE(seen.value(start + 1), QString("mark present"));
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("preparing"));
        QVERIFY(state.started.isValid());

        // A second start, an unmanaged drive-sync, a trashless config, and a unit that does not start.
        QVERIFY(fixCommand(s, {"--start"}).contains("already"));
        QVERIFY(clearFix(s.paths.appState));
        s.paths.managed = false;
        QVERIFY(fixCommand(s, {"--start"}).contains("take it over"));
        s.paths.managed = true;
        SyncConfig trashless = s.config;
        trashless.rcloneFlags << "--drive-use-trash=false";
        QString error;
        QVERIFY(saveSyncConfig(s.paths.config, trashless, &error));
        QVERIFY(fixCommand(s, {"--start"}).contains("--drive-use-trash=false"));
        QVERIFY(!fixPresent(s.paths.appState));
        QVERIFY(saveSyncConfig(s.paths.config, s.config, &error));
        QVERIFY(write(dir.filePath("start-exit"), "1\n"));
        QVERIFY(fixCommand(s, {"--start"}).contains("drive-sync-fix.service"));
        QVERIFY(!fixPresent(s.paths.appState));
    }

    void fixPreparesWithoutTouchingTheMainLog()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QByteArray mainLog = "2026/11/11 11:00:00 INFO  : Bisync successful\n";
        QVERIFY(write(s.config.logFile, mainLog));
        QVERIFY(write(dir.filePath("service-active"), "active\n"));  // a normal run that ends after one poll
        QVERIFY(writeFix(s.paths.appState, fixAt("preparing")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        QCOMPARE(*s.waits, QList<int>{5});
        QCOMPARE(readBytes(s.config.logFile), mainLog);
        QVERIFY(readBytes(fixDryRunLogPath(s.paths.appState)).contains("Skipped copy as --dry-run"));
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("confirm"));
        QCOMPARE(state.copies, 2);
        const QStringList rclone = calls(dir.path()).filter(QRegularExpression("^rclone "));
        QCOMPARE(rclone.size(), 1);
        QVERIFY(rclone.first().contains("--resync --resync-mode newer --check-sync=false --max-lock 2m -v --log-file "
                                        + fixDryRunLogPath(s.paths.appState) + " --dry-run"));
        QVERIFY(!rclone.first().contains("--resilient"));
    }

    void fixConfirmOnlyFromConfirm()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(writeFix(s.paths.appState, fixAt("questions")));
        QVERIFY(fixCommand(s, {"--confirm"}).contains("nothing to confirm"));
        QCOMPARE(readFix(s.paths.appState).step, QString("questions"));
        QVERIFY(calls(dir.path()).filter("start").isEmpty());
        QVERIFY(writeFix(s.paths.appState, fixAt("confirm")));
        QCOMPARE(fixCommand(s, {"--confirm"}), QString());
        QCOMPARE(readFix(s.paths.appState).step, QString("base"));
        QVERIFY(calls(dir.path()).contains("systemctl --user start --no-block drive-sync-fix.service"));
        QVERIFY(clearFix(s.paths.appState));
        QVERIFY(fixCommand(s, {"--confirm"}).contains("no fix"));
    }

    void fixBaseWithoutQuestionsFinishes()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(cannedListings(dir.path(), listingLine("same.txt", 3, when), listingLine("same.txt", 3, when)));
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        QVERIFY(!fixPresent(s.paths.appState));
        QVERIFY(QFile::exists(s.config.filtersFile + ".md5"));
        const QStringList rclone = calls(dir.path()).filter(QRegularExpression("^rclone "));
        QCOMPARE(rclone.size(), 2);
        QVERIFY(rclone.at(0).contains("--resync"));
        QVERIFY(!rclone.at(0).contains("--dry-run"));
        QVERIFY(rclone.at(0).contains("--log-file " + s.config.logFile));
        QVERIFY(!rclone.at(1).contains("--resync"));  // the normal run, with the routine flags back
        QVERIFY(rclone.at(1).contains("--resilient"));
        QVERIFY(notifications(dir.path()).join('\n').contains("Drive is syncing again"));
    }

    void fixRetryWaitsOutTheLock()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        // A lock line from an older run is in the log, and this run fails on something else.
        QVERIFY(write(s.config.logFile, "2026/11/10 09:00:00 ERROR : Bisync critical error: prior lock file found: old\n"));
        QVERIFY(write(dir.filePath("base-fails"), ""));
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        QVERIFY(s.waits->isEmpty());
        FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        QCOMPARE(state.failedStep, QString("base"));
        QVERIFY2(state.error.contains("boom"), qPrintable(state.error));
        QVERIFY(notifications(dir.path()).join('\n').contains("The Drive fix stopped"));
        QVERIFY(state.lastReminder.isValid());

        // This run meets the lock a killed run left: it waits it out and tries once more.
        QVERIFY(QFile::remove(dir.filePath("base-fails")));
        QVERIFY(write(dir.filePath("lock-once"), ""));
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(cannedListings(dir.path(), listingLine("same.txt", 3, when), listingLine("same.txt", 3, when)));
        QCOMPARE(fixCommand(s, {"--retry"}), QString());
        QCOMPARE(readFix(s.paths.appState).step, QString("base"));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        QCOMPARE(*s.waits, QList<int>{130});
        QVERIFY(!fixPresent(s.paths.appState));
    }

    void fixBaseTouchesNotebookNewerFiles()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        for (const QString &name : QStringList{"edited.txt", "new.txt"}) {
            QVERIFY(write(s.config.localPath + "/" + name, "data\n"));
            QVERIFY(setOldMtime(s.config.localPath + "/" + name));
        }
        QVERIFY(cannedListings(dir.path(), listingLine("edited.txt", 5, when) + listingLine("gone.txt", 2, when),
                               listingLine("edited.txt", 5, when.addSecs(60)) + listingLine("gone.txt", 2, when.addSecs(60))
                                   + listingLine("new.txt", 5, when)));
        QVERIFY(write(dir.filePath("normal-fails"), ""));  // stops at the end, so the state can be read
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        QCOMPARE(state.failedStep, QString("finishing"));
        QVERIFY2(state.error.contains("normal broke"), qPrintable(state.error));
        QCOMPARE(state.touched, (QStringList{"edited.txt", "new.txt"}));  // gone.txt is not there to touch
        QVERIFY(state.questions.isEmpty());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY(mtimeOf(s.config.localPath + "/edited.txt") >= now - 60);
        QVERIFY(mtimeOf(s.config.localPath + "/new.txt") >= now - 60);
        QVERIFY(!QFile::exists(s.config.localPath + "/gone.txt"));
    }

    void fixBaseAsksAboutCloudFiles()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(cannedListings(dir.path(),
                               listingLine("remote.txt", 7, when.addSecs(60)) + listingLine("only-cloud.txt", 4, when),
                               listingLine("remote.txt", 6, when)));
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("questions"));
        QCOMPARE(state.questions.size(), 2);
        QCOMPARE(state.questions.at(0).path, QString("only-cloud.txt"));
        QCOMPARE(state.questions.at(0).kind, QString("onlyCloud"));
        QCOMPARE(state.questions.at(0).cloudSize, 4LL);
        QCOMPARE(state.questions.at(0).notebookSize, -1LL);
        QCOMPARE(state.questions.at(0).notebookTime, 0LL);
        QCOMPARE(state.questions.at(1).path, QString("remote.txt"));
        QCOMPARE(state.questions.at(1).kind, QString("differs"));
        QCOMPARE(state.questions.at(1).cloudTime, when.addSecs(60).toSecsSinceEpoch());
        QCOMPARE(state.questions.at(1).notebookTime, when.toSecsSinceEpoch());
        QCOMPARE(state.questions.at(1).notebookSize, 6LL);
        QCOMPARE(calls(dir.path()).filter(QRegularExpression("^rclone ")).size(), 1);  // no normal run yet
        // A card at once, not an hour later: the timer is held from now on.
        QVERIFY(notifications(dir.path()).join('\n').contains("The Drive fix is waiting for you"));
        QVERIFY(state.lastReminder.isValid());
    }

    void fixAnswersMustCoverEveryQuestion()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        FixState state = fixAt("questions");
        state.questions = {question("a.txt", "differs"), question("b.txt", "onlyCloud")};
        QVERIFY(writeFix(s.paths.appState, state));
        const QString answers = dir.filePath("answers.json");
        QVERIFY(write(answers, R"({"a.txt": "cloud"})"));
        QVERIFY(fixCommand(s, {"--answers", answers}).contains("b.txt"));
        QVERIFY(write(answers, R"({"a.txt": "cloud", "b.txt": "maybe"})"));
        QVERIFY(fixCommand(s, {"--answers", answers}).contains("b.txt"));
        QVERIFY(write(answers, "not json"));
        QVERIFY(!fixCommand(s, {"--answers", answers}).isEmpty());
        QCOMPARE(readFix(s.paths.appState).step, QString("questions"));
        QVERIFY(calls(dir.path()).filter("start").isEmpty());

        // A path that was never asked about is ignored, never acted on.
        QVERIFY(write(answers, R"({"a.txt": "cloud", "b.txt": "notebook", "../../etc/passwd": "notebook"})"));
        QCOMPARE(fixCommand(s, {"--answers", answers}), QString());
        const FixState after = readFix(s.paths.appState);
        QCOMPARE(after.step, QString("finishing"));
        QCOMPARE(after.questions.size(), 2);
        QCOMPARE(after.questions.at(0).answer, QString("cloud"));
        QCOMPARE(after.questions.at(1).answer, QString("notebook"));
        QVERIFY(calls(dir.path()).contains("systemctl --user start --no-block drive-sync-fix.service"));
    }

    void fixApplyIsIdempotent()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(write(s.config.filtersFile + ".md5",
                      QCryptographicHash::hash(readBytes(s.config.filtersFile), QCryptographicHash::Md5).toHex()));
        QVERIFY(write(s.config.localPath + "/c.txt", "mine\n"));
        QVERIFY(setOldMtime(s.config.localPath + "/c.txt"));
        QVERIFY(baseListings(s));
        FixState state = fixAt("finishing");
        state.questions = {question("a.txt", "differs", "cloud"), question("b b.txt", "onlyCloud", "notebook"),
                           question("c.txt", "differs", "notebook"), question("d.txt", "differs", "notebook")};
        QVERIFY(writeFix(s.paths.appState, state));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        QVERIFY(!fixPresent(s.paths.appState));
        QStringList rclone = calls(dir.path()).filter(QRegularExpression("^rclone "));
        // The config's remote flags go along, and a delete always goes to the Drive trash.
        QCOMPARE(rclone.mid(0, 2), (QStringList{"rclone touch --no-create gdrive:a.txt --drive-skip-gdocs",
                                                "rclone deletefile gdrive:b b.txt --drive-skip-gdocs --drive-use-trash=true"}));
        QVERIFY(rclone.at(2).startsWith("rclone bisync"));
        QVERIFY(mtimeOf(s.config.localPath + "/c.txt") >= QDateTime::currentSecsSinceEpoch() - 60);
        QVERIFY(!QFile::exists(s.config.localPath + "/d.txt"));  // gone in the meantime: skipped

        // The unit died after the answers were applied: the second time the
        // file is already in the trash, and deletefile says so with 4.
        QVERIFY(write(dir.filePath("deletefile-exit"), "4\n"));
        QVERIFY(writeFix(s.paths.appState, state));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        QVERIFY(!fixPresent(s.paths.appState));

        // Any other exit is a failure, and the fix stops there.
        QVERIFY(write(dir.filePath("deletefile-exit"), "5\n"));
        QVERIFY(writeFix(s.paths.appState, state));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState failed = readFix(s.paths.appState);
        QCOMPARE(failed.failedStep, QString("finishing"));
        QVERIFY2(failed.error.contains("b b.txt"), qPrintable(failed.error));
    }

    void fixFinishingRefusesWithoutTheMd5()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(baseListings(s));
        QVERIFY(writeFix(s.paths.appState, fixAt("finishing")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        QCOMPARE(state.failedStep, QString("finishing"));
        QVERIFY2(state.error.contains(".md5"), qPrintable(state.error));
        QVERIFY(calls(dir.path()).filter(QRegularExpression("^rclone ")).isEmpty());
    }

    void fixCancelWaitsForTheStep()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QVERIFY(write(fixDryRunLogPath(s.paths.appState), "a log"));
        QVERIFY(write(dir.filePath("fix-active"), "active\n"));
        QVERIFY(fixCommand(s, {"--cancel"}).contains("wait"));
        QVERIFY(fixPresent(s.paths.appState));
        QVERIFY(write(dir.filePath("fix-active"), "inactive\n"));
        QCOMPARE(fixCommand(s, {"--cancel"}), QString());
        QVERIFY(!fixPresent(s.paths.appState));
        QVERIFY(!QFile::exists(fixDryRunLogPath(s.paths.appState)));
        QCOMPARE(fixCommand(s, {"--cancel"}), QString());  // nothing left to cancel is not an error
    }

    void fixRetryResumesTheStep()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        FixState failed = fixAt("failed");
        failed.failedStep = "base";
        failed.error = "boom";
        QVERIFY(writeFix(s.paths.appState, failed));
        QCOMPARE(fixCommand(s, {"--retry"}), QString());
        FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("base"));
        QVERIFY(state.error.isEmpty());
        QCOMPARE(calls(dir.path()).filter("start --no-block drive-sync-fix.service").size(), 1);

        // Interrupted: a working step with the unit gone.
        QVERIFY(writeFix(s.paths.appState, fixAt("finishing")));
        QCOMPARE(fixCommand(s, {"--retry"}), QString());
        QCOMPARE(readFix(s.paths.appState).step, QString("finishing"));
        QCOMPARE(calls(dir.path()).filter("start --no-block drive-sync-fix.service").size(), 2);

        // Running, or waiting for the user: nothing to retry.
        QVERIFY(write(dir.filePath("fix-active"), "active\n"));
        QVERIFY(!fixCommand(s, {"--retry"}).isEmpty());
        QVERIFY(write(dir.filePath("fix-active"), "inactive\n"));
        QVERIFY(writeFix(s.paths.appState, fixAt("confirm")));
        QVERIFY(fixCommand(s, {"--retry"}).contains("nothing to retry"));

        // A file that cannot be read can only be cancelled.
        QVERIFY(write(fixStatePath(s.paths.appState), "garbage"));
        QVERIFY(fixCommand(s, {"--retry"}).contains("--cancel"));
        QCOMPARE(calls(dir.path()).filter("start --no-block drive-sync-fix.service").size(), 2);
    }

    void takeoverRefusedDuringAFix()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        QVERIFY(writeFix(old.paths.appState, fixAt("confirm")));
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QVERIFY2(plan.blockers.contains("fixActive"), qPrintable(plan.blockers.join(',')));
        QString error;
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("fix"), qPrintable(error));
    }

    void fixEndToEndWithRealRclone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Real rclone on two local folders, with no rclone config at all,
        // and filters that changed since the last base: no .md5.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(QDir().mkpath(dir.filePath("cloud")));
        s.config.remote = dir.filePath("cloud");
        s.config.rcloneFlags = {"--config", "/dev/null", "--resilient", "--recover", "--max-lock", "2m", "--max-delete",
                                "50", "--conflict-resolve", "newer"};
        QString error;
        QVERIFY(saveSyncConfig(s.paths.config, s.config, &error));
        s.tools.rclone = rclone;
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        QVERIFY(write(s.config.localPath + "/two.txt", "two\n"));
        QVERIFY(write(s.config.localPath + "/.git/skipped", "x\n") || QDir().mkpath(s.config.localPath + "/.git"));
        QVERIFY(write(s.config.localPath + "/.git/skipped", "x\n"));

        QCOMPARE(fixCommand(s, {"--start"}), QString());
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        FixState state = readFix(s.paths.appState);
        QVERIFY2(state.step == "confirm", qPrintable(state.step + ": " + state.error));
        QCOMPARE(state.copies, 2);
        QVERIFY(!QFile::exists(s.config.localPath + "/one.txt"));  // the dry run copied nothing
        // Real rclone's headings give each copy its direction.
        int total = 0;
        int toNotebook = 0;
        const QList<DryRunCopy> copies = dryRunCopies(readBytes(fixDryRunLogPath(s.paths.appState)), 10, &total, &toNotebook);
        QCOMPARE(total, 2);
        QCOMPARE(toNotebook, 1);
        QCOMPARE(copies.first().path, QString("one.txt"));
        QVERIFY(copies.first().toNotebook);

        QCOMPARE(fixCommand(s, {"--confirm"}), QString());
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
        state = readFix(s.paths.appState);
        QVERIFY2(!fixPresent(s.paths.appState), qPrintable(state.step + ": " + state.error));
        QVERIFY(QFile::exists(s.config.filtersFile + ".md5"));
        QVERIFY(QFile::exists(s.config.localPath + "/one.txt"));
        QVERIFY(QFile::exists(dir.filePath("cloud/two.txt")));
        QVERIFY(!QFile::exists(dir.filePath("cloud/.git/skipped")));
        QVERIFY(QDir(s.config.workDir).entryList({"*-err"}).isEmpty());
        // The listings carry the name DriveSync computes for these two paths.
        QVERIFY(QFile::exists(s.config.workDir + "/" + bisyncSessionName(s.config.remote, s.config.localPath) + ".path1.lst"));
        QCOMPARE(runConditionCommand(s.paths, s.config, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(readBytes(s.config.logFile).contains("Bisync successful"));
    }
    void runConditionSkipsDuringAFix()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runConditionCommand(paths, config, now), 0);
        QVERIFY(writeFix(paths.appState, fixAt("questions")));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(write(fixStatePath(paths.appState), "garbage"));  // unreadable still holds the runs
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(clearFix(paths.appState));
        QCOMPARE(runConditionCommand(paths, config, now), 0);
    }

    void summarizesAFix()
    {
        SyncInputs in = healthy();
        in.fixStep = "base";
        QCOMPARE(Backend::summarize(in).status, QString("fixing"));
        in.running = true;  // the base run writes to the log: the fix still wins
        QCOMPARE(Backend::summarize(in).status, QString("fixing"));
        in.running = false;
        for (const QString &step : QStringList{"confirm", "questions"}) {
            in.fixStep = step;
            QCOMPARE(Backend::summarize(in).status, QString("fixWaiting"));
        }
        in.fixStep = "failed";
        in.fixError = "boom";
        SyncSummary s = Backend::summarize(in);
        QCOMPARE(s.status, QString("fixFailed"));
        QCOMPARE(s.causeKind, QString("fix"));
        QCOMPARE(s.causeDetail, QString("boom"));
        in.fixStep = "finishing";
        in.fixError.clear();
        in.fixInterrupted = true;
        s = Backend::summarize(in);
        QCOMPARE(s.status, QString("fixFailed"));
        QCOMPARE(s.causeKind, QString("fixInterrupted"));
        QCOMPARE(s.causeDetail, QString("the fix stopped halfway"));
        in.systemctlWorks = false;
        QCOMPARE(Backend::summarize(in).status, QString("unavailable"));
    }

    void snapshotFixInterrupted()
    {
        const QDateTime now(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.addSecs(-3600), 480), now);
        src.managed = true;
        src.fixPresent = true;
        src.fix = fixAt("base");
        src.fix.stepStarted = now.addSecs(-600);
        Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("fixFailed"));
        QCOMPARE(s.summary.causeKind, QString("fixInterrupted"));
        // Just started: systemd may not have the unit up yet.
        src.fix.stepStarted = now.addSecs(-10);
        QCOMPARE(buildSnapshot(src).summary.status, QString("fixing"));
        src.fix.stepStarted = now.addSecs(-600);
        src.fixUnitActive = true;
        s = buildSnapshot(src);
        QCOMPARE(s.summary.status, QString("fixing"));
        QCOMPARE(s.tone, QString("busy"));
        QVERIFY(s.resync);
        QCOMPARE(snapshotToJson(s, QDateTime(), now).value("resync").toBool(), true);
        src.fix = fixAt("questions");
        src.fixUnitActive = false;
        QCOMPARE(buildSnapshot(src).summary.status, QString("fixWaiting"));
        QVERIFY(!buildSnapshot(src).resync);
    }

    void canSyncNowIsFalseDuringAFix()
    {
        const QDateTime now(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.addSecs(-3600), 480), now);
        src.managed = true;
        QVERIFY(buildSnapshot(src).canSyncNow);
        src.fixPresent = true;
        src.fix = fixAt("questions");
        QVERIFY(!buildSnapshot(src).canSyncNow);
    }

    void gatherSourcesReadsTheFix()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = fakeSystemctl(dir.path(), QDateTime::currentDateTime().addSecs(-100), "inactive", "activating");
        QVERIFY(QDir().mkpath(paths.drive));
        SnapshotSources src = gatherSources(paths, QDateTime::currentDateTimeUtc(), 5000, nullptr);
        QVERIFY(src.systemctlWorks);
        QVERIFY(!src.fixPresent);
        QVERIFY(src.fixUnitActive);
        QVERIFY(writeFix(paths.appState, fixAt("base")));
        src = gatherSources(paths, QDateTime::currentDateTimeUtc(), 5000, nullptr);
        QVERIFY(src.fixPresent);
        QCOMPARE(src.fix.step, QString("base"));
        QCOMPARE(buildSnapshot(src).summary.status, QString("fixing"));
    }

    void classifiesChangedFilters()
    {
        QTemporaryDir dir;
        const QString filters = dir.filePath("filters.txt");
        QVERIFY(write(filters, "- .git/**\n"));
        QString why;
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY2(why.contains(kFiltersChangedReason), qPrintable(why));  // the .md5 is missing
        QVERIFY(write(filters + ".md5", "0123456789abcdef0123456789abcdef"));
        QVERIFY(!filtersUsable(filters, &why));
        QVERIFY2(why.contains(kFiltersChangedReason), qPrintable(why));  // the filters changed
        // The line "drive-sync run" writes when it refuses, read back from the log.
        const QString log = QString("2026/11/11 12:00:00 CRITICAL: DriveSync did not start rclone: %1\n").arg(why);
        const QList<SyncRun> runs = SyncLog::parse(log);
        QCOMPARE(runs.size(), 1);
        QCOMPARE(runs.first().outcome, QString("failed"));
        QCOMPARE(runs.first().causeKind, QString("filtersChanged"));
    }
    void watchdogRemindsAWaitingFix()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        FixState state = fixAt("questions");
        state.stepStarted = now;
        QVERIFY(writeFix(paths.appState, state));

        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(1800)), 0);
        QVERIFY(notifications(dir.path()).isEmpty());  // just asked: no card yet
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QVERIFY(notifications(dir.path()).at(0).contains("The Drive fix is waiting for you"));
        QCOMPARE(readFix(paths.appState).lastReminder, now.addSecs(3600));
        QCOMPARE(readFix(paths.appState).step, QString("questions"));
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600 + 59 * 60)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(7200)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);

        // A step the unit left halfway, or one that failed: the fix stopped.
        state = fixAt("base");
        state.stepStarted = now;
        QVERIFY(writeFix(paths.appState, state));
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 3);
        QVERIFY(notifications(dir.path()).at(2).contains("The Drive fix stopped"));
        QVERIFY(notifications(dir.path()).at(2).contains("critical"));
        state = fixAt("failed");
        state.stepStarted = now;
        state.error = "boom";
        QVERIFY(writeFix(paths.appState, state));
        QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 4);
        QVERIFY(notifications(dir.path()).at(3).contains("boom"));

        // A step at work says nothing.
        paths.systemctl = fixSystemctl(dir.path(), paths.appState);
        QVERIFY(write(dir.filePath("fix-active"), "activating\n"));
        state = fixAt("base");
        state.stepStarted = now;
        QVERIFY(writeFix(paths.appState, state));
        watchdogOver(paths, config, tools, now.addSecs(3 * 3600));
        QCOMPARE(notifications(dir.path()).filter("Drive fix").size(), 4);
    }

    void notifyTreatsChangedFiltersAsACause()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, line(QDateTime::currentDateTime(), "CRITICAL",
                                      QString("DriveSync did not start rclone: filters.txt changed since the last "
                                              "resync, %1").arg(kFiltersChangedReason)).toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("-u normal"), qPrintable(sent.at(0)));
        QVERIFY(sent.at(0).contains("Drive filters changed"));
        QVERIFY(sent.at(0).contains("use Fix…"));
        QVERIFY(!sent.at(0).contains("Drive stopped syncing"));
        // Every refused tick does not bring a new card.
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(900)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        // The fix ends it like any other trouble.
        QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(1800)), 0);
        QVERIFY(calls(dir.path()).join('\n').contains("omarchy notification dismiss Drive filters changed"));
    }

    void fixEndCardIsTranslated()
    {
        QFile catalog(QString(SRCDIR) + "/../src/i18n/pt-BR.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const QJsonObject strings = QJsonDocument::fromJson(catalog.readAll()).object();
        for (const char *text : {"The fix rebuilt the base of both sides.", "The Drive fix is waiting for you",
                                 "The Drive fix stopped", "Drive filters changed", "Drive filters still changed"})
            QVERIFY2(strings.contains(text), text);
    }

    // Step 5.2: every per-account card names its account in its title.
    void cardTitlesNameTheAccount()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        {
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.label = "Drive2";
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            const NotifierPaths tools = fakeNotifier(dir.path());
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
            QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 1);
            QVERIFY2(sent.at(0).contains("-i dialog-warning Drive stopped syncing (Drive2) "), qPrintable(sent.at(0)));
            QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(900)), 0);
            const QStringList all = calls(dir.path());
            QVERIFY(all.contains("omarchy notification dismiss Drive stopped syncing (Drive2)"));
            QVERIFY(all.contains("omarchy notification dismiss Drive is still stopped (Drive2)"));
            sent = notifications(dir.path());
            QCOMPARE(sent.size(), 2);
            QVERIFY2(sent.at(1).contains("-i dialog-information Drive is syncing again (Drive2) "),
                     qPrintable(sent.at(1)));
            for (const QString &call : all)
                if (call.startsWith("omarchy notification dismiss "))
                    QVERIFY2(call.endsWith("(Drive2)"), qPrintable(call));
        }
        {
            // The watchdog's timer card, and the card that says it is back.
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            SyncConfig config = oldUnitConfig();
            config.runOnlyWhen.clear();
            config.localPath = dir.filePath("Drive2");
            const NotifierPaths tools = fakeNotifier(dir.path());
            paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
            QCOMPARE(watchdogOver(paths, config, tools, now), 0);
            paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
            QCOMPARE(watchdogOver(paths, config, tools, now.addSecs(3600)), 0);
            const QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 2);
            QVERIFY2(sent.at(0).contains(" Drive will not sync (Drive2) "), qPrintable(sent.at(0)));
            QVERIFY2(sent.at(1).contains(" Drive timer is back (Drive2) "), qPrintable(sent.at(1)));
            QVERIFY(calls(dir.path()).contains("omarchy notification dismiss Drive will not sync (Drive2)"));
        }
        {
            // The reminder of a pause with no end.
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
            SyncConfig config = oldUnitConfig();
            config.runOnlyWhen.clear();
            config.localPath = dir.filePath("Drive2");
            const NotifierPaths tools = fakeNotifier(dir.path());
            const QDateTime start = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
            PauseMark mark;
            mark.present = true;
            mark.since = start;
            QVERIFY(writePause(pauseFilePath(paths.appState), mark));
            QCOMPARE(watchdogOver(paths, config, tools, start.addSecs(86400)), 0);
            QCOMPARE(notifications(dir.path()).size(), 1);
            QVERIFY2(notifications(dir.path()).at(0).contains(" DriveSync is still paused (Drive2) "),
                     qPrintable(notifications(dir.path()).at(0)));
        }
        {
            // The fix's cards and the deletions nobody confirmed.
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.label = "Drive2";
            const NotifierPaths tools = fakeNotifier(dir.path());
            notifyFixNeedsYou(paths, tools, fixAt("questions"), false);
            notifyFixNeedsYou(paths, tools, fixAt("failed"), true);
            notifyFixEnded(paths, tools);
            UnconfirmedDeletes extra;
            extra.cloudCount = 2;
            extra.backupDir = dir.filePath("backup");
            notifyUnconfirmedDeletes(paths, tools, extra);
            const QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 4);
            QVERIFY2(sent.at(0).contains(" The Drive fix is waiting for you (Drive2) "), qPrintable(sent.at(0)));
            QVERIFY2(sent.at(1).contains(" The Drive fix stopped (Drive2) "), qPrintable(sent.at(1)));
            QVERIFY2(sent.at(2).contains(" Drive is syncing again (Drive2) "), qPrintable(sent.at(2)));
            QVERIFY2(sent.at(3).contains(" Drive deleted files you did not confirm (Drive2) "), qPrintable(sent.at(3)));
            for (const QString &call : calls(dir.path()))
                if (call.startsWith("omarchy notification dismiss "))
                    QVERIFY2(call.endsWith("(Drive2)"), qPrintable(call));
        }
        // Paths with no label take their folder's base name; with no folder
        // either, the bare title.
        BackendPaths bare;
        QCOMPARE(accountCardTitle("Drive stopped syncing", bare), QString("Drive stopped syncing"));
        bare.drive = "/h/Mirror/";
        QCOMPARE(accountCardTitle("Drive stopped syncing", bare), QString("Drive stopped syncing (Mirror)"));
    }

    // Step 5.2: omarchy notification dismiss closes every card whose title
    // contains its argument, so no labelled title of one account may sit
    // inside another account's, in English or in Portuguese.
    void cardTitlesNeverMatchAnotherAccount()
    {
        QFile catalog(QString(SRCDIR) + "/../src/i18n/pt-BR.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const QJsonObject strings = QJsonDocument::fromJson(catalog.readAll()).object();
        QVERIFY(strings.contains("%1 (%2)"));
        const QStringList english = accountCardBareTitles();
        QCOMPARE(english.size(), 14);
        QStringList portuguese;
        for (const QString &title : english) {
            QVERIFY2(strings.value(title).isString(), qPrintable(title));
            portuguese << strings.value(title).toString();
        }
        const QStringList labels{"Drive", "Drive2", "My Drive", "Drive (old)", "a/Drive", "b/Drive"};
        const auto check = [&](const QStringList &titles, const QString &format) {
            for (int i = 0; i < titles.size(); ++i)
                for (int j = 0; j < titles.size(); ++j)
                    if (i != j)
                        QVERIFY2(!titles.at(j).contains(titles.at(i)),
                                 qPrintable(titles.at(i) + " in " + titles.at(j)));
            for (const QString &a : labels)
                for (const QString &b : labels) {
                    if (a == b)
                        continue;
                    for (const QString &one : titles)
                        for (const QString &other : titles) {
                            const QString mine = format.arg(one, a);
                            const QString theirs = format.arg(other, b);
                            QVERIFY2(!theirs.contains(mine), qPrintable(mine + " in " + theirs));
                        }
                }
        };
        check(english, "%1 (%2)");
        check(portuguese, strings.value("%1 (%2)").toString());
        // The English format is the one accountCardTitle builds.
        BackendPaths account;
        account.label = "My Drive";
        QCOMPARE(accountCardTitle(english.first(), account), QString("%1 (%2)").arg(english.first(), "My Drive"));
    }

    // Step 5.2: one account's recovery closes only that account's cards.
    void recoveryClosesOnlyItsAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.home = dir.path();
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", QString()));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", QString()));
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.accounts.size(), 2);
        const QDateTime since = QDateTime::currentDateTimeUtc().addSecs(-1800);
        int id = 41;
        for (const BackendPaths &account : set.accounts) {
            QVERIFY(QDir().mkpath(account.appState));
            QVERIFY(write(account.appState + "/notify-run.json",
                          QJsonDocument(QJsonObject{{"since", since.toSecsSinceEpoch()},
                                                    {"last", since.toSecsSinceEpoch()},
                                                    {"id", id++},
                                                    {"key", ""}})
                              .toJson()));
        }
        // As main gives it with two accounts: no bare title is ever dismissed.
        const NotifierPaths tools = fakeNotifier(dir.path());
        QVERIFY(tools.bareTitlesMark.isEmpty());
        QCOMPARE(runNotifyCommand("success", set.accounts.at(0), tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList all = calls(dir.path());
        const QStringList dismissed = all.filter(QRegularExpression("^omarchy notification dismiss "));
        QVERIFY(!dismissed.isEmpty());
        const QStringList bare = accountCardBareTitles();
        for (const QString &call : dismissed) {
            QVERIFY2(call.endsWith("(Drive)"), qPrintable(call));
            QVERIFY2(!bare.contains(call.mid(QString("omarchy notification dismiss ").size())), qPrintable(call));
        }
        const QStringList closedIds = all.filter("CloseNotification");
        QCOMPARE(closedIds.size(), 1);
        QVERIFY2(closedIds.first().endsWith(" 41"), qPrintable(closedIds.first()));
        QVERIFY(!QFile::exists(set.accounts.at(0).appState + "/notify-run.json"));
        QVERIFY(QFile::exists(set.accounts.at(1).appState + "/notify-run.json"));
    }

    // Step 5.2: the 5.1 cards had bare titles. They are dismissed once, before
    // the first labelled card, and only when the mark could be written.
    void bareTitlesGoOnceWithOneAccount()
    {
        const QStringList bare = accountCardBareTitles();
        const auto bareDismissals = [&bare](const QString &dir) {
            QStringList found;
            for (const QString &call : calls(dir))
                if (call.startsWith("omarchy notification dismiss ")
                    && bare.contains(call.mid(QString("omarchy notification dismiss ").size())))
                    found << call;
            return found;
        };
        const QDateTime now = QDateTime::currentDateTimeUtc();
        {
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            NotifierPaths tools = fakeNotifier(dir.path());
            tools.bareTitlesMark = paths.appState + "/notify-bare-titles.done";
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
            QVERIFY(QFile::exists(tools.bareTitlesMark));
            QCOMPARE(bareDismissals(dir.path()).size(), 14);
            const QStringList all = calls(dir.path());
            qsizetype lastBare = -1;
            for (qsizetype i = 0; i < all.size(); ++i)
                if (bareDismissals(dir.path()).contains(all.at(i)))
                    lastBare = i;
            qsizetype firstNotify = -1;
            for (qsizetype i = 0; i < all.size() && firstNotify < 0; ++i)
                if (all.at(i).startsWith("notify-send"))
                    firstNotify = i;
            QVERIFY(lastBare >= 0 && firstNotify > lastBare);
            // The hourly reminder dismisses no bare title.
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3700)), 0);
            QCOMPARE(notifications(dir.path()).size(), 2);
            QCOMPARE(bareDismissals(dir.path()).size(), 14);
        }
        {
            // A mark that cannot be written: never a bare title, since each
            // one would also close the account's own labelled card.
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            const QString locked = dir.filePath("locked");
            QVERIFY(QDir().mkpath(locked));
            QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner | QFile::ExeOwner));
            const auto restore = qScopeGuard([&locked] {
                QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            });
            NotifierPaths tools = fakeNotifier(dir.path());
            tools.bareTitlesMark = locked + "/notify-bare-titles.done";
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3700)), 0);
            QCOMPARE(notifications(dir.path()).size(), 2);
            QVERIFY(bareDismissals(dir.path()).isEmpty());
            QVERIFY(!QFile::exists(tools.bareTitlesMark));
        }
        {
            // While the move's journal is there: nothing, and no mark.
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            QVERIFY(QDir().mkpath(paths.globalState()));
            QVERIFY(write(migrationJournalPath(paths.globalState()), "{}"));
            NotifierPaths tools = fakeNotifier(dir.path());
            tools.bareTitlesMark = paths.appState + "/notify-bare-titles.done";
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
            QVERIFY(bareDismissals(dir.path()).isEmpty());
            QVERIFY(!QFile::exists(tools.bareTitlesMark));
        }
        {
            // No mark path (several accounts): never.
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, failedRun(QDateTime::currentDateTime().addSecs(-60)).toUtf8()));
            const NotifierPaths tools = fakeNotifier(dir.path());
            QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
            QCOMPARE(runNotifyCommand("success", paths, tools, now.addSecs(900)), 0);
            QVERIFY(bareDismissals(dir.path()).isEmpty());
        }
        {
            // The watchdog sets the mark itself, with one account only.
            QTemporaryDir one;
            const MoveHome h = makeAccountsHome(one.path(), {"account1"});
            QCOMPARE(runWatchdogCommand(h.roots, h.tools, fakeNotifier(one.path()), now), 0);
            QVERIFY(QFile::exists(h.roots.stateRoot + "/notify-bare-titles.done"));
            QCOMPARE(bareDismissals(one.path()).size(), 14);
            QTemporaryDir two;
            const MoveHome h2 = makeAccountsHome(two.path(), {"account1", "account2"});
            QCOMPARE(runWatchdogCommand(h2.roots, h2.tools, fakeNotifier(two.path()), now), 0);
            QVERIFY(!QFile::exists(h2.roots.stateRoot + "/notify-bare-titles.done"));
            QVERIFY(bareDismissals(two.path()).isEmpty());
        }
    }

    // Step 5.2: the cards about every account keep their bare titles.
    void globalCardsKeepTheirTitles()
    {
        QTemporaryDir dir;
        const MoveHome h = makeAccountsHome(dir.path(), {"account1"});
        QVERIFY(write(h.config, "{}\n"));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.first().contains("-i dialog-information DriveSync ignores an old config DriveSync ignores "),
                 qPrintable(sent.first()));
    }
    void backendReadsTheFixState()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDriveSync(dir.path(), "{}");
        FixState state = fixAt("questions");
        state.questions = {question("a.txt", "differs"), question("b.txt", "onlyCloud")};
        state.questions[0].cloudTime = 1762862500;
        state.questions[0].cloudSize = 10;
        QVERIFY(writeFix(paths.appState, state));
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("fixWaiting"));
        QCOMPARE(backend.fixStep(), QString("questions"));
        QVERIFY(backend.fixBaseDone());  // cancelling now throws a base run away
        QCOMPARE(backend.fixQuestions().size(), 2);
        const QVariantMap first = backend.fixQuestions().at(0).toMap();
        QCOMPARE(first.value("path").toString(), QString("a.txt"));
        QCOMPARE(first.value("kind").toString(), QString("differs"));
        QCOMPARE(first.value("cloudSize").toLongLong(), 10LL);
        QCOMPARE(first.value("cloudTime").toDateTime().toSecsSinceEpoch(), 1762862500LL);
        QVERIFY(!first.value("notebookTime").toDateTime().isValid());

        // One answer per question, in the order fixQuestions lists them.
        backend.answerFix({"cloud", "notebook"});
        QTRY_COMPARE(calls(dir.path()).filter("drive-sync fix --answers").size(), 1);
        const QJsonObject sent = QJsonDocument::fromJson(readBytes(paths.appState + "/fix-answers.json")).object();
        QCOMPARE(sent.value("a.txt").toString(), QString("cloud"));
        QCOMPARE(sent.value("b.txt").toString(), QString("notebook"));

        // The file changes: the window follows without waiting for a poll.
        QSignalSpy spy(&backend, &Backend::fixChanged);
        state = fixAt("confirm");
        state.copies = 1200;
        QByteArray dryRun;
        for (int i = 0; i < 1200; ++i)
            dryRun += QString("2026/11/11 12:00:01 NOTICE: f%1.txt: Skipped copy as --dry-run is set (size 1)\n").arg(i).toUtf8();
        QVERIFY(write(fixDryRunLogPath(paths.appState), dryRun));
        QVERIFY(writeFix(paths.appState, state));
        QTRY_COMPARE(backend.fixStep(), QString("confirm"));
        QVERIFY(!backend.fixBaseDone());
        QCOMPARE(backend.fixCopies(), 1200);
        QCOMPARE(backend.fixCopyList().size(), 500);
        QCOMPARE(backend.fixCopyList().first().toMap().value("path").toString(), QString("f0.txt"));
        QCOMPARE(backend.fixCopiesToNotebook(), 0);
        QVERIFY(spy.count() >= 1);
        backend.confirmFix();
        QTRY_COMPARE(calls(dir.path()).filter("drive-sync fix --confirm --json").size(), 1);
        QVERIFY(clearFix(paths.appState));
        QTRY_COMPARE(backend.fixStep(), QString());
        QVERIFY(backend.fixCopyList().isEmpty());
        QTRY_COMPARE(backend.status(), QString("ok"));
        backend.startFix();
        QTRY_COMPARE(calls(dir.path()).filter("drive-sync fix --start --json").size(), 1);
    }
    void backendReadsTheBlockedDeletes()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        const QString log = successfulRun(start.addSecs(-1800), 480)
                            + abortedRun(start, {"c1.txt", "c2.txt"}, numberedFiles("n%1.txt", 9), 21);
        QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDriveSync(dir.path(), "{}");
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("failed"));
        QCOMPARE(backend.causeKind(), QString("maxDelete"));
        QCOMPARE(backend.blockedCount(), 11);
        QCOMPARE(backend.blockedCloudCount(), 2);
        QCOMPARE(backend.blockedNotebookCount(), 9);
        QCOMPARE(backend.blockedList().size(), 11);
        // Gone from the cloud, so deleted on the notebook for good: first.
        QCOMPARE(backend.blockedList().at(0).toMap().value("path").toString(), QString("c1.txt"));
        QCOMPARE(backend.blockedList().at(0).toMap().value("deletedInCloud").toBool(), true);
        QCOMPARE(backend.blockedList().at(2).toMap().value("path").toString(), QString("n1.txt"));
        QCOMPARE(backend.blockedList().at(2).toMap().value("deletedInCloud").toBool(), false);

        backend.allowDeletes(11);
        QTRY_COMPARE(calls(dir.path()).filter("drive-sync deletes --allow 11 --json").size(), 1);

        // A run after it holds nothing back.
        QSignalSpy spy(&backend, &Backend::blockedChanged);
        QVERIFY(write(dir.filePath("drive-sync.log"), (log + successfulRun(start.addSecs(120), 60)).toUtf8()));
        QTRY_COMPARE(backend.blockedCount(), 0);
        QVERIFY(backend.blockedList().isEmpty());
        QCOMPARE(backend.blockedCloudCount(), 0);
        QVERIFY(spy.count() >= 1);
    }
    void fixRefusesWhereRunsAreSkipped()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        // The emergency home: the config's runOnlyWhen says no, and a resync
        // there would push a stale ~/Drive over the cloud.
        s.config.runOnlyWhen = "exit 1";
        QString error;
        QVERIFY(saveSyncConfig(s.paths.config, s.config, &error));
        QVERIFY(fixCommand(s, {"--start"}).contains("runOnlyWhen"));
        QVERIFY(!fixPresent(s.paths.appState));
        // A fix already under way when the home changed (a reboot) stops at its next step.
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        QCOMPARE(state.failedStep, QString("base"));
        QVERIFY2(state.error.contains("runOnlyWhen"), qPrintable(state.error));
        QVERIFY(calls(dir.path()).filter(QRegularExpression("^rclone ")).isEmpty());
    }

    void fixRefusedAfterTooManyDeletes()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        // What rclone 1.75.2 writes when --max-delete stops a run (tried 2026-11-12).
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        QString log = successfulRun(start.addSecs(-900), 480);
        log += line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "ERROR", R"(Safety abort: too many deletes (>5%, 11 of 20) on Path2 "/home/someone/Drive/". Run with --force if desired.)");
        log += line(start, "NOTICE", "Bisync aborted. Please try again.");
        log += line(start, "NOTICE", "Failed to bisync: too many deletes");
        QVERIFY(write(s.config.logFile, log.toUtf8()));
        const QList<SyncRun> runs = SyncLog::parse(log);
        QCOMPARE(runs.last().causeKind, QString("maxDelete"));
        QVERIFY(runs.last().causeDetail.contains("too many deletes"));
        // Spec 5b: a resync would bring the deleted files back.
        QVERIFY(fixCommand(s, {"--start"}).contains("too many"));
        QVERIFY(!fixPresent(s.paths.appState));
    }

    void fixFinishingWithoutListingsGoesBackToTheBase()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(write(s.config.filtersFile + ".md5",
                      QCryptographicHash::hash(readBytes(s.config.filtersFile), QCryptographicHash::Md5).toHex()));
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(baseListings(s));
        QVERIFY(write(dir.filePath("normal-fails"), ""));
        QVERIFY(write(dir.filePath("normal-loses-listings"), ""));
        QVERIFY(writeFix(s.paths.appState, fixAt("finishing")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        // The final run threw the new base away: only a new base helps.
        QCOMPARE(state.failedStep, QString("base"));
        QVERIFY2(state.error.contains("normal broke"), qPrintable(state.error));
    }
    void backendFollowsAFixInAStateFolderMadeLater()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        // A first start: DriveSync's own state folder does not exist yet.
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        Backend backend(paths);
        QTRY_VERIFY(QFileInfo(paths.appState).isDir());  // the conflict cache makes it
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(writeFix(paths.appState, fixAt("confirm")));
        QTRY_COMPARE_WITH_TIMEOUT(backend.fixStep(), QString("confirm"), 3000);
    }
    void syncAndResumeDuringAFix()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(writeFix(paths.appState, fixAt("questions")));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({}, {paths}, o, e), 1);
        QVERIFY2(err.contains("fix"), qPrintable(err));
        QVERIFY(calls(dir.path()).isEmpty());
        // Resume still ends the pause, but starts no run the fix would skip.
        PauseMark mark;
        mark.present = true;
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QCOMPARE(runResumeCommand({}, {paths}, o, e), 0);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);
        QVERIFY(calls(dir.path()).isEmpty());
    }

    void watchdogSkipsAFixItCannotAsk()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled", "fail");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        FixState state = fixAt("base");
        state.stepStarted = now;
        QVERIFY(writeFix(paths.appState, state));
        // systemd did not answer: the fix may well be at work, so no "stopped" card.
        QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), now.addSecs(7200)), 0);
        QVERIFY(notifications(dir.path()).filter("Drive fix").isEmpty());
    }

    void snapshotFixKeepsItsFeetWhenTheClockMoves()
    {
        const QDateTime now(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.addSecs(-3600), 480), now);
        src.managed = true;
        src.fixPresent = true;
        src.fix = fixAt("base");
        // The clock went back an hour after the step began: not "just started".
        src.fix.stepStarted = now.addSecs(3600);
        QCOMPARE(buildSnapshot(src).summary.status, QString("fixFailed"));
        // The unit failed on its own: say how, not "a logout or a kill".
        src.fix.stepStarted = now.addSecs(-600);
        src.fixUnitResult = "exit-code";
        const Snapshot s = buildSnapshot(src);
        QCOMPARE(s.summary.causeKind, QString("fixInterrupted"));
        QCOMPARE(s.summary.causeDetail, QString("exit-code"));
        src.fixUnitResult = "signal";
        QCOMPARE(buildSnapshot(src).summary.causeDetail, QString("the fix stopped halfway"));
    }
    void bisyncSessionNameMatchesRclone()
    {
        // Names read off real rclone 1.75.2 work dirs on 2026-11-12.
        QCOMPARE(bisyncSessionName("gdrive:", "/home/someone/Drive"), QString("gdrive_..home_someone_Drive"));
        QCOMPARE(bisyncSessionName("/x/a b/", ":local:/y"), QString("x_a_b.._local__y"));
        QCOMPARE(bisyncSessionName(QString::fromUtf8("/s/ação+(1)~é"), "/s/b@c,d/"),
                 QString::fromUtf8("s_ação+(1)~é..s_b@c,d"));
    }

    void takeoverWantsTheListingsOfItsOwnPaths()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        const QString work = old.home + "/.local/state/rclone/bisync/";
        const QString session = bisyncSessionName("gdrive:", old.home + "/Drive");
        QVERIFY(QFile::rename(work + session + ".path1.lst", work + "gdrive_..elsewhere.path1.lst"));
        QVERIFY(QFile::rename(work + session + ".path2.lst", work + "gdrive_..elsewhere.path2.lst"));
        QVERIFY(detectTakeover(old.paths, settledSnapshot()).blockers.contains("noListings"));
    }
    void backendAnswersByPosition()
    {
        // File names that mean something to JavaScript objects stay plain names.
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDriveSync(dir.path(), "{}");
        FixState state = fixAt("questions");
        state.questions = {question("__proto__", "differs"), question("constructor", "onlyCloud")};
        QVERIFY(writeFix(paths.appState, state));
        Backend backend(paths);
        QTRY_COMPARE(backend.fixStep(), QString("questions"));
        backend.answerFix({"notebook", "cloud"});
        QTRY_COMPARE(calls(dir.path()).filter("drive-sync fix --answers").size(), 1);
        const QJsonObject sent = QJsonDocument::fromJson(readBytes(paths.appState + "/fix-answers.json")).object();
        QCOMPARE(sent.value("__proto__").toString(), QString("notebook"));
        QCOMPARE(sent.value("constructor").toString(), QString("cloud"));
        // A count that does not match the questions sends nothing.
        backend.answerFix({"cloud"});
        QTest::qWait(300);
        QCOMPARE(calls(dir.path()).filter("drive-sync fix --answers").size(), 1);
        QVERIFY(!backend.message().isEmpty());
    }
    void fixWritersTakeTheLock()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(writeFix(s.paths.appState, fixAt("confirm")));
        std::atomic<bool> done{false};
        QString error = "not run";
        std::thread confirm;
        {
            // Another writer (the watchdog, say) is in the middle of a change.
            FixLock held(s.paths.appState);
            QVERIFY(held.held());
            confirm = std::thread([&] {
                error = fixCommand(s, {"--confirm"});
                done = true;
            });
            QTest::qWait(400);
            QVERIFY(!done);
            QCOMPARE(readFix(s.paths.appState).step, QString("confirm"));
        }
        confirm.join();
        QVERIFY(done);
        QCOMPARE(error, QString());
        QCOMPARE(readFix(s.paths.appState).step, QString("base"));
    }

    void blockedDeletesReadsTheAbortedRun()
    {
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        const QString log = successfulRun(start.addSecs(-900), 480)
                            + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21);
        const auto blocked = blockedDeletes(log, 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->runStart, start);
        QCOMPARE(blocked->count, 11);
        QCOMPARE(blocked->notebookCount, 11);
        QCOMPARE(blocked->cloudCount, 0);
        QCOMPARE(blocked->guardCount, 11);
        QCOMPARE(blocked->guardTotal, 21);
        QCOMPARE(blocked->list.size(), 11);
        QCOMPARE(blocked->list.first().path, QString("f1.txt"));
        for (const BlockedDelete &d : blocked->list)
            QVERIFY(!d.deletedInCloud);
    }

    void blockedDeletesReadsBothSides()
    {
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        const auto blocked = blockedDeletes(abortedRun(start, {"c1", "c2", "c3"}, {"n1", "n2"}, 20), 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->count, 5);
        QCOMPARE(blocked->cloudCount, 3);
        QCOMPARE(blocked->notebookCount, 2);
        QCOMPARE(blocked->guardCount, 3);  // Path1 is checked first
        QCOMPARE(blocked->guardTotal, 20);
        // Gone from the cloud means deleted on the notebook, for good: those first.
        QStringList order;
        for (const BlockedDelete &d : blocked->list)
            order << QString("%1%2").arg(d.deletedInCloud ? "cloud:" : "notebook:", d.path);
        QCOMPARE(order, (QStringList{"cloud:c1", "cloud:c2", "cloud:c3", "notebook:n1", "notebook:n2"}));
    }

    void blockedDeletesKeepsOddNames()
    {
        // As rclone 1.75.2 writes them: ": ", accents and a leading space raw,
        // a name with " or \ quoted the Go way, and NOTICE in a dry run.
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        // Plain strings here: moc reads a ' inside a raw string as a char literal.
        QString log = abortedRun(start, {}, {"My Docs/a: b.txt", "Ação/é.md", " lead.txt", "\"back\\\\slash.txt\"",
                                             "\"q'uote\\\".txt\"", "- dash.txt"},
                                 21);
        log.replace("INFO  : - Path2             File was deleted                            - Ação",
                    "NOTICE: - Path2             File was deleted                            - Ação");
        const auto blocked = blockedDeletes(log, 500);
        QVERIFY(blocked.has_value());
        QStringList paths;
        for (const BlockedDelete &d : blocked->list)
            paths << d.path;
        QCOMPARE(paths, (QStringList{"My Docs/a: b.txt", "Ação/é.md", " lead.txt", "back\\slash.txt",
                                     "q'uote\".txt", "- dash.txt"}));
    }

    void blockedDeletesStopsAtTheLimit()
    {
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        const auto blocked = blockedDeletes(abortedRun(start, {}, numberedFiles("f%1.txt", 1200), 5000), 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->count, 1200);
        QCOMPARE(blocked->notebookCount, 1200);
        QCOMPARE(blocked->list.size(), 500);
    }

    void blockedDeletesOnlyForTheNewestRun()
    {
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        const QString aborted = abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21);
        QVERIFY(!blockedDeletes(aborted + successfulRun(start.addSecs(900), 480), 500).has_value());
        QVERIFY(!blockedDeletes(successfulRun(start, 480), 500).has_value());
        QVERIFY(!blockedDeletes(QString(), 500).has_value());
        // A run DriveSync refused after the abort is the newest run now.
        QVERIFY(!blockedDeletes(aborted + line(start.addSecs(900), "CRITICAL",
                                               "DriveSync did not start rclone: the filters file changed"),
                                500)
                     .has_value());
        // A run under way after the abort holds nothing back either.
        QVERIFY(!blockedDeletes(aborted + line(start.addSecs(900), "INFO",
                                               R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")"),
                                500)
                     .has_value());
    }

    void aGuardStopLongerThanTheTailIsRead()
    {
        // A wiped ~/Drive stops with tens of thousands of deletion lines,
        // more than the 3 MB the status reads: the run's start must still
        // be found, or the headline, the card, D and the fix's refusal miss it.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        const QString log = successfulRun(start.addSecs(-7200), 480)
                            + abortedRun(start, {}, numberedFiles("Projects/a folder with a long name/sub/file-%1.txt", 25000),
                                         33411);
        QVERIFY(log.toUtf8().size() > kTailBytes);
        QVERIFY(write(s.config.logFile, log.toUtf8()));
        QVERIFY(!SyncLog::readTail(s.config.logFile, kTailBytes).contains("Synching Path1"));

        bool found = false, truncated = false;
        const QString text = SyncLog::readRecent(s.config.logFile, kTailBytes, &found, &truncated);
        QVERIFY(found);
        QVERIFY(text.contains("Synching Path1"));
        QCOMPARE(SyncLog::parse(text, 40, truncated).last().causeKind, QString("maxDelete"));
        const auto blocked = blockedDeletes(text, 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->count, 25000);

        BackendPaths paths{s.config.logFile, dir.filePath("Drive"), dir.filePath("state"), dir.filePath("proc"),
                           fakeSystemctl(dir.path(), start), QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("causeKind").toString(), QString("maxDelete"));

        // Spec 5b: a resync would bring the deleted files back.
        QVERIFY(fixCommand(s, {"--start"}).contains("too many"));
    }

    void readRecentFindsTheRunStartAtALineStart()
    {
        // A file named like the run's start line, near the end of a stop
        // longer than the tail, must not end the search: only a whole
        // "Synching Path1" line starts a run. Both sides deleted, so the
        // log is several times the tail, and the reading stays fast.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QStringList notebook = numberedFiles("Projects/a folder with a long name/sub/file-%1.txt", 25000);
        notebook.last() = "notes/2026/11/12 18:41:19 INFO  : Synching Path1 copy.txt";
        const QString log = successfulRun(start.addSecs(-7200), 480)
                            + abortedRun(start, numberedFiles("Photos/an album with a long name/img-%1.jpg", 25000),
                                         notebook, 50411);
        QVERIFY(log.toUtf8().size() > 2 * kTailBytes);
        QVERIFY(write(s.config.logFile, log.toUtf8()));

        bool found = false, truncated = false;
        const QString text = SyncLog::readRecent(s.config.logFile, kTailBytes, &found, &truncated);
        QVERIFY(found);
        // The second step reaches the file's start: every line, none lost or
        // read twice where the tail and the chunk before it meet.
        QVERIFY(!truncated);
        QVERIFY(text == log);
        const auto blocked = blockedDeletes(text, 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->count, 50000);
        QCOMPARE(blocked->notebookCount, 25000);

        BackendPaths paths{s.config.logFile, dir.filePath("Drive"), dir.filePath("state"), dir.filePath("proc"),
                           fakeSystemctl(dir.path(), start), QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        QString out, err;
        QTextStream o(&out), e(&err);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(runStatusCommand({"--json"}, {paths}, {}, o, e), 0);
        const qint64 elapsed = timer.elapsed();
        qInfo("status --json over a %lld-byte stop: %lld ms", qint64(log.toUtf8().size()), elapsed);
        QVERIFY2(elapsed < 2000, qPrintable(QString::number(elapsed)));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("causeKind").toString(), QString("maxDelete"));
    }

    void maxDeletePercentRoundsUp()
    {
        // rclone compares the real percentage (tried 2026-11-12): 11 of 21
        // stops at 52 and passes at 53; 11 of 20 passes at 55.
        QCOMPARE(maxDeletePercent(11, 21, 5), 53);
        QCOMPARE(maxDeletePercent(11, 20, 5), 55);
        QCOMPARE(maxDeletePercent(1, 33411, 5), 5);  // never below the config's own
        QCOMPARE(maxDeletePercent(50, 40, 5), 100);
        QCOMPARE(maxDeletePercent(3, 0, 5), 100);  // no total read: only "all" lets them through
    }

    void maxDeletePercentCoversTheLargerSide()
    {
        // bisync checks each side on its own, so the guard follows the side
        // with more deletions: 3 of 21 is 15%.
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QVERIFY(write(config.filtersFile + ".md5", QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        AllowDeletes allow;
        allow.count = 5;
        allow.cloudCount = 3;
        allow.notebookCount = 2;
        allow.total = 21;
        QString refusal;
        const QStringList plan = runCommandPlan(config, &refusal, allow);
        QCOMPARE(plan.value(plan.indexOf("--max-delete") + 1), QString("15"));
    }

    void withMaxDeleteReplacesTheFlag()
    {
        QCOMPARE(withMaxDelete({"bisync", "--max-delete", "5", "--log-file", "x"}, 53),
                 (QStringList{"bisync", "--max-delete", "53", "--log-file", "x"}));
        QCOMPARE(withMaxDelete({"bisync", "--max-delete=5", "-v"}, 53), (QStringList{"bisync", "--max-delete=53", "-v"}));
        QCOMPARE(withMaxDelete({"bisync", "-v"}, 53), (QStringList{"bisync", "-v", "--max-delete", "53"}));
    }

    void allowDeletesMarkIsUsedOnce()
    {
        QTemporaryDir dir;
        const QString appState = dir.filePath("app");
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        AllowDeletes allow;
        allow.count = 11;
        allow.notebookCount = 11;
        allow.total = 21;
        allow.at = now.addSecs(-60);
        allow.runStart = now.addSecs(-300);
        QVERIFY(writeAllowDeletes(appState, allow));
        const auto taken = takeAllowDeletes(appState, now);
        QVERIFY(taken.has_value());
        QCOMPARE(taken->runStart, allow.runStart);
        QCOMPARE(taken->count, 11);
        QCOMPARE(taken->cloudCount, 0);
        QCOMPARE(taken->notebookCount, 11);
        QCOMPARE(taken->total, 21);
        QCOMPARE(taken->at, allow.at);
        QVERIFY(!QFile::exists(allowDeletesPath(appState)));
        QVERIFY(!takeAllowDeletes(appState, now).has_value());
    }

    void allowDeletesMarkExpires()
    {
        // A confirmation whose run never started (a pause set right after it)
        // is not used half an hour later.
        QTemporaryDir dir;
        const QString appState = dir.filePath("app");
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        AllowDeletes allow;
        allow.count = 11;
        allow.notebookCount = 11;
        allow.total = 21;
        allow.at = now.addSecs(-31 * 60);
        QVERIFY(writeAllowDeletes(appState, allow));
        QVERIFY(!takeAllowDeletes(appState, now).has_value());
        QVERIFY(!QFile::exists(allowDeletesPath(appState)));
        // Nor one that cannot be read.
        QVERIFY(write(allowDeletesPath(appState), "{not json"));
        QVERIFY(!takeAllowDeletes(appState, now).has_value());
        QVERIFY(!QFile::exists(allowDeletesPath(appState)));
    }

    void runPlanRaisesTheGuardOnce()
    {
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();  // --max-delete 5
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QVERIFY(write(config.filtersFile + ".md5", QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        AllowDeletes allow;
        allow.count = 11;
        allow.notebookCount = 11;
        allow.total = 21;
        QString refusal;
        const QStringList raised = runCommandPlan(config, &refusal, allow);
        QCOMPARE(raised.count("--max-delete"), 1);
        QCOMPARE(raised.value(raised.indexOf("--max-delete") + 1), QString("53"));
        const QStringList normal = runCommandPlan(config, &refusal);
        QCOMPARE(normal.value(normal.indexOf("--max-delete") + 1), QString("5"));
        // The mark never gets past the filters check.
        QVERIFY(write(config.filtersFile, "- changed/**\n"));
        QVERIFY(runCommandPlan(config, &refusal, allow).isEmpty());
        QVERIFY(refusal.contains("so bisync would lose its base"));
    }

    void runPlanRaisesTheGuardPerSide()
    {
        // rclone checks each side against that side's own listing, Path1
        // first, so the abort line's total is Path1's: 10 of 40 there, while
        // 11 of the notebook's 21 is the side that needs the most.
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();  // --max-delete 5
        config.workDir = dir.filePath("work");
        QVERIFY(QDir().mkpath(config.workDir));
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QVERIFY(write(config.filtersFile + ".md5", QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QByteArray cloud = "# bisync listing v1\n", notebook = cloud;
        for (int i = 1; i <= 40; ++i)
            cloud += listingLine(QString("f%1.txt").arg(i), 1, when);
        for (int i = 1; i <= 21; ++i)
            notebook += listingLine(QString("f%1.txt").arg(i), 1, when);
        const QString session = config.workDir + "/" + bisyncSessionName(config.remote, config.localPath);
        QVERIFY(write(session + ".path1.lst", cloud));
        QVERIFY(write(session + ".path2.lst", notebook));
        AllowDeletes allow;
        allow.count = 21;
        allow.cloudCount = 10;
        allow.notebookCount = 11;
        allow.total = 40;
        QString refusal;
        const QStringList raised = runCommandPlan(config, &refusal, allow);
        QCOMPARE(raised.value(raised.indexOf("--max-delete") + 1), QString("53"));
        // Never below the configured guard.
        allow.cloudCount = 1;
        allow.notebookCount = 1;
        const QStringList small = runCommandPlan(config, &refusal, allow);
        QCOMPARE(small.value(small.indexOf("--max-delete") + 1), QString("5"));
    }

    void allowWritesTheMarkAndStartsARun()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        QVERIFY(write(paths.log, (successfulRun(start.addSecs(-900), 480)
                                  + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21))
                                     .toUtf8()));

        const QJsonObject list = deletesCommand(paths, {"--list"});
        QCOMPARE(list.value("ok").toBool(), true);
        QCOMPARE(list.value("count").toInt(), 11);
        QCOMPARE(list.value("notebookCount").toInt(), 11);
        QCOMPARE(list.value("cloudCount").toInt(), 0);
        QCOMPARE(list.value("guardTotal").toInt(), 21);
        QCOMPARE(list.value("list").toArray().size(), 11);
        QCOMPARE(list.value("list").toArray().first().toObject().value("path").toString(), QString("f1.txt"));
        QCOMPARE(list.value("list").toArray().first().toObject().value("deletedInCloud").toBool(), false);

        // The count typed must be the count in the list.
        const QJsonObject wrong = deletesCommand(paths, {"--allow", "10"});
        QCOMPARE(wrong.value("ok").toBool(), false);
        QVERIFY2(wrong.value("error").toString().contains("11"), qPrintable(wrong.value("error").toString()));
        QVERIFY(!QFile::exists(allowDeletesPath(paths.appState)));
        QVERIFY(calls(dir.path()).isEmpty());

        QCOMPARE(deletesCommand(paths, {"--allow", "11"}).value("ok").toBool(), true);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync.service"});
        const auto mark = takeAllowDeletes(paths.appState, QDateTime::currentDateTimeUtc());
        QVERIFY(mark.has_value());
        QCOMPARE(mark->count, 11);
        QCOMPARE(mark->notebookCount, 11);
        QCOMPARE(mark->cloudCount, 0);
        QCOMPARE(mark->total, 21);

        // systemd said no: no mark is left for a later run.
        QTemporaryDir failing;
        BackendPaths refused = controlPaths(failing.path(), true, 1);
        QVERIFY(QFile::copy(paths.log, refused.log));
        QCOMPARE(deletesCommand(refused, {"--allow", "11"}).value("ok").toBool(), false);
        QVERIFY(!QFile::exists(allowDeletesPath(refused.appState)));
    }

    void allowRefusedWithoutTheList()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        // No log at all, then a log whose newest run went well.
        QCOMPARE(deletesCommand(paths, {"--list"}).value("count").toInt(), 0);
        QVERIFY(deletesCommand(paths, {"--allow", "11"}).value("error").toString().contains("nothing is held back"));
        QVERIFY(write(paths.log, (abortedRun(start.addSecs(-900), {}, numberedFiles("f%1.txt", 11), 21)
                                  + successfulRun(start, 480))
                                     .toUtf8()));
        const QJsonObject list = deletesCommand(paths, {"--list"});
        QCOMPARE(list.value("ok").toBool(), true);
        QCOMPARE(list.value("count").toInt(), 0);
        QVERIFY(deletesCommand(paths, {"--allow", "11"}).value("error").toString().contains("nothing is held back"));
        QVERIFY(!QFile::exists(allowDeletesPath(paths.appState)));
        QVERIFY(calls(dir.path()).isEmpty());

        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runDeletesCommand({"--list"}, paths, o, e), 0);
        QVERIFY2(out.contains("Nothing is held back"), qPrintable(out));
        QCOMPARE(runDeletesCommand({"--allow"}, paths, o, e), 1);
        QCOMPARE(runDeletesCommand({"--allow", "eleven"}, paths, o, e), 1);
        QCOMPARE(runDeletesCommand({}, paths, o, e), 1);
        QVERIFY(err.contains("usage: drive-sync deletes"));
    }

    void allowRefusedWhilePausedOrFixing()
    {
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        const QByteArray log = (successfulRun(start.addSecs(-900), 480)
                                + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21))
                                   .toUtf8();
        {
            QTemporaryDir dir;
            BackendPaths paths = controlPaths(dir.path(), true);
            QVERIFY(write(paths.log, log));
            PauseMark pause;
            pause.present = true;
            QVERIFY(writePause(pauseFilePath(paths.appState), pause));
            QVERIFY(deletesCommand(paths, {"--allow", "11"}).value("error").toString().contains("paused"));
            QVERIFY(clearPause(pauseFilePath(paths.appState)));
            QVERIFY(writeFix(paths.appState, fixAt("questions")));
            QVERIFY(deletesCommand(paths, {"--allow", "11"}).value("error").toString().contains("fix"));
            QVERIFY(clearFix(paths.appState));
            paths.managed = false;
            QVERIFY(deletesCommand(paths, {"--allow", "11"}).value("error").toString().contains("take it over"));
            QVERIFY(!QFile::exists(allowDeletesPath(paths.appState)));
            QVERIFY(calls(dir.path()).isEmpty());
        }
        // A fix whose final run stopped at the guard: the confirmation is for
        // that run, so the fix goes back to it.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(write(s.config.logFile, log));
        FixState failed = fixAt("failed");
        failed.failedStep = "finishing";
        failed.error = R"(Safety abort: too many deletes (>5%, 11 of 21) on Path2 "/home/someone/Drive/". Run with --force if desired.)";
        QVERIFY(writeFix(s.paths.appState, failed));
        QCOMPARE(deletesCommand(s.paths, {"--allow", "11"}).value("ok").toBool(), true);
        QVERIFY(QFile::exists(allowDeletesPath(s.paths.appState)));
        QCOMPARE(readFix(s.paths.appState).step, QString("finishing"));
        QVERIFY(calls(dir.path()).contains("systemctl --user start --no-block drive-sync-fix.service"));
        QVERIFY(!calls(dir.path()).contains("systemctl --user start --no-block drive-sync.service"));
    }

    void runLogsTheRaisedGuard()
    {
        // drive-sync run writes this before it execs rclone; never call
        // runRunCommand here, it would exec the real rclone.
        AllowDeletes allow;
        allow.count = 11;
        allow.notebookCount = 11;
        allow.total = 21;
        QCOMPARE(allowedRunLogLine(allow, 53),
                 QString("DriveSync raised --max-delete to 53% for this run: 11 deletions confirmed"));
        // No mark: the plan is the normal one and nothing runs before it.
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.filtersFile = dir.filePath("filters.txt");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QVERIFY(write(config.filtersFile + ".md5", QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        QString refusal, note;
        QCOMPARE(runPlanTakingMark(config, dir.filePath("app"), dir.filePath("no-rclone"), QDateTime::currentDateTimeUtc(),
                                   &refusal, &note),
                 runCommandPlan(config, &refusal));
        QVERIFY(note.isEmpty());
    }

    void allowedRunStopsAtMoreDeletesWithRealRclone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Real rclone on two local folders: "cloud" is Path1, "notebook" Path2.
        QTemporaryDir dir;
        QDir d(dir.path());
        for (const char *folder : {"cloud", "notebook", "work"})
            d.mkpath(folder);
        for (int i = 1; i <= 21; ++i)
            QVERIFY(write(dir.filePath(QString("cloud/f%1.txt").arg(i)), QByteArray::number(i)));
        SyncConfig config;
        config.remote = dir.filePath("cloud");
        config.localPath = dir.filePath("notebook");
        config.workDir = dir.filePath("work");
        config.filtersFile = dir.filePath("filters.txt");
        config.logFile = dir.filePath("drive-sync.log");
        config.rcloneFlags = {"--config", "/dev/null", "--resilient", "--recover", "--max-lock", "2m",
                              "--max-delete", "5", "--log-level", "INFO"};
        QVERIFY(write(config.filtersFile, "- *.skip\n"));
        QProcess base;
        base.start(rclone, QStringList{"--config", "/dev/null", "bisync", config.remote, config.localPath, "--workdir",
                                       config.workDir, "--filters-file", config.filtersFile, "--resync", "-q"});
        base.waitForFinished(60000);
        QCOMPARE(base.exitCode(), 0);
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.log = config.logFile;

        // What drive-sync run does, without the exec.
        const auto run = [&]() {
            QString refusal, note;
            const QStringList arguments =
                runPlanTakingMark(config, paths.appState, rclone, QDateTime::currentDateTimeUtc(), &refusal, &note);
            if (!note.isEmpty())
                appendToLog(config.logFile, "INFO", note);
            QProcess p;
            p.start(rclone, arguments);
            p.waitForFinished(60000);
            return p.exitCode();
        };
        const auto removeFiles = [&](const QString &side, int from, int to) {
            for (int i = from; i <= to; ++i)
                QVERIFY(QFile::remove(dir.filePath(QString("%1/f%2.txt").arg(side).arg(i))));
        };
        const auto blockedCount = [&]() {
            const auto blocked = blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), 500);
            return blocked ? blocked->count : -1;
        };

        removeFiles("notebook", 1, 11);
        QCOMPARE(run(), 1);
        QCOMPARE(blockedCount(), 11);
        QCOMPARE(deletesCommand(paths, {"--allow", "11"}).value("ok").toBool(), true);

        // Two more gone since the confirmation: stopped again, with the new list.
        removeFiles("notebook", 12, 13);
        QCOMPARE(run(), 1);
        QVERIFY(!QFile::exists(allowDeletesPath(paths.appState)));
        QCOMPARE(blockedCount(), 13);
        QVERIFY(readBytes(config.logFile)
                    .contains("DriveSync kept --max-delete at 5% for this run: its dry run with the raised guard "
                              "still stopped, so more files would go than the 11 confirmed"));
        QCOMPARE(deletesCommand(paths, {"--allow", "13"}).value("ok").toBool(), true);

        // One gone from the cloud, which nobody confirmed: the raised guard
        // would cover it (one --max-delete for both sides), the dry run does not.
        removeFiles("cloud", 21, 21);
        QCOMPARE(run(), 1);
        const auto both = blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), 500);
        QVERIFY(both.has_value());
        QCOMPARE(both->cloudCount, 1);
        QCOMPARE(both->notebookCount, 13);
        QCOMPARE(QDir(config.remote).entryList(QDir::Files).size(), 20);
        QVERIFY(readBytes(config.logFile)
                    .contains("DriveSync kept --max-delete at 5% for this run: a dry run finds 1 deletion not in the "
                              "list that was confirmed (gone from the cloud: 1, gone from the notebook: 13)"));
        QCOMPARE(deletesCommand(paths, {"--allow", "14"}).value("ok").toBool(), true);

        QCOMPARE(run(), 0);
        QCOMPARE(QDir(config.remote).entryList(QDir::Files).size(), 7);
        QCOMPARE(QDir(config.localPath).entryList(QDir::Files).size(), 7);
        QVERIFY(readBytes(config.logFile).contains("DriveSync raised --max-delete to 62% for this run: 14 deletions confirmed"));
        // The next run has the guard again.
        QString refusal;
        const QStringList next = runCommandPlan(config, &refusal);
        QCOMPARE(next.value(next.indexOf("--max-delete") + 1), QString("5"));
        QVERIFY(!QFile::exists(allowDeletesPath(paths.appState)));
        QCOMPARE(run(), 0);
    }

    void allowedRunNeedsTheSameFilesWithRealRclone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // The same count, other files: one restored, another deleted after
        // the confirmation. Nobody saw the new one, so it must not go.
        QTemporaryDir dir;
        QDir d(dir.path());
        for (const char *folder : {"cloud", "notebook", "work"})
            d.mkpath(folder);
        for (int i = 1; i <= 21; ++i)
            QVERIFY(write(dir.filePath(QString("cloud/f%1.txt").arg(i)), QByteArray::number(i)));
        SyncConfig config;
        config.remote = dir.filePath("cloud");
        config.localPath = dir.filePath("notebook");
        config.workDir = dir.filePath("work");
        config.filtersFile = dir.filePath("filters.txt");
        config.logFile = dir.filePath("drive-sync.log");
        config.rcloneFlags = {"--config", "/dev/null", "--resilient", "--recover", "--max-lock", "2m",
                              "--max-delete", "5", "--log-level", "INFO"};
        QVERIFY(write(config.filtersFile, "- *.skip\n"));
        QProcess base;
        base.start(rclone, QStringList{"--config", "/dev/null", "bisync", config.remote, config.localPath, "--workdir",
                                       config.workDir, "--filters-file", config.filtersFile, "--resync", "-q"});
        base.waitForFinished(60000);
        QCOMPARE(base.exitCode(), 0);
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.log = config.logFile;
        const auto run = [&]() {
            QString refusal, note;
            const QStringList arguments =
                runPlanTakingMark(config, paths.appState, rclone, QDateTime::currentDateTimeUtc(), &refusal, &note);
            if (!note.isEmpty())
                appendToLog(config.logFile, "INFO", note);
            QProcess p;
            p.start(rclone, arguments);
            p.waitForFinished(60000);
            return p.exitCode();
        };

        for (int i = 1; i <= 11; ++i)
            QVERIFY(QFile::remove(dir.filePath(QString("notebook/f%1.txt").arg(i))));
        QCOMPARE(run(), 1);
        QCOMPARE(deletesCommand(paths, {"--allow", "11"}).value("ok").toBool(), true);
        QVERIFY(QFile::copy(dir.filePath("cloud/f11.txt"), dir.filePath("notebook/f11.txt")));
        QVERIFY(QFile::remove(dir.filePath("notebook/f12.txt")));
        QCOMPARE(run(), 1);
        QCOMPARE(QDir(config.remote).entryList(QDir::Files).size(), 21);
        const auto now = blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), 500);
        QVERIFY(now.has_value());
        QCOMPARE(now->count, 11);
        QStringList listed;
        for (const BlockedDelete &b : now->list)
            listed << b.path;
        QVERIFY(listed.contains("f12.txt"));
        QVERIFY(!listed.contains("f11.txt"));
    }

    void allowMarkServesOnlyTheRunThatStopped()
    {
        // A confirmation left for a later run (written while the confirmed
        // run was already under way) is dropped once another run came after
        // the stopped one, before any dry run.
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.filtersFile = dir.filePath("filters.txt");
        config.logFile = dir.filePath("drive-sync.log");
        QVERIFY(write(config.filtersFile, "- .git/**\n"));
        QVERIFY(write(config.filtersFile + ".md5", QCryptographicHash::hash("- .git/**\n", QCryptographicHash::Md5).toHex()));
        const QDateTime start = QDateTime::currentDateTime().addSecs(-900);
        const QString aborted = abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21);
        QVERIFY(write(config.logFile, (aborted + successfulRun(start.addSecs(120), 60)).toUtf8()));
        const QString appState = dir.filePath("app");
        AllowDeletes allow;
        allow.count = 11;
        allow.notebookCount = 11;
        allow.total = 21;
        allow.at = QDateTime::currentDateTimeUtc().addSecs(-60);
        allow.runStart = start;
        QVERIFY(writeAllowDeletes(appState, allow));
        QString refusal, note;
        const QStringList plan = runPlanTakingMark(config, appState, dir.filePath("no-rclone"),
                                                   QDateTime::currentDateTimeUtc(), &refusal, &note);
        QCOMPARE(plan.value(plan.indexOf("--max-delete") + 1), QString("5"));
        QVERIFY(!QFile::exists(allowDeletesPath(appState)));
        QVERIFY2(note.contains("no longer the last one"), qPrintable(note));

        // Nor a mark for another stopped run than the newest one.
        QVERIFY(write(config.logFile, aborted.toUtf8()));
        allow.runStart = start.addSecs(-3600);
        QVERIFY(writeAllowDeletes(appState, allow));
        const QStringList other = runPlanTakingMark(config, appState, dir.filePath("no-rclone"),
                                                    QDateTime::currentDateTimeUtc(), &refusal, &note);
        QCOMPARE(other.value(other.indexOf("--max-delete") + 1), QString("5"));
        QVERIFY2(note.contains("no longer the last one"), qPrintable(note));
    }

    void deletesListPrintsEveryPath()
    {
        // The window shows 500; the command line is where all of them are.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, abortedRun(QDateTime::currentDateTime().addSecs(-300), {},
                                            numberedFiles("f%1.txt", 1200), 5000)
                                     .toUtf8()));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runDeletesCommand({"--list"}, paths, o, e), 0);
        QCOMPARE(out.split('\n').filter(QRegularExpression("^  f\\d+\\.txt$")).size(), 1200);
    }

    void fixRefusedAfterTooManyDeletesThenARefusal()
    {
        // A run DriveSync refused (edited filters) after the guard stop never
        // reached bisync: the stop still holds, and a resync would bring the
        // deleted files back.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime start = QDateTime::currentDateTime().addSecs(-900);
        QString log = successfulRun(start.addSecs(-900), 480) + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21);
        log += line(start.addSecs(600), "CRITICAL",
                    QString("DriveSync did not start rclone: %1 changed since the last resync, %2")
                        .arg(s.config.filtersFile, kFiltersChangedReason));
        QVERIFY(write(s.config.logFile, log.toUtf8()));
        QCOMPARE(SyncLog::parse(log).last().causeKind, QString("filtersChanged"));
        const QString refusal = fixCommand(s, {"--start"});
        QVERIFY(refusal.contains("too many"));
        // What to do about it: the edit goes first, then the list shows.
        QVERIFY2(refusal.contains("undo the filters edit first"), qPrintable(refusal));
        QVERIFY(!fixPresent(s.paths.appState));
    }

    void guardStopHoldsLooksPastRefusals()
    {
        const QDateTime start(QDate(2026, 11, 12), QTime(18, 41, 19));
        const QString aborted = abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21);
        const QString refused = line(start.addSecs(900), "CRITICAL",
                                     QString("DriveSync did not start rclone: filters.txt changed since the last resync, %1")
                                         .arg(kFiltersChangedReason));
        QVERIFY(guardStopHolds(SyncLog::parse(aborted)));
        QVERIFY(guardStopHolds(SyncLog::parse(aborted + refused)));
        QVERIFY(guardStopHolds(SyncLog::parse(aborted + refused + refused)));
        QVERIFY(!guardStopHolds(SyncLog::parse(aborted + successfulRun(start.addSecs(900), 480))));
        QVERIFY(!guardStopHolds(SyncLog::parse(successfulRun(start, 480) + refused)));
        QVERIFY(!guardStopHolds({}));
    }

    void notifyNamesTheGuardBehindEditedFilters()
    {
        // Edited filters after a guard stop: F would bring the deleted files
        // back, so the card must not say "use Fix…".
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime start = QDateTime::currentDateTime().addSecs(-900);
        QVERIFY(write(paths.log, (abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21)
                                  + line(QDateTime::currentDateTime().addSecs(-30), "CRITICAL",
                                         QString("DriveSync did not start rclone: filters.txt changed since the last "
                                                 "resync, %1")
                                             .arg(kFiltersChangedReason)))
                                     .toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("Undo the filters edit first"), qPrintable(sent.at(0)));
        QVERIFY(!sent.at(0).contains("use Fix…"));
    }

    void backendSeesTheGuardBehindARefusal()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-900);
        QDir(dir.path()).mkpath("Drive");
        const QString refused = line(QDateTime::currentDateTime().addSecs(-30), "CRITICAL",
                                     QString("DriveSync did not start rclone: filters.txt changed since the last "
                                             "resync, %1")
                                         .arg(kFiltersChangedReason));
        QVERIFY(write(dir.filePath("drive-sync.log"),
                      (abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21) + refused).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("failed"));
        QCOMPARE(backend.causeKind(), QString("filtersChanged"));
        QVERIFY(backend.guardStopHolds());
        QCOMPARE(backend.blockedCount(), 0);  // the newest record is the refusal

        // Edited filters with no stop before them: the usual F.
        QVERIFY(write(dir.filePath("drive-sync.log"), (successfulRun(start, 480) + refused).toUtf8()));
        QTRY_VERIFY(!backend.guardStopHolds());
        QCOMPARE(backend.causeKind(), QString("filtersChanged"));
    }

    void fixFinalRunTakesTheConfirmation()
    {
        // A fix whose final run stopped at the guard: D sends it back to that
        // run, which dry-runs with the raised guard, as drive-sync run does.
        const auto setUp = [](const QString &dir, FixSetup *s) {
            *s = makeFixSetup(dir);
            QVERIFY(write(s->config.filtersFile + ".md5",
                          QCryptographicHash::hash(readBytes(s->config.filtersFile), QCryptographicHash::Md5).toHex()));
            // 21 files on each side, as the abort line says: the raised guard
            // is counted against each side's listing.
            QByteArray body = "# bisync listing v1\n";
            for (int i = 1; i <= 21; ++i)
                body += listingLine(QString("f%1.txt").arg(i), 1, QDateTime::currentDateTimeUtc().addSecs(-3600));
            const QString session = s->config.workDir + "/" + bisyncSessionName(s->config.remote, s->config.localPath);
            QVERIFY(write(session + ".path1.lst", body) && write(session + ".path2.lst", body));
            const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
            QVERIFY(write(s->config.logFile, (successfulRun(start.addSecs(-1800), 480)
                                              + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21))
                                                 .toUtf8()));
            FixState failed = fixAt("failed");
            failed.failedStep = "finishing";
            failed.error = R"(Safety abort: too many deletes (>5%, 11 of 21) on Path2 "/home/someone/Drive/". Run with --force if desired.)";
            QVERIFY(writeFix(s->paths.appState, failed));
            QCOMPARE(deletesCommand(s->paths, {"--allow", "11"}).value("ok").toBool(), true);
            QCOMPARE(readFix(s->paths.appState).step, QString("finishing"));
        };
        const auto dryRun = [](const QStringList &files) {
            const QDateTime t = QDateTime::currentDateTime();
            QString log = line(t, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
            for (const QString &f : files)
                log += line(t, "NOTICE", change("Path2", "File was deleted", f));
            return (log + line(t, "INFO", "Bisync successful")).toUtf8();
        };
        const auto normalRun = [](const QString &dir) {
            const QStringList bisync = calls(dir).filter(QRegularExpression("^rclone bisync "));
            for (auto it = bisync.crbegin(); it != bisync.crend(); ++it)
                if (!it->contains("--dry-run"))
                    return *it;
            return QString();
        };
        {
            QTemporaryDir dir;
            FixSetup s;
            setUp(dir.path(), &s);
            QVERIFY(write(dir.filePath("normal-dry"), dryRun(numberedFiles("f%1.txt", 11))));
            QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
            QVERIFY(!fixPresent(s.paths.appState));
            QVERIFY(!QFile::exists(allowDeletesPath(s.paths.appState)));
            QVERIFY2(normalRun(dir.path()).contains("--max-delete 53"), qPrintable(normalRun(dir.path())));
            QVERIFY(readBytes(s.config.logFile).contains("raised --max-delete to 53%"));
        }
        {
            // One file deleted since the confirmation: the usual guard.
            QTemporaryDir dir;
            FixSetup s;
            setUp(dir.path(), &s);
            QVERIFY(write(dir.filePath("normal-dry"), dryRun(numberedFiles("f%1.txt", 12))));
            QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 0);
            QVERIFY2(normalRun(dir.path()).contains("--max-delete 5 "), qPrintable(normalRun(dir.path())));
            QVERIFY(readBytes(s.config.logFile).contains("not in the list that was confirmed"));
        }
    }

    void allowAndDAgreeOnAFixAfterTheAnswersFailed()
    {
        // The fix's final step stopped at the guard, D sent it back, and this
        // time it failed on an answer, before its run: the guard stop is still
        // the newest run, so D and --allow both still offer the run.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        QVERIFY(write(s.config.logFile, (successfulRun(start.addSecs(-1800), 480)
                                          + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21))
                                             .toUtf8()));
        FixState failed = fixAt("failed");
        failed.failedStep = "finishing";
        failed.error = "could not mark a.txt in the cloud: no network";
        QVERIFY(writeFix(s.paths.appState, failed));

        QVERIFY(QDir().mkpath(dir.filePath("view")));
        BackendPaths view{s.config.logFile, s.config.localPath, dir.filePath("state"), dir.filePath("proc"),
                          fakeSystemctl(dir.filePath("view"), start), QStandardPaths::findExecutable("find"),
                          s.paths.appState};
        view.managed = true;
        Backend backend(view);
        QTRY_COMPARE(backend.blockedCount(), 11);
        QTRY_VERIFY(backend.deletesHeld());
        QCOMPARE(deletesCommand(s.paths, {"--allow", "11"}).value("ok").toBool(), true);
        QCOMPARE(readFix(s.paths.appState).step, QString("finishing"));

        // A fix at any other step holds every run: neither offers it.
        QVERIFY(QFile::remove(allowDeletesPath(s.paths.appState)));
        QVERIFY(writeFix(s.paths.appState, fixAt("questions")));
        QTRY_VERIFY(!backend.deletesHeld());
        QVERIFY(deletesCommand(s.paths, {"--allow", "11"}).value("error").toString().contains("a fix is under way"));
        QVERIFY(!QFile::exists(allowDeletesPath(s.paths.appState)));
    }

    void deleteDialogClosesWhenNothingIsHeld()
    {
        // The real DeleteDialog.qml over a stand-in backend: a run that
        // starts, or a list that goes, takes the dialog away instead of
        // leaving it open on a count of 0 with its button off.
        QQmlEngine engine;
        QQmlPropertyMap backend, theme;
        const auto hold = [&backend](bool held, int count) {
            QVariantList list;
            for (int i = 1; i <= count; ++i)
                list << QVariantMap{{"path", QString("f%1.txt").arg(i)}, {"deletedInCloud", false}};
            backend.insert("blockedList", list);
            backend.insert("blockedCount", count);
            backend.insert("blockedCloudCount", 0);
            backend.insert("blockedNotebookCount", count);
            backend.insert("deletesHeld", held);
        };
        hold(true, 11);
        theme.insert("radius", 8);
        theme.insert("accent", QColor("#7aa2f7"));
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        QQmlComponent component(&engine);
        component.setData(QString("import QtQuick\nimport QtQuick.Controls\nimport \"%1\"\n"
                                  "ApplicationWindow { width: 800; height: 600; visible: true\n"
                                  "  property alias dialog: d\n  DeleteDialog { id: d } }\n")
                              .arg(QUrl::fromLocalFile(QString(SRCDIR) + "/../src").toString())
                              .toUtf8(),
                          QUrl());
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        const auto open = [dialog]() {
            QMetaObject::invokeMethod(dialog, "open");
            QTRY_VERIFY(dialog->property("visible").toBool());
        };

        open();
        hold(false, 11);  // a run started: the status is no longer failed
        QTRY_VERIFY(!dialog->property("visible").toBool());

        hold(true, 11);
        open();
        hold(false, 0);  // the stop is gone from the newest run
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    void allowedRunBacksUpWhatItDeletesOnTheNotebookWithRealRclone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // 11 files gone from the cloud: the run let through deletes them on
        // the notebook, and --backup-dir2 keeps them in DriveSync's state.
        QTemporaryDir dir;
        QDir d(dir.path());
        for (const char *folder : {"cloud", "notebook", "work"})
            d.mkpath(folder);
        for (int i = 1; i <= 21; ++i)
            QVERIFY(write(dir.filePath(QString("cloud/f%1.txt").arg(i)), QByteArray::number(i)));
        SyncConfig config;
        config.remote = dir.filePath("cloud");
        config.localPath = dir.filePath("notebook");
        config.workDir = dir.filePath("work");
        config.filtersFile = dir.filePath("filters.txt");
        config.logFile = dir.filePath("drive-sync.log");
        config.rcloneFlags = {"--config", "/dev/null", "--resilient", "--recover", "--max-lock", "2m",
                              "--max-delete", "5", "--log-level", "INFO"};
        QVERIFY(write(config.filtersFile, "- *.skip\n"));
        QProcess base;
        base.start(rclone, QStringList{"--config", "/dev/null", "bisync", config.remote, config.localPath, "--workdir",
                                       config.workDir, "--filters-file", config.filtersFile, "--resync", "-q"});
        base.waitForFinished(60000);
        QCOMPARE(base.exitCode(), 0);
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.log = config.logFile;
        const auto run = [&]() {
            QString refusal, note;
            const QStringList arguments =
                runPlanTakingMark(config, paths.appState, rclone, QDateTime::currentDateTimeUtc(), &refusal, &note);
            if (!note.isEmpty())
                appendToLog(config.logFile, "INFO", note);
            QProcess p;
            p.start(rclone, arguments);
            p.waitForFinished(60000);
            return p.exitCode();
        };

        for (int i = 1; i <= 11; ++i)
            QVERIFY(QFile::remove(dir.filePath(QString("cloud/f%1.txt").arg(i))));
        QCOMPARE(run(), 1);
        const auto stopped = blockedDeletes(SyncLog::readRecent(config.logFile, kTailBytes), 500);
        QVERIFY(stopped.has_value());
        QCOMPARE(stopped->cloudCount, 11);
        QCOMPARE(deletesCommand(paths, {"--allow", "11"}).value("ok").toBool(), true);
        QCOMPARE(run(), 0);
        QCOMPARE(QDir(config.localPath).entryList(QDir::Files).size(), 10);
        const QString backup = paths.appState + "/allowed-backup/"
                               + stopped->runStart.toLocalTime().toString("yyyyMMdd-HHmmss");
        QCOMPARE(QDir(backup).entryList(QDir::Files).size(), 11);
        QCOMPARE(readBytes(backup + "/f1.txt"), QByteArray("1"));
        // Nothing went that was not confirmed.
        const UnconfirmedDeletes check = checkConfirmedRun(paths.appState, config.logFile);
        QCOMPARE(check.cloudCount + check.notebookCount, 0);
        QVERIFY(!readUnconfirmedDeletes(paths.appState).has_value());
        QVERIFY(!readBytes(config.logFile).contains("not in the confirmed list"));
    }

    void notifyNamesDeletionsNobodyConfirmed()
    {
        // Between the dry run and the run let through, one more file went on
        // each side. The run's own lines say so: the log, the card and the
        // window name them and where they went.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QDir(dir.path()).mkpath("Drive");
        const QDateTime start = QDateTime::currentDateTime().addSecs(-30);
        ConfirmedRun confirmed;
        confirmed.at = QDateTime::currentDateTimeUtc().addSecs(-60);
        confirmed.backupDir = paths.appState + "/allowed-backup/20261114-120000";
        for (int i = 1; i <= 11; ++i)
            confirmed.confirmed << QString("2:f%1.txt").arg(i);
        QVERIFY(writeConfirmedRun(paths.appState, confirmed));
        QString log = line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", change("Path1", "File was deleted", "c1.txt"));
        for (int i = 1; i <= 12; ++i)
            log += line(start, "INFO", change("Path2", "File was deleted", QString("f%1.txt").arg(i)));
        log += line(start.addSecs(20), "INFO", "Bisync successful");
        QVERIFY(write(paths.log, log.toUtf8()));

        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runNotifyCommand("success", paths, tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("Drive deleted files you did not confirm"), qPrintable(sent.at(0)));
        QVERIFY2(sent.at(0).contains("2 files"), qPrintable(sent.at(0)));
        QVERIFY2(sent.at(0).contains(confirmed.backupDir), qPrintable(sent.at(0)));
        QVERIFY(readBytes(paths.log).contains("NOTICE: DriveSync: the run let through deleted 2 files that were not "
                                              "in the confirmed list"));
        // The log line is no run of its own: the headline stays on the success.
        QCOMPARE(SyncLog::parse(QString::fromUtf8(readBytes(paths.log))).last().outcome, QString("success"));
        const auto extra = readUnconfirmedDeletes(paths.appState);
        QVERIFY(extra.has_value());
        QCOMPARE(extra->cloudCount, 1);
        QCOMPARE(extra->notebookCount, 1);
        // Checked once: the next notify has nothing more to say.
        QCOMPARE(runNotifyCommand("success", paths, tools, QDateTime::currentDateTimeUtc()), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);

        BackendPaths view{paths.log, dir.filePath("Drive"), dir.filePath("state"), dir.filePath("proc"),
                          fakeSystemctl(dir.path(), start), QStandardPaths::findExecutable("find"), paths.appState};
        view.managed = true;
        Backend backend(view);
        QTRY_COMPARE(backend.unconfirmedCloudCount(), 1);
        QCOMPARE(backend.unconfirmedNotebookCount(), 1);
        QCOMPARE(backend.unconfirmedBackupDir(), confirmed.backupDir);
        // The next run takes it out of the window.
        QVERIFY(write(paths.log, (log + successfulRun(QDateTime::currentDateTime().addSecs(-5), 4)).toUtf8()));
        QTRY_COMPARE(backend.unconfirmedCloudCount(), 0);
    }

    void aConfirmedRunThatStoppedDeletedNothing()
    {
        // One more file went between the dry run and the run let through, so
        // the raised guard stopped it too: its deletion lines are what it
        // would have done, not what it did. Only the stop card goes out.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const QDateTime start = QDateTime::currentDateTime().addSecs(-30);
        ConfirmedRun confirmed;
        confirmed.at = QDateTime::currentDateTimeUtc().addSecs(-60);
        confirmed.backupDir = paths.appState + "/allowed-backup/20261114-120000";
        for (int i = 1; i <= 11; ++i)
            confirmed.confirmed << QString("2:f%1.txt").arg(i);
        QVERIFY(writeConfirmedRun(paths.appState, confirmed));
        QVERIFY(write(paths.log, abortedRun(start, {}, numberedFiles("f%1.txt", 12), 21).toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("Drive stopped syncing"), qPrintable(sent.at(0)));
        QVERIFY(!readBytes(paths.log).contains("not in the confirmed list"));
        QVERIFY(!readUnconfirmedDeletes(paths.appState).has_value());

        // A record left by a run that never reported (a power cut) is not
        // held against a later run: only the first run after it counts.
        QVERIFY(writeConfirmedRun(paths.appState, confirmed));
        QString later = successfulRun(start, 20);
        const QDateTime next = QDateTime::currentDateTime().addSecs(-5);
        later += line(next, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        later += line(next, "INFO", "Applying changes");
        later += line(next, "INFO", change("Path1", "File was deleted", "c1.txt"));
        later += line(next, "INFO", "Bisync successful");
        QVERIFY(write(paths.log, later.toUtf8()));
        const UnconfirmedDeletes check = checkConfirmedRun(paths.appState, paths.log);
        QCOMPARE(check.cloudCount + check.notebookCount, 0);
        QVERIFY(!readBytes(paths.log).contains("not in the confirmed list"));
    }

    void aConfirmedRecordWithNoRunIsDropped()
    {
        // drive-sync run wrote the record and the machine died before rclone
        // logged anything: the next ordinary run, much later, is not the run
        // let through, and its own deletions are no news.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        ConfirmedRun confirmed;
        confirmed.at = QDateTime::currentDateTimeUtc().addSecs(-3600);
        confirmed.backupDir = paths.appState + "/allowed-backup/20261114-120000";
        confirmed.confirmed << "2:f1.txt";
        QVERIFY(writeConfirmedRun(paths.appState, confirmed));
        const QDateTime start = QDateTime::currentDateTime().addSecs(-30);
        QString log = line(start, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
        log += line(start, "INFO", "Applying changes");
        log += line(start, "INFO", change("Path2", "File was deleted", "other.txt"));
        log += line(start, "INFO", "Bisync successful");
        QVERIFY(write(paths.log, log.toUtf8()));
        const UnconfirmedDeletes check = checkConfirmedRun(paths.appState, paths.log);
        QCOMPARE(check.cloudCount + check.notebookCount, 0);
        QVERIFY(!readBytes(paths.log).contains("not in the confirmed list"));
        QVERIFY(!QFile::exists(paths.appState + "/allowed-run.json"));
    }

    void aKilledConfirmedRunIsStillChecked()
    {
        // The fix's final run, let through, was killed while applying (the
        // fix unit has no ExecStopPost): what it deleted is checked later,
        // even with another run after it, and at once when fix-run starts.
        const auto setUp = [](const QString &appState, const QString &logFile) {
            ConfirmedRun confirmed;
            confirmed.at = QDateTime::currentDateTimeUtc().addSecs(-120);
            confirmed.backupDir = appState + "/allowed-backup/20261114-120000";
            confirmed.confirmed << "2:f1.txt";
            QVERIFY(writeConfirmedRun(appState, confirmed));
            const QDateTime killed = QDateTime::currentDateTime().addSecs(-100);
            QString log = line(killed, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
            log += line(killed, "INFO", "Applying changes");
            log += line(killed, "INFO", change("Path1", "File was deleted", "c1.txt"));
            log += line(killed, "INFO", change("Path2", "File was deleted", "f1.txt"));
            log += successfulRun(QDateTime::currentDateTime().addSecs(-10), 5);
            QVERIFY(write(logFile, log.toUtf8()));
        };
        {
            QTemporaryDir dir;
            const BackendPaths paths = controlPaths(dir.path(), true);
            setUp(paths.appState, paths.log);
            const UnconfirmedDeletes check = checkConfirmedRun(paths.appState, paths.log);
            QCOMPARE(check.cloudCount, 1);
            QCOMPARE(check.notebookCount, 0);
            // The killed run, not the one after it.
            QVERIFY(qAbs(check.runStart.secsTo(QDateTime::currentDateTime().addSecs(-100))) <= 2);
        }
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        setUp(s.paths.appState, s.config.logFile);
        FixState failed = fixAt("failed");
        failed.failedStep = "finishing";
        QVERIFY(writeFix(s.paths.appState, failed));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        QVERIFY2(notifications(dir.path()).join("\n").contains("Drive deleted files you did not confirm"),
                 qPrintable(notifications(dir.path()).join("\n")));
    }

    void notifyKeepsTheGuardKeyAcrossOtherCards()
    {
        // A card about another failure in between must not make the same
        // guard stop look new afterwards.
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime t0 = QDateTime::currentDateTime().addSecs(-7200);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QString log = abortedRun(t0, {}, numberedFiles("f%1.txt", 11), 21);
        QVERIFY(write(paths.log, log.toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now), 0);
        log += failedRun(t0.addSecs(3600));
        QVERIFY(write(paths.log, log.toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3700)), 0);  // the hourly reminder
        QCOMPARE(notifications(dir.path()).size(), 2);
        log += abortedRun(t0.addSecs(3900), {}, numberedFiles("f%1.txt", 11), 21);
        QVERIFY(write(paths.log, log.toUtf8()));
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, now.addSecs(3900)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);  // the same list, 200 s after a card
    }

    void keptReasonSurvivesAChunkBoundary()
    {
        // A stop longer than the tail, whose note sits right before its
        // start: where readRecent's step back cuts the file must not drop it.
        QTemporaryDir dir;
        const QString path = dir.filePath("drive-sync.log");
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        const QString before = successfulRun(start.addSecs(-7200), 480);
        const QString note = line(start, "INFO",
                                  "DriveSync kept --max-delete at 5% for this run: a dry run finds 1 deletion not in "
                                  "the list that was confirmed (gone from the cloud: 0, gone from the notebook: 2000)");
        const QString stop = abortedRun(start, {}, numberedFiles("Projects/sub/file-%1.txt", 2000), 33411);
        const QByteArray bytes = (before + note + stop).toUtf8();
        QVERIFY(write(path, bytes));
        // The second step starts 20 bytes into the note line.
        const qint64 noteAt = (before).toUtf8().size();
        const qint64 maxBytes = (bytes.size() - noteAt - 20) / 4;
        QVERIFY(bytes.size() - 4 * maxBytes > noteAt);
        QVERIFY(bytes.size() - 4 * maxBytes < noteAt + note.toUtf8().size());
        QVERIFY(bytes.size() - maxBytes > noteAt + note.toUtf8().size());  // the tail has no start
        const auto blocked = blockedDeletes(SyncLog::readRecent(path, maxBytes), 500);
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->keptReason, QString("unconfirmed"));
    }

    void allowedRunNeedsADryRunThatReachedBisync()
    {
        // A dry run that ended well but wrote no "Synching Path1" line found
        // nothing, which says nothing about what the real run would delete:
        // the usual guard stays.
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        QVERIFY(write(s.config.filtersFile + ".md5",
                      QCryptographicHash::hash(readBytes(s.config.filtersFile), QCryptographicHash::Md5).toHex()));
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        QVERIFY(write(s.config.logFile, (successfulRun(start.addSecs(-1800), 480)
                                          + abortedRun(start, {}, numberedFiles("f%1.txt", 11), 21))
                                             .toUtf8()));
        QCOMPARE(deletesCommand(s.paths, {"--allow", "11"}).value("ok").toBool(), true);
        // The fake rclone's dry run writes nothing when dir/normal-dry is missing.
        QString refusal, note;
        const QStringList plan = runPlanTakingMark(s.config, s.paths.appState, s.tools.rclone,
                                                   QDateTime::currentDateTimeUtc(), &refusal, &note);
        QCOMPARE(plan.value(plan.indexOf("--max-delete") + 1), QString("5"));
        QVERIFY2(note.startsWith("DriveSync kept --max-delete at 5% for this run: its dry run with the raised guard "
                                 "wrote no run to its log"),
                 qPrintable(note));
        QVERIFY(!QFile::exists(allowDeletesPath(s.paths.appState)));
    }

    void signinCreatesTheRemoteWhenNoConfig()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        const CommandResult r = signInCommand(paths, signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        const QList<QStringList> calls = rcloneCalls(dir.path());
        QVERIFY(calls.size() >= 2);
        QCOMPARE(calls.at(0), QStringList{"listremotes"});
        QVERIFY(!pageArgument(calls.at(1)).isEmpty());
        QVERIFY(calls.at(1) == createRemoteArguments(kNewRemoteName, fakeClient(), pageArgument(calls.at(1))));
        // The token is proven before the window moves on.
        QCOMPARE(calls.last().mid(0, 2), (QStringList{"lsjson", "drive-sync:"}));
        QCOMPARE(remoteName("gdrive:"), QString("gdrive"));
        QCOMPARE(remoteName("gdrive:sub/dir"), QString("gdrive"));
        QCOMPARE(createRemoteArguments("x", {"i", "s"}),
                 (QStringList{"config", "create", "x", "drive", "client_id", "i", "client_secret", "s", "scope", "drive"}));
        QCOMPARE(refreshTokenArguments("x"), (QStringList{"config", "update", "x", "config_refresh_token", "true"}));
    }

    void signinRefusesAnExistingRemote()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QVERIFY(write(dir.filePath("remotes"), "gdrive:\ndrive-sync:\n"));
        // A remote of that name the user made with another client: never
        // taken, never touched, whether it has a token or not.
        for (const char *dump : {R"({"drive-sync":{"type":"drive","client_id":"mine","token":"an-access-token"}})",
                                 R"({"drive-sync":{"type":"drive","client_id":"mine"}})",
                                 R"({"drive-sync":{"type":"s3"}})"}) {
            QVERIFY(write(dir.filePath("dump"), dump));
            const CommandResult r = signInCommand(paths, signInTools(dir.path()), {});
            QCOMPARE(r.code, 1);
            QVERIFY2(r.err.contains("drive-sync") && r.err.contains("did not make"), qPrintable(r.err));
            const QJsonObject answer = QJsonDocument::fromJson(signInCommand(paths, signInTools(dir.path()), {"--json"}).out.toUtf8()).object();
            QCOMPARE(answer.value("ok").toBool(true), false);
            QCOMPARE(answer.value("reason").toString(), QString("remoteExists"));
        }
        for (const QStringList &call : rcloneCalls(dir.path()))
            QVERIFY2(call.first() != "lsjson" && call.mid(0, 2) != (QStringList{"config", "create"})
                         && call.mid(0, 2) != (QStringList{"config", "update"}) && !call.contains("delete"),
                     qPrintable(call.join(' ')));
    }

    void signinAdoptsOrRepairsItsOwnRemote()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QVERIFY(write(dir.filePath("remotes"), "drive-sync:\n"));
        const auto configCalls = [&dir] {
            QList<QStringList> found;
            for (const QStringList &call : rcloneCalls(dir.path()))
                if (call.first() == "config" && call.value(1) != "dump")
                    found << call;
            return found;
        };
        // DriveSync's own remote with a token Drive takes: signed in already.
        QVERIFY(write(dir.filePath("dump"),
                      R"({"drive-sync":{"type":"drive","client_id":"fake-client.apps.googleusercontent.com","token":"t"}})"));
        CommandResult r = signInCommand(paths, signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        QVERIFY(configCalls().isEmpty());
        QCOMPARE(rcloneCalls(dir.path()).last().first(), QString("lsjson"));
        // Its token no longer works: the browser signs it in again.
        QVERIFY(write(dir.filePath("lsjson-fails-once"), ""));
        r = signInCommand(paths, signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        QCOMPARE(configCalls(), QList<QStringList>{refreshTokenArguments(kNewRemoteName, pageArgument(configCalls().value(0)))});
        // No token at all (a sign-in cut short): made again from scratch.
        QVERIFY(QFile::remove(dir.filePath("rclone-calls.log")));
        QVERIFY(write(dir.filePath("dump"), R"({"drive-sync":{"type":"drive","client_id":"fake-client.apps.googleusercontent.com"}})"));
        r = signInCommand(paths, signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        QCOMPARE(configCalls(), (QList<QStringList>{{"config", "delete", "drive-sync"},
                                                   createRemoteArguments(kNewRemoteName, fakeClient(),
                                                                         pageArgument(configCalls().value(1)))}));
    }

    void signinStopsCleanlyOnSigterm()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("create-hangs"), ""));
        SignInTools tools = signInTools(dir.path());
        tools.timeoutMs = 20000;
        // Without the command's own handler this one takes the signal, so a
        // missing handler fails the test instead of killing the suite.
        static std::atomic<bool> caught{false};
        caught = false;
        struct sigaction mine {}, previous {};
        mine.sa_handler = [](int) { caught = true; };
        sigemptyset(&mine.sa_mask);
        QCOMPARE(sigaction(SIGTERM, &mine, &previous), 0);
        const auto restore = qScopeGuard([&previous] { sigaction(SIGTERM, &previous, nullptr); });
        std::thread window([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            ::kill(::getpid(), SIGTERM);
        });
        QElapsedTimer clock;
        clock.start();
        const CommandResult r = signInCommand(controlPaths(dir.path(), false), tools, {});
        window.join();
        QCOMPARE(r.code, 1);
        QVERIFY2(clock.elapsed() < 4000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(rcloneCalls(dir.path()).contains(QStringList({"config", "delete", "drive-sync"})));
        QVERIFY2(r.err.contains("stopped"), qPrintable(r.err));
        // The test's own handler is back.
        struct sigaction now {};
        sigaction(SIGTERM, nullptr, &now);
        QVERIFY(now.sa_handler == mine.sa_handler);
    }

    void signinRelaysTheBrowserLink()
    {
        QTemporaryDir dir;
        const QByteArray notice = "2026/11/14 23:00:00 NOTICE: If your browser doesn't open automatically go to the "
                                  "following link: http://127.0.0.1:53682/auth?state=abc\n"
                                  "2026/11/14 23:00:00 NOTICE: Log in and authorize rclone for access\n";
        QVERIFY(write(dir.filePath("create-notice"), notice));
        const CommandResult json = signInCommand(controlPaths(dir.path(), false), signInTools(dir.path()), {"--json"});
        QCOMPARE(json.code, 0);
        const QStringList lines = json.out.split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(lines.first().toUtf8()).object().value("link").toString(),
                 QString("http://127.0.0.1:53682/auth?state=abc"));
        QCOMPARE(QJsonDocument::fromJson(lines.last().toUtf8()).object().value("ok").toBool(), true);
        QTemporaryDir second;
        QVERIFY(write(second.filePath("create-notice"), notice));
        const CommandResult text = signInCommand(controlPaths(second.path(), false), signInTools(second.path()), {});
        QCOMPARE(text.code, 0);
        QVERIFY2(text.err.contains("http://127.0.0.1:53682/auth?state=abc"), qPrintable(text.err));
    }

    void signinDeletesTheHalfMadeRemote()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), false);
        QVERIFY(write(dir.filePath("create-exit"), "1\n"));
        // rclone's own error may quote the client: it never reaches the output.
        QVERIFY(write(dir.filePath("create-stderr"), "boom: fake-client.apps.googleusercontent.com fake-secret\n"));
        CommandResult r = signInCommand(paths, signInTools(dir.path()), {});
        QCOMPARE(r.code, 1);
        QVERIFY(rcloneCalls(dir.path()).contains(QStringList({"config", "delete", "drive-sync"})));
        QVERIFY2(!r.err.contains("fake-secret") && !r.err.contains("fake-client"), qPrintable(r.err));
        QVERIFY2(r.err.contains("boom"), qPrintable(r.err));

        // The browser never answered: killed at the timeout, then cleaned up.
        QTemporaryDir second;
        QVERIFY(write(second.filePath("create-hangs"), ""));
        SignInTools tools = signInTools(second.path());
        tools.timeoutMs = 200;
        QElapsedTimer clock;
        clock.start();
        r = signInCommand(controlPaths(second.path(), false), tools, {});
        QCOMPARE(r.code, 1);
        QVERIFY2(clock.elapsed() < 4000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(rcloneCalls(second.path()).contains(QStringList({"config", "delete", "drive-sync"})));
    }

    void signinRefreshesTheConfiguredRemote()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.config = dir.filePath("config.json");
        QString error;
        // No runOnlyWhen: the old unit's check fails on any machine but its own.
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        const CommandResult r = signInCommand(paths, signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        QList<QStringList> configCalls;
        for (const QStringList &call : rcloneCalls(dir.path())) {
            if (call.first() == "config")
                configCalls << call;
            // The remote keeps its own client: the shipped one never shows up.
            QVERIFY2(!call.join(' ').contains("fake-client") && !call.join(' ').contains("fake-secret"),
                     qPrintable(call.join(' ')));
        }
        QCOMPARE(configCalls, QList<QStringList>{refreshTokenArguments("gdrive", pageArgument(configCalls.value(0)))});
        QCOMPARE(rcloneCalls(dir.path()).last().mid(0, 2), (QStringList{"lsjson", "gdrive:"}));

        // A fix that waits for the user, or stopped (on the token, say),
        // lets the sign-in through; one at work does not.
        for (const QString &step : {QString("confirm"), QString("failed")}) {
            QVERIFY(writeFix(paths.appState, fixAt(step)));
            const CommandResult waiting = signInCommand(paths, signInTools(dir.path()), {});
            QVERIFY2(waiting.code == 0, qPrintable(step + ": " + waiting.err));
        }
        QVERIFY(writeFix(paths.appState, fixAt("base")));
        const CommandResult held = signInCommand(paths, signInTools(dir.path()), {});
        QCOMPARE(held.code, 1);
        QVERIFY2(held.err.contains("fix"), qPrintable(held.err));
        QVERIFY(clearFix(paths.appState));

        // The config's own rclone.conf goes along.
        SyncConfig own = config;
        own.rcloneFlags << "--config" << dir.filePath("own.conf");
        QVERIFY(saveSyncConfig(paths.config, own, &error));
        QVERIFY(QFile::remove(dir.filePath("rclone-calls.log")));
        QCOMPARE(signInCommand(paths, signInTools(dir.path()), {}).code, 0);
        const QList<QStringList> owned = rcloneCalls(dir.path());
        QCOMPARE(owned.first(), refreshTokenArguments("gdrive", pageArgument(owned.first()))
                                    + QStringList({"--config", dir.filePath("own.conf")}));
        QVERIFY2(owned.last().join(' ').endsWith("--config " + dir.filePath("own.conf")), qPrintable(owned.last().join(' ')));
    }

    void signinWithoutShippedClientSaysSo()
    {
        QTemporaryDir dir;
        SignInTools tools = signInTools(dir.path());
        tools.client.reset();
        const CommandResult r = signInCommand(controlPaths(dir.path(), false), tools, {});
        QCOMPARE(r.code, 1);
        QVERIFY2(r.err.contains("this build has no Google client"), qPrintable(r.err));
        QVERIFY(rcloneCalls(dir.path()).isEmpty());
    }

    // Both browser flows get DriveSync's page in place of rclone's bare one:
    // a private file that exists only while rclone runs.
    void signinPassesItsPageOnBothPaths()
    {
        const QStringList leftBefore = leftoverPages();
        QTemporaryDir dir;
        CommandResult r = signInCommand(controlPaths(dir.path(), false), signInTools(dir.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        const QStringList create = rcloneCalls(dir.path()).value(1);
        const QString createPage = pageArgument(create);
        QVERIFY2(!createPage.isEmpty(), qPrintable(create.join(' ')));
        QCOMPARE(create, createRemoteArguments(kNewRemoteName, fakeClient(), createPage));
        QVERIFY2(!QFile::exists(createPage), qPrintable(createPage));
        QCOMPARE(readBytes(dir.filePath("page-mode")).trimmed(), QByteArray("600"));
        const QByteArray seen = readBytes(dir.filePath("page-seen.html"));
        QVERIFY2(seen.contains("{{if .OK}}") && seen.contains("{{.Description}}"), seen.constData());
        QVERIFY2(seen.contains("You can go back to DriveSync") && seen.contains("You can close this tab."), seen.constData());
        QVERIFY2(seen.contains("Google did not complete the sign-in"), seen.constData());
        QVERIFY2(seen.contains("<svg") && seen.contains("lang=\"en\""), seen.constData());
        QVERIFY2(!seen.contains("@OK_TITLE@") && !seen.contains("@LOGO@") && !seen.contains("<desc>"), seen.constData());
        // Nothing on the page is fetched: it is served from 127.0.0.1.
        QVERIFY2(!seen.contains("src=") && !seen.contains("url(") && !seen.contains("@import") && !seen.contains("href=\"http"),
                 seen.constData());
        QVERIFY2(!seen.contains("<!--"), seen.constData());

        QTemporaryDir second;
        BackendPaths paths = controlPaths(second.path(), true);
        paths.config = second.filePath("config.json");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QString error;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        QVERIFY(QFile::remove(dir.filePath("page-seen.html")));
        r = signInCommand(paths, signInTools(second.path()), {});
        QVERIFY2(r.code == 0, qPrintable(r.err));
        const QStringList update = rcloneCalls(second.path()).value(0);
        const QString updatePage = pageArgument(update);
        QVERIFY2(!updatePage.isEmpty(), qPrintable(update.join(' ')));
        QCOMPARE(update, refreshTokenArguments("gdrive", updatePage));
        QVERIFY2(!QFile::exists(updatePage), qPrintable(updatePage));
        QVERIFY(readBytes(second.filePath("page-seen.html")).contains("You can go back to DriveSync"));
        QCOMPARE(leftoverPages(), leftBefore);
    }

    // Translated text is escaped before rclone sees it: a translation can
    // neither add markup nor a Go template action.
    void signinPageEscapesItsOwnText()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("language"), "pt-BR\n"));
        QVERIFY(write(dir.filePath("pt-BR.json"),
                      QJsonDocument(QJsonObject{{"You can go back to DriveSync", "<b>{{.Code}}</b> & co"}}).toJson()));
        const QLocale locale;
        Translator translator(dir.filePath("language"), dir.path());
        QVERIFY(QCoreApplication::installTranslator(&translator));
        const auto restore = qScopeGuard([&] {
            QCoreApplication::removeTranslator(&translator);
            QLocale::setDefault(locale);
        });
        const QByteArray page = signInPage("pt-BR\"><x");
        QVERIFY2(page.contains("&lt;b&gt;&#123;&#123;.Code&#125;&#125;&lt;/b&gt; &amp; co"), page.constData());
        QVERIFY2(!page.contains("<b>") && !page.contains("{{.Code}}"), page.constData());
        QVERIFY2(page.contains("lang=\"pt-BR&quot;&gt;&lt;x\""), page.constData());
    }

    // The real rclone serves the page in the user's language when Google
    // sends the browser back with a code, and the client never shows.
    void signinServesTheTranslatedPage()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QFile catalog(QString(SRCDIR) + "/../src/i18n/pt-BR.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const QJsonObject strings = QJsonDocument::fromJson(catalog.readAll()).object();
        for (const char *key : {"You can go back to DriveSync", "DriveSync finishes the sign-in and shows how it went.",
                                "You can close this tab.", "Google did not complete the sign-in",
                                "Go back to DriveSync and try again."})
            QVERIFY2(!strings.value(key).toString().isEmpty(), key);

        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("language"), "pt-BR\n"));
        const QLocale locale;
        Translator translator(dir.filePath("language"), QString(SRCDIR) + "/../src/i18n");
        QVERIFY(QCoreApplication::installTranslator(&translator));
        const auto restore = qScopeGuard([&] {
            QCoreApplication::removeTranslator(&translator);
            QLocale::setDefault(locale);
        });
        const QStringList leftBefore = leftoverPages();
        SignInTools tools = signInTools(dir.path());
        tools.rclone = rclone;
        tools.language = translator.language();
        const ServedSignIn served = signInThroughRclone(dir.path(), controlPaths(dir.path(), false), tools, "", "code=x");
        const QByteArray title = strings.value("You can go back to DriveSync").toString().toUtf8();
        QVERIFY2(served.page.contains(title), served.page.constData());
        QVERIFY2(served.page.contains(strings.value("You can close this tab.").toString().toUtf8()), served.page.constData());
        QVERIFY2(served.page.contains("lang=\"pt-BR\""), served.page.constData());
        QVERIFY2(!served.page.contains("{{") && !served.page.contains("@OK_TITLE@"), served.page.constData());
        // The code goes to a token address nobody answers: the sign-in fails
        // after the page, and DriveSync says so.
        QCOMPARE(served.result.code, 1);
        for (const QString &text : {served.result.out, served.result.err})
            QVERIFY2(!text.contains("fake-client") && !text.contains("fake-secret"), qPrintable(text));
        QCOMPARE(leftoverPages(), leftBefore);
    }

    // What Google puts in its error redirect reaches the page escaped: rclone
    // renders it with html/template. Signing in again takes the page too.
    void signinPageEscapesWhatGoogleSends()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.config = dir.filePath("config.json");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QString error;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        SignInTools tools = signInTools(dir.path());
        tools.rclone = rclone;
        const QByteArray conf = "[gdrive]\ntype = drive\nclient_id = fake-client.apps.googleusercontent.com\n"
                                "client_secret = fake-secret\nscope = drive\n";
        const QByteArray query = "error=" + QUrl::toPercentEncoding("<script>alert(1)</script>")
                                 + "&error_description=" + QUrl::toPercentEncoding("<img src=x onerror=alert(2)> & \"q\"");
        const ServedSignIn served = signInThroughRclone(dir.path(), paths, tools, conf, query);
        QVERIFY2(served.page.contains("Google did not complete the sign-in"), served.page.constData());
        QVERIFY2(served.page.contains("Go back to DriveSync and try again."), served.page.constData());
        QVERIFY2(served.page.contains("&lt;script&gt;alert(1)&lt;/script&gt;"), served.page.constData());
        QVERIFY2(served.page.contains("&lt;img src=x onerror=alert(2)&gt; &amp; &#34;q&#34;"), served.page.constData());
        QVERIFY2(!served.page.contains("<script") && !served.page.contains("<img"), served.page.constData());
        QVERIFY2(!served.page.contains("You can go back to DriveSync"), served.page.constData());
        QCOMPARE(served.result.code, 1);
        for (const QString &text : {served.result.out, served.result.err})
            QVERIFY2(!text.contains("fake-client") && !text.contains("fake-secret"), qPrintable(text));
    }

    void abandonKeepsASignedInRemote()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), false);
        QVERIFY(write(dir.filePath("dump"), R"({"drive-sync":{"type":"drive","token":"an-access-token"}})"));
        QCOMPARE(signInCommand(paths, signInTools(dir.path()), {"--abandon"}).code, 0);
        for (const QStringList &call : rcloneCalls(dir.path()))
            QVERIFY2(!call.contains("delete"), qPrintable(call.join(' ')));

        // Someone else's remote with no token stays too.
        QVERIFY(write(dir.filePath("dump"), R"({"gdrive":{"type":"drive","token":"t"},"drive-sync":{"type":"drive","client_id":"c"}})"));
        QCOMPARE(signInCommand(paths, signInTools(dir.path()), {"--abandon", "--json"}).code, 0);
        for (const QStringList &call : rcloneCalls(dir.path()))
            QVERIFY2(!call.contains("delete"), qPrintable(call.join(' ')));
        // A build with no client cannot tell its own: nothing goes.
        SignInTools clientless = signInTools(dir.path());
        clientless.client.reset();
        QVERIFY(write(dir.filePath("dump"), R"({"drive-sync":{"type":"drive","client_id":"fake-client.apps.googleusercontent.com"}})"));
        QCOMPARE(signInCommand(paths, clientless, {"--abandon"}).code, 0);
        for (const QStringList &call : rcloneCalls(dir.path()))
            QVERIFY2(!call.contains("delete"), qPrintable(call.join(' ')));
        QCOMPARE(signInCommand(paths, signInTools(dir.path()), {"--abandon", "--json"}).code, 0);
        int deletes = 0;
        for (const QStringList &call : rcloneCalls(dir.path()))
            deletes += call == QStringList({"config", "delete", "drive-sync"}) ? 1 : 0;
        QCOMPARE(deletes, 1);

        // Once DriveSync runs drive-sync, the remote belongs to it: never deleted.
        QTemporaryDir managed;
        paths = controlPaths(managed.path(), true);
        paths.config = managed.filePath("config.json");
        QString error;
        QVERIFY(saveSyncConfig(paths.config, oldUnitConfig(), &error));
        QVERIFY(write(managed.filePath("dump"), R"({"drive-sync":{"type":"drive"}})"));
        QCOMPARE(signInCommand(paths, signInTools(managed.path()), {"--abandon"}).code, 0);
        QVERIFY(rcloneCalls(managed.path()).isEmpty());
    }

    void newUserConfigHasTheProvenFlags()
    {
        const SyncConfig config = newUserConfig("/h/Drive", "/h");
        QCOMPARE(config.remote, QString("drive-sync:"));
        QCOMPARE(config.localPath, QString("/h/Drive"));
        QCOMPARE(config.workDir, QString("/h/.local/state/drive-sync-app/bisync"));
        QCOMPARE(config.filtersFile, QString("/h/.config/drive-sync/filters.txt"));
        QCOMPARE(config.logFile, QString("/h/.local/state/drive-sync.log"));
        QVERIFY(config.runOnlyWhen.isEmpty());
        const QString line = rcloneRunArguments(config).join(' ');
        for (const char *flag : {"--drive-skip-gdocs", "--drive-skip-shortcuts", "--resilient", "--recover",
                                 "--max-lock 2m", "--max-delete 5", "--conflict-resolve newer", "--log-level INFO",
                                 "--fast-list", "--log-file /h/.local/state/drive-sync.log"})
            QVERIFY2(line.contains(flag), flag);
        // The generic rules of spec 6, and none of the maintainer's own.
        const QByteArray filters = defaultFilters();
        for (const char *rule : {"- .git/**", "- node_modules/**", "- .venv/**", "- __pycache__/**", "- *.pyc",
                                 "- *.gdoc", "- *.gsheet", "- *.gvid", "- desktop.ini", "- Thumbs.db", "- .DS_Store",
                                 "- .~lock.*#", "- .tmp.drivedownload/**", "- .tmp.driveupload/**"})
            QVERIFY2(filters.contains(QByteArray(rule) + "\n"), rule);
        QVERIFY(!filters.contains("/GitHub/"));
        QVERIFY(filters.contains("use Fix…"));
    }

    void setupRefusesWhenAConfigExists()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QString error;
        QVERIFY(QDir().mkpath(s.takeover.appConfigDir));
        QVERIFY(saveSyncConfig(s.paths.config, oldUnitConfig(), &error));
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.contains("already"), qPrintable(refusal));
        QCOMPARE(setupCommand(s, s.folder).code, 1);
        QVERIFY(!QFile::exists(s.folder));
    }

    void setupRefusesWhenAnOldDriveSyncIsFound()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(QDir().mkpath(s.takeover.userUnits));
        QVERIFY(write(s.takeover.userUnits + "/drive-sync.service", "[Service]\nExecStart=/bin/true\n"));
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.contains("take it over"), qPrintable(refusal));
    }

    void setupRefusesWithoutTheRemote()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(write(dir.filePath("remotes"), "gdrive:\n"));
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.contains("sign in"), qPrintable(refusal));
    }

    void setupRefusesWhenFiltersFileExists()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(QDir().mkpath(s.takeover.appConfigDir));
        QVERIFY(write(s.takeover.appConfigDir + "/filters.txt", "- mine/**\n"));
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.contains("filters.txt"), qPrintable(refusal));
        QCOMPARE(setupCommand(s, s.folder).code, 1);
        QCOMPARE(readBytes(s.takeover.appConfigDir + "/filters.txt"), QByteArray("- mine/**\n"));
    }

    void setupRefusesForHomeItself()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        for (const QString &folder : {QDir::homePath(), QDir::homePath() + "/", QDir::rootPath(), QString("Drive"), QString()}) {
            const QString refusal = setupRefusal(s.paths, s.takeover, folder, s.takeover.rclone);
            QVERIFY2(!refusal.isEmpty(), qPrintable(folder));
        }
        // Refused before rclone is even asked.
        QVERIFY(rcloneCalls(dir.path()).isEmpty());
    }

    void setupRefusesForOldRclone()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(write(dir.filePath("version"), "rclone v1.65.0\n- os/version: arch\n"));
        QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.contains("1.65.0") && refusal.contains("1.66"), qPrintable(refusal));
        QVERIFY(write(dir.filePath("version"), "rclone v1.66.0\n"));
        refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.isEmpty(), qPrintable(refusal));
    }

    void setupWritesFixMarkBeforeEnablingTimers()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        const CommandResult r = setupCommand(s, s.folder + "/");
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        const QStringList log = calls(dir.path());
        const qsizetype start = log.indexOf("systemctl --user start --no-block drive-sync-fix.service");
        const qsizetype enable = log.indexOf("systemctl --user enable --now drive-sync.timer drive-sync-watchdog.timer");
        QVERIFY2(start >= 0 && enable > start, qPrintable(log.join('\n')));
        QCOMPARE(log.value(enable + 1), QString("mark preparing"));
        QString error;
        const auto config = loadSyncConfig(s.paths.config, &error);
        QVERIFY2(config, qPrintable(error));
        // The folder is written without the slash: bisync names its listings
        // after this string.
        QCOMPARE(config->localPath, s.folder);
        QCOMPARE(config->remote, QString("drive-sync:"));
        QCOMPARE(config->filtersFile, s.takeover.appConfigDir + "/filters.txt");
        QCOMPARE(config->workDir, s.paths.appState + "/bisync");
        QCOMPARE(config->logFile, s.paths.log);
        QCOMPARE(readBytes(config->filtersFile), defaultFilters());
        QVERIFY(!QFile::exists(config->filtersFile + ".md5"));  // the base run writes it
        QVERIFY(QFileInfo(s.folder).isDir());
        QVERIFY(QFileInfo(config->workDir).isDir());
        QCOMPARE(readFix(s.paths.appState).step, QString("preparing"));
        // What this setup wrote, so cancelling before the base can undo it.
        QVERIFY(QFile::exists(firstSetupMarkPath(s.paths.appState)));
    }

    void setupKeepsTheFixWhenEnableFails()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(write(dir.filePath("enable-exit"), "1\n"));
        const CommandResult r = setupCommand(s, s.folder);
        QCOMPARE(r.code, 1);
        const QString error = setupError(r);
        QVERIFY2(error.contains("drive-sync.timer") && error.contains("denied"), qPrintable(error));
        // The fix owns the state from its mark on: nothing is rolled back.
        QVERIFY(fixPresent(s.paths.appState));
        QVERIFY(QFile::exists(s.paths.config));
    }

    void setupRollsBackWhenTheFixCannotStart()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(write(dir.filePath("start-exit"), "1\n"));
        const CommandResult r = setupCommand(s, s.folder);
        QCOMPARE(r.code, 1);
        QVERIFY2(setupError(r).contains("drive-sync-fix.service"), qPrintable(setupError(r)));
        QVERIFY(!QFile::exists(s.paths.config));
        QVERIFY(!QFile::exists(s.takeover.appConfigDir + "/filters.txt"));
        QVERIFY(!QFile::exists(s.folder));  // made by this run, still empty
        QVERIFY(!fixPresent(s.paths.appState));
        QVERIFY(calls(dir.path()).filter("enable").isEmpty());
        // A folder that was there before stays, with what it holds.
        QVERIFY(QDir().mkpath(s.folder));
        QVERIFY(write(s.folder + "/keep.txt", "mine\n"));
        QCOMPARE(setupCommand(s, s.folder).code, 1);
        QCOMPARE(readBytes(s.folder + "/keep.txt"), QByteArray("mine\n"));
    }

    void firstSetupBaseRunWritesTheMd5()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Setup with the fake rclone (it knows the drive-sync remote), then
        // the fix with real rclone on two local folders: the config's remote
        // becomes the "cloud" folder, since an rclone alias would name the
        // listings after its target instead.
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(QDir().mkpath(dir.filePath("cloud")));
        // A folder that already holds files, on both sides (review focus 2).
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        QVERIFY(QDir().mkpath(s.folder + "/.git"));
        QVERIFY(write(s.folder + "/two.txt", "two\n"));
        QVERIFY(write(s.folder + "/.git/skipped", "x\n"));
        const auto cloudBefore = filesUnder(dir.filePath("cloud"));
        const auto notebookBefore = filesUnder(s.folder);

        const CommandResult r = setupCommand(s, s.folder);
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        QString error;
        auto config = loadSyncConfig(s.paths.config, &error);
        QVERIFY2(config, qPrintable(error));
        config->remote = dir.filePath("cloud");
        QVERIFY(saveSyncConfig(s.paths.config, *config, &error));
        BackendPaths managed = s.paths;
        managed.managed = true;
        managed.log = config->logFile;
        managed.drive = config->localPath;
        FixTools tools;
        tools.rclone = rclone;
        tools.wait = [](int) {};
        const NotifierPaths notify = fakeNotifier(dir.path());

        // The dry run lists both directions and writes nothing.
        QCOMPARE(runFixUnit(managed, *config, tools, notify), 0);
        FixState state = readFix(s.paths.appState);
        QVERIFY2(state.step == "confirm", qPrintable(state.step + ": " + state.error));
        int total = 0;
        int toNotebook = 0;
        dryRunCopies(readBytes(fixDryRunLogPath(s.paths.appState)), 10, &total, &toNotebook);
        QCOMPARE(total, 2);
        QCOMPARE(toNotebook, 1);
        QCOMPARE(filesUnder(dir.filePath("cloud")), cloudBefore);
        QCOMPARE(filesUnder(s.folder), notebookBefore);

        FixSetup fix;
        fix.paths = managed;
        QCOMPARE(fixCommand(fix, {"--confirm"}), QString());
        const int unit = runFixUnit(managed, *config, tools, notify);
        state = readFix(s.paths.appState);
        QVERIFY2(unit == 0 && !fixPresent(s.paths.appState),
                 qPrintable(state.step + " after " + state.failedStep + ": " + state.error));
        QVERIFY(QFile::exists(config->filtersFile + ".md5"));
        QString why;
        QVERIFY2(filtersUsable(config->filtersFile, &why), qPrintable(why));
        QVERIFY(QFile::exists(s.folder + "/one.txt"));
        QVERIFY(QFile::exists(dir.filePath("cloud/two.txt")));
        QVERIFY(!QFile::exists(dir.filePath("cloud/.git/skipped")));
        QVERIFY(QFile::exists(config->workDir + "/" + bisyncSessionName(config->remote, config->localPath) + ".path1.lst"));
        QVERIFY(readBytes(config->logFile).contains("Bisync successful"));
    }

    void normalRunBeforeTheBaseTouchesNothing()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // As above: setup with the fake rclone, then real rclone on two
        // local folders.
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(QDir().mkpath(dir.filePath("cloud")));
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        QVERIFY(QDir().mkpath(s.folder));
        QVERIFY(write(s.folder + "/two.txt", "two\n"));
        QCOMPARE(setupCommand(s, s.folder).code, 0);
        QString error;
        auto config = loadSyncConfig(s.paths.config, &error);
        QVERIFY2(config, qPrintable(error));
        config->remote = dir.filePath("cloud");
        QVERIFY(saveSyncConfig(s.paths.config, *config, &error));
        // The first-setup fix goes away without the undo (a setup whose mark
        // is gone), with the timers already on.
        QVERIFY(QFile::remove(firstSetupMarkPath(s.paths.appState)));
        FixSetup fix;
        fix.paths = s.paths;
        fix.paths.managed = true;
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        QVERIFY(QFile::exists(s.paths.config));
        const auto cloudBefore = filesUnder(dir.filePath("cloud"));
        const auto notebookBefore = filesUnder(s.folder);

        // The next tick: no .md5 and no listings, so drive-sync run refuses
        // before rclone with its own cause, never "the filters changed".
        QString refusal;
        QVERIFY(runCommandPlan(*config, &refusal).isEmpty());
        QVERIFY2(refusal.contains(kFirstSyncPendingReason) && !refusal.contains(kFiltersChangedReason),
                 qPrintable(refusal));
        appendToLog(config->logFile, "CRITICAL", "DriveSync did not start rclone: " + refusal);
        QList<SyncRun> runs = SyncLog::parse(QString::fromUtf8(readBytes(config->logFile)));
        QVERIFY(!runs.isEmpty());
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("firstSyncPending"));
        QVERIFY(!guardStopHolds(runs));
        QVERIFY(fixBannerOffers("failed", "firstSyncPending"));
        // Its card is news, not an alarm, and says what to press.
        BackendPaths cardPaths = controlPaths(dir.path(), true);
        cardPaths.log = config->logFile;
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runNotifyCommand("exit-code", cardPaths, tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("-u normal") && sent.at(0).contains("use Fix…") && !sent.at(0).contains("filters"),
                 qPrintable(sent.at(0)));

        // Even with an .md5 in place, bisync without listings aborts and
        // touches no file: a cause that offers F too.
        QVERIFY(write(config->filtersFile + ".md5",
                      QCryptographicHash::hash(readBytes(config->filtersFile), QCryptographicHash::Md5).toHex()));
        QProcess run;
        run.start(rclone, rcloneRunArguments(*config));
        QVERIFY(run.waitForFinished(60000));
        QVERIFY(run.exitCode() != 0);
        QCOMPARE(filesUnder(dir.filePath("cloud")), cloudBefore);
        QCOMPARE(filesUnder(s.folder), notebookBefore);
        runs = SyncLog::parse(QString::fromUtf8(readBytes(config->logFile)));
        QCOMPARE(runs.last().outcome, QString("failed"));
        QVERIFY2(runs.last().causeKind == "lostListings",
                 qPrintable(runs.last().causeKind + ": " + runs.last().errors.join(" | ")));
        QVERIFY(fixBannerOffers("failed", "lostListings"));
    }

    void tokenErrorIsSignIn()
    {
        const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
        // What rclone 1.75 writes when Google refuses the refresh token: the
        // remote fails before bisync starts.
        const QString revoked = line(start, "ERROR",
            "Failed to create file system for \"gdrive:\": couldn't find root directory ID: Get "
            "\"https://www.googleapis.com/drive/v3/files/root\": couldn't fetch token: invalid_grant: maybe token "
            "expired? - try refreshing with \"rclone config reconnect gdrive:\"");
        QList<SyncRun> runs = SyncLog::parse(successfulRun(start.addSecs(-900), 60) + revoked);
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("signIn"));
        // rclone's advice needs a terminal: the detail never carries it.
        QVERIFY2(runs.last().causeDetail.isEmpty(), qPrintable(runs.last().causeDetail));
        // Any case, and inside a bisync run too.
        for (const char *refused : {"oauth2: cannot fetch token: 401 Unauthorized Response: {\"error\": \"UNAUTHORIZED_CLIENT\"}",
                                    "oauth2: cannot fetch token: 401 Unauthorized Response: {\"error\": \"invalid_client\"}"}) {
            runs = SyncLog::parse(line(start, "INFO", R"(Synching Path1 "gdrive:" with Path2 "/home/someone/Drive/")")
                                  + line(start, "ERROR", QString("Path1 listing failed: %1").arg(refused))
                                  + line(start, "ERROR", "Bisync aborted. Must run --resync to recover."));
            QVERIFY2(runs.last().causeKind == "signIn", refused);
        }
        // The network, not the sign-in: rclone 1.75.2 says "couldn't fetch
        // token" for those too (tried with a token URL nobody answers).
        for (const char *network : {
                 "Failed to create file system for \"x:\": couldn't find root directory ID: Get \"https://www.googleapis.com/drive/v3/files/root\": couldn't fetch token: Post \"http://127.0.0.1:9/token\": dial tcp 127.0.0.1:9: connect: connection refused",
                 "couldn't fetch token: Post \"https://oauth2.googleapis.com/token\": dial tcp: lookup oauth2.googleapis.com: no such host",
                 "oauth2: cannot fetch token: 503 Service Unavailable"}) {
            runs = SyncLog::parse(line(start, "ERROR", network));
            QVERIFY2(runs.last().causeKind == "error", network);
        }
    }

    void otherErrorStaysError()
    {
        const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
        const QList<SyncRun> runs = SyncLog::parse(
            line(start, "ERROR", "Failed to create file system for \"gdrive:\": didn't find section in config file (\"gdrive\")"));
        QCOMPARE(runs.last().outcome, QString("failed"));
        QCOMPARE(runs.last().causeKind, QString("error"));
        QVERIFY(runs.last().causeDetail.contains("didn't find section"));
    }

    void notifyAsksToSignInAgain()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(write(paths.log, line(QDateTime::currentDateTime(), "ERROR",
                                      "Failed to create file system for \"gdrive:\": couldn't fetch token: invalid_grant: maybe token "
                                      "expired? - try refreshing with \"rclone config reconnect gdrive:\"").toUtf8()));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runNotifyCommand("exit-code", paths, tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        // Nothing syncs until the user signs in: the critical card.
        QVERIFY2(sent.at(0).contains("-u critical") && sent.at(0).contains("Drive stopped syncing"), qPrintable(sent.at(0)));
        QVERIFY2(sent.at(0).contains("Open DriveSync and sign in again"), qPrintable(sent.at(0)));
        QVERIFY2(!sent.at(0).contains("reconnect"), qPrintable(sent.at(0)));
        QFile catalog(QString(SRCDIR) + "/../src/i18n/pt-BR.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const QJsonObject strings = QJsonDocument::fromJson(catalog.readAll()).object();
        QVERIFY(strings.contains("Google no longer accepts DriveSync's sign-in, so nothing syncs. Open DriveSync and sign in again."));
    }

    void setupControllerCancelStopsOnlyTheSignIn()
    {
        QTemporaryDir dir;
        SetupController controller(fakeSetupDrivesync(dir.path()), dir.filePath("Drive"));
        QCOMPARE(controller.state(), QString("idle"));
        QCOMPARE(controller.folder(), dir.filePath("Drive"));
        controller.signIn();
        QCOMPARE(controller.state(), QString("signingIn"));
        QTRY_VERIFY(QFile::exists(dir.filePath("child.pid")) && QFile::exists(dir.filePath("grandchild.pid")));
        const qint64 child = readBytes(dir.filePath("child.pid")).trimmed().toLongLong();
        const qint64 grandchild = readBytes(dir.filePath("grandchild.pid")).trimmed().toLongLong();
        const qint64 browser = readBytes(dir.filePath("browser.pid")).trimmed().toLongLong();
        const auto noBrowser = qScopeGuard([browser] { ::kill(static_cast<pid_t>(browser), SIGKILL); });
        QVERIFY(child > 0 && grandchild > 0 && browser > 0);
        QVERIFY(!processGone(grandchild));
        controller.cancel();
        QCOMPARE(controller.state(), QString("idle"));
        // The command stops its rclone and cleans up; the browser, in the
        // same group, is the user's and stays.
        QTRY_VERIFY(processGone(child) && processGone(grandchild));
        QTRY_VERIFY(calls(dir.path()).contains("cleaned"));
        QTRY_VERIFY(calls(dir.path()).contains("signin --abandon"));
        QVERIFY(!processGone(browser));

        // Closing the window while it waits does the same.
        QTemporaryDir second;
        auto window = std::make_unique<SetupController>(fakeSetupDrivesync(second.path()), second.filePath("Drive"));
        window->signIn();
        QTRY_VERIFY(QFile::exists(second.filePath("grandchild.pid")) && QFile::exists(second.filePath("child.pid")));
        const qint64 orphan = readBytes(second.filePath("grandchild.pid")).trimmed().toLongLong();
        const qint64 secondBrowser = readBytes(second.filePath("browser.pid")).trimmed().toLongLong();
        const auto noSecondBrowser = qScopeGuard([secondBrowser] { ::kill(static_cast<pid_t>(secondBrowser), SIGKILL); });
        window.reset();
        QTRY_VERIFY(processGone(orphan));
        QTRY_VERIFY(calls(second.path()).contains("cleaned"));
        QVERIFY(!processGone(secondBrowser));
    }

    void setupControllerFollowsTheCommands()
    {
        QTemporaryDir dir;
        SetupController controller(fakeSetupDrivesync(dir.path()), dir.filePath("Drive"));
        QSignalSpy signedIn(&controller, &SetupController::signedIn);
        QSignalSpy started(&controller, &SetupController::setupStarted);
        // A failed sign-in says why.
        QVERIFY(write(dir.filePath("signin-answer"), R"({"ok":false,"error":"the browser said no"})"));
        controller.signIn();
        QTRY_COMPARE(controller.state(), QString("failed"));
        QCOMPARE(controller.detail(), QString("the browser said no"));
        // A remote of that name the user made is never adopted.
        QVERIFY(write(dir.filePath("signin-answer"), R"({"ok":false,"error":"not ours","reason":"remoteExists","signedIn":true})"));
        controller.signIn();
        QTRY_COMPARE(controller.detail(), QString("not ours"));
        QCOMPARE(controller.state(), QString("failed"));
        // The browser link shows while the sign-in waits, and goes after it.
        QVERIFY(write(dir.filePath("signin-link"), "{\"link\":\"http://127.0.0.1:53682/auth?state=abc\"}\n"));
        QVERIFY(write(dir.filePath("signin-answer"), R"({"ok":true,"error":""})"));
        controller.signIn();
        QTRY_COMPARE(controller.link(), QString("http://127.0.0.1:53682/auth?state=abc"));
        QCOMPARE(controller.state(), QString("signingIn"));
        QVERIFY(write(dir.filePath("signin-release"), ""));
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        QCOMPARE(controller.link(), QString());
        QCOMPARE(controller.detail(), QString());
        QCOMPARE(signedIn.size(), 1);
        QVERIFY(QFile::remove(dir.filePath("signin-link")));
        // A failed setup keeps the sign-in.
        QVERIFY(write(dir.filePath("setup-answer"), R"({"ok":false,"error":"no room"})"));
        controller.start(dir.filePath("Elsewhere"));
        QCOMPARE(controller.state(), QString("settingUp"));
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        QCOMPARE(controller.detail(), QString("no room"));
        QCOMPARE(controller.folder(), dir.filePath("Elsewhere"));
        QVERIFY(calls(dir.path()).contains("setup --folder " + dir.filePath("Elsewhere") + " --json"));
        QVERIFY(write(dir.filePath("setup-answer"), R"({"ok":true,"error":""})"));
        controller.start(dir.filePath("Elsewhere"));
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(controller.state(), QString("idle"));
        // The dialog shows the home as "~"; the command gets the real path.
        controller.signIn();
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        controller.start("~/Drive");
        QTRY_COMPARE(started.size(), 2);
        QVERIFY(calls(dir.path()).contains("setup --folder " + QDir::homePath() + "/Drive --json"));
        // The cloud folders left out in the dialog's tree go along.
        controller.signIn();
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        controller.start("~/Drive", {"Photos", "Old Stuff"});
        QTRY_COMPARE(started.size(), 3);
        QVERIFY(calls(dir.path()).contains("setup --folder " + QDir::homePath() + "/Drive --skip Photos --skip Old Stuff --json"));
        // Nothing to cancel once setup started: no abandon.
        controller.cancel();
        QVERIFY(!calls(dir.path()).contains("signin --abandon"));
    }

    // The window after "drive-sync setup" answers ok: it started with no
    // config and no state folder, then reloads the config, and must follow
    // the fix that setup started up to the dry run waiting for the user.
    void backendFollowsTheFixFirstSetupStarts()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.config = dir.filePath("config/config.json");
        paths.self = fakeDriveSync(dir.path(), "{}");
        Backend backend(paths);
        QVERIFY(!backend.managed());
        QCOMPARE(backend.fixStep(), QString());

        SyncConfig config = newUserConfig(dir.filePath("Drive"), dir.path());
        config.logFile = paths.log;
        QVERIFY(QDir().mkpath(dir.filePath("config")));
        QString error;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        FixState state = fixAt("preparing");
        QVERIFY(writeFix(paths.appState, state));
        backend.reloadConfig();
        QTRY_VERIFY(backend.managed());
        QTRY_COMPARE(backend.fixStep(), QString("preparing"));

        QSignalSpy changed(&backend, &Backend::fixChanged);
        state = fixAt("confirm");
        state.copies = 2;
        QVERIFY(writeFix(paths.appState, state));
        QTRY_COMPARE(backend.fixStep(), QString("confirm"));
        QVERIFY(changed.size() >= 1);
        QTRY_COMPARE(backend.status(), QString("fixWaiting"));
    }

    void setupDialogStaysOpenWhileSigningIn()
    {
        // The real SetupDialog.qml over a stand-in controller: a click
        // outside must not drop a sign-in that waits for the browser.
        QQmlEngine engine;
        QQmlPropertyMap controller, theme, translator;
        controller.insert("state", "idle");
        controller.insert("detail", "");
        controller.insert("folder", "/home/someone/Drive");
        controller.insert("link", "");
        theme.insert("radius", 8);
        theme.insert("accent", QColor("#7aa2f7"));
        translator.insert("language", "en");
        engine.rootContext()->setContextProperty("setupController", &controller);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        // The dialog's tree of cloud folders, over a made-up drive-sync.
        QTemporaryDir dir;
        fakeSettingsDrivesync(dir.path());
        QVERIFY(write(dir.filePath("folders-root.json"),
                      QJsonDocument(QJsonObject{{"ok", true}, {"error", ""},
                                                {"folders", QJsonArray{QJsonObject{{"name", "Photos"}, {"path", "Photos"},
                                                                                   {"local", false}, {"unsupported", false}}}}})
                          .toJson(QJsonDocument::Compact)));
        FolderTree tree(dir.filePath("drive-sync"));
        engine.rootContext()->setContextProperty("folderTree", &tree);
        // Step 4's filters editor, over the same made-up folder.
        LocalTree localTree;
        FilterEditor filterEditor;
        engine.rootContext()->setContextProperty("localTree", &localTree);
        engine.rootContext()->setContextProperty("filterEditor", &filterEditor);
        QTest::failOnWarning(QRegularExpression("ReferenceError"));
        QQmlComponent component(&engine);
        component.setData(QString("import QtQuick\nimport QtQuick.Controls\nimport \"%1\"\n"
                                  "ApplicationWindow { width: 800; height: 600; visible: true\n"
                                  "  property alias dialog: d\n  SetupDialog { id: d } }\n")
                              .arg(QUrl::fromLocalFile(QString(SRCDIR) + "/../src").toString())
                              .toUtf8(),
                          QUrl());
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        constexpr int noAutoClose = 0;  // Popup.NoAutoClose
        QVERIFY(dialog->property("closePolicy").toInt() != noAutoClose);
        controller.insert("state", "signingIn");
        QTRY_COMPARE(dialog->property("closePolicy").toInt(), noAutoClose);
        controller.insert("state", "signedIn");
        QTRY_VERIFY(dialog->property("closePolicy").toInt() != noAutoClose);
        // Signed in, the dialog lists DriveSync's own remote for the tree.
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "openSetup"));
        QTRY_COMPARE(tree.rows().size(), 1);
        QVERIFY(calls(dir.path()).contains("folders --remote drive-sync: --json"));
    }

    void cancellingAFirstSetupFixUndoesTheSetup()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QCOMPARE(setupCommand(s, s.folder).code, 0);
        FixSetup fix;
        fix.paths = s.paths;
        fix.paths.managed = true;
        // X on the dry run: no base was ever built for this config.
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        QVERIFY(!QFile::exists(s.paths.config));
        QVERIFY(!QFile::exists(s.takeover.appConfigDir + "/filters.txt"));
        QVERIFY(!QFile::exists(s.folder));  // made by setup, still empty
        QVERIFY(!QFile::exists(s.paths.appState + "/bisync"));
        QVERIFY(!fixPresent(s.paths.appState));
        QVERIFY(!QFile::exists(firstSetupMarkPath(s.paths.appState)));
        QVERIFY2(calls(dir.path()).contains("systemctl --user disable --now drive-sync.timer drive-sync-watchdog.timer"),
                 qPrintable(calls(dir.path()).join('\n')));
        // The window can offer setup again, for another folder.
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.isEmpty(), qPrintable(refusal));

        // What the user had before setup stays.
        QVERIFY(QDir().mkpath(s.folder));
        QVERIFY(write(s.folder + "/keep.txt", "mine\n"));
        QCOMPARE(setupCommand(s, s.folder).code, 0);
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(s.folder + "/keep.txt"), QByteArray("mine\n"));
        QVERIFY(!QFile::exists(s.paths.config));

        // Once the base ran (its .md5 is there), a cancel undoes nothing.
        QCOMPARE(setupCommand(s, s.folder).code, 0);
        const QString filters = s.takeover.appConfigDir + "/filters.txt";
        QVERIFY(write(filters + ".md5", QCryptographicHash::hash(readBytes(filters), QCryptographicHash::Md5).toHex()));
        QVERIFY(QFile::remove(dir.filePath("calls.log")));
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        QVERIFY(QFile::exists(s.paths.config));
        QVERIFY(QFile::exists(filters));
        QVERIFY(calls(dir.path()).filter("disable").isEmpty());

        // A taken-over drive-sync has no setup mark: never undone.
        QTemporaryDir other;
        const FixSetup taken = makeFixSetup(other.path());
        QVERIFY(writeFix(taken.paths.appState, fixAt("confirm")));
        QCOMPARE(fixCommand(taken, {"--cancel"}), QString());
        QVERIFY(QFile::exists(taken.paths.config));
        QVERIFY(QFile::exists(taken.config.filtersFile));
    }

    void setupRefusesFoldersThatHoldItsOwnFiles()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        const QString sandbox = QFileInfo(QString::fromLocal8Bit(qgetenv("RCLONE_CONFIG"))).absolutePath();
        for (const QString &folder : {s.paths.appState, s.paths.appState + "/Drive", s.paths.appState + "/bisync/x",
                                      s.takeover.appConfigDir + "/Drive", dir.filePath("logs"), dir.filePath("logs/Drive"),
                                      dir.path(), sandbox, sandbox + "/Drive", QDir::homePath() + "/.config/Drive",
                                      QDir::homePath() + "/.local/state/Drive", QDir::homePath() + "/.local"}) {
            const QString refusal = setupRefusal(s.paths, s.takeover, folder, s.takeover.rclone);
            QVERIFY2(!refusal.isEmpty(), qPrintable(folder));
        }
        const QString refusal = setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone);
        QVERIFY2(refusal.isEmpty(), qPrintable(refusal));
    }

    void setupRemovesAHalfWrittenFilters()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        // A disk that fills up while the filters are written: no file may be
        // larger than this, so the filters stop halfway and the config fits.
        const rlim_t limit = rlim_t(defaultFilters().size() / 2);
        struct rlimit previous {};
        QCOMPARE(getrlimit(RLIMIT_FSIZE, &previous), 0);
        const auto oldHandler = ::signal(SIGXFSZ, SIG_IGN);
        struct rlimit small = previous;
        small.rlim_cur = limit;
        QCOMPARE(setrlimit(RLIMIT_FSIZE, &small), 0);
        CommandResult r = setupCommand(s, s.folder);
        setrlimit(RLIMIT_FSIZE, &previous);
        ::signal(SIGXFSZ, oldHandler);
        QCOMPARE(r.code, 1);
        QVERIFY2(setupError(r).contains("filters.txt"), qPrintable(setupError(r)));
        QVERIFY(!QFile::exists(s.takeover.appConfigDir + "/filters.txt"));
        QVERIFY(!QFile::exists(s.paths.config));
        QVERIFY(!fixPresent(s.paths.appState));
    }

    void aFixStoppedOnTheTokenAsksToSignInAgain()
    {
        SyncInputs in = healthy();
        in.fixStep = "failed";
        in.fixError = "Failed to create file system for \"drive-sync:\": couldn't fetch token: invalid_grant: maybe "
                      "token expired? - try refreshing with \"rclone config reconnect drive-sync:\"";
        const SyncSummary summary = Backend::summarize(in);
        QCOMPARE(summary.status, QString("fixFailed"));
        QCOMPARE(summary.causeKind, QString("signIn"));
        QVERIFY2(!summary.causeDetail.contains("reconnect"), qPrintable(summary.causeDetail));
        // Any other stop keeps its own line.
        in.fixError = "boom";
        QCOMPARE(Backend::summarize(in).causeKind, QString("fix"));

        QTemporaryDir dir;
        const NotifierPaths tools = fakeNotifier(dir.path());
        FixState state = fixAt("failed");
        state.error = "couldn't fetch token: invalid_grant: maybe token expired? - try refreshing with \"rclone config reconnect drive-sync:\"";
        notifyFixNeedsYou(controlPaths(dir.path(), true), tools, state, true);
        const QStringList sent = notifications(dir.path());
        QVERIFY(!sent.isEmpty());
        QVERIFY2(sent.last().contains("sign in again") && !sent.last().contains("reconnect"), qPrintable(sent.last()));
    }

    // Step 4b. A config written before the interval existed keeps loading,
    // with the 15 minutes the packaged timer has.
    void configWithoutIntervalReadsFifteen()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        QVERIFY(write(path, R"({"version":1,"remote":"gdrive:","localPath":"/h/Drive","workDir":"/h/w",)"
                            R"("filtersFile":"/h/f.txt","logFile":"/h/l.log","rcloneFlags":[],"runOnlyWhen":""})"));
        QString error;
        auto config = loadSyncConfig(path, &error);
        QVERIFY2(config, qPrintable(error));
        QCOMPARE(config->intervalMinutes, 15);
        for (const QByteArray &bad : {QByteArray("0"), QByteArray("\"x\""), QByteArray("-5")}) {
            QVERIFY(write(path, R"({"version":1,"remote":"gdrive:","localPath":"/h/Drive","workDir":"/h/w",)"
                                R"("filtersFile":"/h/f.txt","logFile":"/h/l.log","rcloneFlags":[],"intervalMinutes":)"
                                    + bad + "}"));
            config = loadSyncConfig(path, &error);
            QVERIFY2(config, qPrintable(error));
            QCOMPARE(config->intervalMinutes, 15);
        }
        config->intervalMinutes = 30;
        QVERIFY(saveSyncConfig(path, *config, &error));
        QCOMPARE(loadSyncConfig(path, &error)->intervalMinutes, 30);
    }

    // Spec 4: "late" is three missed runs, 45 minutes at the default 15.
    void staleIsThreeIntervals()
    {
        SyncInputs in = healthy();
        in.intervalSeconds = 1800;
        in.lastSuccess = in.now.addSecs(-89 * 60);
        QCOMPARE(Backend::summarize(in).status, QString("ok"));
        in.lastSuccess = in.now.addSecs(-91 * 60);
        QCOMPARE(Backend::summarize(in).status, QString("stale"));
        SyncInputs standard = healthy();
        standard.lastSuccess = standard.now.addSecs(-44 * 60);
        QCOMPARE(Backend::summarize(standard).status, QString("ok"));
        standard.lastSuccess = standard.now.addSecs(-46 * 60);
        QCOMPARE(Backend::summarize(standard).status, QString("stale"));
    }

    void snapshotCarriesTheInterval()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        SnapshotSources src = idleSources(successfulRun(now.toLocalTime().addSecs(-2 * 3600), 60), now);
        QCOMPARE(buildSnapshot(src).summary.status, QString("stale"));
        src.intervalSeconds = 3600;
        QCOMPARE(buildSnapshot(src).summary.status, QString("ok"));
        // BackendPaths carries it from the config, for the window and the status command.
        SyncConfig config;
        config.intervalMinutes = 60;
        BackendPaths paths;
        applySyncConfig(&paths, config);
        QCOMPARE(paths.intervalSeconds, 3600);
    }

    void settingsShowsTheConfig()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {});
        QCOMPARE(code, 0);
        QVERIFY2(answer.value("ok").toBool(), qPrintable(answer.value("error").toString()));
        QCOMPARE(answer.value("folder").toString(), s.fix.config.localPath);
        QCOMPARE(answer.value("remote").toString(), QString("gdrive:"));
        QCOMPARE(answer.value("intervalMinutes").toInt(), 15);
        QCOMPARE(answer.value("maxDelete").toInt(), 5);
        QCOMPARE(answer.value("filtersFile").toString(), s.fix.config.filtersFile);
        QCOMPARE(answer.value("filters").toString(), QString("- .git/**\n"));
        // Nothing to show before DriveSync runs drive-sync.
        SettingsFixture unmanaged = s;
        unmanaged.fix.paths.managed = false;
        const auto refused = settingsCommand(unmanaged, {});
        QCOMPARE(refused.first, 1);
        QVERIFY(refused.second.value("error").toString().contains("take it over"));
    }

    void settingsSetsMaxDelete()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {"--max-delete", "10"});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QCOMPARE(answer.value("fixStarted").toBool(), false);
        QString error;
        QCOMPARE(loadSyncConfig(s.fix.paths.config, &error)->rcloneFlags,
                 (QStringList{"--drive-skip-gdocs", "--resilient", "--max-delete", "10"}));
        for (const QString &bad : {QString("0"), QString("51"), QString("x")})
            QCOMPARE(settingsCommand(s, {"--max-delete", bad}).first, 1);
        QCOMPARE(configuredMaxDelete(loadSyncConfig(s.fix.paths.config, &error)->rcloneFlags), 10);

        // After a guard stop only D lets deletions through: a raise is
        // refused, a lower guard is fine.
        const QDateTime start = QDateTime::currentDateTime().addSecs(-300);
        QVERIFY(write(s.fix.config.logFile, abortedRun(start, {}, numberedFiles("p%1.jpg", 11), 20).toUtf8()));
        const auto raised = settingsCommand(s, {"--max-delete", "20"});
        QCOMPARE(raised.first, 1);
        QVERIFY2(raised.second.value("error").toString().contains("D"), qPrintable(raised.second.value("error").toString()));
        QCOMPARE(configuredMaxDelete(loadSyncConfig(s.fix.paths.config, &error)->rcloneFlags), 10);
        QCOMPARE(settingsCommand(s, {"--max-delete", "3"}).first, 0);
        QCOMPARE(configuredMaxDelete(loadSyncConfig(s.fix.paths.config, &error)->rcloneFlags), 3);
    }

    void settingsSetsTheIntervalThroughADropIn()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {"--interval", "30"});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QString error;
        QCOMPARE(loadSyncConfig(s.fix.paths.config, &error)->intervalMinutes, 30);
        const QString dropIn = dir.filePath("units/drive-sync.timer.d/drive-sync-interval.conf");
        QCOMPARE(dropIn, intervalDropInPath(s.takeover.userUnits, "drive-sync.timer"));
        QCOMPARE(readBytes(dropIn), intervalDropIn(30));
        QVERIFY(intervalDropIn(30).contains("[Timer]\nOnUnitActiveSec=\nOnBootSec=5min\nOnUnitActiveSec=30min\n"));
        const QStringList sent = calls(dir.path());
        QVERIFY2(sent.indexOf("systemctl --user daemon-reload") >= 0
                     && sent.indexOf("systemctl --user daemon-reload") < sent.indexOf("systemctl --user try-restart drive-sync.timer"),
                 qPrintable(sent.join('\n')));
        // Back to the packaged 15 minutes: no drop-in at all.
        QCOMPARE(settingsCommand(s, {"--interval", "15"}).first, 0);
        QVERIFY(!QFile::exists(dropIn));
        QVERIFY(!QFile::exists(dir.filePath("units/drive-sync.timer.d")));
        QCOMPARE(loadSyncConfig(s.fix.paths.config, &error)->intervalMinutes, 15);
        for (const QString &bad : {QString("4"), QString("1441"), QString("ten")})
            QCOMPARE(settingsCommand(s, {"--interval", bad}).first, 1);
        // A fix holds every change.
        QVERIFY(writeFix(s.fix.paths.appState, fixAt("confirm")));
        QCOMPARE(settingsCommand(s, {"--interval", "60"}).first, 1);
        QVERIFY(!QFile::exists(dropIn));
    }

    void undoTakeoverRemovesTheIntervalDropIn()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        // What "drive-sync settings --interval 30" leaves.
        QVERIFY(QDir().mkpath(old.paths.userUnits + "/drive-sync.timer.d"));
        QVERIFY(write(intervalDropInPath(old.paths.userUnits, "drive-sync.timer"), intervalDropIn(30)));
        QVERIFY2(undoTakeover(old.paths, &error), qPrintable(error));
        QVERIFY(!QFile::exists(old.paths.userUnits + "/drive-sync.timer.d"));
        QVERIFY(!detectTakeover(old.paths, settledSnapshot()).blockers.contains("dropIn"));
    }

    // New filters are written after the fix's mark (no tick runs on them) and
    // before its unit starts (the unit reads them for the dry run).
    void settingsFiltersStartAFixAfterTheMark()
    {
        QTemporaryDir dir;
        SettingsFixture s = makeSettingsFixture(dir.path());
        // At the unit's start fix.json exists and the filters are still the
        // old ones: the new bytes wait in the mark for the preparing step.
        const QString systemctl = dir.filePath("systemctl");
        QVERIFY(write(systemctl, QString("#!/bin/sh\necho \"systemctl $*\" >> '%1/calls.log'\n"
                                         "case \"$*\" in *\" start \"*) [ -f '%2/fix.json' ] && cp '%3' '%1/at-start.txt' ;; esac\n"
                                         "exit 0\n")
                                     .arg(dir.path(), s.fix.paths.appState, s.fix.config.filtersFile)
                                     .toUtf8()));
        const QByteArray edited = "- .git/**\n- *.iso\n";
        QVERIFY(write(dir.filePath("new-filters.txt"), edited));
        const auto [code, answer] = settingsCommand(s, {"--filters", dir.filePath("new-filters.txt")});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QVERIFY(answer.value("fixStarted").toBool());
        QCOMPARE(readBytes(dir.filePath("at-start.txt")), QByteArray("- .git/**\n"));
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- .git/**\n"));
        QCOMPARE(readFix(s.fix.paths.appState).step, QString("preparing"));
        const QJsonObject mark = QJsonDocument::fromJson(readBytes(settingsChangeMarkPath(s.fix.paths.appState))).object();
        QCOMPARE(QByteArray::fromBase64(mark.value("previousFilters").toString().toLatin1()), QByteArray("- .git/**\n"));
        QCOMPARE(QByteArray::fromBase64(mark.value("newFilters").toString().toLatin1()), edited);
        QCOMPARE(mark.value("config").toString(), s.fix.paths.config);
        QCOMPARE(mark.value("fix").toInteger(), readFix(s.fix.paths.appState).started.toSecsSinceEpoch());
        QVERIFY(!mark.value("applied").toBool());
    }

    // Review: a run that passed run-condition just before the fix's mark
    // reads the filters with the old .md5; new filters under it would abort
    // it and take the base along. The preparing step writes them only once
    // drive-sync.service is idle.
    void settingsBytesWaitForTheRunningRun()
    {
        QTemporaryDir dir;
        SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("new.txt"), "- *.iso\n"));
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        // drive-sync.service says active twice, then inactive; each time it
        // is active, what the run would read is kept.
        QVERIFY(write(dir.filePath("systemctl"),
                      QString(R"sh(#!/bin/sh
echo "systemctl $*" >> '%1/calls.log'
case "$*" in
  *drive-sync-fix.service*) echo inactive ;;
  *drive-sync.service*)
    if [ -s '%1/service-active' ]; then head -n 1 '%1/service-active'; sed -i 1d '%1/service-active'; cat '%2' >> '%1/read-by-run.txt'; else echo inactive; fi ;;
esac
)sh").arg(dir.path(), s.fix.config.filtersFile).toUtf8()));
        QVERIFY(write(dir.filePath("service-active"), "active\nactive\n"));
        QVERIFY(runToConfirm(s));
        QCOMPARE(readBytes(dir.filePath("read-by-run.txt")), QByteArray("- .git/**\n- .git/**\n"));
        QCOMPARE(*s.fix.waits, (QList<int>{5, 5}));
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- *.iso\n"));
        QVERIFY(QJsonDocument::fromJson(readBytes(settingsChangeMarkPath(s.fix.paths.appState))).object().value("applied").toBool());
        // The dry run read the new filters.
        QVERIFY(calls(dir.path()).filter("--dry-run").join('\n').contains("--filters-file " + s.fix.config.filtersFile));
    }

    void settingsFiltersWithTheSameBytesStartNothing()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("same.txt"), "- .git/**\n"));
        const auto [code, answer] = settingsCommand(s, {"--filters", dir.filePath("same.txt")});
        QCOMPARE(code, 0);
        QCOMPARE(answer.value("fixStarted").toBool(), false);
        QVERIFY(!fixPresent(s.fix.paths.appState));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }

    void settingsRefusesFiltersRcloneCannotRead()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QTemporaryDir dir;
        SettingsFixture s = makeSettingsFixture(dir.path());
        s.takeover.rclone = rclone;
        QVERIFY(write(dir.filePath("bad.txt"), "Photos/**\n"));
        const auto [code, answer] = settingsCommand(s, {"--filters", dir.filePath("bad.txt")});
        QCOMPARE(code, 1);
        QVERIFY2(answer.value("error").toString().contains("malformed rule"), qPrintable(answer.value("error").toString()));
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- .git/**\n"));
        QVERIFY(!fixPresent(s.fix.paths.appState));
        // A good file passes the same check.
        QVERIFY(write(dir.filePath("good.txt"), "- /Photos/**\n- .git/**\n"));
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("good.txt")}).first, 0);
    }

    void settingsRollsBackWhenTheFixCannotStart()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("start-exit"), "1\n"));
        QVERIFY(write(dir.filePath("new.txt"), "- *.iso\n"));
        const auto [code, answer] = settingsCommand(s, {"--filters", dir.filePath("new.txt")});
        QCOMPARE(code, 1);
        QVERIFY(answer.value("error").toString().contains("drive-sync-fix.service"));
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- .git/**\n"));
        QVERIFY(!fixPresent(s.fix.paths.appState));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }

    // Review Focus 1: X at the dry run gives back the base the sync had.
    void cancellingASettingsFixPutsTheOldFiltersBack()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const QString filters = s.fix.config.filtersFile;
        QVERIFY(write(filters + ".md5", QCryptographicHash::hash(readBytes(filters), QCryptographicHash::Md5).toHex()));
        QVERIFY(filtersUsable(filters, nullptr));
        QVERIFY(write(dir.filePath("new.txt"), "- *.iso\n"));
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QVERIFY(runToConfirm(s));
        QVERIFY(!filtersUsable(filters, nullptr));
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(filters), QByteArray("- .git/**\n"));
        QVERIFY(filtersUsable(filters, nullptr));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
        QVERIFY(!fixPresent(s.fix.paths.appState));

        // Cancelled before the preparing step wrote anything: nothing to undo.
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(filters), QByteArray("- .git/**\n"));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));

        // The dry run failed after the bytes were written: they go back too.
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QVERIFY(runToConfirm(s));
        QVERIFY(moveFix(s.fix.paths.appState, "failed", "preparing"));
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(filters), QByteArray("- .git/**\n"));
        QVERIFY(filtersUsable(filters, nullptr));

        // Once a base run started, the new filters are the base's: kept.
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QVERIFY(runToConfirm(s));
        QVERIFY(moveFix(s.fix.paths.appState, "failed", "base"));
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(filters), QByteArray("- *.iso\n"));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }

    // A mark serves the fix that wrote it: one left behind never acts on a
    // later fix, and a fix started without a change clears it.
    void aSettingsMarkServesItsOwnFixOnly()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("new.txt"), "- *.iso\n"));
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QVERIFY(runToConfirm(s));
        const QByteArray mark = readBytes(settingsChangeMarkPath(s.fix.paths.appState));
        // fix.json removed by hand, the mark left behind; a later fix.
        QVERIFY(clearFix(s.fix.paths.appState));
        QVERIFY(write(settingsChangeMarkPath(s.fix.paths.appState), mark));
        FixState later = fixAt("confirm");
        later.started = later.started.addSecs(-7200);
        QVERIFY(writeFix(s.fix.paths.appState, later));
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- *.iso\n"));
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
        // "fix --start" clears a mark left behind before anything reads it.
        QVERIFY(write(settingsChangeMarkPath(s.fix.paths.appState), mark));
        QCOMPARE(fixCommand(s.fix, {"--start"}), QString());
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }

    void fixUnitDropsTheSettingsMarkAfterTheBase()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("new.txt"), "- *.iso\n"));
        QCOMPARE(settingsCommand(s, {"--filters", dir.filePath("new.txt")}).first, 0);
        QVERIFY(QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(cannedListings(dir.path(), listingLine("same.txt", 3, when), listingLine("same.txt", 3, when)));
        QVERIFY(writeFix(s.fix.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.fix.paths, s.fix.config, s.fix.tools, s.fix.notify), 0);
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }

    void settingsMovesTheFolderThroughAFix()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(s.fix.config.localPath + "/mine.txt", "mine\n"));
        const QByteArray configBefore = readBytes(s.fix.paths.config);
        const QString other = dir.filePath("Other/Drive");
        const auto [code, answer] = settingsCommand(s, {"--folder", other});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QVERIFY(answer.value("fixStarted").toBool());
        QString error;
        // The config changes in the preparing step, not before.
        QCOMPARE(readBytes(s.fix.paths.config), configBefore);
        QVERIFY(QFileInfo(other).isDir());
        const QJsonObject mark = QJsonDocument::fromJson(readBytes(settingsChangeMarkPath(s.fix.paths.appState))).object();
        QCOMPARE(QByteArray::fromBase64(mark.value("previousConfig").toString().toLatin1()), configBefore);
        QVERIFY(!mark.contains("previousFilters"));
        QCOMPARE(mark.value("createdFolders").toArray().size(), 2);  // Other and Other/Drive
        QVERIFY(runToConfirm(s));
        QCOMPARE(loadSyncConfig(s.fix.paths.config, &error)->localPath, other);
        // The dry run already compared the new folder.
        QVERIFY(calls(dir.path()).filter("--dry-run").join('\n').contains(" " + other + " "));
        QCOMPARE(readBytes(s.fix.config.localPath + "/mine.txt"), QByteArray("mine\n"));
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- .git/**\n"));
    }

    void settingsRefusesNestedFolders()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const QByteArray before = readBytes(s.fix.paths.config);
        for (const QString &folder : {s.fix.config.localPath + "/Sub", dir.path(), QDir::homePath(),
                                      s.fix.paths.appState + "/Drive", QString("relative/Drive")}) {
            const auto refused = settingsCommand(s, {"--folder", folder});
            QVERIFY2(refused.first == 1, qPrintable(folder));
        }
        QCOMPARE(readBytes(s.fix.paths.config), before);
        QVERIFY(!fixPresent(s.fix.paths.appState));
    }

    void settingsSameFolderStartsNothing()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {"--folder", s.fix.config.localPath + "/"});
        QCOMPARE(code, 0);
        QCOMPARE(answer.value("fixStarted").toBool(), false);
        QVERIFY(!fixPresent(s.fix.paths.appState));
    }

    void cancellingAFolderChangePutsTheOldConfigBack()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const QByteArray before = readBytes(s.fix.paths.config);
        const QString other = dir.filePath("Other");
        QCOMPARE(settingsCommand(s, {"--folder", other}).first, 0);
        QVERIFY(runToConfirm(s));
        QVERIFY(readBytes(s.fix.paths.config) != before);
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(s.fix.paths.config), before);
        QVERIFY(!QFile::exists(other));
        // A file put in the new folder meanwhile keeps it there.
        QCOMPARE(settingsCommand(s, {"--folder", other}).first, 0);
        QVERIFY(write(other + "/new.txt", "new\n"));
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(s.fix.paths.config), before);
        QCOMPARE(readBytes(other + "/new.txt"), QByteArray("new\n"));
    }

    void folderChangeBaseLeavesTheOldFolderAlone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QTemporaryDir dir;
        SettingsFixture s = makeSettingsFixture(dir.path());
        // Local folders as both sides, with real rclone.
        QVERIFY(QDir().mkpath(dir.filePath("cloud/Docs")));
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        QVERIFY(write(dir.filePath("cloud/Docs/two.txt"), "two\n"));
        SyncConfig config = s.fix.config;
        config.remote = dir.filePath("cloud");
        QString error;
        QVERIFY(saveSyncConfig(s.fix.paths.config, config, &error));
        s.fix.tools.rclone = rclone;
        s.takeover.rclone = rclone;
        auto runFix = [&](const QString &what) {
            const SyncConfig now = *loadSyncConfig(s.fix.paths.config, &error);
            QCOMPARE(runFixUnit(s.fix.paths, now, s.fix.tools, s.fix.notify), 0);
            QVERIFY2(readFix(s.fix.paths.appState).step == "confirm", qPrintable(what + ": " + readFix(s.fix.paths.appState).error));
            QCOMPARE(fixCommand(s.fix, {"--confirm"}), QString());
            // A new start of the unit reads the config again, as systemd's does.
            const int unit = runFixUnit(s.fix.paths, *loadSyncConfig(s.fix.paths.config, &error), s.fix.tools, s.fix.notify);
            QVERIFY2(unit == 0 && !fixPresent(s.fix.paths.appState), qPrintable(what + ": " + readFix(s.fix.paths.appState).error));
        };
        QCOMPARE(fixCommand(s.fix, {"--start"}), QString());
        runFix("first base");
        QVERIFY(write(config.localPath + "/local-only.txt", "kept\n"));
        const auto oldFolder = filesUnder(config.localPath);
        const QString other = dir.filePath("Other");
        const auto [code, answer] = settingsCommand(s, {"--folder", other});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        runFix("base of the new folder");
        QCOMPARE(readBytes(other + "/one.txt"), QByteArray("one\n"));
        QCOMPARE(readBytes(other + "/Docs/two.txt"), QByteArray("two\n"));
        QVERIFY(!QFile::exists(other + "/local-only.txt"));
        QCOMPARE(filesUnder(config.localPath), oldFolder);
        QVERIFY(!QFile::exists(settingsChangeMarkPath(s.fix.paths.appState)));
    }
    // Step 4b, selective sync. Ticking back what was left out gives the
    // filters their old bytes: the .md5 matches and no fix is needed.
    void skipBlockRoundTripKeepsTheFileByteForByte()
    {
        const QByteArray original = defaultFilters();
        QString error;
        const auto with = withSkippedFolders(original, {"Work/Old", "Photos"}, &error);
        QVERIFY2(with, qPrintable(error));
        QVERIFY(with->startsWith("# BEGIN DriveSync: cloud folders left out of this computer\n- /Photos/**\n"
                                 "- /Work/Old/**\n# END DriveSync: cloud folders left out of this computer\n\n"));
        QVERIFY(with->endsWith(original));
        QCOMPARE(*skippedFolders(*with, &error), (QStringList{"Photos", "Work/Old"}));
        QCOMPARE(*withSkippedFolders(*with, {}, &error), original);
        // Replaced in place: no duplicates, nothing inside a folder already left out.
        const auto again = withSkippedFolders(*with, {"Work/Old", "Archive", "Work", "Archive"}, &error);
        QVERIFY2(again, qPrintable(error));
        QCOMPARE(*skippedFolders(*again, &error), (QStringList{"Archive", "Work"}));
        QVERIFY(again->endsWith(original));
        QCOMPARE(*skippedFolders(original, &error), QStringList());
    }

    void skipBlockEscapesGlobCharacters()
    {
        QCOMPARE(escapeFilterPath("a[1]"), QString("a\\[1\\]"));
        QCOMPARE(escapeFilterPath("b*c"), QString("b\\*c"));
        QCOMPARE(escapeFilterPath("{x}"), QString("\\{x\\}"));
        QCOMPARE(escapeFilterPath("back\\slash"), QString("back\\\\slash"));
        QCOMPARE(escapeFilterPath("q?mark"), QString("q\\?mark"));
        QCOMPARE(escapeFilterPath("sp ace/ lead"), QString("sp ace/ lead"));
        const QStringList names{" lead", "a[1]", "b*c", "back\\slash", "q?mark", "sp ace", "{x}", "Ünï/sub"};
        QString error;
        const auto filters = withSkippedFolders("- .git/**\n", names, &error);
        QVERIFY2(filters, qPrintable(error));
        QVERIFY(filters->contains("- /a\\[1\\]/**\n"));
        QCOMPARE(*skippedFolders(*filters, &error), names);
    }

    void skipBlockRefusesNewlines()
    {
        QString error;
        for (const QString &bad : {QString("a\nb"), QString("a\rb"), QString(), QString("/Photos"), QString("a//b"),
                                   QString("Photos/")}) {
            QVERIFY2(!withSkippedFolders("- .git/**\n", {bad}, &error), qPrintable(bad));
            QVERIFY(!error.isEmpty());
        }
    }

    void skipBlockEditedByHandIsReported()
    {
        QString error;
        QVERIFY(!skippedFolders("# BEGIN DriveSync: cloud folders left out of this computer\n- /A/**\n", &error));
        QVERIFY(error.contains("END"));
        QVERIFY(!skippedFolders("# BEGIN DriveSync: cloud folders left out of this computer\n+ /A/**\n"
                                "# END DriveSync: cloud folders left out of this computer\n\n",
                                &error));
        QVERIFY2(error.contains("+ /A/**"), qPrintable(error));
        // withSkippedFolders refuses to rewrite a block it cannot read.
        QVERIFY(!withSkippedFolders("# BEGIN DriveSync: cloud folders left out of this computer\n- /A/**\n", {"B"}, &error));
    }

    void togglingFoldersFollowsTheTree()
    {
        QStringList skipped;
        skipped = toggledFolder(skipped, "Work/Old");
        QCOMPARE(skipped, QStringList{"Work/Old"});
        QVERIFY(folderSkipped(skipped, "Work/Old/2019"));
        QVERIFY(!folderTickable(skipped, "Work/Old/2019"));
        QVERIFY(folderTickable(skipped, "Work/Old"));
        // Leaving the parent out takes in the child it already left out.
        skipped = toggledFolder(skipped, "Work");
        QCOMPARE(skipped, QStringList{"Work"});
        QVERIFY(!folderTickable(skipped, "Work/Old"));
        QCOMPARE(toggledFolder(skipped, "Work/Old"), skipped);  // not tickable: no change
        QVERIFY(!folderSkipped(skipped, "Workshop"));
        skipped = toggledFolder(skipped, "Work");
        QCOMPARE(skipped, QStringList());
    }

    void foldersCommandListsOneLevel()
    {
        QTemporaryDir dir;
        SettingsFixture s = makeSettingsFixture(dir.path());
        s.fix.config.rcloneFlags = {"--config", "/x/rclone.conf", "--drive-skip-shortcuts", "--resilient"};
        QString error;
        QVERIFY(saveSyncConfig(s.fix.paths.config, s.fix.config, &error));
        QVERIFY(QDir().mkpath(s.fix.config.localPath + "/Work/Old"));
        QVERIFY(write(dir.filePath("lsjson.json"),
                      R"([{"Path":"Old","Name":"Old","Size":-1,"MimeType":"inode/directory","IsDir":true},)"
                      R"({"Path":"archive","Name":"archive","IsDir":true},{"Path":"a\nb","Name":"a\nb","IsDir":true}])"));
        const QString rclone = fakeFoldersRclone(dir.path());
        const auto [code, answer] = foldersCommand(s.fix.paths, rclone, {"Work"});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        const QJsonArray folders = answer.value("folders").toArray();
        QCOMPARE(folders.size(), 3);
        QCOMPARE(folders.at(0).toObject().value("name").toString(), QString("a\nb"));
        QCOMPARE(folders.at(0).toObject().value("unsupported").toBool(), true);
        QCOMPARE(folders.at(1).toObject().value("path").toString(), QString("Work/archive"));
        QCOMPARE(folders.at(2).toObject().value("path").toString(), QString("Work/Old"));
        QCOMPARE(folders.at(2).toObject().value("local").toBool(), true);
        QCOMPARE(folders.at(1).toObject().value("local").toBool(), false);
        QCOMPARE(rcloneCalls(dir.path()).last(),
                 (QStringList{"lsjson", "gdrive:Work", "--dirs-only", "--config", "/x/rclone.conf", "--drive-skip-shortcuts"}));
        // First setup has no config yet: the remote it names, at the root.
        BackendPaths fresh = s.fix.paths;
        fresh.config = dir.filePath("none.json");
        fresh.managed = false;
        QCOMPARE(foldersCommand(fresh, rclone, {"--remote", "drive-sync:"}).first, 0);
        // With the flags a new sync gets, so the tree shows what will sync.
        QCOMPARE(rcloneCalls(dir.path()).last(),
                 (QStringList{"lsjson", "drive-sync:", "--dirs-only", "--drive-skip-gdocs", "--drive-skip-shortcuts"}));
    }

    void foldersCommandSaysAGoneFolder()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(write(dir.filePath("lsjson-exit"), "3\n"));
        const auto [code, answer] = foldersCommand(s.fix.paths, fakeFoldersRclone(dir.path()), {"Gone"});
        QCOMPARE(code, 1);
        QVERIFY2(answer.value("error").toString().contains("no longer"), qPrintable(answer.value("error").toString()));
    }

    void realRcloneHonoursTheSkipBlock()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QTemporaryDir dir;
        const QStringList names{" lead", "a[1]", "b*c", "back\\slash", "q?mark", "sp ace", "{x}", "Work/Old"};
        for (const QString &name : names + QStringList{"qXmark", "Work"}) {
            QVERIFY(QDir().mkpath(dir.filePath("cloud/" + name)));
            QVERIFY(write(dir.filePath("cloud/" + name + "/f.txt"), "x\n"));
        }
        QVERIFY(write(dir.filePath("cloud/top.txt"), "t\n"));
        QString error;
        const auto filters = withSkippedFolders(defaultFilters(), names, &error);
        QVERIFY2(filters, qPrintable(error));
        QVERIFY(write(dir.filePath("filters.txt"), *filters));
        // A real base run between the "cloud" and an empty folder: only what
        // the block lets through comes down.
        QVERIFY(QDir().mkpath(dir.filePath("local")));
        const QStringList bisync{"bisync", dir.filePath("cloud"), dir.filePath("local"), "--workdir", dir.filePath("work"),
                                 "--filters-file", dir.filePath("filters.txt")};
        QVERIFY2(runProcess(rclone, bisync + QStringList{"--resync", "--resync-mode", "newer"}, 60000, nullptr, &error),
                 qPrintable(error));
        QStringList listed = filesUnder(dir.filePath("local")).keys();
        listed.sort();
        QCOMPARE(listed, (QStringList{"Work/f.txt", "qXmark/f.txt", "top.txt"}));
        // A normal run leaves a folder left out alone on both sides.
        QVERIFY(QDir().mkpath(dir.filePath("local/Work/Old")));
        QVERIFY(write(dir.filePath("local/Work/Old/mine.txt"), "mine\n"));
        QVERIFY2(runProcess(rclone, bisync, 60000, nullptr, &error), qPrintable(error));
        QVERIFY(!QFile::exists(dir.filePath("cloud/Work/Old/mine.txt")));
        QVERIFY(QFile::exists(dir.filePath("cloud/Work/Old/f.txt")));
    }

    void settingsSkipStartsAFixAndCancelRestores()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {"--skip", "Photos", "--skip", "Work/Old"});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QVERIFY(answer.value("fixStarted").toBool());
        QString error;
        QVERIFY(runToConfirm(s));
        QCOMPARE(*skippedFolders(readBytes(s.fix.config.filtersFile), &error), (QStringList{"Photos", "Work/Old"}));
        QCOMPARE(settingsCommand(s, {}).second.value("skipped").toArray().size(), 2);
        QCOMPARE(fixCommand(s.fix, {"--cancel"}), QString());
        QCOMPARE(readBytes(s.fix.config.filtersFile), QByteArray("- .git/**\n"));
        // Nothing left out already: no change, no fix.
        const auto none = settingsCommand(s, {"--skip-none"});
        QCOMPARE(none.first, 0);
        QCOMPARE(none.second.value("fixStarted").toBool(), false);
        QCOMPARE(settingsCommand(s, {"--skip", "a\nb"}).first, 1);
    }

    void setupWritesTheSkippedFolders()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        CommandResult r;
        QTextStream out(&r.out);
        QTextStream err(&r.err);
        r.code = runSetupCommand({"--folder", s.folder, "--skip", "Photos", "--skip", "Old Stuff", "--json"}, s.paths,
                                 s.takeover, out, err);
        out.flush();
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        QString error;
        const QByteArray filters = readBytes(s.takeover.appConfigDir + "/filters.txt");
        QCOMPARE(*skippedFolders(filters, &error), (QStringList{"Old Stuff", "Photos"}));
        QVERIFY(filters.endsWith(defaultFilters()));
    }
    void filterModelRoundTrips()
    {
        FilterModel model;
        model.presets = {"git", "python"};
        model.localPaths = {"Projetos/velho/", "notas soltas.txt"};
        model.nameRules = {{"name", "a*b"}, {"folder", "build"}, {"extension", "log"}, {"path", "Fotos/2019/"}};
        model.advanced = "# mine\n+ /keep/**\n";
        const QByteArray bytes = writeFilterModel(model);
        const FilterModel back = readFilterModel(bytes);
        QCOMPARE(back.presets, model.presets);
        QCOMPARE(back.localPaths, model.localPaths);
        QCOMPARE(back.nameRules, model.nameRules);
        QCOMPARE(back.advanced, model.advanced);
        QVERIFY(bytes.startsWith(model.advanced));
        QVERIFY(bytes.contains("- a\\*b\n"));
        QVERIFY(bytes.contains("- build/**\n"));
        QVERIFY(bytes.contains("- *.log\n"));
        QVERIFY(bytes.contains("- /Fotos/2019/**\n"));
        QVERIFY(bytes.contains("- /notas soltas.txt\n"));
    }
    void filterModelKeepsTextItDidNotWrite()
    {
        // No blocks: everything is advanced, and writing it back changes no byte.
        const QByteArray text = "# BEGIN DriveSync: cloud folders left out of this computer\n- /Fotos/**\n"
                                "# END DriveSync: cloud folders left out of this computer\n\n- *.tmp\n";
        QCOMPARE(readFilterModel(text).advanced, text);
        QCOMPARE(writeFilterModel(readFilterModel(text)), text);
        // A block with a line the screen never writes stays text, whole.
        const QByteArray edited = "# BEGIN DriveSync: rules by name\n+ keep\n# END DriveSync: rules by name\n";
        const FilterModel model = readFilterModel(edited);
        QVERIFY(model.nameRules.isEmpty());
        QCOMPARE(model.advanced, edited);
    }
    void filterModelRefusesUnwritableRules()
    {
        QCOMPARE(nameRuleLine({"name", "a/b"}), QString());
        QCOMPARE(nameRuleLine({"extension", ""}), QString());
        QCOMPARE(nameRuleLine({"folder", "x\ny"}), QString());
        QCOMPARE(nameRuleLine({"extension", ".PDF"}), QString("- *.PDF"));
        QCOMPARE(nameRuleLine({"path", "/Docs/"}), QString("- /Docs/**"));
    }
    void importedFiltersLightUpWhatTheScreenShows()
    {
        QByteArray old = "# old sync\n- .git/**\n- .git\n- *.log\n- /Old/**\n- /GitHub/**/build/**\n- tmp/**\n";
        for (const FilterPreset &preset : filterPresets())
            if (preset.id == "googleDocs")
                old += preset.rules.join('\n').toUtf8() + '\n';
        const FilterModel model = importedFilterModel(old);
        QCOMPARE(model.presets, (QStringList{"git", "googleDocs"}));
        QCOMPARE(model.nameRules, (QList<NameRule>{{"extension", "log"}, {"folder", "tmp"}}));
        QCOMPARE(model.localPaths, QStringList{"Old/"});
        QCOMPARE(model.advanced, QByteArray("# old sync\n- /GitHub/**/build/**\n"));
        // An include anywhere: nothing moves.
        const QByteArray withInclude = "+ /Old/keep\n" + old;
        const FilterModel kept = importedFilterModel(withInclude);
        QVERIFY(kept.presets.isEmpty() && kept.localPaths.isEmpty() && kept.nameRules.isEmpty());
        QCOMPARE(kept.advanced, withInclude);
    }
    void rcloneReadsTheBuiltFilters()
    {
        QTemporaryDir dir;
        for (const char *file : {"keep.txt", "a.log", "build/out.o", "Docs/x.txt", "Docs/y.txt", "deep/build/z.o",
                                 "deep/ok.txt", "solo.txt"}) {
            const QString path = dir.filePath("tree/") + file;
            QVERIFY(QDir().mkpath(QFileInfo(path).path()));
            QVERIFY(write(path, "x"));
        }
        FilterModel model;
        model.localPaths = {"Docs/", "solo.txt"};
        model.nameRules = {{"extension", "log"}, {"folder", "build"}};
        QVERIFY(write(dir.filePath("filters.txt"), writeFilterModel(model)));
        QByteArray listed;
        QString error;
        QVERIFY2(runProcess("rclone", {"lsf", "-R", "--files-only", dir.filePath("tree"), "--filter-from",
                                       dir.filePath("filters.txt")},
                            60000, &listed, &error),
                 qPrintable(error));
        QStringList files = QString::fromUtf8(listed).split('\n', Qt::SkipEmptyParts);
        files.sort();
        QCOMPARE(files, (QStringList{"deep/ok.txt", "keep.txt"}));
    }
    void localTreeLeavesPathsOut()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath("Drive");
        QVERIFY(QDir().mkpath(root + "/Repo/build"));
        QVERIFY(write(root + "/Repo/build/out.o", "x"));
        QVERIFY(write(root + "/Repo/main.c", "x"));
        QVERIFY(write(root + "/notes.txt", "x"));
        QVERIFY(QFile::link(root + "/notes.txt", root + "/link.txt"));
        LocalTree tree;
        tree.reset(root, {"Repo/build/"});
        auto row = [&](const QString &path) {
            for (const QVariant &r : tree.rows())
                if (r.toMap().value("path").toString() == path)
                    return r.toMap();
            return QVariantMap();
        };
        auto index = [&](const QString &path) {
            const QVariantList rows = tree.rows();
            for (int i = 0; i < rows.size(); ++i)
                if (rows.at(i).toMap().value("path").toString() == path)
                    return i;
            return -1;
        };
        // Folders first, then files, each by name.
        QCOMPARE(tree.rows().size(), 3);
        QCOMPARE(tree.rows().first().toMap().value("path").toString(), QString("Repo/"));
        QVERIFY(row("link.txt").value("link").toBool());
        QVERIFY(!tree.dirty());
        tree.expand(index("Repo/"));
        QVERIFY(!row("Repo/build/").value("ticked").toBool());
        QVERIFY(row("Repo/main.c").value("ticked").toBool());
        // Leaving out a folder takes what was left out inside it with it.
        tree.toggle(index("Repo/"));
        QCOMPARE(tree.excluded(), QStringList{"Repo/"});
        QVERIFY(!row("Repo/main.c").value("tickable").toBool());
        tree.toggle(index("link.txt"));
        QCOMPARE(tree.excluded(), (QStringList{"Repo/", "link.txt"}));
        QVERIFY(tree.dirty());
        tree.toggle(index("Repo/"));
        QCOMPARE(tree.excluded(), QStringList{"link.txt"});
        // A folder that is not there yet: no rows, no error.
        tree.reset(dir.filePath("Nope"), {});
        QVERIFY(tree.rows().isEmpty());
        QCOMPARE(tree.error(), QString());
    }
    void filterEditorBuildsTheText()
    {
        FilterEditor editor;
        editor.load(QString::fromUtf8(defaultFilters()));
        QStringList ticked;
        for (const QVariant &p : editor.presets())
            if (p.toMap().value("ticked").toBool())
                ticked << p.toMap().value("id").toString();
        QCOMPARE(ticked, defaultPresetIds());
        QCOMPARE(editor.addNameRule("folder", "build"), QString());
        QVERIFY(!editor.addNameRule("name", "a/b").isEmpty());
        QVERIFY(!editor.addNameRule("folder", "build").isEmpty());  // already there
        QCOMPARE(editor.nameRules().size(), 1);
        editor.setPreset("git", false);
        editor.setAdvanced("# mine\n");
        const FilterModel model = readFilterModel(editor.text({"Old/"}).toUtf8());
        QVERIFY(!model.presets.contains("git"));
        QCOMPARE(model.nameRules, (QList<NameRule>{{"folder", "build"}}));
        QCOMPARE(model.localPaths, QStringList{"Old/"});
        QCOMPARE(model.advanced, QByteArray("# mine\n"));
        editor.removeNameRule(0);
        QVERIFY(editor.nameRules().isEmpty());
        // An old file lights up the groups and lists its rules.
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("old.txt"), "- .git/**\n- .git\n- *.iso\n- /Old/**\n"));
        QSignalSpy loaded(&editor, &FilterEditor::loaded);
        QCOMPARE(editor.importFile(dir.filePath("old.txt")), QString());
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(editor.localPaths(), QStringList{"Old/"});
        QCOMPARE(editor.nameRules().first().toMap().value("value").toString(), QString("iso"));
        QVERIFY(!editor.importFile(dir.filePath("missing.txt")).isEmpty());
    }
    void filterModelIsStableOnTheFilesItMeets()
    {
        // What setup writes, with cloud folders left out on top.
        QString error;
        const QByteArray setupFile = *withSkippedFolders(defaultFilters(), {"Photos"}, &error);
        QCOMPARE(writeFilterModel(readFilterModel(setupFile)), setupFile);
        // An import written once is stable from then on.
        const QByteArray once =
            writeFilterModel(importedFilterModel("# old\n- .git/**\n- .git\n- *.iso\n- /Old/**\n- /x/**/y/**\n"));
        QCOMPARE(writeFilterModel(readFilterModel(once)), once);
        // Text after DriveSync's blocks: they stay text, so nothing moves and a
        // "+" rule written after them keeps losing.
        const QByteArray after = defaultFilters() + "+ /keep/**\n";
        const FilterModel model = readFilterModel(after);
        QVERIFY(model.presets.isEmpty());
        QCOMPARE(model.advanced, after);
        QCOMPARE(writeFilterModel(model), after);
    }
    void localPathsKeepTheirSpaces()
    {
        QTemporaryDir dir;
        for (const char *file : {" lead.txt", "trail.txt ", "two  ", "dir trail /b", " dir lead/a", "keep.txt"}) {
            const QString path = dir.filePath("tree/") + file;
            QVERIFY(QDir().mkpath(QFileInfo(path).path()));
            QVERIFY(write(path, "x"));
        }
        FilterModel model;
        model.localPaths = {" lead.txt", "trail.txt ", "two  ", "dir trail /", " dir lead/"};
        const QByteArray filters = writeFilterModel(model);
        QCOMPARE(readFilterModel(filters).localPaths, model.localPaths);
        QVERIFY(write(dir.filePath("filters.txt"), filters));
        QByteArray listed;
        QString error;
        QVERIFY2(runProcess("rclone", {"lsf", "-R", "--files-only", dir.filePath("tree"), "--filter-from",
                                       dir.filePath("filters.txt")},
                            60000, &listed, &error),
                 qPrintable(error));
        QCOMPARE(QString::fromUtf8(listed), QString("keep.txt\n"));
    }
    void localTreeShowsWhatIsLeftOut()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath("Drive");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(write(root + "/a\nb", "x"));
        QVERIFY(write(root + "/here.txt", "x"));
        LocalTree tree;
        tree.reset(root, {"here.txt", "gone/"});
        // A name with a line break cannot be a rule: shown, never ticked off.
        const QVariantMap odd = tree.rows().first().toMap();
        QCOMPARE(odd.value("path").toString(), QString("a\nb"));
        QVERIFY(odd.value("unsupported").toBool());
        QVERIFY(!odd.value("tickable").toBool());
        tree.toggle(0);
        QCOMPARE(tree.excluded(), (QStringList{"here.txt", "gone/"}));
        // The flat list says which ones are not on this computer, and drops one.
        QVERIFY(tree.exists("here.txt"));
        QVERIFY(!tree.exists("gone/"));
        tree.remove("gone/");
        QCOMPARE(tree.excluded(), QStringList{"here.txt"});
    }
    void filterEditorSeesNoChangeWithoutEdits()
    {
        QString error;
        const QString file = QString::fromUtf8(*withSkippedFolders(defaultFilters(), {"Photos"}, &error));
        FilterEditor editor;
        editor.load(file);
        QVERIFY(!editor.edited(editor.localPaths()));
        QCOMPARE(editor.text(editor.localPaths()), file);
        editor.setPreset("builds", true);
        QVERIFY(editor.edited(editor.localPaths()));
        editor.setPreset("builds", false);
        QVERIFY(!editor.edited(editor.localPaths()));
        QVERIFY(editor.edited({"Old/"}));
    }
    void setupImportsTheFiltersItIsGiven()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        const QByteArray imported = "# from the old sync\n- /GitHub/**/build/**\n- /GitHub/**/venv/**\n";
        QVERIFY(write(dir.filePath("old-filters.txt"), imported));
        CommandResult r;
        QTextStream out(&r.out);
        QTextStream err(&r.err);
        r.code = runSetupCommand({"--folder", s.folder, "--skip", "Photos", "--filters", dir.filePath("old-filters.txt"),
                                  "--json"},
                                 s.paths, s.takeover, out, err);
        out.flush();
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        QString error;
        const QByteArray filters = readBytes(s.takeover.appConfigDir + "/filters.txt");
        QCOMPARE(*skippedFolders(filters, &error), QStringList{"Photos"});
        QVERIFY(filters.endsWith(imported));
        QVERIFY(!filters.contains(defaultFilters()));
    }
    void setupRefusesFiltersRcloneCannotRead()
    {
        QTemporaryDir dir;
        const SetupFixture s = makeSetupFixture(dir.path());
        QVERIFY(write(dir.filePath("lsf-refuses"), ""));
        QVERIFY(write(dir.filePath("old-filters.txt"), "Photos/**\n"));
        CommandResult r;
        QTextStream out(&r.out);
        QTextStream err(&r.err);
        r.code = runSetupCommand({"--folder", s.folder, "--filters", dir.filePath("old-filters.txt"), "--json"}, s.paths,
                                 s.takeover, out, err);
        out.flush();
        QCOMPARE(r.code, 1);
        QVERIFY2(r.out.contains("rclone cannot read these filters: malformed rule"), qPrintable(r.out));
        QVERIFY(!QFile::exists(s.takeover.appConfigDir + "/filters.txt"));
        QVERIFY(!QFile::exists(s.paths.config));
        QVERIFY(!fixPresent(s.paths.appState));
    }
    void setupControllerHandsTheFiltersOver()
    {
        QTemporaryDir dir;
        SetupController controller(fakeSetupDrivesync(dir.path()), dir.filePath("Drive"));
        QSignalSpy started(&controller, &SetupController::setupStarted);
        QVERIFY(write(dir.filePath("signin-answer"), R"({"ok":true,"error":""})"));
        QVERIFY(write(dir.filePath("setup-answer"), R"({"ok":true,"error":""})"));
        controller.signIn();
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        // The text built on screen goes over standard input.
        const QString built = "# mine\n- /GitHub/**/build/**\n";
        controller.start(dir.filePath("Drive"), {}, built);
        QTRY_COMPARE(started.size(), 1);
        QVERIFY(calls(dir.path()).contains("setup --folder " + dir.filePath("Drive") + " --filters - --json"));
        QCOMPARE(readBytes(dir.filePath("setup-stdin")), built.toUtf8());
        // No text: setup's own defaults, and no --filters.
        controller.signIn();
        QTRY_COMPARE(controller.state(), QString("signedIn"));
        controller.start(dir.filePath("Drive"));
        QTRY_COMPARE(started.size(), 2);
        QVERIFY(calls(dir.path()).contains("setup --folder " + dir.filePath("Drive") + " --json"));
    }
    void settingsControllerFollowsTheCommands()
    {
        QTemporaryDir dir;
        const QJsonObject settings{{"ok", true}, {"error", ""}, {"folder", "/h/Drive"}, {"remote", "gdrive:"},
                                   {"intervalMinutes", 30}, {"maxDelete", 5}, {"filtersFile", "/h/f.txt"},
                                   {"filters", "- .git/**\n"}, {"skipped", QJsonArray{"Photos"}}, {"skipError", ""}};
        QVERIFY(write(dir.filePath("settings.json"), QJsonDocument(settings).toJson(QJsonDocument::Compact)));
        auto answerWith = [&](bool ok, const QString &error, bool fixStarted) {
            return write(dir.filePath("change-answer.json"),
                         QJsonDocument(QJsonObject{{"ok", ok}, {"error", error}, {"fixStarted", fixStarted}})
                             .toJson(QJsonDocument::Compact));
        };
        QVERIFY(answerWith(true, "", false));
        SettingsController controller(fakeSettingsDrivesync(dir.path()));
        QSignalSpy fixes(&controller, &SettingsController::fixStarted);
        controller.load();
        QTRY_VERIFY(controller.loaded());
        QCOMPARE(controller.folder(), QString("/h/Drive"));
        QCOMPARE(controller.intervalMinutes(), 30);
        QCOMPARE(controller.maxDelete(), 5);
        QCOMPARE(controller.filters(), QString("- .git/**\n"));
        QCOMPARE(controller.skipped(), QStringList{"Photos"});

        // Two changes in a row run in order, then the settings are read back.
        controller.setInterval(60);
        controller.setMaxDelete(10);
        QTRY_VERIFY(controller.saved());
        QTRY_COMPARE(controller.state(), QString("idle"));
        QTRY_COMPARE(calls(dir.path()).size(), 4);
        QCOMPARE(calls(dir.path()).mid(1), (QStringList{"settings --interval 60 --json", "settings --max-delete 10 --json",
                                                         "settings --json"}));
        QCOMPARE(fixes.size(), 0);

        // The editor's text, byte for byte, through a file.
        QVERIFY(answerWith(true, "", true));
        controller.setFilters("- .git/**\n- *.iso\n");
        QTRY_COMPARE(fixes.size(), 1);
        QCOMPARE(readBytes(dir.filePath("filters-sent.txt")), QByteArray("- .git/**\n- *.iso\n"));
        // On stdin, so the child needs no file of the window's.
        QVERIFY(calls(dir.path()).contains("settings --filters - --json"));
        QVERIFY(!controller.saved());
        controller.setSkipped({});
        QTRY_COMPARE(fixes.size(), 2);
        QVERIFY(calls(dir.path()).contains("settings --skip-none --json"));
        controller.setSkipped({"A b", "C"});
        QTRY_VERIFY(calls(dir.path()).contains("settings --skip A b --skip C --json"));

        // A refusal shows the command's reason and drops what was queued after it.
        QTRY_COMPARE(controller.state(), QString("idle"));
        QVERIFY(answerWith(false, "a fix is under way", false));
        const int before = int(calls(dir.path()).size());
        controller.setFolder("~/Other");
        controller.setInterval(15);
        QTRY_COMPARE(controller.error(), QString("a fix is under way"));
        QTRY_COMPARE(controller.state(), QString("idle"));
        QCOMPARE(calls(dir.path()).mid(before), QStringList{"settings --folder " + QDir::homePath() + "/Other --json"});
    }

    void folderTreeLoadsLevelsAndTicks()
    {
        QTemporaryDir dir;
        fakeSettingsDrivesync(dir.path());
        auto level = [](const QList<QPair<QString, bool>> &folders) {
            QJsonArray list;
            for (const auto &[path, local] : folders)
                list.append(QJsonObject{{"name", path.section('/', -1)}, {"path", path}, {"local", local},
                                        {"unsupported", false}});
            return QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"folders", list}}).toJson(QJsonDocument::Compact);
        };
        QVERIFY(write(dir.filePath("folders-root.json"), level({{"Photos", true}, {"Work", true}})));
        QVERIFY(write(dir.filePath("folders-Work.json"), level({{"Work/Old", false}})));
        FolderTree tree(dir.filePath("drive-sync"));
        tree.reset({"Work"}, "drive-sync:");
        QTRY_COMPARE(tree.rows().size(), 2);
        QVERIFY(calls(dir.path()).contains("folders --remote drive-sync: --json"));
        QVERIFY(tree.rows().at(0).toMap().value("ticked").toBool());
        QVERIFY(!tree.rows().at(1).toMap().value("ticked").toBool());
        tree.expand(1);
        QTRY_COMPARE(tree.rows().size(), 3);
        QVERIFY(calls(dir.path()).contains("folders --remote drive-sync: --json -- Work"));
        const QVariantMap old = tree.rows().at(2).toMap();
        QCOMPARE(old.value("depth").toInt(), 1);
        QCOMPARE(old.value("path").toString(), QString("Work/Old"));
        QVERIFY(!old.value("tickable").toBool());  // its parent is left out
        QVERIFY(!tree.dirty());

        // Work ticked again: its local copy goes back up (Review Focus 2).
        tree.toggle(1);
        QCOMPARE(tree.skipped(), QStringList());
        QCOMPARE(tree.reticked(), QStringList{"Work"});
        QVERIFY(tree.rows().at(2).toMap().value("tickable").toBool());
        // Photos left out after it was synced: its copy stays here (Review Focus 3).
        tree.toggle(0);
        QCOMPARE(tree.newlySkippedLocal(), QStringList{"Photos"});
        QVERIFY(tree.dirty());
        tree.collapse(1);
        QCOMPARE(tree.rows().size(), 2);

        // A listing that names a folder outside the one listed (the folder
        // itself, say) adds nothing: the tree must not nest it in itself.
        QVERIFY(write(dir.filePath("folders-Photos.json"), level({{"Photos", false}, {"Elsewhere/x", false}})));
        tree.expand(0);
        QTRY_VERIFY(calls(dir.path()).contains("folders --remote drive-sync: --json -- Photos"));
        QTRY_VERIFY(!tree.loading());
        QCOMPARE(tree.rows().size(), 2);

        // A level that cannot be listed says why.
        QVERIFY(write(dir.filePath("folders-root.json"),
                      QJsonDocument(QJsonObject{{"ok", false}, {"error", "rclone could not list the cloud: boom"}})
                          .toJson(QJsonDocument::Compact)));
        tree.reset({}, QString());
        QTRY_COMPARE(tree.error(), QString("rclone could not list the cloud: boom"));
        QVERIFY(calls(dir.path()).contains("folders --json"));
    }
    // "drive-sync settings --interval" from the window or a terminal: the
    // window follows the config on disk, as the status command (and so the
    // bar) does on every call, so the two never disagree on "late".
    void backendFollowsAnIntervalChangeOnDisk()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-51 * 60);
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 60).toUtf8()));
        QVERIFY(QDir().mkpath(dir.filePath("Drive")));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.config = dir.filePath("conf/config.json");
        SyncConfig config = newUserConfig(dir.filePath("Drive"), dir.path());
        config.logFile = paths.log;
        QVERIFY(QDir().mkpath(dir.filePath("conf")));
        QString error;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        applySyncConfig(&paths, config);
        Backend backend(paths);
        QTRY_COMPARE(backend.status(), QString("stale"));
        config.intervalMinutes = 30;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        QTRY_COMPARE(backend.status(), QString("ok"));
        config.intervalMinutes = 15;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        QTRY_COMPARE(backend.status(), QString("stale"));
        // A config that goes away (an undo) takes its interval with it.
        config.intervalMinutes = 30;
        QVERIFY(saveSyncConfig(paths.config, config, &error));
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(QFile::remove(paths.config));
        QTRY_VERIFY(!backend.managed());
        QTRY_COMPARE(backend.status(), QString("stale"));
    }
    // Review: a link leads where it points, and the work dir is DriveSync's.
    void settingsRefusesLinksAndTheWorkDir()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QVERIFY(QFile::link(QDir::homePath(), dir.filePath("to-home")));
        QVERIFY(QFile::link(s.fix.config.localPath, dir.filePath("to-drive")));
        QVERIFY(QDir().mkpath(dir.filePath("real/Inside")));
        QVERIFY(QFile::link(s.fix.config.localPath, dir.filePath("real/Inside/drive-link")));
        const QByteArray before = readBytes(s.fix.paths.config);
        for (const QString &folder : {dir.filePath("to-home"), dir.filePath("to-drive"), dir.filePath("to-drive/Sub"),
                                      s.fix.config.workDir, s.fix.config.workDir + "/Drive"}) {
            const auto refused = settingsCommand(s, {"--folder", folder});
            QVERIFY2(refused.first == 1, qPrintable(folder));
        }
        QCOMPARE(readBytes(s.fix.paths.config), before);
        QVERIFY(!fixPresent(s.fix.paths.appState));
        // First setup takes no link to the home either.
        QVERIFY(QDir().mkpath(dir.filePath("setup")));
        const SetupFixture setup = makeSetupFixture(dir.filePath("setup"));
        QVERIFY(!setupRefusal(setup.paths, setup.takeover, dir.filePath("to-home"), setup.takeover.rclone).isEmpty());
    }
    void settingsReadsFiltersFromStdin()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        QByteArray bytes = "- .git/**\n- *.iso\n";
        QBuffer input(&bytes);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto [code, answer] = settingsCommand(s, {"--filters", "-"}, &input);
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        QVERIFY(answer.value("fixStarted").toBool());
        const QJsonObject mark = QJsonDocument::fromJson(readBytes(settingsChangeMarkPath(s.fix.paths.appState))).object();
        QCOMPARE(QByteArray::fromBase64(mark.value("newFilters").toString().toLatin1()), bytes);
    }

    // Values are taken as they are: names that look like options are names.
    void settingsTakesNamesThatLookLikeOptions()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const auto [code, answer] = settingsCommand(s, {"--skip", "--json", "--skip", "--skip-none"});
        QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
        const QJsonObject mark = QJsonDocument::fromJson(readBytes(settingsChangeMarkPath(s.fix.paths.appState))).object();
        QString error;
        QCOMPARE(*skippedFolders(QByteArray::fromBase64(mark.value("newFilters").toString().toLatin1()), &error),
                 (QStringList{"--json", "--skip-none"}));
        // Two changes at once, or a lone value, are refused.
        QCOMPARE(settingsCommand(s, {"--interval", "30", "--max-delete", "10"}).first, 1);
        QCOMPARE(settingsCommand(s, {"--folder"}).first, 1);

        // First setup and the folder listing take them too.
        QVERIFY(QDir().mkpath(dir.filePath("setup")));
        const SetupFixture setup = makeSetupFixture(dir.filePath("setup"));
        CommandResult r;
        QTextStream out(&r.out);
        QTextStream err(&r.err);
        r.code = runSetupCommand({"--folder", setup.folder, "--skip", "--json", "--json"}, setup.paths, setup.takeover, out,
                                 err);
        out.flush();
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        QCOMPARE(*skippedFolders(readBytes(setup.takeover.appConfigDir + "/filters.txt"), &error), QStringList{"--json"});
        QVERIFY(write(dir.filePath("lsjson.json"), "[]"));
        const QString rclone = fakeFoldersRclone(dir.path());
        QCOMPARE(foldersCommand(s.fix.paths, rclone, {"--", "--json"}).first, 0);
        QCOMPARE(rcloneCalls(dir.path()).last().value(1), QString("gdrive:--json"));
        QCOMPARE(foldersCommand(s.fix.paths, rclone, {"--odd"}).first, 1);
    }
    // The window's tree stops a listing with SIGTERM: "drive-sync folders"
    // passes it to its rclone, so no lsjson is left behind.
    void foldersCommandStopsItsRcloneOnSigterm()
    {
        QTemporaryDir dir;
        const SettingsFixture s = makeSettingsFixture(dir.path());
        const QString rclone = dir.filePath("slow-rclone");
        QVERIFY(write(rclone, QString("#!/bin/sh\necho $$ > '%1/rclone.pid'\nexec sleep 10\n").arg(dir.path()).toUtf8()));
        QFile::setPermissions(rclone, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        static std::atomic<bool> caught{false};
        caught = false;
        struct sigaction mine {}, previous {};
        mine.sa_handler = [](int) { caught = true; };
        sigemptyset(&mine.sa_mask);
        QCOMPARE(sigaction(SIGTERM, &mine, &previous), 0);
        const auto restore = qScopeGuard([&previous] { sigaction(SIGTERM, &previous, nullptr); });
        std::thread window([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            ::kill(::getpid(), SIGTERM);
        });
        QElapsedTimer clock;
        clock.start();
        const auto [code, answer] = foldersCommand(s.fix.paths, rclone, {});
        window.join();
        QCOMPARE(code, 1);
        QVERIFY2(clock.elapsed() < 4000, qPrintable(QString::number(clock.elapsed())));
        const qint64 pid = readBytes(dir.filePath("rclone.pid")).trimmed().toLongLong();
        QVERIFY(pid > 0);
        QTRY_VERIFY(processGone(pid));
        // The test's own handler is back.
        struct sigaction now {};
        sigaction(SIGTERM, nullptr, &now);
        QVERIFY(now.sa_handler == mine.sa_handler);
    }
    // Review: the old units sync the folder they always did; after a folder
    // change in Settings, going back to them would need a resync.
    void undoTakeoverRefusesAfterAFolderChange()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        const QString configPath = old.paths.appConfigDir + "/config.json";
        SyncConfig config = *loadSyncConfig(configPath, &error);
        const QString original = config.localPath;
        config.localPath = dir.filePath("Elsewhere");
        QVERIFY(saveSyncConfig(configPath, config, &error));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("folder"), qPrintable(error));
        QVERIFY(QFile::exists(configPath));
        // A takeover made before the manifest named the folder: read from the old unit.
        QDir backups(old.paths.backupRoot);
        const QString manifestPath = backups.filePath(backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot).last() + "/manifest.json");
        QJsonObject manifest = QJsonDocument::fromJson(readBytes(manifestPath)).object();
        QCOMPARE(manifest.value("localPath").toString(), original);
        manifest.remove("localPath");
        QVERIFY(write(manifestPath, QJsonDocument(manifest).toJson()));
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("folder"), qPrintable(error));
        // Back to the old folder, the undo goes through.
        config.localPath = original;
        QVERIFY(saveSyncConfig(configPath, config, &error));
        QVERIFY2(undoTakeover(old.paths, &error), qPrintable(error));
    }
    // Review: the interval drop-in goes before the old units come back, and
    // one that cannot go stops the undo with nothing moved.
    void undoTakeoverStopsWhenTheDropInCannotGo()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        const QString folder = old.paths.userUnits + "/drive-sync.timer.d";
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(write(intervalDropInPath(old.paths.userUnits, "drive-sync.timer"), intervalDropIn(30)));
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto writable = qScopeGuard([&] {
            QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QVERIFY(!undoTakeover(old.paths, &error));
        QVERIFY2(error.contains("drive-sync-interval.conf"), qPrintable(error));
        QVERIFY(!QFile::exists(old.paths.userUnits + "/drive-sync.service"));
        QVERIFY(QFile::exists(intervalDropInPath(old.paths.userUnits, "drive-sync.timer")));
        QVERIFY(QFile::exists(old.paths.appConfigDir + "/config.json"));
    }
    // Review: a block written by hand without the blank line after it, and a
    // file with Windows line ends, keep their layout through the tree.
    void skipBlockKeepsAHandWrittenLayout()
    {
        const QByteArray begin = kSkipBlockBegin;
        const QByteArray end = kSkipBlockEnd;
        QString error;
        const QByteArray hand = begin + "\n- /A/**\n" + end + "\n- .git/**\n";
        const auto two = withSkippedFolders(hand, {"A", "B"}, &error);
        QVERIFY2(two, qPrintable(error));
        QCOMPARE(*withSkippedFolders(*two, {"A"}, &error), hand);

        const QByteArray crlf = begin + "\r\n- /A/**\r\n" + end + "\r\n\r\n- .git/**\r\n";
        QCOMPARE(*skippedFolders(crlf, &error), QStringList{"A"});
        const auto crlfTwo = withSkippedFolders(crlf, {"A", "B"}, &error);
        QVERIFY2(crlfTwo, qPrintable(error));
        QCOMPARE(crlfTwo->count(begin), 1);
        QVERIFY(crlfTwo->contains("- /B/**\r\n"));
        QCOMPARE(*withSkippedFolders(*crlfTwo, {"A"}, &error), crlf);
        const QByteArray plain = "- .git/**\r\n";
        const auto added = withSkippedFolders(plain, {"C"}, &error);
        QVERIFY(added->startsWith(begin + "\r\n- /C/**\r\n"));
        QCOMPARE(*withSkippedFolders(*added, {}, &error), plain);
    }
    // Review: the fix dialog names the folder the config syncs, not ~/Drive.
    void fixDialogNamesTheConfiguredFolder()
    {
        QQmlEngine engine;
        QQmlPropertyMap backend, theme, translator;
        for (const char *key : {"status", "fixStep", "causeKind", "causeDetail", "fixError"})
            backend.insert(key, QString());
        backend.insert("fixCopies", 0);
        backend.insert("fixCopiesToNotebook", 0);
        backend.insert("fixCopyList", QVariantList());
        backend.insert("fixBaseDone", false);
        backend.insert("drivePath", QDir::homePath() + "/Sync/Google");
        theme.insert("radius", 8);
        theme.insert("accent", QColor("#7aa2f7"));
        translator.insert("language", "en");
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        QTest::failOnWarning(QRegularExpression("ReferenceError|TypeError"));
        QQmlComponent component(&engine);
        component.setData(QString("import QtQuick\nimport QtQuick.Controls\nimport \"%1\"\n"
                                  "ApplicationWindow { width: 800; height: 600; visible: true\n"
                                  "  property alias dialog: d\n  FixDialog { id: d } }\n")
                              .arg(QUrl::fromLocalFile(QString(SRCDIR) + "/../src").toString())
                              .toUtf8(),
                          QUrl());
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QObject *text = window->findChild<QObject *>("fixStartText");
        QVERIFY(text);
        QTRY_VERIFY(text->property("text").toString().contains("~/Sync/Google stays in use"));
        QVERIFY(!text->property("text").toString().contains("~/Drive"));
    }

    // Step 5.1: the id is "account" and a number from 1 to 99, nothing else.
    void accountIdsAreStrict()
    {
        for (const QString &id : {QString("account1"), QString("account9"), QString("account10"), QString("account99")})
            QVERIFY2(validAccountId(id), qPrintable(id));
        for (const QString &id : {QString("account0"), QString("account01"), QString("account100"), QString("Account1"),
                                  QString("account1 "), QString(" account1"), QString("acc1"), QString("account"),
                                  QString("account-1"), QString()})
            QVERIFY2(!validAccountId(id), qPrintable(id));
    }

    // Step 5.1: with only the old config, account1 is exactly what
    // BackendPaths::defaults() gave before accounts.
    void legacyModeKeepsTodaysPaths()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        QVERIFY(writeAccountConfig(roots.configRoot + "/config.json", dir.filePath("Sync"), dir.filePath("sync.log"), 30));

        const AccountLayout layout = readAccountLayout(roots.configRoot);
        QCOMPARE(layout.mode, QString("legacy"));
        QCOMPARE(layout.ids, QStringList{"account1"});
        QVERIFY(!layout.staleConfig);

        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.mode, QString("legacy"));
        QVERIFY(set.notices.isEmpty());
        QCOMPARE(set.accounts.size(), 1);
        const BackendPaths &account = set.accounts.first();
        // Before accounts: the defaults with the config's log, folder,
        // interval and managed applied.
        BackendPaths before = roots.base;
        before.config = roots.configRoot + "/config.json";
        before.managed = true;
        before.log = dir.filePath("sync.log");
        before.drive = dir.filePath("Sync");
        before.intervalSeconds = 30 * 60;
        QCOMPARE(account.id, QString("account1"));
        QCOMPARE(account.config, before.config);
        QCOMPARE(account.appState, roots.stateRoot);
        QCOMPARE(account.stateDir, before.stateDir);
        QCOMPARE(account.log, before.log);
        QCOMPARE(account.drive, before.drive);
        QCOMPARE(account.proc, before.proc);
        QCOMPARE(account.systemctl, before.systemctl);
        QCOMPARE(account.intervalSeconds, before.intervalSeconds);
        QVERIFY(account.managed);
        QVERIFY(!account.accountsMode);
        QCOMPARE(account.serviceUnit, QString("drive-sync.service"));
        QCOMPARE(account.timerUnit, QString("drive-sync.timer"));
        QCOMPARE(account.fixUnit, QString("drive-sync-fix.service"));
        QCOMPARE(account.configRoot, roots.configRoot);
        QCOMPARE(account.stateRoot, roots.stateRoot);
        QCOMPARE(account.globalState(), roots.stateRoot);
    }

    // Step 5.1: accounts are the accounts/<id>/ folders named by a valid id
    // that hold a config, by the number in the id.
    void accountsModeListsFoldersWithAConfig()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        const QString accounts = roots.configRoot + "/accounts";
        QVERIFY(writeAccountConfig(accounts + "/account2/config.json", dir.filePath("Drive2"), dir.filePath("two.log")));
        QVERIFY(writeAccountConfig(accounts + "/account10/config.json", dir.filePath("Drive10"), dir.filePath("ten.log")));
        QVERIFY(QDir().mkpath(accounts + "/account3"));
        QVERIFY(writeAccountConfig(accounts + "/foo/config.json", dir.filePath("Foo"), dir.filePath("foo.log")));
        QVERIFY(writeAccountConfig(accounts + "/account02/config.json", dir.filePath("Zero"), dir.filePath("zero.log")));

        const AccountLayout layout = readAccountLayout(roots.configRoot);
        QCOMPARE(layout.mode, QString("accounts"));
        QCOMPARE(layout.ids, (QStringList{"account2", "account10"}));
        QVERIFY(!layout.staleConfig);

        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.mode, QString("accounts"));
        QCOMPARE(set.accounts.size(), 2);
        const BackendPaths &two = set.accounts.at(0);
        QCOMPARE(two.id, QString("account2"));
        QCOMPARE(two.config, accounts + "/account2/config.json");
        QCOMPARE(two.appState, roots.stateRoot + "/accounts/account2");
        QCOMPARE(two.stateDir, QString());
        QCOMPARE(two.log, dir.filePath("two.log"));
        QCOMPARE(two.drive, dir.filePath("Drive2"));
        QCOMPARE(two.serviceUnit, QString("drive-sync@account2.service"));
        QCOMPARE(two.timerUnit, QString("drive-sync@account2.timer"));
        QCOMPARE(two.fixUnit, QString("drive-sync-fix@account2.service"));
        QVERIFY(two.managed);
        QVERIFY(two.accountsMode);
        QCOMPARE(two.globalState(), roots.stateRoot);
        QCOMPARE(set.accounts.at(1).id, QString("account10"));
        QCOMPARE(set.accounts.at(1).serviceUnit, QString("drive-sync@account10.service"));
        // No drive-sync.sh state folder in accounts mode: never a resync.
        QVERIFY(!Backend::resyncRunning(two.stateDir, two.proc));

        // A new account's paths follow the same layout, config or not.
        const BackendPaths three = newAccountPaths(roots, "account3");
        QCOMPARE(three.id, QString("account3"));
        QCOMPARE(three.config, accounts + "/account3/config.json");
        QCOMPARE(three.appState, roots.stateRoot + "/accounts/account3");
        QCOMPARE(three.log, roots.stateRoot + "/accounts/account3/drive-sync.log");
        QCOMPARE(three.timerUnit, QString("drive-sync@account3.timer"));
        QVERIFY(three.accountsMode);

        // With no account at all: the plain view, nothing managed.
        const BackendPaths none = noAccountPaths(roots);
        QCOMPARE(none.id, QString());
        QCOMPARE(none.config, QString());
        QVERIFY(!none.managed);
        QCOMPARE(none.serviceUnit, QString("drive-sync.service"));
        QCOMPARE(none.log, roots.base.log);
        QCOMPARE(none.stateDir, roots.base.stateDir);
        QCOMPARE(none.globalState(), roots.stateRoot);
    }

    // Step 5.1: disconnected/ alone keeps accounts mode, so an old config
    // left behind never comes back as a ghost account1.
    void disconnectedAloneKeepsAccountsMode()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        QVERIFY(writeAccountConfig(roots.configRoot + "/config.json", dir.filePath("Drive"), dir.filePath("old.log")));
        QVERIFY(QDir().mkpath(roots.configRoot + "/disconnected"));

        const AccountLayout layout = readAccountLayout(roots.configRoot);
        QCOMPARE(layout.mode, QString("accounts"));
        QVERIFY(layout.ids.isEmpty());
        QVERIFY(layout.staleConfig);
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.mode, QString("accounts"));
        QVERIFY(set.accounts.isEmpty());
        QCOMPARE(set.notices.size(), 1);
        QCOMPARE(set.notices.first().kind, QString("staleConfig"));
        QCOMPARE(set.notices.first().path, roots.configRoot + "/config.json");

        // Nothing configured at all is accounts mode too, with no notice.
        QTemporaryDir empty;
        const AccountSet nothing = resolveAccounts(accountRoots(empty.path()));
        QCOMPARE(nothing.mode, QString("accounts"));
        QVERIFY(nothing.accounts.isEmpty());
        QVERIFY(nothing.notices.isEmpty());
    }

    // Step 5.1: a broken config never drops its account.
    void unreadableConfigIsListed()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        QVERIFY(QDir().mkpath(roots.configRoot + "/accounts/account1"));
        QVERIFY(write(roots.configRoot + "/accounts/account1/config.json", "{broken"));
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.accounts.size(), 1);
        const BackendPaths &account = set.accounts.first();
        QCOMPARE(account.id, QString("account1"));
        QVERIFY(account.managed);
        QCOMPARE(account.log, roots.stateRoot + "/accounts/account1/drive-sync.log");
        QCOMPARE(account.drive, roots.base.drive);
        QCOMPARE(account.intervalSeconds, kDefaultIntervalMinutes * 60);

        // The same in legacy mode: the old log and folder.
        QTemporaryDir legacyDir;
        const AccountRoots legacyRoots = accountRoots(legacyDir.path());
        QVERIFY(QDir().mkpath(legacyRoots.configRoot));
        QVERIFY(write(legacyRoots.configRoot + "/config.json", "{broken"));
        const AccountSet legacy = resolveAccounts(legacyRoots);
        QCOMPARE(legacy.mode, QString("legacy"));
        QCOMPARE(legacy.accounts.size(), 1);
        QVERIFY(legacy.accounts.first().managed);
        QCOMPARE(legacy.accounts.first().log, legacyRoots.base.log);
    }

    // Step 5.1: a journal marked stuck, or one nobody holds the lock for,
    // is a move that stopped halfway.
    void stuckJournalIsNoticed()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        QVERIFY(writeAccountConfig(roots.configRoot + "/config.json", dir.filePath("Drive"), dir.filePath("old.log")));
        QVERIFY(QDir().mkpath(roots.stateRoot));
        const QString journal = migrationJournalPath(roots.stateRoot);
        QCOMPARE(journal, roots.stateRoot + "/migration.json");
        QVERIFY(!moveInProgress(roots.stateRoot));

        QVERIFY(write(journal, QJsonDocument(QJsonObject{{"version", 1}, {"stuck", true}}).toJson()));
        QVERIFY(moveInProgress(roots.stateRoot));
        AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.notices.size(), 1);
        QCOMPARE(set.notices.first().kind, QString("moveStuck"));
        QCOMPARE(set.notices.first().path, journal);

        // Not marked, but no live drive-sync holds the lock: stuck too.
        QVERIFY(write(journal, QJsonDocument(QJsonObject{{"version", 1}}).toJson()));
        set = resolveAccounts(roots);
        QCOMPARE(set.notices.size(), 1);
        QCOMPARE(set.notices.first().kind, QString("moveStuck"));

        // A drive-sync that holds the lock is moving right now: no notice.
        QVERIFY(fakeProcess(roots.base.proc, 4242, "drive-sync"));
        QVERIFY(write(roots.stateRoot + "/takeover-running", "4242\n"));
        set = resolveAccounts(roots);
        QVERIFY(set.notices.isEmpty());
    }

    // Step 5.1: an account's label is its folder's name, or its path under
    // the home when two folders share a name.
    void labelsUseTheFolderName()
    {
        auto account = [](const QString &id, const QString &drive) {
            BackendPaths paths;
            paths.id = id;
            paths.drive = drive;
            return paths;
        };
        const QList<BackendPaths> distinct{account("account1", "/h/Drive"), account("account2", "/h/Drive2/")};
        QCOMPARE(accountLabel(distinct.at(0), distinct, "/h"), QString("Drive"));
        QCOMPARE(accountLabel(distinct.at(1), distinct, "/h"), QString("Drive2"));
        const QList<BackendPaths> same{account("account1", "/h/a/Drive"), account("account2", "/h/b/Drive"),
                                       account("account3", "/mnt/c/Drive")};
        QCOMPARE(accountLabel(same.at(0), same, "/h"), QString("a/Drive"));
        QCOMPARE(accountLabel(same.at(1), same, "/h"), QString("b/Drive"));
        QCOMPARE(accountLabel(same.at(2), same, "/h"), QString("/mnt/c/Drive"));
        QCOMPARE(accountLabel(distinct.at(0), {distinct.at(0)}, "/h"), QString("Drive"));
    }

    // Step 5.2: the resolver names each account once, so the cards, the
    // JSON, the status text and the window read one label.
    void resolverLabelsEveryAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.home = dir.path();
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", QString()));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", QString()));
        AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.accounts.size(), 2);
        QCOMPARE(set.accounts.at(0).label, QString("Drive"));
        QCOMPARE(set.accounts.at(1).label, QString("Drive2"));

        // accounts --json reads the same labels.
        QString jsonText;
        QString errText;
        {
            QTextStream out(&jsonText);
            QTextStream err(&errText);
            QCOMPARE(runAccountsCommand({"--json"}, roots, TakeoverPaths{}, out, err), 0);
        }
        const QJsonArray listed = QJsonDocument::fromJson(jsonText.toUtf8()).object().value("accounts").toArray();
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed.at(0).toObject().value("label").toString(), QString("Drive"));
        QCOMPARE(listed.at(1).toObject().value("label").toString(), QString("Drive2"));

        // Two folders with one name: their paths under the home.
        QTemporaryDir same;
        AccountRoots sameRoots = accountRoots(same.path());
        sameRoots.home = same.path();
        QVERIFY(makeLabelledAccount(sameRoots, same.path(), "account1", "a/Drive", QString()));
        QVERIFY(makeLabelledAccount(sameRoots, same.path(), "account2", "b/Drive", QString()));
        set = resolveAccounts(sameRoots);
        QCOMPARE(set.accounts.size(), 2);
        QCOMPARE(set.accounts.at(0).label, QString("a/Drive"));
        QCOMPARE(set.accounts.at(1).label, QString("b/Drive"));
        jsonText.clear();
        {
            QTextStream out(&jsonText);
            QTextStream err(&errText);
            QCOMPARE(runAccountsCommand({"--json"}, sameRoots, TakeoverPaths{}, out, err), 0);
        }
        const QJsonArray sameListed = QJsonDocument::fromJson(jsonText.toUtf8()).object().value("accounts").toArray();
        QCOMPARE(sameListed.at(0).toObject().value("label").toString(), QString("a/Drive"));
        QCOMPARE(sameListed.at(1).toObject().value("label").toString(), QString("b/Drive"));

        // Legacy mode: account1 is named by its folder.
        QTemporaryDir legacy;
        AccountRoots legacyRoots = accountRoots(legacy.path());
        legacyRoots.home = legacy.path();
        QVERIFY(QDir().mkpath(legacy.filePath("Mirror")));
        QVERIFY(writeAccountConfig(legacyRoots.configRoot + "/config.json", legacy.filePath("Mirror"),
                                   legacy.filePath("drive-sync.log")));
        set = resolveAccounts(legacyRoots);
        QCOMPARE(set.mode, QString("legacy"));
        QCOMPARE(set.accounts.size(), 1);
        QCOMPARE(set.accounts.at(0).label, QString("Mirror"));

        // The view with no account and a new account: the base folder's name.
        QTemporaryDir none;
        AccountRoots noneRoots = accountRoots(none.path());
        noneRoots.home = none.path();
        QCOMPARE(noAccountPaths(noneRoots).label, QString("Drive"));
        QCOMPARE(newAccountPaths(noneRoots, "account1").label, QString("Drive"));
    }

    // Step 5.2: the window lists every account the resolver does, in order,
    // with the notices; with none, the view with no account.
    void windowAccountsListsEveryAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.home = dir.path();
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", QString()));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", QString()));
        QVERIFY(writeAccountConfig(roots.configRoot + "/config.json", dir.filePath("Drive"),
                                   dir.filePath("drive-sync.log")));
        const WindowAccounts window = windowAccounts(roots);
        QCOMPARE(window.accounts.size(), 2);
        QCOMPARE(window.accounts.at(0).id, QString("account1"));
        QCOMPARE(window.accounts.at(0).label, QString("Drive"));
        QCOMPARE(window.accounts.at(1).id, QString("account2"));
        QCOMPARE(window.accounts.at(1).label, QString("Drive2"));
        QCOMPARE(window.notices.size(), 1);
        QCOMPARE(window.notices.at(0).kind, QString("staleConfig"));
        // 5.1's callers still get the first one.
        QCOMPARE(windowAccount(roots).paths.id, QString("account1"));

        QTemporaryDir empty;
        const WindowAccounts nothing = windowAccounts(accountRoots(empty.path()));
        QCOMPARE(nothing.accounts.size(), 1);
        QVERIFY(nothing.accounts.at(0).id.isEmpty());
        QVERIFY(!nothing.accounts.at(0).managed);
        QVERIFY(nothing.notices.isEmpty());
    }

    // Step 5.2: drive-sync open --account <id> opens the window on a listed
    // account; Qt's own options leave the choice to the window.
    void openTakesAListedAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", QString()));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", QString()));
        const AccountSet set = resolveAccounts(roots);

        OpenChoice choice = chooseOpenAccount({"--account", "account2"}, set);
        QCOMPARE(choice.id, QString("account2"));
        QVERIFY(choice.error.isEmpty());

        choice = chooseOpenAccount({"--account", "account9"}, set);
        QVERIFY(choice.id.isEmpty());
        QCOMPARE(choice.error, QString("no account account9; accounts: account1, account2"));

        choice = chooseOpenAccount({"--account"}, set);
        QVERIFY(choice.id.isEmpty());
        QCOMPARE(choice.error, QString("usage: drive-sync open [--account <id>]"));

        choice = chooseOpenAccount({"-platform", "offscreen"}, set);
        QVERIFY(choice.id.isEmpty());
        QVERIFY(choice.error.isEmpty());

        choice = chooseOpenAccount({}, AccountSet{});
        QVERIFY(choice.id.isEmpty());
        QVERIFY(choice.error.isEmpty());
        choice = chooseOpenAccount({"--account", "account1"}, AccountSet{});
        QCOMPARE(choice.error, QString("no account account1; accounts: none"));

        QTemporaryDir legacy;
        AccountRoots legacyRoots = accountRoots(legacy.path());
        QVERIFY(writeAccountConfig(legacyRoots.configRoot + "/config.json", legacy.filePath("Drive"),
                                   legacy.filePath("drive-sync.log")));
        choice = chooseOpenAccount({"--account", "account1"}, resolveAccounts(legacyRoots));
        QCOMPARE(choice.id, QString("account1"));
        QVERIFY(choice.error.isEmpty());
    }

    // Step 5.1: the takeover lock is one for every account, in the state
    // root, never in an account's own state folder.
    void takeoverLockLivesInTheStateRoot()
    {
        QTemporaryDir dir;
        const AccountRoots roots = accountRoots(dir.path());
        QVERIFY(writeAccountConfig(roots.configRoot + "/accounts/account1/config.json", dir.filePath("Drive"),
                                   dir.filePath("one.log")));
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.accounts.size(), 1);
        const BackendPaths paths = set.accounts.first();
        QVERIFY(paths.appState != roots.stateRoot);
        QVERIFY(fakeProcess(roots.base.proc, 4242, "drive-sync"));
        QVERIFY(QDir().mkpath(roots.stateRoot));
        QVERIFY(write(roots.stateRoot + "/takeover-running", "4242\n"));
        QString output;
        QString errors;
        QTextStream out(&output);
        QTextStream err(&errors);
        QCOMPARE(runSyncCommand({"--json"}, {paths}, out, err), 1);
        QVERIFY2(output.contains("a takeover is running"), qPrintable(output));
        QVERIFY(!calls(dir.path()).join('\n').contains(" start "));
    }

    // Step 5.1: the show call names the account's three units, Id first.
    void showArgumentsNameTheAccountUnits()
    {
        const BackendPaths account = newAccountPaths(AccountRoots(), "account1");
        QCOMPARE(accountUnits(account), (QStringList{"drive-sync@account1.service", "drive-sync@account1.timer",
                                                     "drive-sync-fix@account1.service"}));
        const QStringList arguments = systemdShowArguments(accountUnits(account));
        QCOMPARE(arguments.mid(0, 6), (QStringList{"--user", "show", "--timestamp=unix", "drive-sync@account1.service",
                                                   "drive-sync@account1.timer", "drive-sync-fix@account1.service"}));
        QCOMPARE(arguments.value(6), QString("-p"));
        QVERIFY2(arguments.value(7).startsWith("Id,"), qPrintable(arguments.value(7)));
        QCOMPARE(arguments.size(), 8);
        QCOMPARE(listTimersArguments({"drive-sync@account1.timer"}),
                 (QStringList{"--user", "list-timers", "--all", "-o", "json", "drive-sync@account1.timer"}));
    }

    // Step 5.1: systemd's answers are mapped by Id, never by position, and an
    // account needs all three of its units in the answer.
    void parseShowMapsById()
    {
        const auto units = Backend::parseShowById("Id=drive-sync-fix@account1.service\nActiveState=activating\n\n"
                                                  "Id=drive-sync@account1.timer\nActiveState=active\n\n"
                                                  "LoadState=loaded\nActiveState=failed\n\n"
                                                  "Id=drive-sync@account1.service\nActiveState=inactive\n");
        QCOMPARE(units.size(), 3);
        QCOMPARE(units.value("drive-sync@account1.service").value("ActiveState"), QString("inactive"));
        QCOMPARE(units.value("drive-sync@account1.timer").value("ActiveState"), QString("active"));
        QCOMPARE(units.value("drive-sync-fix@account1.service").value("ActiveState"), QString("activating"));

        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        BackendPaths paths = withAccountUnits(controlPaths(dir.path(), true), "account1");
        paths.systemctl = fakeSystemctl(dir.path(), now.addSecs(-100), "inactive", "activating");
        SnapshotSources src = gatherSources(paths, now, 5000, nullptr);
        QVERIFY(src.systemctlWorks);
        QCOMPARE(src.service.value("Id"), QString("drive-sync@account1.service"));
        QCOMPARE(src.timer.value("Id"), QString("drive-sync@account1.timer"));
        QVERIFY(src.fixUnitActive);

        // Two of the three units answered: systemd cannot be read.
        const QString two = dir.filePath("two-units");
        QVERIFY(write(two, "#!/bin/sh\nprintf 'Id=drive-sync@account1.timer\\nActiveState=active\\n\\n"
                           "Id=drive-sync@account1.service\\nLoadState=loaded\\n'\n"));
        QFile::setPermissions(two, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        paths.systemctl = two;
        src = gatherSources(paths, now, 5000, nullptr);
        QVERIFY(!src.systemctlWorks);
        QCOMPARE(buildSnapshot(src).summary.status, QString("unavailable"));
    }

    // Step 5.1: the next run is the account's own timer's.
    void nextElapsePicksTheTimer()
    {
        const QByteArray json = R"([{"next":1794076998808087,"unit":"drive-sync@account2.timer"},)"
                                R"({"next":1794077998808087,"unit":"drive-sync@account1.timer"}])";
        QCOMPARE(Backend::parseNextElapse(json, "drive-sync@account1.timer").toMSecsSinceEpoch(), 1794077998808LL);
        QCOMPARE(Backend::parseNextElapse(json, "drive-sync@account2.timer").toMSecsSinceEpoch(), 1794076998808LL);
        QVERIFY(!Backend::parseNextElapse(json, "drive-sync.timer").isValid());

        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        BackendPaths paths = withAccountUnits(controlPaths(dir.path(), true), "account1");
        paths.systemctl = fakeSystemctl(dir.path(), now.addSecs(-100));
        QDateTime next;
        gatherSources(paths, now, 5000, &next);
        QVERIFY(next.isValid());
        QVERIFY(next > now);
    }

    // Step 5.1: every command acts on the account's own units.
    void commandsNameTheAccountUnits()
    {
        {
            QTemporaryDir dir;
            const BackendPaths paths = withAccountUnits(controlPaths(dir.path(), true), "account1");
            QString output;
            QString errors;
            QTextStream out(&output);
            QTextStream err(&errors);
            QCOMPARE(runSyncCommand({"--json"}, {paths}, out, err), 0);
            QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync@account1.service"});
        }
        {
            QTemporaryDir dir;
            FixSetup s = makeFixSetup(dir.path());
            s.paths = withAccountUnits(s.paths, "account1");
            QCOMPARE(fixCommand(s, {"--start"}), QString());
            const QStringList seen = calls(dir.path());
            QVERIFY2(seen.contains("systemctl --user start --no-block drive-sync-fix@account1.service"),
                     qPrintable(seen.join('\n')));
            QVERIFY(!seen.join('\n').contains("drive-sync-fix.service"));
            QVERIFY(write(dir.filePath("start-exit"), "1\n"));
            QVERIFY(clearFix(s.paths.appState));
            QVERIFY(fixCommand(s, {"--start"}).contains("drive-sync-fix@account1.service"));
        }
        {
            QTemporaryDir dir;
            SettingsFixture s = makeSettingsFixture(dir.path());
            s.fix.paths = withAccountUnits(s.fix.paths, "account1");
            const auto [code, answer] = settingsCommand(s, {"--interval", "30"});
            QVERIFY2(code == 0, qPrintable(answer.value("error").toString()));
            const QString dropIn = dir.filePath("units/drive-sync@account1.timer.d/drive-sync-interval.conf");
            QCOMPARE(intervalDropInPath(s.takeover.userUnits, "drive-sync@account1.timer"), dropIn);
            QCOMPARE(readBytes(dropIn), intervalDropIn(30));
            QVERIFY(!QFile::exists(dir.filePath("units/drive-sync.timer.d")));
            QVERIFY2(calls(dir.path()).contains("systemctl --user try-restart drive-sync@account1.timer"),
                     qPrintable(calls(dir.path()).join('\n')));
        }
        {
            QTemporaryDir dir;
            SyncConfig config = oldUnitConfig();
            config.runOnlyWhen.clear();
            BackendPaths paths = controlPaths(dir.path(), true);
            paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
            QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc(), true), 0);
            const QStringList sent = notifications(dir.path());
            QCOMPARE(sent.size(), 1);
            QVERIFY2(sent.at(0).contains("drive-sync@account1.timer is inactive"), qPrintable(sent.at(0)));
            QVERIFY2(sent.at(0).contains("systemctl --user enable --now drive-sync@account1.timer"), qPrintable(sent.at(0)));
        }
    }

    // Step 5.1: a config that cannot be read is said right after systemd
    // itself, before anything the log says.
    void configErrorComesRightAfterUnavailable()
    {
        SyncInputs in = healthy();
        in.configError = "/x/config.json is not a JSON object: illegal value";
        in.lastOutcome = "failed";
        in.lastCauseKind = "lostListings";
        SyncSummary s = Backend::summarize(in);
        QCOMPARE(s.status, QString("configError"));
        QCOMPARE(s.causeDetail, in.configError);
        in.fixStep = "failed";
        QCOMPARE(Backend::summarize(in).status, QString("configError"));
        in.systemctlWorks = false;
        QCOMPARE(Backend::summarize(in).status, QString("unavailable"));
        in.systemctlWorks = true;
        in.serviceLoad = "not-found";
        QCOMPARE(Backend::summarize(in).status, QString("configError"));
    }

    void configErrorIsAnError() { QCOMPARE(toneFor("configError"), QString("error")); }

    // Step 5.1: with several accounts, --json is the worst one's version 2
    // object: error, then warning, then busy, then ok; a tie, the first.
    void statusPicksTheWorstAccount()
    {
        const QDateTime now = QDateTime::currentDateTime();
        const QString good = successfulRun(now.addSecs(-400), 300);
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            QVERIFY(makeStatusAccount(roots, dir.path(), "account1", good));
            QVERIFY(makeStatusAccount(roots, dir.path(), "account2", QString()));  // no run yet: a warning
            QVERIFY(makeStatusAccount(roots, dir.path(), "account3", good, "{broken"));
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
            QCOMPARE(accounts.size(), 3);
            const StatusAnswer answer = statusOf({"--json"}, accounts);
            QCOMPARE(answer.code, 0);
            const QJsonObject object = QJsonDocument::fromJson(answer.out.toUtf8()).object();
            QCOMPARE(object.keys().size(), 20);
            QCOMPARE(object.value("version").toInt(), 2);
            QCOMPARE(object.value("status").toString(), QString("configError"));
            QCOMPARE(object.value("tone").toString(), QString("error"));
            QVERIFY2(object.value("causeDetail").toString().contains("account3"),
                     qPrintable(object.value("causeDetail").toString()));
            const QStringList text = statusOf({}, accounts).out.split('\n', Qt::SkipEmptyParts);
            // Step 5.2: a block per account, headed by its label (the
            // folders share a name and the roots no home: absolute paths).
            QCOMPARE(text, (QStringList{dir.filePath("account1/Drive") + " (account1)", "  ok",
                                        dir.filePath("account2/Drive") + " (account2)", "  noRuns",
                                        dir.filePath("Drive") + " (account3)", "  configError"}));
        }
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            QVERIFY(makeStatusAccount(roots, dir.path(), "account1", good));
            QVERIFY(makeStatusAccount(roots, dir.path(), "account2", good + openRun(now.addSecs(-5))));
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
            const QJsonObject object = QJsonDocument::fromJson(statusOf({"--json"}, accounts).out.toUtf8()).object();
            QCOMPARE(object.value("status").toString(), QString("running"));
            QCOMPARE(object.value("tone").toString(), QString("busy"));
        }
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            QVERIFY(makeStatusAccount(roots, dir.path(), "account1", good, "{first"));
            QVERIFY(makeStatusAccount(roots, dir.path(), "account2", good, "{second"));
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
            const QJsonObject object = QJsonDocument::fromJson(statusOf({"--json"}, accounts).out.toUtf8()).object();
            QVERIFY2(object.value("causeDetail").toString().contains("account1/"),
                     qPrintable(object.value("causeDetail").toString()));
        }
        Snapshot ok, warning, busy, error;
        ok.tone = "ok";
        warning.tone = "warning";
        busy.tone = "busy";
        error.tone = "error";
        QCOMPARE(worstAccount({ok, busy, warning}), 2);
        QCOMPARE(worstAccount({ok, busy}), 1);
        QCOMPARE(worstAccount({error, ok, error}), 0);
        QCOMPARE(worstAccount({ok}), 0);
    }

    // Step 5.1: one account prints exactly the line it printed before.
    void statusTextKeepsOneLineForOneAccount()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", successfulRun(now.addSecs(-400), 300)));
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        QVERIFY(write(dir.filePath("account1/Drive/a.txt.conflict1"), "x"));
        roots.base.find = QStandardPaths::findExecutable("find");
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        const StatusAnswer answer = statusOf({}, accounts);
        QCOMPARE(answer.code, 0);
        QCOMPARE(answer.out, QString("conflicts\n"));
        QCOMPARE(answer.err, QString());
    }

    // Step 5.1: several accounts print one line each, by id. Step 5.2: one
    // block each, headed by the label and the id.
    void statusTextListsAccounts()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(makeStatusAccount(roots, dir.path(), "account2", failedRun(now.addSecs(-200))));
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        const StatusAnswer answer = statusOf({}, resolveAccounts(roots).accounts);
        QCOMPARE(answer.code, 0);
        QCOMPARE(answer.out, dir.filePath("account1/Drive") + " (account1)\n  ok\n" + dir.filePath("account2/Drive")
                                 + " (account2)\n  failed\n");
    }

    // Step 5.1: notices go to stderr, one line each, and leave the JSON as it is.
    void statusPrintsNotices()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", successfulRun(now.addSecs(-400), 300)));
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        const QList<AccountNotice> notices{{"staleConfig", roots.configRoot + "/config.json"},
                                           {"moveStuck", roots.stateRoot + "/migration.json"}};
        const StatusAnswer plain = statusOf({"--json"}, accounts);
        const StatusAnswer noticed = statusOf({"--json"}, accounts, notices);
        QCOMPARE(noticed.code, 0);
        QJsonObject a = QJsonDocument::fromJson(plain.out.toUtf8()).object();
        QJsonObject b = QJsonDocument::fromJson(noticed.out.toUtf8()).object();
        a.remove("now");
        b.remove("now");
        QCOMPARE(b, a);
        QCOMPARE(noticed.err.split('\n', Qt::SkipEmptyParts),
                 (QStringList{"drive-sync: " + roots.configRoot
                                  + "/config.json is ignored: DriveSync keeps each account's config under accounts/",
                              "drive-sync: the move to accounts stopped halfway; clear what stuckReason in "
                                  + roots.stateRoot + "/migration.json names, then run: drive-sync accounts --migrate"}));
        QCOMPARE(plain.err, QString());
    }

    // Step 5.1: every account is read with one show call and one list-timers.
    void statusMakesOneShowCall()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(makeStatusAccount(roots, dir.path(), "account2", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(QDir().mkpath(dir.filePath("answers")));
        const QString answers = fakeSystemctl(dir.filePath("answers"), now.addSecs(-400));
        const QString recording = dir.filePath("systemctl");
        QVERIFY(write(recording, QString("#!/bin/sh\necho \"systemctl $*\" >> '%1/calls.log'\nexec '%2' \"$@\"\n")
                                     .arg(dir.path(), answers)
                                     .toUtf8()));
        QFile::setPermissions(recording, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        roots.base.systemctl = recording;
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        QList<QDateTime> nextRuns;
        const QList<SnapshotSources> sources = gatherAccounts(accounts, now.toUTC(), 5000, &nextRuns);
        QCOMPARE(sources.size(), 2);
        QCOMPARE(nextRuns.size(), 2);
        QVERIFY(nextRuns.at(1).isValid());
        QVERIFY(sources.at(0).systemctlWorks && sources.at(1).systemctlWorks);
        QCOMPARE(sources.at(1).service.value("Id"), QString("drive-sync@account2.service"));
        const QStringList seen = calls(dir.path());
        QCOMPARE(seen.filter(" show ").size(), 1);
        QCOMPARE(seen.filter("list-timers").size(), 1);
        QVERIFY2(seen.filter(" show ").first().contains("drive-sync-fix@account2.service"), qPrintable(seen.join('\n')));
        QVERIFY(seen.filter("list-timers").first().contains("drive-sync@account1.timer drive-sync@account2.timer"));
        QCOMPARE(statusOf({"--json"}, accounts).code, 0);
        QCOMPARE(calls(dir.path()).filter(" show ").size(), 2);
    }

    // Step 5.2: plain --json stays version 2, byte for byte, for the old
    // widget. The literal is what the 5.1 code printed for this snapshot.
    void versionTwoIsUnchanged()
    {
        const QDateTime base(QDate(2026, 11, 20), QTime(12, 0), QTimeZone::UTC);
        Snapshot s;
        s.summary.status = "failed";
        s.summary.causeKind = "maxDelete";
        s.summary.causeDetail = "12 deletions";
        s.summary.warnings = {"conflicts", "timerDisabled"};
        s.tone = "error";
        s.running = false;
        s.resync = true;
        s.canSyncNow = true;
        s.currentRun = QVariantMap{{"start", base.addSecs(-30)}, {"phase", "Synching"}};
        s.lastSuccess = base.addSecs(-3600);
        s.lastRunStart = base.addSecs(-900);
        s.lastRunEnd = base.addSecs(-840);
        s.lastRunOutcome = "failed";
        s.failingSince = base.addSecs(-1800);
        s.typicalSeconds = 42;
        s.conflicts = 3;
        s.managed = true;
        s.paused = true;
        s.pausedUntil = base.addSecs(7200);
        const QString expected =
            "{\"canSyncNow\":true,\"causeDetail\":\"12 deletions\",\"causeKind\":\"maxDelete\","
            "\"conflicts\":3,\"currentRun\":{\"phase\":\"Synching\",\"start\":1795175970},"
            "\"failingSince\":1795174200,\"lastRun\":{\"end\":1795175160,\"outcome\":\"failed\","
            "\"start\":1795175100},\"lastSuccess\":1795172400,\"managed\":true,\"nextRun\":1795176600,"
            "\"now\":1795176000,\"paused\":true,\"pausedUntil\":1795183200,\"resync\":true,"
            "\"running\":false,\"status\":\"failed\",\"tone\":\"error\",\"typicalSeconds\":42,"
            "\"version\":2,\"warnings\":[\"conflicts\",\"timerDisabled\"]}";
        QCOMPARE(QString::fromUtf8(QJsonDocument(snapshotToJson(s, base.addSecs(600), base)).toJson(QJsonDocument::Compact)),
                 expected);
    }

    // Step 5.2: --json --all lists every account, version 3.
    void statusAllIsVersionThree()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        roots.home = dir.path();
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", failedRun(now.addSecs(-200))));
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        QCOMPARE(accounts.size(), 2);
        StatusAnswer answer = statusOf({"--json", "--all"}, accounts);
        QCOMPARE(answer.code, 0);
        QJsonObject object = QJsonDocument::fromJson(answer.out.toUtf8()).object();
        QStringList top = object.keys();
        top.sort();
        QCOMPARE(top, (QStringList{"accounts", "canSyncNow", "now", "tone", "version", "worst"}));
        QCOMPARE(object.value("version").toInt(), 3);
        QCOMPARE(object.value("worst").toString(), QString("account2"));
        QCOMPARE(object.value("tone").toString(), QString("error"));
        QVERIFY(object.value("now").toInteger() > 0);
        QVERIFY(object.value("canSyncNow").toBool());
        QStringList perAccount{"status", "tone", "causeKind", "causeDetail", "warnings", "running", "resync",
                               "canSyncNow", "lastSuccess", "lastRun", "currentRun", "nextRun", "failingSince",
                               "typicalSeconds", "conflicts", "managed", "paused", "pausedUntil", "id", "label",
                               "folder"};
        perAccount.sort();
        const QJsonArray list = object.value("accounts").toArray();
        QCOMPARE(list.size(), 2);
        const QStringList labels{"Drive", "Drive2"};
        for (int i = 0; i < 2; ++i) {
            const QJsonObject account = list.at(i).toObject();
            QStringList keys = account.keys();
            keys.sort();
            QCOMPARE(keys, perAccount);
            QCOMPARE(account.value("id").toString(), QString("account%1").arg(i + 1));
            QCOMPARE(account.value("label").toString(), labels.at(i));
            QCOMPARE(account.value("folder").toString(), dir.filePath(labels.at(i)));
            QVERIFY(QDir::isAbsolutePath(account.value("folder").toString()));
            QVERIFY(account.value("managed").toBool());
        }
        QCOMPARE(list.at(0).toObject().value("status").toString(), QString("ok"));
        QCOMPARE(list.at(1).toObject().value("status").toString(), QString("failed"));
        QVERIFY(list.at(0).toObject().value("nextRun").toInteger() > 0);

        // Both paused: no account can sync now.
        for (const BackendPaths &account : accounts) {
            QVERIFY(QDir().mkpath(account.appState));
            QVERIFY(write(account.appState + "/paused.json", "{\"since\":1794067200}"));
        }
        object = QJsonDocument::fromJson(statusOf({"--all", "--json"}, accounts).out.toUtf8()).object();
        QCOMPARE(object.value("version").toInt(), 3);
        QVERIFY(!object.value("canSyncNow").toBool(true));
        QVERIFY(object.value("accounts").toArray().at(0).toObject().value("paused").toBool());
    }

    // Step 5.2: with no account, version 3 lists the view with no account
    // only when an old drive-sync's log is there.
    void statusAllWithNoAccount()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        QVERIFY(QDir().mkpath(dir.filePath("Drive")));
        StatusAnswer answer = statusOf({"--json", "--all"}, {noAccountPaths(roots)});
        QCOMPARE(answer.code, 0);
        QJsonObject object = QJsonDocument::fromJson(answer.out.toUtf8()).object();
        QCOMPARE(object.value("version").toInt(), 3);
        QVERIFY(object.value("accounts").toArray().isEmpty());
        QCOMPARE(object.value("tone").toString(), QString("error"));
        QCOMPARE(object.value("worst").toString(), QString());
        QVERIFY(object.contains("worst"));
        QVERIFY(!object.value("canSyncNow").toBool(true));

        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(now.addSecs(-400), 300).toUtf8()));
        object = QJsonDocument::fromJson(statusOf({"--json", "--all"}, {noAccountPaths(roots)}).out.toUtf8()).object();
        const QJsonArray list = object.value("accounts").toArray();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0).toObject().value("id").toString(), QString());
        QVERIFY(list.at(0).toObject().contains("id"));
        QVERIFY(!list.at(0).toObject().value("managed").toBool(true));
        QCOMPARE(list.at(0).toObject().value("label").toString(), QString("Drive"));
        QCOMPARE(object.value("worst").toString(), QString());
        QVERIFY(object.contains("worst"));
    }

    // Step 5.2: --all goes with --json, and each option comes once.
    void statusAllNeedsJson()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.base.systemctl = fakeSystemctl(dir.path(), QDateTime::currentDateTime().addSecs(-400));
        const QList<BackendPaths> view{noAccountPaths(roots)};
        for (const QStringList &arguments : {QStringList{"--all"}, QStringList{"--json", "--json"},
                                             QStringList{"--json", "--all", "--all"}, QStringList{"--json", "-x"}}) {
            const StatusAnswer answer = statusOf(arguments, view);
            QCOMPARE(answer.code, 1);
            QVERIFY(answer.out.isEmpty());
            QCOMPARE(answer.err, usageText());
        }
        QVERIFY2(usageText().contains("drive-sync status [--json [--all]]\n"), qPrintable(usageText()));
        QVERIFY2(usageText().contains("drive-sync [open [--account <id>]]\n"), qPrintable(usageText()));
    }

    // Step 5.2: "status --account account2 --json --all" is version 3 with
    // the one account main hands it.
    void statusAllForOneAccount()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        roots.home = dir.path();
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", failedRun(now.addSecs(-200))));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", successfulRun(now.addSecs(-400), 300)));
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
        const AccountSet set = resolveAccounts(roots);
        const AccountChoice choice = chooseAccounts(set, "status", "account2", roots);
        QVERIFY(choice.error.isEmpty());
        const QJsonObject object =
            QJsonDocument::fromJson(statusOf({"--json", "--all"}, choice.targets).out.toUtf8()).object();
        const QJsonArray list = object.value("accounts").toArray();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0).toObject().value("id").toString(), QString("account2"));
        QCOMPARE(list.at(0).toObject().value("label").toString(), QString("Drive2"));
        QCOMPARE(object.value("worst").toString(), QString("account2"));
        QCOMPARE(object.value("tone").toString(), QString("ok"));
    }

    // Step 5.2 (the maintainer's answer to Question 3): one account keeps
    // the 5.1 line; from two on, a block per account headed by its label
    // and id.
    void statusTextPrintsABlockPerAccount()
    {
        const QDateTime now = QDateTime::currentDateTime();
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            roots.home = dir.path();
            QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-400), 300)));
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            QCOMPARE(statusOf({}, resolveAccounts(roots).accounts).out, QString("ok\n"));
        }
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            roots.home = dir.path();
            QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-400), 300)));
            QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", failedRun(now.addSecs(-200))));
            QVERIFY(write(dir.filePath("Drive2/a.txt.conflict1"), "x"));
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            roots.base.find = QStandardPaths::findExecutable("find");
            const StatusAnswer answer = statusOf({}, resolveAccounts(roots).accounts);
            QCOMPARE(answer.code, 0);
            QCOMPARE(answer.out,
                     QString("Drive (account1)\n  ok\nDrive2 (account2)\n  failed (warnings: conflicts)\n"));
        }
        {
            QTemporaryDir dir;
            AccountRoots roots = accountRoots(dir.path());
            roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-400));
            QCOMPARE(statusOf({}, {noAccountPaths(roots)}).out, QString("noLog\n"));
        }
    }

    // Step 5.2: --all reads every account with the same one show call and
    // one list-timers.
    void statusAllMakesOneShowCall()
    {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(makeStatusAccount(roots, dir.path(), "account2", successfulRun(now.addSecs(-400), 300)));
        QVERIFY(QDir().mkpath(dir.filePath("answers")));
        const QString answers = fakeSystemctl(dir.filePath("answers"), now.addSecs(-400));
        const QString recording = dir.filePath("systemctl");
        QVERIFY(write(recording, QString("#!/bin/sh\necho \"systemctl $*\" >> \"%1/calls.log\"\nexec \"%2\" \"$@\"\n")
                                     .arg(dir.path(), answers)
                                     .toUtf8()));
        QFile::setPermissions(recording, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        roots.base.systemctl = recording;
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        const StatusAnswer answer = statusOf({"--json", "--all"}, accounts);
        QCOMPARE(answer.code, 0);
        QCOMPARE(QJsonDocument::fromJson(answer.out.toUtf8()).object().value("accounts").toArray().size(), 2);
        const QStringList seen = calls(dir.path());
        QCOMPARE(seen.filter(" show ").size(), 1);
        QCOMPARE(seen.filter("list-timers").size(), 1);
        QVERIFY(seen.filter("list-timers").first().contains("drive-sync@account1.timer drive-sync@account2.timer"));
    }

    void templatesCarryTheAccount()
    {
        const QString units = QString(SRCDIR) + "/../pkgbuild/units/";
        const auto text = [&](const QString &name) {
            QFile file(units + name);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        // What a unit does, without its comments, its description and the
        // commands it runs, which are what a template may change.
        const auto settings = [&](const QString &name) {
            QStringList kept;
            for (const QString &l : text(name).split('\n'))
                if (!l.startsWith('#') && !l.startsWith("Exec") && !l.startsWith("Description="))
                    kept << l;
            return kept;
        };
        const QString service = text("drive-sync@.service");
        QVERIFY(service.contains("\nExecCondition=/usr/bin/drive-sync run-condition --account %i\n"));
        QVERIFY(service.contains("\nExecStart=/usr/bin/drive-sync run --account %i\n"));
        QVERIFY(service.contains("\nExecStopPost=-/usr/bin/drive-sync notify --account %i\n"));
        QVERIFY(text("drive-sync-fix@.service").contains("\nExecStart=/usr/bin/drive-sync fix-run --account %i\n"));
        const QString timer = text("drive-sync@.timer");
        QVERIFY(timer.contains("\nOnBootSec=5min\n"));
        QVERIFY(timer.contains("\nOnUnitActiveSec=15min\n"));
        QVERIFY(timer.contains("\nWantedBy=timers.target\n"));
        // Today's limits and scheduling, line for line.
        QCOMPARE(settings("drive-sync@.service"), settings("drive-sync.service"));
        QCOMPARE(settings("drive-sync@.timer"), settings("drive-sync.timer"));
        QCOMPARE(settings("drive-sync-fix@.service"), settings("drive-sync-fix.service"));
        // The plain units stay as they were: the moves of older machines need them.
        for (const QString &name : {QString("drive-sync.service"), QString("drive-sync.timer"),
                                    QString("drive-sync-fix.service")}) {
            QVERIFY(!text(name).isEmpty());
            QVERIFY2(!text(name).contains("--account"), qPrintable(name));
        }
    }

    void accountOptionIsTakenOnlyFirst()
    {
        QString id;
        QStringList arguments{"--account", "account2", "--json"};
        QVERIFY(takeAccountOption(&arguments, &id));
        QCOMPARE(id, QString("account2"));
        QCOMPARE(arguments, QStringList{"--json"});

        // Anywhere else it is a value, such as a cloud folder named --account.
        for (const QStringList &left : {QStringList{"--skip", "--account"},
                                        QStringList{"--json", "--account", "account1"}, QStringList{}}) {
            arguments = left;
            id = "unchanged";
            QVERIFY(takeAccountOption(&arguments, &id));
            QVERIFY(id.isEmpty());
            QCOMPARE(arguments, left);
        }
        arguments = {"--account"};
        QVERIFY(!takeAccountOption(&arguments, &id));
    }

    void chooseAccountsFollowsTheRules()
    {
        QTemporaryDir dir;
        const auto rootsIn = [&](const QString &name) {
            AccountRoots roots = accountRoots(dir.filePath(name));
            QDir().mkpath(roots.configRoot);
            return roots;
        };
        const AccountRoots legacyRoots = rootsIn("legacy");
        QVERIFY(writeAccountConfig(legacyRoots.configRoot + "/config.json", dir.filePath("legacy/Drive"),
                                   dir.filePath("legacy/drive-sync.log")));
        const AccountRoots twoRoots = rootsIn("two");
        QVERIFY(makeStatusAccount(twoRoots, dir.filePath("two"), "account1", QString()));
        QVERIFY(makeStatusAccount(twoRoots, dir.filePath("two"), "account2", QString()));
        const AccountRoots oneRoots = rootsIn("one");
        QVERIFY(makeStatusAccount(oneRoots, dir.filePath("one"), "account1", QString()));
        const AccountRoots noneRoots = rootsIn("none");
        QVERIFY(QDir().mkpath(noneRoots.configRoot + "/accounts"));
        const AccountSet legacy = resolveAccounts(legacyRoots);
        const AccountSet two = resolveAccounts(twoRoots);
        const AccountSet one = resolveAccounts(oneRoots);
        const AccountSet none = resolveAccounts(noneRoots);
        QCOMPARE(legacy.mode, QString("legacy"));
        QCOMPARE(none.mode, QString("accounts"));
        const auto ids = [](const AccountChoice &choice) {
            QStringList list;
            for (const BackendPaths &paths : choice.targets)
                list << paths.id;
            return list;
        };

        // An id the resolver does not list.
        AccountChoice choice = chooseAccounts(two, "sync", "account7", twoRoots);
        QCOMPARE(choice.error, QString("no account account7; accounts: account1, account2"));
        QCOMPARE(choice.exitCode, 1);
        QVERIFY(choice.targets.isEmpty());
        QCOMPARE(chooseAccounts(none, "fix", "account1", noneRoots).error,
                 QString("no account account1; accounts: none"));

        // status, sync, pause and resume act on every account.
        for (const QString &command : {QString("status"), QString("sync"), QString("pause"), QString("resume")}) {
            choice = chooseAccounts(two, command, QString(), twoRoots);
            QVERIFY(choice.error.isEmpty());
            QCOMPARE(ids(choice), QStringList({"account1", "account2"}));
            QCOMPARE(ids(chooseAccounts(two, command, "account2", twoRoots)), QStringList{"account2"});
            choice = chooseAccounts(none, command, QString(), noneRoots);
            QCOMPARE(ids(choice), QStringList{QString()});
            QVERIFY(!choice.targets.first().managed);
        }

        // The others act on the only account.
        for (const QString &command :
             {QString("fix"), QString("deletes"), QString("settings"), QString("folders"), QString("signin")}) {
            QCOMPARE(ids(chooseAccounts(one, command, QString(), oneRoots)), QStringList{"account1"});
            QCOMPARE(ids(chooseAccounts(none, command, QString(), noneRoots)), QStringList{QString()});
            choice = chooseAccounts(two, command, QString(), twoRoots);
            QCOMPARE(choice.error, QString("several accounts (account1, account2); pick one with --account"));
            QCOMPARE(choice.exitCode, 1);
            QCOMPARE(ids(chooseAccounts(two, command, "account2", twoRoots)), QStringList{"account2"});
        }

        // What the units call: the plain ones in legacy mode, the instances after.
        for (const QString &command : {QString("run-condition"), QString("run"), QString("notify"), QString("fix-run")}) {
            choice = chooseAccounts(legacy, command, QString(), legacyRoots);
            QCOMPARE(ids(choice), QStringList{"account1"});
            QCOMPARE(choice.targets.first().config, legacyRoots.configRoot + "/config.json");
            choice = chooseAccounts(legacy, command, "account1", legacyRoots);
            QVERIFY2(!choice.error.isEmpty(), qPrintable(command));
            QCOMPARE(choice.exitCode, 1);
            QVERIFY(choice.targets.isEmpty());
            choice = chooseAccounts(one, command, QString(), oneRoots);
            QVERIFY(!choice.error.isEmpty());
            QCOMPARE(choice.exitCode, 1);
            QVERIFY(!chooseAccounts(none, command, QString(), noneRoots).error.isEmpty());
            QCOMPARE(ids(chooseAccounts(two, command, "account2", twoRoots)), QStringList{"account2"});
        }

        // Commands that never act on a chosen account.
        for (const QString &command : {QString("setup"), QString("takeover"), QString("watchdog"), QString("accounts")}) {
            choice = chooseAccounts(one, command, "account1", oneRoots);
            QVERIFY(choice.error.startsWith("usage: "));
            QCOMPARE(choice.exitCode, 1);
            QVERIFY(chooseAccounts(one, command, QString(), oneRoots).error.isEmpty());
        }
    }

    void syncOverEveryAccountKeepsOkAndError()
    {
        QTemporaryDir dir;
        BackendPaths first = withAccountUnits(controlPaths(dir.path(), true), "account1");
        first.appState = dir.filePath("app/accounts/account1");
        BackendPaths second = withAccountUnits(controlPaths(dir.path(), true), "account2");
        second.appState = dir.filePath("app/accounts/account2");
        QVERIFY(writeFix(second.appState, fixAt("questions")));
        QString out, err;
        QTextStream o(&out), e(&err);
        QCOMPARE(runSyncCommand({"--json"}, {first, second}, o, e), 1);
        QJsonObject answer = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(answer.value("ok").toBool(true), false);
        QCOMPARE(answer.value("error").toString(),
                 QString("a fix is under way, and it holds every run until it ends"));
        const QJsonArray accounts = answer.value("accounts").toArray();
        QCOMPARE(accounts.size(), 2);
        QCOMPARE(accounts.at(0).toObject(), (QJsonObject{{"id", "account1"}, {"ok", true}, {"error", ""}}));
        QCOMPARE(accounts.at(1).toObject().value("id").toString(), QString("account2"));
        QCOMPARE(accounts.at(1).toObject().value("ok").toBool(true), false);
        // The account that could sync did.
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync@account1.service"});

        // Without --json, each refusal names its account.
        QCOMPARE(runSyncCommand({}, {first, second}, o, e), 1);
        QVERIFY2(err.contains("drive-sync: account2: a fix is under way"), qPrintable(err));

        out.clear();
        QCOMPARE(runSyncCommand({"--json"}, {first}, o, e), 0);
        answer = QJsonDocument::fromJson(out.toUtf8()).object();
        const QJsonObject expected{{"ok", true},
                                   {"error", ""},
                                   {"accounts", QJsonArray{QJsonObject{{"id", "account1"}, {"ok", true}, {"error", ""}}}}};
        QCOMPARE(answer, expected);

        // Pause pauses each one; resume resumes each paused one.
        QCOMPARE(runPauseCommand({"--json"}, {first, second}, QDateTime::currentDateTimeUtc(), o, e), 0);
        QVERIFY(readPause(pauseFilePath(first.appState)).present);
        QVERIFY(readPause(pauseFilePath(second.appState)).present);
        QVERIFY(clearPause(pauseFilePath(second.appState)));
        QFile::remove(dir.filePath("calls.log"));
        QCOMPARE(runResumeCommand({}, {first, second}, o, e), 0);
        QVERIFY(!readPause(pauseFilePath(first.appState)).present);
        QCOMPARE(calls(dir.path()), QStringList{"systemctl --user start --no-block drive-sync@account1.service"});
    }

    void runConditionSkipsWhileMoving()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.stateRoot = dir.filePath("root");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        // An ended pause, which a tick that runs would clear.
        PauseMark mark;
        mark.present = true;
        mark.until = now.addSecs(-60);
        QVERIFY(writePause(pauseFilePath(paths.appState), mark));
        QVERIFY(QDir().mkpath(paths.stateRoot));
        QVERIFY(write(migrationJournalPath(paths.stateRoot), "{}"));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(readPause(pauseFilePath(paths.appState)).present);
        QVERIFY(QFile::remove(migrationJournalPath(paths.stateRoot)));
        QCOMPARE(runConditionCommand(paths, config, now), 0);
        QVERIFY(!readPause(pauseFilePath(paths.appState)).present);

        // main's gate skips before it reads any config: one may be gone halfway.
        const AccountRoots roots = accountRoots(dir.filePath("gate"));
        QVERIFY(QDir().mkpath(roots.stateRoot));
        QVERIFY(write(migrationJournalPath(roots.stateRoot), "{}"));
        QString out, err;
        QTextStream o(&out), e(&err);
        bool ran = false;
        const auto runner = [&](const QStringList &, const QList<BackendPaths> &) {
            ran = true;
            return 0;
        };
        QCOMPARE(dispatchToAccounts("run-condition", {}, resolveAccounts(roots), roots, o, e, runner), 1);
        QVERIFY(!ran);
    }

    void unitCommandsRefuseTheWrongMode()
    {
        QTemporaryDir dir;
        QString out, err;
        QTextStream o(&out), e(&err);
        QStringList ran;
        const auto runner = [&](const QStringList &, const QList<BackendPaths> &targets) {
            ran << (targets.isEmpty() ? QString("none") : targets.first().id);
            return 0;
        };

        // Legacy mode: the plain units run account1, never an instance.
        const AccountRoots legacyRoots = accountRoots(dir.filePath("legacy"));
        const QString legacyLog = dir.filePath("legacy/drive-sync.log");
        QVERIFY(writeAccountConfig(legacyRoots.configRoot + "/config.json", dir.filePath("legacy/Drive"), legacyLog));
        const AccountSet legacy = resolveAccounts(legacyRoots);
        for (const QString &command : {QString("run-condition"), QString("run"), QString("notify"), QString("fix-run")})
            QCOMPARE(dispatchToAccounts(command, {"--account", "account1"}, legacy, legacyRoots, o, e, runner), 1);
        QVERIFY(ran.isEmpty());
        QVERIFY(!QFileInfo::exists(legacyLog));
        QVERIFY2(err.contains("--account"), qPrintable(err));
        QCOMPARE(dispatchToAccounts("run", {}, legacy, legacyRoots, o, e, runner), 0);
        QCOMPARE(ran, QStringList{"account1"});

        // Accounts mode: a plain unit, with no --account, runs nothing.
        ran.clear();
        const AccountRoots roots = accountRoots(dir.filePath("accounts"));
        QVERIFY(makeStatusAccount(roots, dir.filePath("accounts"), "account1", QString()));
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.mode, QString("accounts"));
        for (const QString &command : {QString("run-condition"), QString("run"), QString("notify"), QString("fix-run")})
            QCOMPARE(dispatchToAccounts(command, {}, set, roots, o, e, runner), 1);
        QVERIFY(ran.isEmpty());
        QVERIFY(!QFileInfo::exists(roots.stateRoot));
        QVERIFY(!QFileInfo::exists(roots.base.log));
        QCOMPARE(QFileInfo(dir.filePath("accounts/account1/drive-sync.log")).size(), 0);
        QCOMPARE(dispatchToAccounts("run", {"--account", "account1"}, set, roots, o, e, runner), 0);
        QCOMPARE(ran, QStringList{"account1"});

        // --account with no id is a usage error, which a JSON reader gets as JSON.
        out.clear();
        QCOMPARE(dispatchToAccounts("sync", {"--account"}, set, roots, o, e, runner), 1);
        QCOMPARE(dispatchToAccounts("sync", {"--account", "account9", "--json"}, set, roots, o, e, runner), 1);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("ok").toBool(true), false);
        QCOMPARE(ran, QStringList{"account1"});
    }

    void canSyncNowIsFalseWithAConfigError()
    {
        // drive-sync run would refuse it, so neither the window nor the bar offers it.
        const QDateTime now(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.addSecs(-3600), 480), now);
        src.managed = true;
        QVERIFY(buildSnapshot(src).canSyncNow);
        src.configError = "config.json: unexpected end of file";
        const Snapshot snap = buildSnapshot(src);
        QCOMPARE(snap.summary.status, QString("configError"));
        QVERIFY(!snap.canSyncNow);
    }

    // Step 5.1, Task 6: runOnlyWhen guards the machine, from machine.json in accounts mode.
    void machineGuardFollowsTheMode()
    {
        QTemporaryDir dir;
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen = "false";
        BackendPaths legacy = controlPaths(dir.path(), true);
        legacy.config = dir.filePath("config.json");
        legacy.configRoot = dir.path();
        QString error;
        QCOMPARE(machineGuardCommand(legacy, config, &error), QString("false"));
        QVERIFY(error.isEmpty());
        QCOMPARE(machineGuardRefusal(legacy, config),
                 QString("the runOnlyWhen check says no (false), so this is not the home drive-sync runs on"));

        // Accounts mode reads the file and ignores the config's check.
        BackendPaths account = withAccountUnits(legacy, "account1");
        account.config = dir.filePath("accounts/account1/config.json");
        const QString file = machineGuardPath(dir.path());
        QCOMPARE(file, dir.filePath("machine.json"));
        QCOMPARE(machineGuardCommand(account, config, &error), QString());
        QVERIFY(error.isEmpty());
        QVERIFY(machineGuardRefusal(account, config).isEmpty());
        QVERIFY(writeMachineGuard(file, "false", &error));
        const QJsonObject written = QJsonDocument::fromJson(readBytes(file)).object();
        QCOMPARE(written, (QJsonObject{{"version", 1}, {"runOnlyWhen", "false"}}));
        QCOMPARE(machineGuardCommand(account, SyncConfig(), &error), QString("false"));
        QVERIFY(machineGuardRefusal(account, SyncConfig()).contains("says no (false)"));
        QVERIFY(writeMachineGuard(file, "", &error));
        QCOMPARE(readMachineGuard(file, &error), std::optional<QString>(QString()));
        QVERIFY(machineGuardRefusal(account, config).isEmpty());

        // With no account at all (setup, a first sign-in) the file guards too.
        BackendPaths none = controlPaths(dir.path(), false);
        none.configRoot = dir.path();
        QVERIFY(writeMachineGuard(file, "false", &error));
        QVERIFY(machineGuardRefusal(none, SyncConfig()).contains("says no (false)"));

        // A file DriveSync cannot read holds, naming it.
        for (const QByteArray &broken : {QByteArray("{not json"), QByteArray(R"({"version": 2, "runOnlyWhen": ""})"),
                                         QByteArray(R"({"version": 1, "runOnlyWhen": 7})")}) {
            QVERIFY(write(file, broken));
            error.clear();
            QVERIFY(!readMachineGuard(file, &error).has_value());
            QVERIFY(!error.isEmpty());
            const QString refusal = machineGuardRefusal(account, SyncConfig());
            QVERIFY2(refusal.startsWith(file + " cannot be read: "), qPrintable(refusal));
        }
        QVERIFY(QFile::remove(file));
        QCOMPARE(readMachineGuard(file, &error), std::optional<QString>(QString()));
    }

    void runConditionUsesTheMachineGuard()
    {
        QTemporaryDir dir;
        BackendPaths paths = withAccountUnits(controlPaths(dir.path(), true), "account1");
        paths.configRoot = dir.path();
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen = "false";
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        const QString file = machineGuardPath(dir.path());
        QString error;
        QCOMPARE(runConditionCommand(paths, config, now), 0);  // the config's check is not the machine's
        QVERIFY(writeMachineGuard(file, "false", &error));
        QCOMPARE(runConditionCommand(paths, config, now), 1);
        QVERIFY(writeMachineGuard(file, "exit 255", &error));
        QCOMPARE(runConditionCommand(paths, config, now), 255);
        QVERIFY(writeMachineGuard(file, "true", &error));
        QCOMPARE(runConditionCommand(paths, config, now), 0);
        QVERIFY(write(file, "{broken"));
        QCOMPARE(runConditionCommand(paths, config, now), 255);

        // Legacy mode keeps the config's check.
        const BackendPaths legacy = controlPaths(dir.path(), true);
        QCOMPARE(runConditionCommand(legacy, config, now), 1);
        config.runOnlyWhen = "exit 255";
        QCOMPARE(runConditionCommand(legacy, config, now), 255);
        config.runOnlyWhen.clear();
        QCOMPARE(runConditionCommand(legacy, config, now), 0);
    }

    void fixHonoursTheMachineGuard()
    {
        QTemporaryDir dir;
        FixSetup s = makeFixSetup(dir.path());
        s.paths = withAccountUnits(s.paths, "account1");
        s.paths.configRoot = dir.path();
        QString error;
        QVERIFY(writeMachineGuard(machineGuardPath(dir.path()), "exit 1", &error));
        QVERIFY(fixCommand(s, {"--start"}).contains("runOnlyWhen check says no (exit 1)"));
        QVERIFY(!fixPresent(s.paths.appState));
        // A fix already under way stops at its next step.
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        const FixState state = readFix(s.paths.appState);
        QCOMPARE(state.step, QString("failed"));
        QVERIFY2(state.error.contains("runOnlyWhen check says no"), qPrintable(state.error));
        QVERIFY(calls(dir.path()).filter(QRegularExpression("^rclone ")).isEmpty());
        // A file it cannot read stops it too, naming the file.
        QVERIFY(write(machineGuardPath(dir.path()), "{broken"));
        QVERIFY(writeFix(s.paths.appState, fixAt("base")));
        QCOMPARE(runFixUnit(s.paths, s.config, s.tools, s.notify), 1);
        QVERIFY(readFix(s.paths.appState).error.startsWith(machineGuardPath(dir.path()) + " cannot be read"));
    }

    void signinAndSetupHonourTheMachineGuard()
    {
        QTemporaryDir dir;
        SetupFixture s = makeSetupFixture(dir.path());
        s.paths.configRoot = dir.filePath("config");
        s.paths.accountsMode = true;
        QString error;
        QVERIFY(writeMachineGuard(machineGuardPath(s.paths.configRoot), "false", &error));
        QVERIFY(setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone).contains("runOnlyWhen check says no"));
        const CommandResult setup = setupCommand(s, s.folder);
        QCOMPARE(setup.code, 1);
        QVERIFY(!QFile::exists(s.paths.config));

        const CommandResult signin = signInCommand(s.paths, signInTools(dir.path()), {"--json"});
        QCOMPARE(signin.code, 1);
        QVERIFY2(signin.out.contains("runOnlyWhen check says no"), qPrintable(signin.out));
        for (const QStringList &call : rcloneCalls(dir.path()))
            QVERIFY2(call.value(0) != "config", qPrintable(call.join(' ')));
        // Leaving a half-made sign-in behind is never held.
        QVERIFY(!signInCommand(s.paths, signInTools(dir.path()), {"--abandon", "--json"}).out.contains("runOnlyWhen"));

        QVERIFY(writeMachineGuard(machineGuardPath(s.paths.configRoot), "true", &error));
        QVERIFY(!setupRefusal(s.paths, s.takeover, s.folder, s.takeover.rclone).contains("runOnlyWhen"));
    }

    void watchdogStaysSilentWhenTheGuardSaysNo()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "inactive", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        QString error;
        const QString machine = machineGuardPath(dir.filePath("config"));
        QVERIFY(writeMachineGuard(machine, "false", &error));
        QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc(), true), 0);
        QVERIFY(notifications(dir.path()).isEmpty());
        // The config's own check no longer silences it in accounts mode.
        QVERIFY(QFile::remove(machine));
        config.runOnlyWhen = "false";
        QCOMPARE(watchdogOver(paths, config, fakeNotifier(dir.path()), QDateTime::currentDateTimeUtc(), true), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
    }

    void settingsDropsRunOnlyWhenInAccountsMode()
    {
        // The guard is the machine's (machine.json): a save in accounts mode
        // leaves none in the account's config.
        for (const bool accounts : {true, false}) {
            QTemporaryDir dir;
            SettingsFixture s = makeSettingsFixture(dir.path());
            s.fix.config.runOnlyWhen = "true";
            QString error;
            QVERIFY(saveSyncConfig(s.fix.paths.config, s.fix.config, &error));
            if (accounts) {
                s.fix.paths = withAccountUnits(s.fix.paths, "account1");
                s.fix.paths.configRoot = dir.filePath("conf");
            }
            QCOMPARE(settingsCommand(s, {"--max-delete", "7"}).first, 0);
            const QJsonObject saved = QJsonDocument::fromJson(readBytes(s.fix.paths.config)).object();
            QCOMPARE(saved.value("runOnlyWhen").toString(), accounts ? QString() : QString("true"));
            QVERIFY(saved.value("rcloneFlags").toArray().contains("7"));
        }
    }

    // Step 5.1, Task 7: packaged template instances and the drop-ins that could reach them.
    void packagedUnitCheckMapsById()
    {
        QTemporaryDir dir;
        TakeoverPaths paths;
        paths.systemctl = dir.filePath("systemctl");
        QVERIFY(write(paths.systemctl, QString("#!/bin/sh\necho \"systemctl $*\" >> '%1/calls.log'\ncat '%1/show.txt'\n")
                                          .arg(dir.path())
                                          .toUtf8()));
        QFile::setPermissions(paths.systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QStringList units{"drive-sync@account1.service", "drive-sync@account1.timer"};
        const QString allowed = dir.filePath("units/drive-sync@account1.timer.d/drive-sync-interval.conf");
        // Blocks in the order systemd prints them for these arguments, reversed.
        const auto answer = [&](const QString &serviceFragment, const QString &timerLoad, const QString &timerDropIns) {
            return write(dir.filePath("show.txt"),
                         QString("Id=drive-sync@account1.timer\nLoadState=%1\nFragmentPath=%2\nDropInPaths=%3\n\n"
                                 "Id=drive-sync@account1.service\nLoadState=loaded\nFragmentPath=%4\nDropInPaths=\n")
                             .arg(timerLoad,
                                  timerLoad == "loaded" ? QString("/usr/lib/systemd/user/drive-sync@.timer") : QString(),
                                  timerDropIns, serviceFragment)
                             .toUtf8());
        };
        const QString packaged = "/usr/lib/systemd/user/drive-sync@.service";
        QVERIFY(answer(packaged, "loaded", QString()));
        QCOMPARE(checkPackagedUnits(paths, units, QString()), QString());
        QCOMPARE(calls(dir.path()),
                 QStringList{"systemctl --user show -p Id,LoadState,FragmentPath,DropInPaths "
                             "drive-sync@account1.service drive-sync@account1.timer"});

        // A unit from the home wins over the template: refused, naming it.
        const QString home = dir.filePath("units/drive-sync@.service");
        QVERIFY(answer(home, "loaded", QString()));
        QVERIFY(checkPackagedUnits(paths, units, QString()).contains(home));

        QVERIFY(answer(packaged, "not-found", QString()));
        QVERIFY(checkPackagedUnits(paths, units, QString()).contains("not-found"));

        // DriveSync's own interval drop-in passes only where it was named.
        QVERIFY(answer(packaged, "loaded", allowed));
        QCOMPARE(checkPackagedUnits(paths, units, allowed), QString());
        QVERIFY(checkPackagedUnits(paths, units, QString()).contains(allowed));
        QVERIFY(!checkPackagedUnits(paths, units, dir.filePath("other.conf")).isEmpty());
        QVERIFY(answer(packaged, "loaded", allowed + " " + dir.filePath("etc/drive-.timer.d/x.conf")));
        QVERIFY(checkPackagedUnits(paths, units, allowed).contains(dir.filePath("etc/drive-.timer.d/x.conf")));

        // A unit systemd did not describe is not one to trust.
        QVERIFY(write(dir.filePath("show.txt"),
                      "Id=drive-sync@account1.service\nLoadState=loaded\nFragmentPath=/usr/lib/systemd/user/"
                      "drive-sync@.service\nDropInPaths=\n"));
        QVERIFY(checkPackagedUnits(paths, units, QString()).contains("drive-sync@account1.timer"));
    }

    void foreignDropInsAreFound()
    {
        QTemporaryDir dir;
        TakeoverPaths paths;
        paths.userUnits = dir.filePath("user");
        paths.systemUserUnits = dir.filePath("etc");
        const auto place = [](const QString &file) {
            return QDir().mkpath(QFileInfo(file).absolutePath()) && write(file, "[Service]\n");
        };
        // DriveSync's own interval files are not foreign.
        QVERIFY(place(paths.userUnits + "/drive-sync.timer.d/drive-sync-interval.conf"));
        QVERIFY(place(paths.userUnits + "/drive-sync@account1.timer.d/drive-sync-interval.conf"));
        QVERIFY(QDir().mkpath(paths.userUnits + "/drive-sync@.timer.d"));  // an empty folder is nothing
        QCOMPARE(foreignDropIns(paths, "account1"), QStringList());

        const QStringList foreign{paths.userUnits + "/drive-sync@.service.d/override.conf",
                                  paths.systemUserUnits + "/drive-sync@.service.d/override.conf",
                                  paths.userUnits + "/drive-.service.d/all.conf",
                                  paths.systemUserUnits + "/drive-.timer.d/all.conf",
                                  paths.userUnits + "/drive-sync-.service.d/x.conf",
                                  paths.userUnits + "/drive-sync-fix@.service.d/x.conf",
                                  paths.userUnits + "/drive-sync-fix.service.d/x.conf",
                                  paths.userUnits + "/drive-sync-fix@account1.service.d/x.conf",
                                  paths.userUnits + "/drive-sync@account1.service.d/x.conf",
                                  paths.userUnits + "/drive-sync@account1.timer.d/other.conf",
                                  paths.systemUserUnits + "/drive-sync@account1.timer.d/drive-sync-interval.conf"};
        for (const QString &file : foreign) {
            QVERIFY(place(file));
            QVERIFY2(foreignDropIns(paths, "account1").contains(file), qPrintable(file));
        }
        QCOMPARE(foreignDropIns(paths, "account1").size(), foreign.size());
        // Another account's instance is that account's business.
        QVERIFY(place(paths.userUnits + "/drive-sync@account2.service.d/x.conf"));
        QCOMPARE(foreignDropIns(paths, "account1").size(), foreign.size());
    }

    // Step 5.1, Task 8: an old drive-sync next to accounts.
    void oldDriveSyncIsFound()
    {
        QTemporaryDir dir;
        const auto block = [](const QString &id, const QString &fragment) {
            return QHash<QString, QString>{{"Id", id}, {"LoadState", "loaded"}, {"FragmentPath", fragment}};
        };
        const QHash<QString, QHash<QString, QString>> packaged{
            {"drive-sync.service", block("drive-sync.service", "/usr/lib/systemd/user/drive-sync.service")},
            {"drive-sync.timer", block("drive-sync.timer", "/usr/lib/systemd/user/drive-sync.timer")}};

        TakeoverPaths clean;
        clean.userUnits = dir.filePath("clean/units");
        clean.binDir = dir.filePath("clean/bin");
        QVERIFY(QDir().mkpath(clean.userUnits) && QDir().mkpath(clean.binDir));
        OldDriveSync old = findOldDriveSync(clean, packaged);
        QVERIFY(old.found.isEmpty());
        QVERIFY(old.folder.isEmpty());

        // Each file alone.
        for (const QString &file :
             {clean.userUnits + "/drive-sync.service", clean.userUnits + "/drive-sync.timer",
              clean.userUnits + "/drive-sync-vigia.service", clean.userUnits + "/drive-sync-vigia.timer",
              clean.binDir + "/drive-sync.sh", clean.binDir + "/drive-sync-aviso"}) {
            QVERIFY(write(file, "x\n"));
            QCOMPARE(findOldDriveSync(clean, packaged).found, QStringList{file});
            QVERIFY(QFile::remove(file));
        }
        // A plain unit systemd loads from somewhere other than the package.
        QHash<QString, QHash<QString, QString>> fromEtc = packaged;
        fromEtc["drive-sync.timer"] = block("drive-sync.timer", "/etc/systemd/user/drive-sync.timer");
        QCOMPARE(findOldDriveSync(clean, fromEtc).found, QStringList{"/etc/systemd/user/drive-sync.timer"});

        // A whole drive-sync.sh setup: its unit names the folder and the filters.
        const OldSetup setup = makeOldSetup(dir.filePath("old"));
        old = findOldDriveSync(setup.paths, packaged);
        QCOMPARE(old.folder, setup.home + "/Drive");
        for (const QString &file : {setup.paths.userUnits + "/drive-sync.service", setup.paths.userUnits + "/drive-sync.timer",
                                    setup.paths.binDir + "/drive-sync.sh", setup.filters})
            QVERIFY2(old.found.contains(file), qPrintable(file));
        // The user unit is also the one systemd loads: named once.
        QHash<QString, QHash<QString, QString>> fromHome = packaged;
        fromHome["drive-sync.service"] = block("drive-sync.service", setup.paths.userUnits + "/drive-sync.service");
        QCOMPARE(findOldDriveSync(setup.paths, fromHome).found.count(setup.paths.userUnits + "/drive-sync.service"), 1);
    }

    void oldDriveSyncHoldsItsFolder()
    {
        QTemporaryDir dir;
        const QString home = dir.filePath("home");
        QVERIFY(QDir().mkpath(home + "/Drive/sub") && QDir().mkpath(home + "/Drive2"));
        QVERIFY(QFile::link(home + "/Drive", home + "/link"));
        const OldDriveSync old{{home + "/.config/systemd/user/drive-sync.service"}, home + "/Drive"};
        QVERIFY(oldDriveSyncHolds(old, home + "/Drive"));
        QVERIFY(!oldDriveSyncHolds(old, home + "/Drive2"));
        QVERIFY(oldDriveSyncHolds(old, home + "/Drive/sub"));
        QVERIFY(oldDriveSyncHolds(old, home));  // holding it
        QVERIFY(oldDriveSyncHolds(old, home + "/link"));
        QVERIFY(oldDriveSyncHolds(old, home + "/Drive/not-made-yet"));
        QVERIFY(!oldDriveSyncHolds(OldDriveSync{{}, home + "/Drive"}, home + "/Drive"));
        // A sync unit whose folder cannot be read holds every account: two
        // schedulers on one folder are the worse mistake. Scripts, the old
        // watchdog or a timer alone schedule no sync of their own.
        QVERIFY(oldDriveSyncHolds(OldDriveSync{{"/etc/systemd/user/drive-sync.service"}, QString()}, home + "/Drive2"));
        QVERIFY(!oldDriveSyncHolds(
            OldDriveSync{{home + "/.local/bin/drive-sync.sh", home + "/.config/systemd/user/drive-sync.timer"}, QString()},
            home + "/Drive2"));
        QVERIFY(foldersOverlap(home + "/Drive/", home + "/Drive"));
        QVERIFY(!foldersOverlap(home + "/Drive", QString()));
    }

    void runConditionHoldsForAnOldDriveSync()
    {
        QTemporaryDir dir;
        const OldSetup setup = makeOldSetup(dir.filePath("old"));
        BackendPaths paths = withAccountUnits(controlPaths(dir.path(), true), "account1");
        paths.configRoot = dir.path();
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        config.localPath = setup.home + "/Drive";
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        // What main hands it: the old unit's folder against this account's.
        const bool hold = oldDriveSyncHolds(findOldDriveSync(setup.paths, {}), config.localPath);
        QVERIFY(hold);
        QCOMPARE(runConditionCommand(paths, config, now, hold), 1);
        QCOMPARE(runConditionCommand(paths, config, now, false), 0);
        // A skip, before the machine's check: never the 255 of a broken check.
        QString error;
        QVERIFY(writeMachineGuard(machineGuardPath(dir.path()), "exit 255", &error));
        QCOMPARE(runConditionCommand(paths, config, now, hold), 1);
        // Legacy mode never had to look: the old units are what a takeover moves aside.
        QCOMPARE(runConditionCommand(controlPaths(dir.path(), true), config, now, true), 0);
    }

    void statusWarnsOfAnOldDriveSync()
    {
        QTemporaryDir dir;
        const OldSetup old = makeOldSetup(dir.filePath("old"));
        const QDateTime now = QDateTime::currentDateTime();
        AccountRoots roots = accountRoots(dir.path());
        const QString log = dir.filePath("account1.log");
        QVERIFY(write(log, successfulRun(now.addSecs(-1200), 300).toUtf8()));
        QVERIFY(writeAccountConfig(roots.configRoot + "/accounts/account1/config.json", old.home + "/Drive", log));
        // systemd: the last tick was skipped (ExecCondition), the plain units are the package's.
        const QString systemctl = dir.filePath("systemctl");
        QVERIFY(write(systemctl, QString(R"sh(#!/bin/sh
echo "systemctl $*" >> '%1/calls.log'
case "$*" in
  *list-timers*) echo '[]' ;;
  *)
    for a in "$@"; do
      case "$a" in
        drive-sync.service|drive-sync.timer) printf 'Id=%s\nLoadState=loaded\nFragmentPath=/usr/lib/systemd/user/%s\n\n' "$a" "$a" ;;
        *.service) printf 'Id=%s\nLoadState=loaded\nActiveState=inactive\nResult=exec-condition\nInactiveEnterTimestamp=@%2\n\n' "$a" ;;
        *.timer) printf 'Id=%s\nLoadState=loaded\nActiveState=active\nUnitFileState=enabled\n\n' "$a" ;;
      esac
    done ;;
esac
)sh").arg(dir.path()).arg(now.addSecs(-60).toSecsSinceEpoch()).toUtf8()));
        QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        roots.base.systemctl = systemctl;
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        QCOMPARE(accounts.size(), 1);
        const OldDriveSyncCheck check = oldDriveSyncCheck(old.paths);

        QString output, errors;
        QTextStream out(&output), err(&errors);
        QCOMPARE(runStatusCommand({"--json"}, accounts, {}, out, err, 5000, check), 0);
        out.flush();
        const QJsonObject json = QJsonDocument::fromJson(output.toUtf8()).object();
        QCOMPARE(json.size(), 20);
        QCOMPARE(json.value("status").toString(), QString("skipped"));
        QCOMPARE(json.value("causeKind").toString(), QString("oldDriveSync"));
        QVERIFY2(json.value("causeDetail").toString().contains(old.paths.userUnits + "/drive-sync.service"),
                 qPrintable(json.value("causeDetail").toString()));
        QVERIFY(json.value("warnings").toArray().contains("oldDriveSync"));
        // Still one show call, the plain units in it.
        const QStringList shows = calls(dir.path()).filter(" show ");
        QCOMPARE(shows.size(), 1);
        QVERIFY(shows.first().contains(" drive-sync.service drive-sync.timer"));

        // Readers given no check (tests, the harness) never look at this machine.
        output.clear();
        QCOMPARE(runStatusCommand({"--json"}, accounts, {}, out, err), 0);
        out.flush();
        QVERIFY(!QJsonDocument::fromJson(output.toUtf8()).object().value("warnings").toArray().contains("oldDriveSync"));

        // The window, once main gives it the check.
        Backend backend(accounts.first());
        backend.setOldDriveSyncCheck(check);
        QTRY_VERIFY_WITH_TIMEOUT(backend.warnings().contains("oldDriveSync"), 5000);
        QTRY_COMPARE(backend.causeKind(), QString("oldDriveSync"));
    }

    void watchdogCardsAnOldDriveSyncOnce()
    {
        QTemporaryDir dir;
        BackendPaths paths = controlPaths(dir.path(), true);
        paths.systemctl = watchdogSystemctl(dir.path(), "loaded", "active", "enabled");
        SyncConfig config = oldUnitConfig();
        config.runOnlyWhen.clear();
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        // The old watchdog's unit and the old script, as a DIY restore brings
        // them back: the watchdog looks for them itself.
        const WatchdogHome h = watchdogHome(paths, config, true);
        const QString vigia = h.tools.userUnits + "/drive-sync-vigia.service";
        const QString script = h.tools.binDir + "/drive-sync.sh";
        QVERIFY(QDir().mkpath(h.tools.userUnits) && QDir().mkpath(h.tools.binDir));
        QVERIFY(write(vigia, "[Service]\n") && write(script, "#!/bin/sh\n"));

        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QVERIFY2(notifications(dir.path()).first().contains(script), qPrintable(notifications(dir.path()).first()));
        QVERIFY(QFile::exists(h.roots.stateRoot + "/notify-accounts.json"));
        // The same list an hour later: no new card.
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        // Another list is news.
        QVERIFY(QFile::remove(vigia));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        // Gone, then back: a card again.
        QVERIFY(QFile::remove(script));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        QVERIFY(write(script, "#!/bin/sh\n"));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 3);

        // Legacy mode leaves an old drive-sync to the takeover.
        QTemporaryDir legacyDir;
        BackendPaths legacy = controlPaths(legacyDir.path(), true);
        legacy.systemctl = watchdogSystemctl(legacyDir.path(), "loaded", "active", "enabled");
        const WatchdogHome old = watchdogHome(legacy, config);
        QVERIFY(QDir().mkpath(old.tools.binDir) && write(old.tools.binDir + "/drive-sync.sh", "#!/bin/sh\n"));
        QCOMPARE(runWatchdogCommand(old.roots, old.tools, fakeNotifier(legacyDir.path()), now), 0);
        QVERIFY(notifications(legacyDir.path()).isEmpty());
    }

    // Step 5.1, Task 9: each precondition of the move, one at a time. Each
    // defers with a reason that names it, touches no file and asks systemd
    // only through show.
    void moveDefersAndTouchesNothing()
    {
        struct Row {
            QString name;
            std::function<void(const MoveHome &)> arrange;
            QString reason;  // a piece of the reason
            QString runOnlyWhen = "true";
        };
        std::unique_ptr<FixLock> held;
        const QList<Row> rows{
            {"instance not-found",
             [](const MoveHome &h) { setUnitProperty(h.dir, "drive-sync@account1.timer", "LoadState", "not-found"); },
             "drive-sync@account1.timer"},
            {"fragment in the home",
             [](const MoveHome &h) {
                 setUnitProperty(h.dir, "drive-sync@account1.service", "FragmentPath",
                                 h.roots.userUnits + "/drive-sync@.service");
             },
             "/units/drive-sync@.service"},
            {"foreign drop-in",
             [](const MoveHome &h) {
                 QDir().mkpath(h.tools.systemUserUnits + "/drive-sync@.service.d");
                 write(h.tools.systemUserUnits + "/drive-sync@.service.d/override.conf", "[Service]\n");
             },
             "override.conf"},
            {"runOnlyWhen says no", [](const MoveHome &) {}, "runOnlyWhen", "false"},
            {"takeover lock",
             [](const MoveHome &h) {
                 fakeProcess(h.roots.base.proc, 4242, "drive-sync");
                 write(h.roots.stateRoot + "/takeover-running", "4242\n");
             },
             "takeover"},
            {"plain service active",
             [](const MoveHome &h) { setUnitProperty(h.dir, "drive-sync.service", "ActiveState", "active"); },
             "drive-sync.service"},
            {"fix unit activating",
             [](const MoveHome &h) { setUnitProperty(h.dir, "drive-sync-fix.service", "ActiveState", "activating"); },
             "drive-sync-fix.service"},
            {"FixLock held", [&held](const MoveHome &h) { held = std::make_unique<FixLock>(h.roots.stateRoot); },
             "fix.lock"},
            {"old drive-sync.sh",
             [](const MoveHome &h) {
                 QDir().mkpath(h.tools.binDir);
                 write(h.tools.binDir + "/drive-sync.sh", "#!/bin/sh\n");
             },
             "drive-sync.sh"},
            {"target exists",
             [](const MoveHome &h) {
                 QDir().mkpath(h.roots.stateRoot + "/accounts/account1");
                 write(h.roots.stateRoot + "/accounts/account1/paused.json", "{}");
             },
             "accounts/account1/paused.json"},
            {"machine.json exists",
             [](const MoveHome &h) { write(h.roots.configRoot + "/machine.json", R"({"version":1,"runOnlyWhen":""})"); },
             "machine.json"},
        };
        for (const Row &row : rows) {
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path(), row.runOnlyWhen);
            QVERIFY(fillLegacyState(h));
            row.arrange(h);
            const auto before = moveHomeFiles(h);
            QFile::remove(dir.filePath("calls.log"));
            const QString blocker = moveBlocker(h.roots, h.tools);
            QVERIFY2(blocker.contains(row.reason), qPrintable(row.name + ": " + blocker));
            const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
            QVERIFY2(outcome.result == "deferred", qPrintable(row.name + ": " + outcome.result));
            QVERIFY2(outcome.reason.contains(row.reason), qPrintable(row.name + ": " + outcome.reason));
            QVERIFY2(moveHomeFiles(h) == before, qPrintable(row.name));
            QVERIFY(!QFileInfo::exists(h.roots.configRoot + "/accounts"));
            QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
            QVERIFY2(changingCalls(dir.path()).isEmpty(), qPrintable(row.name + ": " + changingCalls(dir.path()).join("; ")));
            held.reset();
        }

        // Already the accounts layout: nothing to do.
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(writeAccountConfig(h.roots.configRoot + "/accounts/account1/config.json", dir.filePath("Drive"),
                                   dir.filePath("one.log")));
        const auto before = moveHomeFiles(h);
        QCOMPARE(moveToAccounts(h.roots, h.tools).result, QString("notNeeded"));
        QCOMPARE(moveHomeFiles(h), before);
        QVERIFY(changingCalls(dir.path()).isEmpty());
    }

    void moveCarriesEveryStateFile()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        const QByteArray configBytes = readBytes(h.config);
        const QString manifest = h.tools.backupRoot + "/20261101-090000/manifest.json";
        const QByteArray manifestBytes = readBytes(manifest);
        const QString account = h.roots.stateRoot + "/accounts/account1";
        const QString newConfig = h.roots.configRoot + "/accounts/account1/config.json";
        const QString dropIn = h.roots.userUnits + "/drive-sync@account1.timer.d/drive-sync-interval.conf";
        const QByteArray dropInBytes = readBytes(h.roots.userUnits + "/drive-sync.timer.d/drive-sync-interval.conf");
        // Every path the config names: the folder, the filters with their
        // .md5, the log, the work dir.
        const SyncConfig named = *loadSyncConfig(h.config, nullptr);
        QVERIFY(write(named.filtersFile + ".md5", "0123456789abcdef\n"));
        QVERIFY(write(named.logFile, "a log line\n"));
        QVERIFY(write(named.localPath + "/notes.txt", "synced\n"));
        const QStringList stay{named.filtersFile, named.filtersFile + ".md5", named.logFile,
                               named.localPath + "/notes.txt", named.workDir + "/session.path1.lst"};
        QMap<QString, QByteArray> stayBytes;
        for (const QString &path : stay)
            stayBytes.insert(path, readBytes(path));

        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "moved", qPrintable(outcome.result + ": " + outcome.reason));
        for (const QString &path : stay)
            QVERIFY2(QFile::exists(path) && readBytes(path) == stayBytes.value(path), qPrintable(path));

        // The config, byte for byte, and the guard beside the accounts.
        QCOMPARE(readBytes(newConfig), configBytes);
        QVERIFY(!QFile::exists(h.config));
        QCOMPARE(readJsonObject(h.roots.configRoot + "/machine.json"),
                 (QJsonObject{{"version", 1}, {"runOnlyWhen", "true"}}));
        // Every state entry, the conflict cache deleted.
        for (const char *name : {"paused.json", "fix.json", "fix.lock", "fix-answers.json", "fix-dry-run.log",
                                 "first-setup.json", "settings-change.json", "allow-deletes.json",
                                 "allowed-dry-run.log", "allowed-run.json", "allowed-backup",
                                 "unconfirmed-deletes.json", "notify-run.json", "notify-watchdog.json",
                                 "identity-hold.json"}) {
            QVERIFY2(QFileInfo::exists(account + "/" + name), name);
            QVERIFY2(!QFileInfo::exists(h.roots.stateRoot + "/" + name), name);
        }
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/conflicts.json"));
        QVERIFY(!QFile::exists(account + "/conflicts.json"));
        // The paths in the moved files: the config and the backup folder
        // follow; the work dir, which stays, is still named.
        const QJsonObject setup = readJsonObject(account + "/first-setup.json");
        const QJsonObject settings = readJsonObject(account + "/settings-change.json");
        QCOMPARE(setup.value("config").toString(), newConfig);
        QCOMPARE(settings.value("config").toString(), newConfig);
        QCOMPARE(setup.value("createdFolders").toArray().at(0).toString(), h.roots.stateRoot + "/bisync");
        QCOMPARE(settings.value("createdFolders").toArray().at(0).toString(), h.roots.stateRoot + "/bisync");
        QCOMPARE(setup.value("filters").toString(), h.roots.configRoot + "/filters.txt");
        const QString backup = account + "/allowed-backup/20261119-101010";
        QCOMPARE(readJsonObject(account + "/allowed-run.json").value("backupDir").toString(), backup);
        QCOMPARE(readJsonObject(account + "/unconfirmed-deletes.json").value("backupDir").toString(), backup);
        QVERIFY(!readBytes(account + "/allowed-run.json").contains('\n'));  // compact stays compact
        QCOMPARE(readBytes(backup + "/a.txt"), QByteArray("kept\n"));
        // What stays: the bar's answer, the takeover backups, the work dir.
        QVERIFY(QFile::exists(h.roots.stateRoot + "/bar-declined"));
        QCOMPARE(readBytes(manifest), manifestBytes);
        QVERIFY(QFile::exists(h.roots.stateRoot + "/bisync/session.path1.lst"));
        // The interval goes with the instance; its old folder goes.
        QCOMPARE(readBytes(dropIn), dropInBytes);
        QVERIFY(!QFileInfo::exists(h.roots.userUnits + "/drive-sync.timer.d"));
        // One scheduler at a time, and the step 6 check passed with the
        // moved drop-in on the instance timer.
        QCOMPARE(changingCalls(dir.path()),
                 (QStringList{"systemctl --user disable --now drive-sync.timer", "systemctl --user daemon-reload",
                              "systemctl --user enable drive-sync@account1.timer",
                              "systemctl --user start drive-sync@account1.timer"}));
        QCOMPARE(unitProperty(dir.path(), "drive-sync.timer", "UnitFileState"), QString("disabled"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync@account1.timer", "ActiveState"), QString("active"));
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
        QVERIFY(!QFile::exists(takeoverLockPath(h.roots.stateRoot)));

        const AccountSet set = resolveAccounts(h.roots);
        QCOMPARE(set.mode, QString("accounts"));
        QCOMPARE(set.accounts.size(), 1);
        QCOMPARE(set.accounts.first().id, QString("account1"));
        QCOMPARE(set.accounts.first().config, newConfig);
        QCOMPARE(set.accounts.first().appState, account);
        QCOMPARE(set.accounts.first().drive, dir.filePath("Drive"));
        QVERIFY(set.notices.isEmpty());
        // Run again: nothing left to do.
        QCOMPARE(moveToAccounts(h.roots, h.tools).result, QString("notNeeded"));
    }

    void moveKeepsTheTimerAsItWas()
    {
        {
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            setUnitProperty(dir.path(), "drive-sync.timer", "UnitFileState", "disabled");
            setUnitProperty(dir.path(), "drive-sync.timer", "ActiveState", "inactive");
            QCOMPARE(moveToAccounts(h.roots, h.tools).result, QString("moved"));
            QCOMPARE(changingCalls(dir.path()), (QStringList{"systemctl --user disable --now drive-sync.timer",
                                                             "systemctl --user daemon-reload"}));
        }
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        setUnitProperty(dir.path(), "drive-sync.timer", "ActiveState", "inactive");
        QCOMPARE(moveToAccounts(h.roots, h.tools).result, QString("moved"));
        QCOMPARE(changingCalls(dir.path()),
                 (QStringList{"systemctl --user disable --now drive-sync.timer", "systemctl --user daemon-reload",
                              "systemctl --user enable drive-sync@account1.timer"}));
        // With no interval and no runOnlyWhen guard there is nothing else to move.
        QVERIFY(!QFileInfo::exists(h.roots.userUnits + "/drive-sync@account1.timer.d"));
    }

    // Review focus 1: a crash after any rename, or a failure at any later
    // step, comes back to legacy mode byte for byte, with the plain timer as
    // it was and no journal.
    void moveRollsBackFromEveryStep()
    {
        const auto checkRolledBack = [](const MoveHome &h, const QMap<QString, QByteArray> &before,
                                        const MoveOutcome &outcome, const QString &row) {
            QVERIFY2(outcome.result == "rolledBack" || outcome.result == "deferred",
                     qPrintable(row + ": " + outcome.result + ": " + outcome.reason));
            const auto after = moveHomeFiles(h);
            for (auto it = before.cbegin(); it != before.cend(); ++it)
                QVERIFY2(after.value(it.key()) == it.value(), qPrintable(row + ": " + it.key()));
            QVERIFY2(after.keys() == before.keys(),
                     qPrintable(row + ": " + QStringList(after.keys()).join(", ")));
            QVERIFY2(!QFileInfo::exists(h.roots.configRoot + "/accounts"), qPrintable(row));
            QVERIFY2(!QFileInfo::exists(h.roots.stateRoot + "/accounts"), qPrintable(row));
            QVERIFY2(!QFileInfo::exists(h.roots.userUnits + "/drive-sync@account1.timer.d"), qPrintable(row));
            QVERIFY2(!QFile::exists(migrationJournalPath(h.roots.stateRoot)), qPrintable(row));
            QCOMPARE(unitProperty(h.dir, "drive-sync.timer", "UnitFileState"), QString("enabled"));
            QCOMPARE(unitProperty(h.dir, "drive-sync.timer", "ActiveState"), QString("active"));
            QVERIFY2(unitProperty(h.dir, "drive-sync@account1.timer", "UnitFileState") != "enabled", qPrintable(row));
            QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        };

        // A crash after k of the planned renames (by hand), the journal
        // written as the move writes it and the plain timer already off.
        int planned = 0;
        for (int k = 0; k == 0 || k <= planned; ++k) {
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(fillLegacyState(h));
            const auto before = moveHomeFiles(h);
            const auto moves = plannedMoves(h.roots, resolveAccounts(h.roots).accounts.first());
            planned = int(moves.size());
            QJsonArray list;
            for (const auto &[from, to] : moves)
                list.append(QJsonObject{{"from", from}, {"to", to}});
            const QStringList folders{h.roots.stateRoot + "/accounts", h.roots.stateRoot + "/accounts/account1",
                                      h.roots.userUnits + "/drive-sync@account1.timer.d",
                                      h.roots.configRoot + "/accounts", h.roots.configRoot + "/accounts/account1"};
            QVERIFY(write(migrationJournalPath(h.roots.stateRoot),
                          QJsonDocument(QJsonObject{{"version", 1}, {"account", "account1"}, {"moves", list},
                                                    {"created", QJsonArray::fromStringList(folders)},
                                                    {"plainTimerEnabled", true}, {"plainTimerActive", true},
                                                    {"stuck", false}, {"stuckReason", ""}})
                              .toJson()));
            setUnitProperty(dir.path(), "drive-sync.timer", "UnitFileState", "disabled");
            setUnitProperty(dir.path(), "drive-sync.timer", "ActiveState", "inactive");
            for (int i = 0; i < k; ++i) {
                QVERIFY(QDir().mkpath(QFileInfo(moves.at(i).second).absolutePath()));
                QVERIFY(QDir().rename(moves.at(i).first, moves.at(i).second));
            }
            if (k == planned) {
                // Everything moved and the instance timer on: a crash just
                // before the journal went.
                setUnitProperty(dir.path(), "drive-sync@account1.timer", "UnitFileState", "enabled");
                setUnitProperty(dir.path(), "drive-sync@account1.timer", "ActiveState", "active");
                QCOMPARE(resolveAccounts(h.roots).mode, QString("accounts"));
            }
            checkRolledBack(h, before, moveToAccounts(h.roots, h.tools), QString("crash after %1").arg(k));
        }
        QVERIFY(planned >= 17);  // fifteen state entries, the drop-in and the config

        // A failure at each step of a live move.
        const QList<QPair<QString, std::function<void(const QString &)>>> failures{
            {"disable", [](const QString &d) { write(d + "/fail-disable", ""); }},
            {"daemon-reload", [](const QString &d) { write(d + "/fail-daemon-reload", ""); }},
            {"check", [](const QString &d) {
                 // After the reload systemd loads the instance from the home.
                 write(d + "/on-daemon-reload.sh",
                       "setprop drive-sync@account1.service FragmentPath \"$U/drive-sync@.service\"\n");
             }},
            {"enable", [](const QString &d) { write(d + "/fail-enable", ""); }},
            {"start", [](const QString &d) { write(d + "/fail-start", ""); }},
        };
        for (const auto &[name, arrange] : failures) {
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(fillLegacyState(h));
            const auto before = moveHomeFiles(h);
            arrange(dir.path());
            const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
            QVERIFY2(!outcome.reason.isEmpty(), qPrintable(name));
            QFile::remove(dir.filePath("on-daemon-reload.sh"));
            checkRolledBack(h, before, outcome, name);
        }
    }

    void rollbackToleratesAStrayFixLock()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        const auto moves = plannedMoves(h.roots, resolveAccounts(h.roots).accounts.first());
        QJsonArray list;
        for (const auto &[from, to] : moves)
            list.append(QJsonObject{{"from", from}, {"to", to}});
        QVERIFY(write(migrationJournalPath(h.roots.stateRoot),
                      QJsonDocument(QJsonObject{{"version", 1}, {"account", "account1"}, {"moves", list},
                                                {"created", QJsonArray{h.roots.stateRoot + "/accounts",
                                                                       h.roots.stateRoot + "/accounts/account1"}},
                                                {"plainTimerEnabled", true}, {"plainTimerActive", true},
                                                {"stuck", false}, {"stuckReason", ""}})
                          .toJson()));
        QVERIFY(QDir().mkpath(h.roots.stateRoot + "/accounts/account1"));
        for (const auto &[from, to] : moves)
            if (from.startsWith(h.roots.stateRoot + "/"))
                QVERIFY(QDir().rename(from, to));
        // A writer that waited on the old path made it again.
        QVERIFY(write(h.roots.stateRoot + "/fix.lock", ""));
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "rolledBack", qPrintable(outcome.result + ": " + outcome.reason));
        QVERIFY(QFile::exists(h.roots.stateRoot + "/fix.lock"));
        QVERIFY(!QFileInfo::exists(h.roots.stateRoot + "/accounts"));
        QVERIFY(QFile::exists(h.roots.stateRoot + "/paused.json"));
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
    }

    void stuckRollbackLeavesEveryTimerOff()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        const auto moves = plannedMoves(h.roots, resolveAccounts(h.roots).accounts.first());
        QJsonArray list;
        for (const auto &[from, to] : moves)
            list.append(QJsonObject{{"from", from}, {"to", to}});
        const QString journal = migrationJournalPath(h.roots.stateRoot);
        QVERIFY(write(journal, QJsonDocument(QJsonObject{{"version", 1}, {"account", "account1"}, {"moves", list},
                                                         {"created", QJsonArray{}}, {"plainTimerEnabled", true},
                                                         {"plainTimerActive", true}, {"stuck", false},
                                                         {"stuckReason", ""}})
                                   .toJson()));
        setUnitProperty(dir.path(), "drive-sync.timer", "UnitFileState", "disabled");
        setUnitProperty(dir.path(), "drive-sync.timer", "ActiveState", "inactive");
        QVERIFY(QDir().mkpath(h.roots.stateRoot + "/accounts/account1"));
        for (const auto &[from, to] : moves)
            if (from.startsWith(h.roots.stateRoot + "/"))
                QVERIFY(QDir().rename(from, to));
        // A pause written at the old place after its mark moved: two marks,
        // and no way to tell which one is right.
        const QByteArray secondPause = QJsonDocument(QJsonObject{{"since", 1794070000}}).toJson(QJsonDocument::Compact);
        QVERIFY(write(h.roots.stateRoot + "/paused.json", secondPause));

        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QCOMPARE(outcome.result, QString("stuck"));
        QVERIFY2(outcome.reason.contains("paused.json"), qPrintable(outcome.reason));
        const QJsonObject kept = readJsonObject(journal);
        QVERIFY(kept.value("stuck").toBool());
        QVERIFY2(kept.value("stuckReason").toString().contains("paused.json"), qPrintable(kept.value("stuckReason").toString()));
        const QStringList changed = changingCalls(dir.path());
        QVERIFY2(changed.contains("systemctl --user disable --now drive-sync@account1.timer"), qPrintable(changed.join("; ")));
        for (const QString &call : changed)
            QVERIFY2(!call.contains(" drive-sync.timer") || call.contains("disable"), qPrintable(call));
        QCOMPARE(unitProperty(dir.path(), "drive-sync.timer", "UnitFileState"), QString("disabled"));
        bool noticed = false;
        for (const AccountNotice &notice : resolveAccounts(h.roots).notices)
            noticed = noticed || (notice.kind == "moveStuck" && notice.path == journal);
        QVERIFY(noticed);
        // run-condition keeps skipping while the journal is there.
        QVERIFY(moveInProgress(h.roots.stateRoot));
    }

    // Every place a rollback cannot go on: stuck, the journal kept and marked,
    // no timer back. And the one it goes on past: a plain timer that will not
    // come back still ends it in legacy mode.
    void rollbackStopsWhereItCannotGoOn()
    {
        const auto checkStuck = [](const MoveHome &h, const MoveOutcome &outcome, const QString &piece,
                                   const QString &row) {
            QVERIFY2(outcome.result == "stuck", qPrintable(row + ": " + outcome.result + ": " + outcome.reason));
            QVERIFY2(outcome.reason.contains(piece), qPrintable(row + ": " + outcome.reason));
            const QJsonObject journal = readJsonObject(migrationJournalPath(h.roots.stateRoot));
            QVERIFY2(journal.value("stuck").toBool(), qPrintable(row));
            QVERIFY2(journal.value("stuckReason").toString().contains(piece), qPrintable(row));
            for (const QString &call : changingCalls(h.dir))
                QVERIFY2(!call.contains(" drive-sync.timer") || call.contains("disable"), qPrintable(row + ": " + call));
            QCOMPARE(unitProperty(h.dir, "drive-sync.timer", "UnitFileState"), QString("disabled"));
        };
        {
            // The instance timer, on, will not go off.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(fillLegacyState(h));
            QVERIFY(crashedMove(h, 1000, true));
            QVERIFY(write(dir.filePath("fail-disable"), ""));
            checkStuck(h, moveToAccounts(h.roots, h.tools), "drive-sync@account1.timer", "instance stays on");
        }
        {
            // A moved file is at neither end.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(fillLegacyState(h));
            QVERIFY(crashedMove(h, 1000));
            QVERIFY(QFile::remove(h.roots.stateRoot + "/accounts/account1/notify-run.json"));
            checkStuck(h, moveToAccounts(h.roots, h.tools), "notify-run.json", "file gone");
        }
        {
            // Something else sits in the new config folder, so accounts/
            // cannot go and the resolver would not read legacy mode.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(fillLegacyState(h));
            QVERIFY(crashedMove(h, 1000, true));
            QVERIFY(write(h.roots.configRoot + "/accounts/account1/notes.txt", "mine\n"));
            checkStuck(h, moveToAccounts(h.roots, h.tools), h.roots.configRoot + "/accounts", "accounts/ stays");
            QVERIFY(QFile::exists(h.config));  // the config itself went back
            QCOMPARE(unitProperty(dir.path(), "drive-sync@account1.timer", "UnitFileState"), QString("disabled"));
        }
        // The plain timer will not come back: legacy mode all the same, no
        // journal, and the reason says which timer is off.
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        QVERIFY(write(dir.filePath("fail-disable"), ""));
        QVERIFY(write(dir.filePath("on-disable.sh"),
                      "[ -f \"$D/fail-disable\" ] || { touch \"$D/fail-enable\"; rm -f \"$D/on-disable.sh\"; }\n"));
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "rolledBack", qPrintable(outcome.result + ": " + outcome.reason));
        QVERIFY2(outcome.reason.contains("drive-sync.timer could not be put back"), qPrintable(outcome.reason));
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
    }

    void tickBetweenChecksDefers()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        const auto before = moveHomeFiles(h);
        // A tick fires just as the plain timer goes off.
        QVERIFY(write(dir.filePath("on-disable.sh"),
                      "setprop drive-sync.service ActiveState active\nrm -f \"$D/on-disable.sh\"\n"));
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QCOMPARE(outcome.result, QString("deferred"));
        QVERIFY2(outcome.reason.contains("drive-sync.service"), qPrintable(outcome.reason));
        const QStringList changed = changingCalls(dir.path());
        QVERIFY2(changed.contains("systemctl --user enable drive-sync.timer"), qPrintable(changed.join("; ")));
        QVERIFY2(changed.contains("systemctl --user start drive-sync.timer"), qPrintable(changed.join("; ")));
        QCOMPARE(moveHomeFiles(h), before);
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
        QVERIFY(!QFileInfo::exists(h.roots.stateRoot + "/accounts"));
    }

    void aLiveMoveIsNeverRolledBack()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        QVERIFY(QDir().mkpath(h.roots.stateRoot + "/accounts/account1"));
        QVERIFY(QDir().rename(h.roots.stateRoot + "/paused.json", h.roots.stateRoot + "/accounts/account1/paused.json"));
        QVERIFY(write(migrationJournalPath(h.roots.stateRoot), "{\"version\": 1}\n"));
        // Another drive-sync holds the lock: its move is going.
        QVERIFY(fakeProcess(h.roots.base.proc, 4242, "drive-sync"));
        QVERIFY(write(h.roots.stateRoot + "/takeover-running", "4242\n"));
        const auto before = moveHomeFiles(h);
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QCOMPARE(outcome.result, QString("deferred"));
        QCOMPARE(moveHomeFiles(h), before);
        QVERIFY(changingCalls(dir.path()).isEmpty());
    }

    // Review focus 2: what waits for the user goes on working from its new place.
    void stateWorksAfterTheMove()
    {
        const auto moved = [](const MoveHome &h) {
            const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
            if (outcome.result != "moved")
                qWarning() << outcome.result << outcome.reason;
            const AccountSet set = resolveAccounts(h.roots);
            return outcome.result == "moved" && set.accounts.size() == 1 ? set.accounts.first() : BackendPaths();
        };
        QString output;
        QString errors;
        QTextStream out(&output);
        QTextStream err(&errors);
        {
            // A pause: resume clears it at its new place and starts a run.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            PauseMark mark;
            mark.present = true;
            mark.since = QDateTime::currentDateTimeUtc();
            QVERIFY(writePause(pauseFilePath(h.roots.stateRoot), mark));
            const BackendPaths account = moved(h);
            QVERIFY(!account.id.isEmpty());
            QVERIFY(readPause(pauseFilePath(account.appState)).present);
            QCOMPARE(runResumeCommand({}, {account}, out, err), 0);
            QVERIFY(!QFile::exists(pauseFilePath(account.appState)));
            QVERIFY(!QFile::exists(pauseFilePath(h.roots.stateRoot)));
            QVERIFY(calls(dir.path()).contains("systemctl --user start --no-block drive-sync@account1.service"));
        }
        {
            // A Settings change waiting at confirm: cancel puts the old
            // filters back.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            const QString filters = h.roots.configRoot + "/filters.txt";
            const QByteArray oldFilters = readBytes(filters);
            QVERIFY(write(filters, "- .git/**\n- *.tmp\n"));
            FixState state = fixAt("confirm");
            QVERIFY(writeFix(h.roots.stateRoot, state));
            state = readFix(h.roots.stateRoot);
            QVERIFY(write(settingsChangeMarkPath(h.roots.stateRoot),
                          QJsonDocument(QJsonObject{{"config", h.config},
                                                    {"filtersFile", filters},
                                                    {"previousFilters", QString::fromLatin1(oldFilters.toBase64())},
                                                    {"newFilters", QString::fromLatin1(readBytes(filters).toBase64())},
                                                    {"fix", state.started.toSecsSinceEpoch()},
                                                    {"applied", true}})
                              .toJson()));
            const BackendPaths account = moved(h);
            QVERIFY(!account.id.isEmpty());
            QCOMPARE(runFixCommand({"--cancel"}, account, out, err), 0);
            QCOMPARE(readBytes(filters), oldFilters);
            QVERIFY(!fixPresent(account.appState));
            QVERIFY(!QFile::exists(settingsChangeMarkPath(account.appState)));
        }
        {
            // A fix waiting at confirm: confirming it starts the account's
            // own fix unit, never the plain one, on the moved fix.json.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(writeFix(h.roots.stateRoot, fixAt("confirm")));
            const BackendPaths account = moved(h);
            QVERIFY(!account.id.isEmpty());
            QCOMPARE(runFixCommand({"--confirm"}, account, out, err), 0);
            QCOMPARE(readFix(account.appState).step, QString("base"));
            QVERIFY(!fixPresent(h.roots.stateRoot));
            const QString started = calls(dir.path()).filter(" start ").join("; ");
            QVERIFY2(started.contains("drive-sync-fix@account1.service"), qPrintable(started));
            QVERIFY2(!started.contains("drive-sync-fix.service"), qPrintable(started));
        }
        {
            // A first setup waiting at confirm: cancel removes the account.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path(), QString());
            QVERIFY(write(firstSetupMarkPath(h.roots.stateRoot),
                          QJsonDocument(QJsonObject{{"config", h.config},
                                                    {"filters", h.roots.configRoot + "/filters.txt"},
                                                    {"createdFolders", QJsonArray{h.roots.stateRoot + "/bisync"}}})
                              .toJson()));
            QVERIFY(writeFix(h.roots.stateRoot, fixAt("confirm")));
            const BackendPaths account = moved(h);
            QVERIFY(!account.id.isEmpty());
            QCOMPARE(runFixCommand({"--cancel"}, account, out, err), 0);
            QVERIFY(!QFile::exists(h.roots.configRoot + "/accounts/account1/config.json"));
            QVERIFY(!QFile::exists(h.roots.configRoot + "/filters.txt"));
            const AccountSet set = resolveAccounts(h.roots);
            QCOMPARE(set.mode, QString("accounts"));
            QVERIFY(set.accounts.isEmpty());
            QVERIFY(calls(dir.path()).contains(
                "systemctl --user disable --now drive-sync@account1.timer drive-sync-watchdog.timer"));
        }
        {
            // A confirmed deletion run pending: its mark is taken from the
            // new place, and the run let through finds its moved backup.
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            const QDateTime now = QDateTime::currentDateTimeUtc();
            AllowDeletes allow;
            allow.count = 3;
            allow.notebookCount = 3;
            allow.total = 21;
            allow.at = now.addSecs(-60);
            allow.runStart = now.addSecs(-300);
            QVERIFY(writeAllowDeletes(h.roots.stateRoot, allow));
            ConfirmedRun confirmed;
            confirmed.at = now.addSecs(-120);
            confirmed.backupDir = h.roots.stateRoot + "/allowed-backup/20261114-120000";
            confirmed.confirmed << "2:f1.txt";
            QVERIFY(QDir().mkpath(confirmed.backupDir));
            QVERIFY(writeConfirmedRun(h.roots.stateRoot, confirmed));
            const QDateTime killed = QDateTime::currentDateTime().addSecs(-100);
            QString log = line(killed, "INFO", R"(Synching Path1 "gdrive{x}:/" with Path2 "/home/someone/Drive/")");
            log += line(killed, "INFO", "Applying changes");
            log += line(killed, "INFO", change("Path1", "File was deleted", "c1.txt"));
            log += line(killed, "INFO", change("Path2", "File was deleted", "f1.txt"));
            QVERIFY(write(dir.filePath("drive-sync.log"), log.toUtf8()));
            const BackendPaths account = moved(h);
            QVERIFY(!account.id.isEmpty());
            const auto taken = takeAllowDeletes(account.appState, now);
            QVERIFY(taken.has_value());
            QCOMPARE(taken->count, 3);
            const UnconfirmedDeletes check = checkConfirmedRun(account.appState, dir.filePath("drive-sync.log"));
            QCOMPARE(check.cloudCount, 1);
            QCOMPARE(check.backupDir, account.appState + "/allowed-backup/20261114-120000");
            QVERIFY(QFileInfo(check.backupDir).isDir());
        }
    }

    // Real rclone: the moved account1 runs on the base its legacy config
    // built, with no resync.
    void moveNeedsNoResync()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path(), QString());
        QVERIFY(QDir().mkpath(dir.filePath("cloud")));
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        SyncConfig config = *loadSyncConfig(h.config, nullptr);
        config.remote = dir.filePath("cloud");
        config.rcloneFlags = {"--config", "/dev/null", "--log-level", "INFO"};
        QString error;
        QVERIFY(saveSyncConfig(h.config, config, &error));
        // The base, as a first setup's fix builds it.
        QProcess base;
        base.start(rclone, {"--config", "/dev/null", "bisync", config.remote, config.localPath, "--workdir",
                            config.workDir, "--filters-file", config.filtersFile, "--resync", "-q"});
        QVERIFY(base.waitForFinished(60000));
        QCOMPARE(base.exitCode(), 0);
        const QStringList listings = QDir(config.workDir).entryList(QDir::Files, QDir::Name);
        QVERIFY(!listings.isEmpty());

        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "moved", qPrintable(outcome.reason));
        const AccountSet set = resolveAccounts(h.roots);
        QCOMPARE(set.accounts.size(), 1);
        const auto movedConfig = loadSyncConfig(set.accounts.first().config, &error);
        QVERIFY2(movedConfig.has_value(), qPrintable(error));
        QVERIFY(write(dir.filePath("Drive/two.txt"), "two\n"));
        QProcess run;
        run.start(rclone, runCommandPlan(*movedConfig, &error));
        QVERIFY2(run.waitForFinished(60000), qPrintable(error));
        QCOMPARE(run.exitCode(), 0);
        QVERIFY(readBytes(config.logFile).contains("Bisync successful"));
        QVERIFY(QFile::exists(dir.filePath("cloud/two.txt")));
        // The same session's listings (a normal run adds its -old copies).
        const QStringList after = QDir(config.workDir).entryList(QDir::Files, QDir::Name);
        for (const QString &name : listings)
            QVERIFY2(after.contains(name), qPrintable(after.join(", ")));
        for (const QString &name : after)
            QVERIFY2(listings.contains(name) || name.endsWith("-old"), qPrintable(name));
        QVERIFY(QDir(config.workDir).entryList({"*-err"}).isEmpty());
    }

    void accountsCommandReports()
    {
        const auto accounts = [](const MoveHome &h, const QStringList &arguments, int *code) {
            QString output;
            QString errors;
            QTextStream out(&output);
            QTextStream err(&errors);
            *code = runAccountsCommand(arguments, h.roots, h.tools, out, err);
            out.flush();
            err.flush();
            return output + errors;
        };
        QTemporaryDir dir;
        MoveHome h = makeMoveHome(dir.path());
        int code = -1;
        QString text = accounts(h, {}, &code);
        QCOMPARE(code, 0);
        QVERIFY2(text.contains("mode: legacy\n"), qPrintable(text));
        QVERIFY2(text.contains("account1  Drive  " + dir.filePath("Drive") + "\n"), qPrintable(text));
        QVERIFY2(text.contains("move: ready\n"), qPrintable(text));
        QVERIFY(changingCalls(dir.path()).isEmpty());

        setUnitProperty(dir.path(), "drive-sync.service", "ActiveState", "active");
        text = accounts(h, {}, &code);
        QVERIFY2(text.contains("move: ") && text.contains("drive-sync.service"), qPrintable(text));
        QJsonObject json = QJsonDocument::fromJson(accounts(h, {"--migrate", "--json"}, &code).toUtf8()).object();
        QCOMPARE(code, 1);
        QCOMPARE(json.value("move").toObject().value("result").toString(), QString("deferred"));
        QVERIFY(!json.value("ok").toBool());

        setUnitProperty(dir.path(), "drive-sync.service", "ActiveState", "inactive");
        json = QJsonDocument::fromJson(accounts(h, {"--migrate", "--json"}, &code).toUtf8()).object();
        QCOMPARE(code, 0);
        QVERIFY(json.value("ok").toBool());
        QCOMPARE(json.value("move").toObject().value("result").toString(), QString("moved"));
        QCOMPARE(json.value("mode").toString(), QString("accounts"));
        const QJsonObject one = json.value("accounts").toArray().at(0).toObject();
        QCOMPARE(one.value("id").toString(), QString("account1"));
        QCOMPARE(one.value("label").toString(), QString("Drive"));
        QCOMPARE(one.value("folder").toString(), dir.filePath("Drive"));
        QCOMPARE(one.value("configError").toString(), QString());

        // Accounts mode: the labels and folders of each account.
        QVERIFY(writeAccountConfig(h.roots.configRoot + "/accounts/account2/config.json", dir.filePath("Drive2"),
                                   dir.filePath("two.log")));
        text = accounts(h, {}, &code);
        QCOMPARE(code, 0);
        QVERIFY2(text.contains("mode: accounts\n"), qPrintable(text));
        QVERIFY2(text.contains("account1  Drive  " + dir.filePath("Drive") + "\n"), qPrintable(text));
        QVERIFY2(text.contains("account2  Drive2  " + dir.filePath("Drive2") + "\n"), qPrintable(text));
        QVERIFY2(!text.contains("move:"), qPrintable(text));
        QCOMPARE(accounts(h, {"--bogus"}, &code).contains("usage"), true);
        QCOMPARE(code, 1);
    }

    // Step 5.1, Task 10 (review focus 4): with the move's journal there,
    // every writer of an account's state refuses, after the FixLock, and
    // writes nothing; status keeps its conflict cache to itself.
    void writersRefuseWhileMoving()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        const BackendPaths account = resolveAccounts(h.roots).accounts.first();
        // A stop at the guard, so deletes --allow 3 gets as far as its lock.
        QVERIFY(write(account.log, abortedRun(QDateTime::currentDateTime().addSecs(-60), {},
                                              numberedFiles("f%1.txt", 3), 5)
                                       .toUtf8()));
        QVERIFY(write(migrationJournalPath(h.roots.stateRoot), "{\"version\": 1}\n"));
        const auto before = moveHomeFiles(h);
        const QString refusal = "DriveSync is moving to accounts; try again in a minute";

        const auto answered = [&](const std::function<int(QTextStream &, QTextStream &)> &command, const QString &name) {
            QString output;
            QString errors;
            QTextStream out(&output);
            QTextStream err(&errors);
            const int code = command(out, err);
            out.flush();
            err.flush();
            QVERIFY2(code == 1, qPrintable(name + QString(": exit %1").arg(code)));
            QVERIFY2((output + errors).contains(refusal), qPrintable(name + ": " + output + errors));
        };
        const QDateTime now = QDateTime::currentDateTimeUtc();
        answered([&](QTextStream &o, QTextStream &e) { return runPauseCommand({"--json"}, {account}, now, o, e); },
                 "pause");
        answered([&](QTextStream &o, QTextStream &e) { return runResumeCommand({"--json"}, {account}, o, e); },
                 "resume");
        answered([&](QTextStream &o, QTextStream &e) { return runFixCommand({"--start", "--json"}, account, o, e); },
                 "fix --start");
        answered([&](QTextStream &o, QTextStream &e) {
            return runSettingsCommand({"--interval", "30", "--json"}, account, h.tools, o, e);
        }, "settings --interval");
        answered([&](QTextStream &o, QTextStream &e) {
            return runDeletesCommand({"--allow", "3", "--json"}, account, o, e);
        }, "deletes --allow");
        QCOMPARE(moveHomeFiles(h), before);

        // status reads, and writes no cache while the journal is there.
        QCOMPARE(statusOf({"--json"}, {account}).code, 0);
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/conflicts.json"));
        // The watchdog leaves a move another drive-sync is making alone: no
        // card about the account (its timer is off on purpose, its pause may
        // be moving), and nothing written anywhere.
        QVERIFY(fakeProcess(h.roots.base.proc, 4242, "drive-sync"));
        QVERIFY(write(h.roots.stateRoot + "/takeover-running", "4242\n"));
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc().addDays(-3);
        QVERIFY(writePause(pauseFilePath(h.roots.stateRoot), mark));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const auto beforeWatchdog = moveHomeFiles(h);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QVERIFY2(notifications(dir.path()).isEmpty(), qPrintable(notifications(dir.path()).join('\n')));
        QCOMPARE(moveHomeFiles(h), beforeWatchdog);
    }

    // A writer that waited for the FixLock while the move ran resolves again
    // and acts on the account's new place, never the old one.
    void aWriterThatWaitedStartsOver()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        const AccountSet legacy = resolveAccounts(h.roots);
        QCOMPARE(legacy.mode, QString("legacy"));
        std::atomic<bool> locked{false};
        std::thread mover([&] {
            // As the move does, with the lock held: the config and the state go.
            const FixLock lock(h.roots.stateRoot);
            locked = true;
            QThread::msleep(300);
            QDir().mkpath(h.roots.configRoot + "/accounts/account1");
            QDir().mkpath(h.roots.stateRoot + "/accounts/account1");
            QDir().rename(h.config, h.roots.configRoot + "/accounts/account1/config.json");
        });
        while (!locked)
            QThread::msleep(5);
        QString output;
        QString errors;
        QTextStream out(&output);
        QTextStream err(&errors);
        int runs = 0;
        const int code = dispatchToAccounts(
            "pause", {"--json"}, legacy, h.roots, out, err,
            [&](const QStringList &rest, const QList<BackendPaths> &accounts) {
                ++runs;
                return runPauseCommand(rest, accounts, QDateTime::currentDateTimeUtc(), out, err);
            });
        mover.join();
        out.flush();
        QCOMPARE(code, 0);
        QCOMPARE(runs, 2);
        QCOMPARE(output.count('\n'), 1);  // the first try printed nothing
        QVERIFY2(QJsonDocument::fromJson(output.toUtf8()).object().value("ok").toBool(), qPrintable(output + errors));
        QVERIFY(readPause(h.roots.stateRoot + "/accounts/account1/paused.json").present);
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/paused.json"));
    }

    // Paths built by hand (no roots): the writers behave as before.
    void writersStillWorkWithNoRoots()
    {
        QTemporaryDir dir;
        const BackendPaths paths = controlPaths(dir.path(), true);
        QVERIFY(paths.configRoot.isEmpty());
        QVERIFY(accountPathsCurrent(paths));
        {
            const AccountWriteLock lock(paths);
            QVERIFY(lock.held());
            QVERIFY(!lock.stale());
            QVERIFY(lock.refusal().isEmpty());
        }
        QString output;
        QString errors;
        QTextStream out(&output);
        QTextStream err(&errors);
        QCOMPARE(runPauseCommand({}, {paths}, QDateTime::currentDateTimeUtc(), out, err), 0);
        QVERIFY(readPause(pauseFilePath(paths.appState)).present);
        QCOMPARE(runResumeCommand({}, {paths}, out, err), 0);
        QVERIFY(!QFile::exists(pauseFilePath(paths.appState)));
        // A stale account (its paths resolved before the layout changed).
        QTemporaryDir other;
        const MoveHome h = makeMoveHome(other.path());
        BackendPaths stale = resolveAccounts(h.roots).accounts.first();
        QVERIFY(accountPathsCurrent(stale));
        QVERIFY(QDir().mkpath(h.roots.configRoot + "/accounts/account1"));
        QVERIFY(QDir().rename(h.config, h.roots.configRoot + "/accounts/account1/config.json"));
        QVERIFY(!accountPathsCurrent(stale));
        {
            const AccountWriteLock lock(stale);
            QVERIFY(lock.stale());
        }
        QCOMPARE(runPauseCommand({}, {stale}, QDateTime::currentDateTimeUtc(), out, err), kAccountPathsMoved);
        QVERIFY(!QFile::exists(pauseFilePath(stale.appState)));
    }

    // Step 5.1, Task 11: the watchdog moves the single drive-sync first, then
    // checks what the move left: the instance timer, not the plain one.
    void watchdogMovesFirst()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
        const AccountSet set = resolveAccounts(h.roots);
        QCOMPARE(set.mode, QString("accounts"));
        QCOMPARE(set.accounts.size(), 1);
        QCOMPARE(set.accounts.first().id, QString("account1"));
        const QStringList made = calls(dir.path());
        QVERIFY2(made.contains("systemctl --user is-active drive-sync@account1.timer"), qPrintable(made.join('\n')));
        QVERIFY2(!made.contains("systemctl --user is-active drive-sync.timer"), qPrintable(made.join('\n')));
        // The instance timer is on, as the plain one was: nothing to say.
        QVERIFY2(notifications(dir.path()).isEmpty(), qPrintable(notifications(dir.path()).join('\n')));
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/notify-watchdog.json"));
    }

    void watchdogRollsBackAnOrphanJournal()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        const auto before = moveHomeFiles(h);
        QVERIFY(crashedMove(h, 1));
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync.timer", "UnitFileState"), QString("enabled"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync.timer", "ActiveState"), QString("active"));
        // The home is back, and the check that followed was the plain timer's.
        // The cards' own file is the watchdog's (item 2 of the review), and
        // so is the mark of the bare 5.1 titles (step 5.2).
        auto after = moveHomeFiles(h);
        for (const QString &own : {QString("ids.txt"), QString("notify-send"), QString("gdbus"), QString("omarchy"),
                                   QString("app/notify-accounts.json"), QString("app/notify-bare-titles.done")})
            after.remove(own);
        QCOMPARE(after, before);
        QVERIFY(calls(dir.path()).contains("systemctl --user is-active drive-sync.timer"));
        // Review of step 5.1, item 2: the undone move says so, once.
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.first().contains("DriveSync's move to accounts was undone")
                     && sent.first().contains("the move stopped halfway earlier and was undone"),
                 qPrintable(sent.first()));
    }

    void watchdogChecksEveryAccount()
    {
        QTemporaryDir dir;
        const MoveHome h = makeAccountsHome(dir.path(), {"account1", "account2"});
        setUnitProperty(dir.path(), "drive-sync@account2.timer", "ActiveState", "inactive");
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
        const QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.at(0).contains("drive-sync@account2.timer is inactive"), qPrintable(sent.at(0)));
        // Each account keeps its own bookkeeping.
        QVERIFY(QFile::exists(h.roots.stateRoot + "/accounts/account2/notify-watchdog.json"));
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/accounts/account1/notify-watchdog.json"));
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/notify-watchdog.json"));
        // Back on: the second account's card goes, with a notice.
        setUnitProperty(dir.path(), "drive-sync@account2.timer", "ActiveState", "active");
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc().addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        QVERIFY(notifications(dir.path()).at(1).contains("Drive timer is back"));
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/accounts/account2/notify-watchdog.json"));
    }

    void watchdogReportsTheStaleConfig()
    {
        QTemporaryDir dir;
        const MoveHome h = makeAccountsHome(dir.path(), {"account1"});
        QVERIFY(write(h.config, "{}\n"));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        const QString card = notifications(dir.path()).first();
        QVERIFY2(card.contains("DriveSync ignores " + h.config + ": each account's config now lives under "
                               + h.roots.configRoot + "/accounts. Remove it by hand."),
                 qPrintable(card));
        QVERIFY(readJsonObject(h.roots.stateRoot + "/notify-accounts.json").contains("staleConfig"));
        // Not again within the hour; again after it, like any problem.
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        // Removed by hand: the card goes and nothing more is said.
        QVERIFY(QFile::remove(h.config));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(7200)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        QVERIFY(!readJsonObject(h.roots.stateRoot + "/notify-accounts.json").contains("staleConfig"));
    }

    // Ruling 1 of batch D: while the journal is there the watchdog checks
    // no account and writes no account file (a "timer off" card would be
    // wrong, and a file at the old place could make the rollback stuck); it
    // reports the stuck move alone.
    void watchdogReportsAStuckMove()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        QVERIFY(stuckMove(h));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        const QString journal = migrationJournalPath(h.roots.stateRoot);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QVERIFY(readJsonObject(journal).value("stuck").toBool());
        QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.first().contains("DriveSync's move to accounts stopped halfway, and every drive-sync timer is off. "
                                       "Clear what stuckReason in " + journal
                                       + " names, then run: drive-sync accounts --migrate"),
                 qPrintable(sent.first()));
        // The reason itself follows on the card's next line.
        const QString reason = readJsonObject(journal).value("stuckReason").toString();
        QVERIFY(reason.contains("paused.json"));
        QVERIFY2(calls(dir.path()).contains("stuckReason: " + reason), qPrintable(calls(dir.path()).join('\n')));
        QVERIFY(readJsonObject(h.roots.stateRoot + "/notify-accounts.json").contains("moveStuck"));
        // The watchdog's own file is the one the rollback put back, untouched.
        QCOMPARE(filesNamed(h.dir, "notify-watchdog.json"), QStringList{"app/notify-watchdog.json"});
        QCOMPARE(readBytes(h.roots.stateRoot + "/notify-watchdog.json"), QByteArray("{}"));
        // Within the hour: no card, and not a byte changes.
        const auto settled = moveHomeFiles(h);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QCOMPARE(moveHomeFiles(h), settled);
        // After it, the reminder, and still no card about an account.
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY2(sent.at(1).contains(journal), qPrintable(sent.at(1)));
    }

    // Step 5.1, Task 12: a takeover writes account1 in the accounts layout,
    // the old ExecCondition goes to machine.json, and the instance timer is
    // the one turned on.
    void takeoverWritesTheAccountsLayout()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        const QByteArray filters = readBytes(old.filters);
        const QByteArray md5 = readBytes(old.filters + ".md5");
        const TakeoverPlan plan = detectTakeover(old.paths, settledSnapshot());
        QVERIFY2(plan.canApply(), qPrintable(plan.blockers.join(", ")));
        QString error;
        QVERIFY2(applyTakeover(old.paths, plan, QDateTime::currentDateTimeUtc(), &error), qPrintable(error));

        const QString root = old.paths.appConfigDir;
        const QString account = root + "/accounts/account1";
        const auto config = loadSyncConfig(account + "/config.json", &error);
        QVERIFY2(config.has_value(), qPrintable(error));
        QCOMPARE(config->remote, plan.config.remote);
        QCOMPARE(config->localPath, plan.config.localPath);
        QCOMPARE(config->workDir, plan.config.workDir);
        QCOMPARE(config->logFile, plan.config.logFile);
        QCOMPARE(config->rcloneFlags, plan.config.rcloneFlags);
        QCOMPARE(config->filtersFile, account + "/filters.txt");
        QVERIFY(config->runOnlyWhen.isEmpty());
        QCOMPARE(readBytes(account + "/filters.txt"), filters);
        QCOMPARE(readBytes(account + "/filters.txt.md5"), md5);
        QCOMPARE(readMachineGuard(machineGuardPath(root), &error).value_or("unreadable"), plan.config.runOnlyWhen);
        for (const QString &absent : {root + "/config.json", root + "/filters.txt", account + "/account.json"})
            QVERIFY2(!QFile::exists(absent), qPrintable(absent));

        const QStringList made = systemctlCalls(dir.path());
        QVERIFY2(made.contains("systemctl --user enable --now drive-sync@account1.timer drive-sync-watchdog.timer"),
                 qPrintable(made.join('\n')));
        for (const QString &call : made)
            QVERIFY2(!(call.contains(" enable ") && call.contains(" drive-sync.timer")), qPrintable(call));

        AccountRoots roots;
        roots.configRoot = root;
        roots.stateRoot = old.paths.appState;
        roots.userUnits = old.paths.userUnits;
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(set.mode, QString("accounts"));
        QCOMPARE(set.accounts.size(), 1);
        QCOMPARE(set.accounts.first().id, QString("account1"));
        QCOMPARE(set.accounts.first().drive, plan.config.localPath);
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("managed"));
    }

    void takeoverRollbackRemovesTheAccount()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        // The eleventh call is the enable of DriveSync's timers.
        old.paths.systemctl = takeoverSystemctl(dir.path(), 11);
        const auto before = filesUnder(old.home);
        QString error;
        QVERIFY(!applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error));
        QVERIFY2(error.contains("timers"), qPrintable(error));
        QCOMPARE(filesUnder(old.home), before);
        QVERIFY(!QFileInfo::exists(old.paths.appConfigDir + "/accounts"));
        QVERIFY(!QFile::exists(machineGuardPath(old.paths.appConfigDir)));
        QVERIFY(QFile::exists(old.paths.userUnits + "/drive-sync.service"));
        QVERIFY2(systemctlCalls(dir.path()).contains("systemctl --user enable drive-sync.timer drive-sync-vigia.timer"),
                 qPrintable(systemctlCalls(dir.path()).join('\n')));
        QCOMPARE(detectTakeover(old.paths, settledSnapshot()).state, QString("found"));
    }

    // After step 5.1 drive-sync.sh comes back only by hand: undo would bring
    // its units back while drive-sync@account1.timer keeps running.
    void undoRefusedInAccountsMode()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        const QDir backups(old.paths.backupRoot);
        const QString backup = backups.filePath(backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot).last());
        QFile::remove(dir.filePath("calls.log"));
        const auto taken = filesUnder(dir.path());
        QVERIFY(!undoTakeover(old.paths, &error));
        QCOMPARE(error, QString("DriveSync now runs drive-sync per account (drive-sync@account1); going back to "
                                "drive-sync.sh is done by hand, following %1/manifest.json")
                            .arg(backup));
        QCOMPARE(filesUnder(dir.path()), taken);
        QVERIFY(systemctlCalls(dir.path()).isEmpty());
    }

    void legacyUndoRefusesWithoutConfig()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        QString error;
        QVERIFY(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                              &error));
        QVERIFY(asLegacyTakeover(old));
        const QString config = old.paths.appConfigDir + "/config.json";
        QVERIFY(write(config, "{ not json\n"));
        QFile::remove(dir.filePath("calls.log"));
        const auto taken = filesUnder(dir.path());
        QVERIFY(!undoTakeover(old.paths, &error));
        QCOMPARE(error, QString("cannot read %1, so the folder check cannot be made; nothing was undone").arg(config));
        QCOMPARE(filesUnder(dir.path()), taken);
        QVERIFY(systemctlCalls(dir.path()).isEmpty());
    }

    // Step 5.1, Task 13: with nothing configured, first setup writes account1
    // in the accounts layout and turns on its instance units.
    void setupWritesAccountOne()
    {
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        int runs = 0;
        const CommandResult r = setupThroughMain(s, &runs);
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        QCOMPARE(runs, 1);
        const QString configDir = s.roots.configRoot + "/accounts/account1";
        const QString stateDir = s.roots.stateRoot + "/accounts/account1";
        QString error;
        const auto config = loadSyncConfig(configDir + "/config.json", &error);
        QVERIFY2(config, qPrintable(error));
        QCOMPARE(config->localPath, s.folder);
        QCOMPARE(config->filtersFile, configDir + "/filters.txt");
        QCOMPARE(config->workDir, stateDir + "/bisync");
        QCOMPARE(config->logFile, stateDir + "/drive-sync.log");
        QVERIFY(QFile::exists(config->filtersFile));
        QVERIFY(QFileInfo(config->workDir).isDir());
        QVERIFY(!QFile::exists(s.roots.configRoot + "/config.json"));
        QVERIFY(!QFile::exists(s.roots.configRoot + "/filters.txt"));
        QCOMPARE(readJsonObject(firstSetupMarkPath(stateDir)).value("config").toString(), configDir + "/config.json");
        QCOMPARE(readFix(stateDir).step, QString("preparing"));
        const QStringList made = calls(dir.path());
        QVERIFY2(made.contains("systemctl --user start --no-block drive-sync-fix@account1.service"),
                 qPrintable(made.join('\n')));
        QVERIFY2(made.contains("systemctl --user enable --now drive-sync@account1.timer drive-sync-watchdog.timer"),
                 qPrintable(made.join('\n')));
        const AccountSet set = resolveAccounts(s.roots);
        QCOMPARE(set.mode, QString("accounts"));
        QCOMPARE(set.accounts.size(), 1);
        QCOMPARE(set.accounts.first().config, configDir + "/config.json");
        QCOMPARE(set.accounts.first().appState, stateDir);
    }

    void cancelledSetupRemovesTheAccount()
    {
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        const CommandResult r = setupThroughMain(s);
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        const BackendPaths account = resolveAccounts(s.roots).accounts.value(0);
        QCOMPARE(account.id, QString("account1"));
        QVERIFY(moveFix(account.appState, "confirm"));
        FixSetup fix;
        fix.paths = account;
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        const AccountSet set = resolveAccounts(s.roots);
        QCOMPARE(set.mode, QString("accounts"));
        QVERIFY(set.accounts.isEmpty());
        QVERIFY(!QFileInfo::exists(s.roots.configRoot + "/accounts/account1"));
        QVERIFY(!QFileInfo::exists(s.roots.configRoot + "/accounts"));
        QVERIFY(!fixPresent(account.appState));
        QVERIFY2(calls(dir.path()).contains("systemctl --user disable --now drive-sync@account1.timer "
                                            "drive-sync-watchdog.timer"),
                 qPrintable(calls(dir.path()).join('\n')));
        // And it can be set up again.
        const CommandResult again = setupThroughMain(s);
        QVERIFY2(again.code == 0, qPrintable(again.out + again.err));
    }

    void setupRefusedWithAnAccount()
    {
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        QVERIFY(makeStatusAccount(s.roots, dir.path(), "account2", QString()));
        CommandResult r = setupThroughMain(s);
        QCOMPARE(r.code, 1);
        QCOMPARE(setupError(r), QString("DriveSync already runs drive-sync (account2)"));
        QVERIFY(makeStatusAccount(s.roots, dir.path(), "account1", QString()));
        r = setupThroughMain(s);
        QCOMPARE(setupError(r), QString("DriveSync already runs drive-sync (account1, account2)"));
        // Legacy mode refuses as before, naming the single config.
        QTemporaryDir legacyDir;
        const AccountSetup legacy = makeAccountSetup(legacyDir.path());
        // Its log in a folder of its own: setup refuses a folder inside the log's.
        QVERIFY(writeAccountConfig(legacy.roots.configRoot + "/config.json", legacyDir.filePath("Drive"),
                                   legacyDir.filePath("logs/drive-sync.log")));
        r = setupThroughMain(legacy);
        QCOMPARE(setupError(r),
                 QString("DriveSync already runs drive-sync (%1 exists)").arg(legacy.roots.configRoot + "/config.json"));
        QVERIFY(!QFileInfo::exists(legacy.roots.configRoot + "/accounts"));
    }

    // Ruling 2 of batch D: setup's paths name account1 before it exists, so
    // the writers' lock takes them as current while the layout has no
    // account at all; one that appears while setup waits on the lock (a
    // move that ended, another setup) sends it round again, where it refuses.
    void setupThatWaitedStartsOver()
    {
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        const BackendPaths fresh = newAccountPaths(s.roots, "account1");
        QVERIFY(!fresh.managed);
        QVERIFY(accountPathsCurrent(fresh));
        {
            const AccountWriteLock lock(fresh);
            QVERIFY(!lock.stale());
            QVERIFY(lock.refusal().isEmpty());
        }
        // Another account, the account itself, or the single config of
        // legacy mode: the setup resolved before any of them.
        QVERIFY(makeStatusAccount(s.roots, dir.path(), "account2", QString()));
        QVERIFY(!accountPathsCurrent(fresh));
        QVERIFY(QDir(s.roots.configRoot + "/accounts").removeRecursively());
        QVERIFY(accountPathsCurrent(fresh));
        QVERIFY(makeStatusAccount(s.roots, dir.path(), "account1", QString()));
        QVERIFY(!accountPathsCurrent(fresh));
        QVERIFY(QDir(s.roots.configRoot + "/accounts").removeRecursively());
        QVERIFY(writeAccountConfig(s.roots.configRoot + "/config.json", dir.filePath("Drive"),
                                   dir.filePath("drive-sync.log")));
        QVERIFY(!accountPathsCurrent(fresh));
        QVERIFY(QFile::remove(s.roots.configRoot + "/config.json"));

        // End to end: the move to account1 ends while setup waits for the lock.
        std::atomic<bool> locked{false};
        std::thread mover([&] {
            const FixLock lock(fresh.appState);
            locked = true;
            QThread::msleep(300);
            makeStatusAccount(s.roots, dir.path(), "account1", QString());
        });
        while (!locked)
            QThread::msleep(5);
        int runs = 0;
        const CommandResult r = setupThroughMain(s, &runs);
        mover.join();
        QCOMPARE(runs, 2);
        QCOMPARE(r.code, 1);
        QCOMPARE(r.out.count('\n'), 1);  // the first try printed nothing
        QCOMPARE(setupError(r), QString("DriveSync already runs drive-sync (account1)"));
        QVERIFY(!QFile::exists(s.roots.configRoot + "/accounts/account1/filters.txt"));
    }

    void firstSetupThroughAccountOneWithRealRclone()
    {
        const QString rclone = QStandardPaths::findExecutable("rclone");
        if (rclone.isEmpty())
            QSKIP("rclone is not installed");
        // Setup through main's path with the fake rclone, then the fix of the
        // resolved account1 with real rclone on two local folders.
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        QVERIFY(QDir().mkpath(dir.filePath("cloud")));
        QVERIFY(write(dir.filePath("cloud/one.txt"), "one\n"));
        QVERIFY(QDir().mkpath(s.folder));
        QVERIFY(write(s.folder + "/two.txt", "two\n"));
        const CommandResult r = setupThroughMain(s);
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        BackendPaths account = resolveAccounts(s.roots).accounts.value(0);
        QCOMPARE(account.id, QString("account1"));
        QString error;
        auto config = loadSyncConfig(account.config, &error);
        QVERIFY2(config, qPrintable(error));
        config->remote = dir.filePath("cloud");
        QVERIFY(saveSyncConfig(account.config, *config, &error));
        account = resolveAccounts(s.roots).accounts.value(0);
        FixTools tools;
        tools.rclone = rclone;
        tools.wait = [](int) {};
        const NotifierPaths notify = fakeNotifier(dir.path());
        QCOMPARE(runFixUnit(account, *config, tools, notify), 0);
        QCOMPARE(readFix(account.appState).step, QString("confirm"));
        FixSetup fix;
        fix.paths = account;
        QCOMPARE(fixCommand(fix, {"--confirm"}), QString());
        const int unit = runFixUnit(account, *config, tools, notify);
        const FixState state = readFix(account.appState);
        QVERIFY2(unit == 0 && !fixPresent(account.appState),
                 qPrintable(state.step + " after " + state.failedStep + ": " + state.error));
        QVERIFY(!QFile::exists(firstSetupMarkPath(account.appState)));
        QString why;
        QVERIFY2(filtersUsable(config->filtersFile, &why), qPrintable(why));
        QCOMPARE(config->workDir, s.roots.stateRoot + "/accounts/account1/bisync");
        QVERIFY(QFile::exists(config->workDir + "/" + bisyncSessionName(config->remote, config->localPath) + ".path1.lst"));
        QVERIFY(QFile::exists(s.folder + "/one.txt"));
        QVERIFY(QFile::exists(dir.filePath("cloud/two.txt")));
        QVERIFY(readBytes(config->logFile).contains("Bisync successful"));
    }

    // Step 5.1, Task 14: the window follows the resolver.
    void backendFollowsTheMove()
    {
        QTemporaryDir dir;
        MoveHome h = makeMoveHome(dir.path());
        h.roots.base.find = QStandardPaths::findExecutable("find");
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        const AccountRoots roots = h.roots;
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QTRY_COMPARE(backend.status(), QString("ok"));
        QVERIFY(backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync.service"));
        const QString log = backend.logPath();

        // Halfway through a live move (its journal, the lock held by a
        // drive-sync, accounts/account1 made before the config moved in), the
        // resolver would see no account at all: the window keeps account1.
        QVERIFY(write(takeoverLockPath(roots.stateRoot), QByteArray::number(QCoreApplication::applicationPid()) + "\n"));
        QVERIFY(write(migrationJournalPath(roots.stateRoot), "{}\n"));
        QVERIFY(QDir().mkpath(roots.configRoot + "/accounts/account1"));
        backend.reloadConfig();
        QTest::qWait(400);
        QVERIFY(backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync.service"));
        QVERIFY(backend.notices().isEmpty());
        QVERIFY(QDir(roots.configRoot + "/accounts").removeRecursively());
        QVERIFY(QFile::remove(migrationJournalPath(roots.stateRoot)));
        QVERIFY(QFile::remove(takeoverLockPath(roots.stateRoot)));

        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "moved", qPrintable(outcome.result + ": " + outcome.reason));
        // A systemd answer about the plain units still on its way when the
        // window adopts account1 must not count: the plain timer is off now.
        QStringList shown;
        connect(&backend, &Backend::changed, &backend, [&] { shown << backend.status(); });
        backend.refresh();
        backend.reloadConfig();
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account1.service"));
        QCOMPARE(backend.timerUnit(), QString("drive-sync@account1.timer"));
        QCOMPARE(backend.fixUnit(), QString("drive-sync-fix@account1.service"));
        QVERIFY(backend.managed());
        QCOMPARE(backend.logPath(), log);
        QTRY_COMPARE(backend.status(), QString("ok"));
        QTest::qWait(1000);
        QVERIFY2(!shown.contains("timerOff"), qPrintable(shown.join(", ")));
        QCOMPARE(backend.status(), QString("ok"));
        // The account's own state folder is watched now.
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc();
        QVERIFY(writePause(pauseFilePath(roots.stateRoot + "/accounts/account1"), mark));
        QTRY_VERIFY(backend.paused());
    }

    void backendFollowsTheMoveByItsWatch()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(QDateTime::currentDateTime().addSecs(-600), 480).toUtf8()));
        const AccountRoots roots = h.roots;
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QCOMPARE(backend.serviceUnit(), QString("drive-sync.service"));
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "moved", qPrintable(outcome.result + ": " + outcome.reason));
        // Nobody calls reloadConfig: the watch on the config's folder sees
        // config.json leave it.
        QTRY_COMPARE(backend.serviceUnit(), QString("drive-sync@account1.service"));
        QVERIFY(backend.managed());
    }

    void backendShowsNotices()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.base.systemctl = fakeSystemctl(dir.path(), QDateTime::currentDateTime().addSecs(-600));
        QVERIFY(makeStatusAccount(roots, dir.path(), "account2", QString()));
        const QString stale = roots.configRoot + "/config.json";
        QVERIFY(write(stale, "{}\n"));
        QVERIFY(QDir().mkpath(roots.stateRoot));
        QVERIFY(write(migrationJournalPath(roots.stateRoot), R"({"stuck": true})"));
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account2.service"));
        const QVariantList expected{QVariantMap{{"kind", "staleConfig"}, {"path", stale}},
                                    QVariantMap{{"kind", "moveStuck"}, {"path", migrationJournalPath(roots.stateRoot)}}};
        QCOMPARE(backend.notices(), expected);
        // The old config goes: the watch on the config root sees it.
        QVERIFY(QFile::remove(stale));
        QTRY_COMPARE(backend.notices(), QVariantList{expected.at(1)});
        QVERIFY(QFile::remove(migrationJournalPath(roots.stateRoot)));
        backend.reloadConfig();
        QVERIFY(backend.notices().isEmpty());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account2.service"));
    }

    void backendFollowsASetupMadeInATerminal()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.base.systemctl = fakeSystemctl(dir.path(), QDateTime::currentDateTime().addSecs(-600));
        // No ~/.config/drive-sync at all yet: nothing to watch there.
        QVERIFY(!QFileInfo::exists(roots.configRoot));
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QVERIFY(!backend.managed());
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1", QString()));
        // The next poll (here a refresh, as Space does) finds the new config
        // root and watches it; nobody calls reloadConfig.
        backend.refresh();
        QTRY_VERIFY(backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account1.service"));
    }

    void startMoveRunsTheChild()
    {
        QTemporaryDir dir;
        const QString self = dir.filePath("drive-sync");
        QVERIFY(write(self, QString("#!/bin/sh\necho \"$*\" >> '%1/self.log'\nexit 1\n").arg(dir.path()).toUtf8()));
        QFile::setPermissions(self, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), QString(), QString(), dir.filePath("app")};
        paths.self = self;
        Backend backend(paths);
        int asked = 0;
        backend.setResolver([paths, &asked] {
            ++asked;
            return WindowAccount{paths, {}};
        });
        const int before = asked;
        backend.startMove();
        QTRY_COMPARE(QString::fromUtf8(readBytes(dir.filePath("self.log"))), QString("accounts --migrate --json\n"));
        // Then the window reads the accounts again, and a deferred move (exit
        // 1) says nothing.
        QTRY_VERIFY(asked > before);
        QVERIFY(backend.message().isEmpty());
    }

    void backendFollowsAFirstSetup()
    {
        QTemporaryDir dir;
        const AccountSetup s = makeAccountSetup(dir.path());
        const AccountRoots roots = s.roots;
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QVERIFY(!backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync.service"));

        const CommandResult r = setupThroughMain(s);
        QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
        // What the window does when setup says it started.
        backend.reloadConfig();
        const QString state = roots.stateRoot + "/accounts/account1";
        QVERIFY(backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account1.service"));
        QCOMPARE(backend.logPath(), state + "/drive-sync.log");
        QCOMPARE(backend.drivePath(), s.folder);
        QTRY_COMPARE(backend.fixStep(), QString("preparing"));

        // A setup cancelled at the dry run takes the account away again.
        const BackendPaths account = resolveAccounts(roots).accounts.value(0);
        QVERIFY(moveFix(account.appState, "confirm"));
        FixSetup fix;
        fix.paths = account;
        QCOMPARE(fixCommand(fix, {"--cancel"}), QString());
        backend.reloadConfig();
        QVERIFY(!backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync.service"));
        QTRY_COMPARE(backend.fixStep(), QString());
    }

    void backendFollowsATakeoverIntoAccount1()
    {
        QTemporaryDir dir;
        OldSetup old = makeOldSetup(dir.path());
        old.paths.systemctl = takeoverSystemctl(dir.path(), 0);
        AccountRoots roots;
        roots.configRoot = old.paths.appConfigDir;
        roots.stateRoot = old.paths.appState;
        roots.userUnits = old.paths.userUnits;
        QVERIFY(QDir().mkpath(dir.filePath("window")));
        roots.base = BackendPaths{old.home + "/.local/state/drive-sync.log", old.home + "/Drive", old.paths.oldStateDir,
                                  dir.filePath("proc"),
                                  fakeSystemctl(dir.filePath("window"), QDateTime::currentDateTime().addSecs(-600)),
                                  QString(), roots.stateRoot};
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QVERIFY(!backend.managed());

        QString error;
        QVERIFY2(applyTakeover(old.paths, detectTakeover(old.paths, settledSnapshot()), QDateTime::currentDateTimeUtc(),
                               &error),
                 qPrintable(error));
        // What the window does when the takeover check answers again.
        backend.reloadConfig();
        QVERIFY(backend.managed());
        QCOMPARE(backend.serviceUnit(), QString("drive-sync@account1.service"));
        QCOMPARE(backend.timerUnit(), QString("drive-sync@account1.timer"));
        QCOMPARE(backend.logPath(), old.home + "/.local/state/drive-sync.log");
        QCOMPARE(backend.drivePath(), old.home + "/Drive");
        QTRY_VERIFY(backend.status() != "unavailable" && backend.status() != "notLoaded");
    }

    // Review of step 5.1, item 1: what the card and the README tell the
    // user to do with a stuck move is what --migrate does. With the cause
    // cleared, the first --migrate puts everything back (legacy mode), and
    // the next one makes the move.
    void migrateFinishesAStuckMoveOnceItsCauseIsCleared()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(fillLegacyState(h));
        QVERIFY(stuckMove(h));
        const auto migrate = [&h](QJsonObject *answer) {
            QString out, err;
            QTextStream o(&out), e(&err);
            const int code = runAccountsCommand({"--migrate", "--json"}, h.roots, h.tools, o, e);
            o.flush();
            *answer = QJsonDocument::fromJson(out.toUtf8()).object().value("move").toObject();
            return code;
        };
        QJsonObject move;
        QCOMPARE(migrate(&move), 1);
        QCOMPARE(move.value("result").toString(), QString("stuck"));
        const QString journal = migrationJournalPath(h.roots.stateRoot);
        QVERIFY2(readJsonObject(journal).value("stuckReason").toString().contains("paused.json"),
                 qPrintable(readJsonObject(journal).value("stuckReason").toString()));
        QCOMPARE(migrationStuckReason(journal), readJsonObject(journal).value("stuckReason").toString());
        // The user clears what stuckReason names: the second pause mark goes.
        QVERIFY(QFile::remove(h.roots.stateRoot + "/paused.json"));
        QCOMPARE(migrate(&move), 1);
        QCOMPARE(move.value("result").toString(), QString("rolledBack"));
        QVERIFY(!QFile::exists(journal));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        QCOMPARE(migrate(&move), 0);
        QCOMPARE(move.value("result").toString(), QString("moved"));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("accounts"));
    }

    // Review of step 5.1, item 2: a move the watchdog had to undo says so,
    // every hour while it keeps happening, and goes quietly once it worked.
    void watchdogCardsARolledBackMove()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        const QString cards = h.roots.stateRoot + "/notify-accounts.json";
        // The reload after the config moved fails once: the move is undone.
        QVERIFY(write(dir.filePath("fail-daemon-reload"), ""));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.first().contains("DriveSync's move to accounts was undone")
                     && sent.first().contains("DriveSync tried to move to accounts and put everything back as it was: "
                                              "systemctl daemon-reload failed")
                     && sent.first().contains("It tries again every hour."),
                 qPrintable(sent.first()));
        QVERIFY(readJsonObject(cards).contains("moveRolledBack"));
        // Undone again within the hour: no card; after it, the reminder.
        QVERIFY(write(dir.filePath("fail-daemon-reload"), ""));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QVERIFY(write(dir.filePath("fail-daemon-reload"), ""));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY2(sent.at(1).contains("DriveSync's move to accounts was undone again"), qPrintable(sent.at(1)));
        // The move goes: the card goes, and nothing is said.
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(7200)), 0);
        QCOMPARE(resolveAccounts(h.roots).mode, QString("accounts"));
        QCOMPARE(notifications(dir.path()).size(), 2);
        QVERIFY(!readJsonObject(cards).contains("moveRolledBack"));
    }

    // Review of step 5.1, item 3: the single config gone with DriveSync's
    // plain timer still on lands in accounts mode with no account at all.
    // Before accounts that was run-condition 255 (a failure systemd records)
    // and a watchdog card; it is again.
    void aVanishedSingleConfigFailsAndCards()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        const QByteArray configBytes = readBytes(h.config);
        QVERIFY(QFile::remove(h.config));
        const AccountSet set = resolveAccounts(h.roots);
        QCOMPARE(set.mode, QString("accounts"));
        QVERIFY(set.accounts.isEmpty());
        QString out, err;
        QTextStream o(&out), e(&err);
        bool ran = false;
        const auto runner = [&](const QStringList &, const QList<BackendPaths> &) {
            ran = true;
            return 0;
        };
        QCOMPARE(dispatchToAccounts("run-condition", {}, set, h.roots, o, e, runner), 255);
        QVERIFY(!ran);
        e.flush();
        QVERIFY2(err.startsWith("drive-sync: ") && err.contains(h.config), qPrintable(err));

        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        const QString cards = h.roots.stateRoot + "/notify-accounts.json";
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QStringList sent = notifications(dir.path());
        QCOMPARE(sent.size(), 1);
        QVERIFY2(sent.first().contains("-u critical") && sent.first().contains("DriveSync's config is gone")
                     && sent.first().contains("DriveSync's config cannot be read, so no run happens: ")
                     && sent.first().contains(h.config),
                 qPrintable(sent.first()));
        QVERIFY(readJsonObject(cards).contains("configGone"));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(1800)), 0);
        QCOMPARE(notifications(dir.path()).size(), 1);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        sent = notifications(dir.path());
        QCOMPARE(sent.size(), 2);
        QVERIFY2(sent.at(1).contains("DriveSync's config is still gone"), qPrintable(sent.at(1)));
        // The config back: the card goes with nothing said (and the move runs).
        QVERIFY(write(h.config, configBytes));
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(7200)), 0);
        QCOMPARE(notifications(dir.path()).size(), 2);
        QVERIFY(!readJsonObject(cards).contains("configGone"));
    }

    // No config and no plain timer of DriveSync's turned on is no outage:
    // a machine that never had a drive-sync, or one whose takeover was
    // undone (the old user units shadow the packaged names).
    void noConfigWithoutDriveSyncTimersIsSilent()
    {
        {
            QTemporaryDir dir;
            const MoveHome h = makeMoveHome(dir.path());
            QVERIFY(QFile::remove(h.config));
            setUnitProperty(dir.path(), "drive-sync.timer", "UnitFileState", "disabled");
            setUnitProperty(dir.path(), "drive-sync.timer", "ActiveState", "inactive");
            const NotifierPaths tools = fakeNotifier(dir.path());
            QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
            QVERIFY2(notifications(dir.path()).isEmpty(), qPrintable(notifications(dir.path()).join('\n')));
        }
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(QFile::remove(h.config));
        setUnitProperty(dir.path(), "drive-sync.timer", "FragmentPath", h.roots.userUnits + "/drive-sync.timer");
        const NotifierPaths tools = fakeNotifier(dir.path());
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, QDateTime::currentDateTimeUtc()), 0);
        for (const QString &card : notifications(dir.path()))
            QVERIFY2(!card.contains("DriveSync's config is gone"), qPrintable(card));
        QVERIFY(!readJsonObject(h.roots.stateRoot + "/notify-accounts.json").contains("configGone"));
    }

    // Review of step 5.1, item 4: a rollback that cannot turn the plain
    // timer back on still ends in legacy mode, the watchdog says the timer
    // is off, and the next move turns the instance timer on as the plain one
    // was before the failed rollback, instead of copying it off.
    void aTimerTheRollbackCouldNotRestoreIsNotForgotten()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(crashedMove(h, 1));
        QVERIFY(write(dir.filePath("fail-enable"), ""));
        QVERIFY(write(dir.filePath("fail-start"), ""));
        const NotifierPaths tools = fakeNotifier(dir.path());
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1794067200, QTimeZone::UTC);
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now), 0);
        QVERIFY(!QFile::exists(migrationJournalPath(h.roots.stateRoot)));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync.timer", "UnitFileState"), QString("disabled"));
        const QString sent = notifications(dir.path()).join('\n');
        QVERIFY2(sent.contains("drive-sync.timer could not be put back"), qPrintable(sent));
        QVERIFY2(sent.contains("drive-sync.timer is inactive"), qPrintable(sent));
        // The next round moves, and the instance timer is on and enabled.
        QCOMPARE(runWatchdogCommand(h.roots, h.tools, tools, now.addSecs(3600)), 0);
        QCOMPARE(resolveAccounts(h.roots).mode, QString("accounts"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync@account1.timer", "UnitFileState"), QString("enabled"));
        QCOMPARE(unitProperty(dir.path(), "drive-sync@account1.timer", "ActiveState"), QString("active"));
        // Nothing of the failed rollback is left behind once the move holds.
        QCOMPARE(filesNamed(h.dir, "plain-timer.json"), QStringList());
    }

    // Review of step 5.1, item 5a: two takers of the takeover lock never
    // both see a stale file, remove it and write their own.
    void takeoverLockTakersWaitForEachOther()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath("app");
        QVERIFY(QDir().mkpath(root));
        int held = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(held >= 0);
        QCOMPARE(::flock(held, LOCK_EX), 0);
        std::atomic<bool> done{false};
        bool took = false;
        std::thread taker([&] {
            QString error;
            took = takeTakeoverLock(root, &error);
            done = true;
        });
        // A failed check returns early: the taker still has to end first.
        const auto finish = qScopeGuard([&] {
            if (held >= 0)
                ::close(held);
            held = -1;
            if (taker.joinable())
                taker.join();
        });
        QThread::msleep(300);
        QVERIFY(!done);
        QVERIFY(!QFile::exists(takeoverLockPath(root)));
        ::close(held);
        held = -1;
        taker.join();
        QVERIFY(took);
        QCOMPARE(readBytes(takeoverLockPath(root)).trimmed(), QByteArray::number(QCoreApplication::applicationPid()));
        releaseTakeoverLock(root);
        QVERIFY(!QFile::exists(takeoverLockPath(root)));
    }

    // The rollback of an orphan journal holds the FixLock as the move does,
    // wherever fix.lock sits now, without making one.
    void orphanRollbackWaitsForTheFixLock()
    {
        QTemporaryDir dir;
        const MoveHome h = makeMoveHome(dir.path());
        QVERIFY(crashedMove(h, 1));
        const QString target = h.roots.stateRoot + "/accounts/account1";
        QVERIFY(QFile::exists(target + "/fix.lock"));
        QVERIFY(!QFile::exists(h.roots.stateRoot + "/fix.lock"));
        {
            const FixLock writer(target, false);
            QVERIFY(writer.held());
            const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
            QVERIFY2(outcome.result == "deferred" && outcome.reason.contains("fix.lock"),
                     qPrintable(outcome.result + ": " + outcome.reason));
            QVERIFY(QFile::exists(migrationJournalPath(h.roots.stateRoot)));
            QVERIFY(QFile::exists(target + "/fix.lock"));
            QVERIFY(!QFile::exists(h.roots.stateRoot + "/fix.lock"));
        }
        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "rolledBack", qPrintable(outcome.result + ": " + outcome.reason));
        QVERIFY(QFile::exists(h.roots.stateRoot + "/fix.lock"));
        QCOMPARE(resolveAccounts(h.roots).mode, QString("legacy"));
    }

    // Review of step 5.1, item 5b: run-condition skips every tick while the
    // journal is there, so neither the window nor the bar offers a run.
    void canSyncNowIsFalseWhileMoving()
    {
        const QDateTime now(QDate(2026, 11, 19), QTime(12, 0), QTimeZone::UTC);
        SnapshotSources src = idleSources(successfulRun(now.addSecs(-3600), 480), now);
        src.managed = true;
        QVERIFY(buildSnapshot(src).canSyncNow);
        src.moving = true;
        QVERIFY(!buildSnapshot(src).canSyncNow);

        // Both readers set it: the window and drive-sync status.
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        roots.base.systemctl = fakeSystemctl(dir.path(), QDateTime::currentDateTime().addSecs(-600));
        QVERIFY(makeStatusAccount(roots, dir.path(), "account1",
                                  successfulRun(QDateTime::currentDateTime().addSecs(-600), 300)));
        const QList<BackendPaths> accounts = resolveAccounts(roots).accounts;
        QVERIFY(QJsonDocument::fromJson(statusOf({"--json"}, accounts).out.toUtf8()).object().value("canSyncNow").toBool());
        QVERIFY(QDir().mkpath(roots.stateRoot));
        QVERIFY(write(migrationJournalPath(roots.stateRoot), R"({"stuck": true})"));
        QVERIFY(!QJsonDocument::fromJson(statusOf({"--json"}, accounts).out.toUtf8()).object().value("canSyncNow").toBool(true));
        QVERIFY(QFile::remove(migrationJournalPath(roots.stateRoot)));
        Backend backend(windowAccount(roots).paths);
        backend.setResolver([roots] { return windowAccount(roots); });
        QTRY_VERIFY(backend.canSyncNow());
        QVERIFY(write(migrationJournalPath(roots.stateRoot), R"({"stuck": true})"));
        backend.reloadConfig();
        QTRY_VERIFY(!backend.canSyncNow());
    }

    // Step 5.2, Task 1: the window keeps one name for the selected account,
    // "backend", and swaps the object behind it. This guards that QML
    // re-evaluates the bindings and moves the Connections that read a
    // context property when the property is set again, in a nested file as
    // the window's dialogs are.
    void contextPropertySwapRebinds()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("Child.qml"),
                      "import QtQuick\n"
                      "Item {\n"
                      "    property string seen: backend.objectName\n"
                      "    property int heard: 0\n"
                      "    Connections { target: backend; function onObjectNameChanged() { heard++ } }\n"
                      "}\n"));
        QVERIFY(write(dir.filePath("Main.qml"),
                      "import QtQuick\n"
                      "Item {\n"
                      "    property alias seen: child.seen\n"
                      "    property alias heard: child.heard\n"
                      "    Child { id: child }\n"
                      "}\n"));
        QObject a;
        a.setObjectName("a");
        QObject b;
        b.setObjectName("b");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &a);
        QQmlComponent component(&engine, QUrl::fromLocalFile(dir.filePath("Main.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QCOMPARE(root->property("seen").toString(), QString("a"));

        engine.rootContext()->setContextProperty("backend", &b);
        QTRY_COMPARE(root->property("seen").toString(), QString("b"));

        b.setObjectName("b2");
        QTRY_COMPARE(root->property("seen").toString(), QString("b2"));
        QCOMPARE(root->property("heard").toInt(), 1);

        // The old object is no longer heard.
        a.setObjectName("a2");
        QCoreApplication::processEvents();
        QCOMPARE(root->property("heard").toInt(), 1);
        QCOMPARE(root->property("seen").toString(), QString("b2"));
    }

    // Step 5.2, Task 7: every command the window runs names the account it
    // shows, right after the command word (takeAccountOption reads only the
    // first two arguments); the view with no account names none.
    void windowChildrenNameTheAccount()
    {
        QTemporaryDir dir;
        const QDateTime start = QDateTime::currentDateTime().addSecs(-600);
        QDir(dir.path()).mkpath("Drive");
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(start, 480).toUtf8()));
        BackendPaths paths{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                           dir.filePath("proc"), fakeSystemctl(dir.path(), start),
                           QStandardPaths::findExecutable("find"), dir.filePath("app")};
        paths.managed = true;
        paths.self = fakeDriveSync(dir.path(), "{}");
        paths.id = "account2";
        Backend backend(paths);
        QCOMPARE(backend.accountId(), QString("account2"));
        QCOMPARE(backend.paths().id, QString("account2"));
        QTRY_COMPARE(backend.status(), QString("ok"));

        backend.syncNow();
        backend.pauseFor(60);
        backend.pauseUntilTomorrow();
        backend.pauseUntilResumed();
        backend.resume();
        backend.startFix();
        backend.confirmFix();
        backend.cancelFix();
        backend.retryFix();
        backend.answerFix({});
        backend.allowDeletes(3);
        QTRY_COMPARE(calls(dir.path()).size(), 11);
        QStringList made = calls(dir.path());
        made.sort();
        const QString answers = dir.filePath("app") + "/fix-answers.json";
        QCOMPARE(made, (QStringList{"drive-sync deletes --account account2 --allow 3 --json",
                                    "drive-sync fix --account account2 --answers " + answers + " --json",
                                    "drive-sync fix --account account2 --cancel --json",
                                    "drive-sync fix --account account2 --confirm --json",
                                    "drive-sync fix --account account2 --retry --json",
                                    "drive-sync fix --account account2 --start --json",
                                    "drive-sync pause --account account2 --json",
                                    "drive-sync pause --account account2 --minutes 60 --json",
                                    "drive-sync pause --account account2 --until-tomorrow --json",
                                    "drive-sync resume --account account2 --json",
                                    "drive-sync sync --account account2 --json"}));

        // The move is about every account: it names none.
        QVERIFY(QFile::remove(dir.filePath("calls.log")));
        backend.startMove();
        QTRY_COMPARE(calls(dir.path()), QStringList{"drive-sync accounts --migrate --json"});

        // The view with no account: as before accounts.
        QVERIFY(QFile::remove(dir.filePath("calls.log")));
        paths.id.clear();
        Backend none(paths);
        QCOMPARE(none.accountId(), QString());
        none.syncNow();
        QTRY_COMPARE(calls(dir.path()), QStringList{"drive-sync sync --json"});
    }

    void settingsControllerNamesTheAccount()
    {
        QTemporaryDir dir;
        const QJsonObject settings{{"ok", true}, {"error", ""}, {"folder", "/h/Drive"}, {"remote", "gdrive:"},
                                   {"intervalMinutes", 30}, {"maxDelete", 5}, {"filtersFile", "/h/f.txt"},
                                   {"filters", "- .git/**\n"}, {"skipped", QJsonArray{"Photos"}}, {"skipError", ""}};
        QVERIFY(write(dir.filePath("settings.json"), QJsonDocument(settings).toJson(QJsonDocument::Compact)));
        QVERIFY(write(dir.filePath("change-answer.json"),
                      QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"fixStarted", false}})
                          .toJson(QJsonDocument::Compact)));
        SettingsController controller(fakeSettingsDrivesync(dir.path()));
        QSignalSpy changes(&controller, &SettingsController::changed);
        controller.setAccount("account2");
        QCOMPARE(controller.account(), QString("account2"));
        QVERIFY(changes.size() >= 1);
        controller.load();
        QTRY_VERIFY(controller.loaded());
        QTRY_COMPARE(controller.state(), QString("idle"));
        QCOMPARE(controller.intervalMinutes(), 30);
        QCOMPARE(calls(dir.path()), QStringList{"settings --account account2 --json"});

        // A change keeps the account it was queued with, and so does the read
        // after it, even when the account changes meanwhile.
        controller.setInterval(45);
        controller.setMaxDelete(10);
        controller.setAccount("account1");
        QCOMPARE(controller.account(), QString("account1"));
        QTRY_COMPARE(calls(dir.path()).size(), 4);
        QTRY_COMPARE(controller.state(), QString("idle"));
        QCOMPARE(calls(dir.path()).mid(1), (QStringList{"settings --account account2 --interval 45 --json",
                                                         "settings --account account2 --max-delete 10 --json",
                                                         "settings --account account2 --json"}));
        // What account2's read said is not account1's: nothing shows until
        // account1 is read.
        QVERIFY(!controller.loaded());
        controller.load();
        QTRY_VERIFY(controller.loaded());
        QCOMPARE(calls(dir.path()).last(), QString("settings --account account1 --json"));

        // Another account while idle drops what was read.
        controller.setAccount("account2");
        QVERIFY(!controller.loaded());
        QCOMPARE(controller.intervalMinutes(), 0);

        // No account: 5.1's exact commands.
        QTRY_COMPARE(controller.state(), QString("idle"));
        controller.setAccount("");
        const int before = int(calls(dir.path()).size());
        controller.load();
        QTRY_VERIFY(controller.loaded());
        controller.setInterval(60);
        QTRY_COMPARE(calls(dir.path()).size(), before + 3);
        QTRY_COMPARE(controller.state(), QString("idle"));
        QCOMPARE(calls(dir.path()).mid(before),
                 (QStringList{"settings --json", "settings --interval 60 --json", "settings --json"}));
        QVERIFY(controller.saved());
    }

    void settingsControllerKeepsFixStartedPerAccount()
    {
        QTemporaryDir dir;
        const QJsonObject settings{{"ok", true}, {"error", ""}, {"folder", "/h/Drive"}, {"remote", "gdrive:"},
                                   {"intervalMinutes", 30}, {"maxDelete", 5}, {"filtersFile", "/h/f.txt"},
                                   {"filters", "- .git/**\n"}, {"skipped", QJsonArray{}}, {"skipError", ""}};
        QVERIFY(write(dir.filePath("settings.json"), QJsonDocument(settings).toJson(QJsonDocument::Compact)));
        const auto answer = [&](bool fixStarted) {
            return write(dir.filePath("change-answer.json"),
                         QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"fixStarted", fixStarted}})
                             .toJson(QJsonDocument::Compact));
        };
        QVERIFY(answer(true));
        SettingsController controller(fakeSettingsDrivesync(dir.path()));
        QSignalSpy fixes(&controller, &SettingsController::fixStarted);

        // account2's filters start a fix while the window moves to account1
        // and reads it: that read waits behind the save.
        controller.setAccount("account2");
        controller.setFilters("- *.tmp\n");
        controller.setAccount("account1");
        controller.load();
        QTRY_VERIFY(calls(dir.path()).contains("settings --account account1 --json"));
        QTRY_COMPARE(controller.state(), QString("idle"));
        QVERIFY(controller.loaded());
        // account2's fix is not account1's: Settings stays open on account1.
        QCOMPARE(fixes.size(), 0);

        // account1's own change goes through at once: no fix to report.
        QVERIFY(answer(false));
        controller.setInterval(45);
        QTRY_COMPARE(calls(dir.path()).last(), QString("settings --account account1 --json"));
        QTRY_COMPARE(controller.state(), QString("idle"));
        QCOMPARE(fixes.size(), 0);
        QVERIFY(controller.saved());

        // Nor is account2's change that applied at once: "saved" would make
        // account1's Settings take its fields again and say it saved.
        controller.setAccount("account2");
        controller.setInterval(60);
        controller.setAccount("account1");
        controller.load();
        QTRY_COMPARE(calls(dir.path()).last(), QString("settings --account account2 --json"));
        QTRY_COMPARE(controller.state(), QString("idle"));
        QVERIFY(controller.loaded());
        QVERIFY(!controller.saved());
        QCOMPARE(fixes.size(), 0);
    }

    void folderTreeNamesTheAccount()
    {
        QTemporaryDir dir;
        fakeSettingsDrivesync(dir.path());
        const auto level = [](const QStringList &paths) {
            QJsonArray list;
            for (const QString &path : paths)
                list.append(QJsonObject{{"name", path.section('/', -1)}, {"path", path}, {"local", true},
                                        {"unsupported", false}});
            return QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"folders", list}}).toJson(QJsonDocument::Compact);
        };
        QVERIFY(write(dir.filePath("folders-root.json"), level({"Photos", "Work"})));
        QVERIFY(write(dir.filePath("folders-Work.json"), level({"Work/Old"})));
        FolderTree tree(dir.filePath("drive-sync"));
        tree.setAccount("account2");
        QCOMPARE(tree.account(), QString("account2"));
        tree.reset({}, "");
        QTRY_COMPARE(tree.rows().size(), 2);
        tree.expand(1);
        QTRY_COMPARE(tree.rows().size(), 3);
        QCOMPARE(calls(dir.path()), (QStringList{"folders --account account2 --json",
                                                 "folders --account account2 --json -- Work"}));

        // First setup: DriveSync's own remote, before any account exists.
        tree.setAccount("");
        tree.reset({}, "drive-sync:");
        QTRY_COMPARE(tree.rows().size(), 2);
        QCOMPARE(calls(dir.path()).last(), QString("folders --remote drive-sync: --json"));
    }

    void signInAgainNamesTheAccount()
    {
        // Again: the account's own remote, and no fallback that would remove
        // a half-made first-setup remote.
        QTemporaryDir dir;
        SetupController controller(fakeSetupDrivesync(dir.path()), dir.filePath("Drive"));
        controller.signIn("account1");
        QCOMPARE(controller.state(), QString("signingIn"));
        QTRY_VERIFY(QFile::exists(dir.filePath("child.pid")) && QFile::exists(dir.filePath("browser.pid")));
        const qint64 child = readBytes(dir.filePath("child.pid")).trimmed().toLongLong();
        const qint64 browser = readBytes(dir.filePath("browser.pid")).trimmed().toLongLong();
        const auto noBrowser = qScopeGuard([browser] { ::kill(static_cast<pid_t>(browser), SIGKILL); });
        QCOMPARE(calls(dir.path()).first(), QString("signin --account account1 --json"));
        controller.cancel();
        QCOMPARE(controller.state(), QString("idle"));
        QTRY_VERIFY(processGone(child));
        QTRY_VERIFY(calls(dir.path()).contains("cleaned"));
        QTest::qWait(500);
        QVERIFY2(!calls(dir.path()).contains("signin --abandon"), qPrintable(calls(dir.path()).join(" | ")));

        // First setup: no account, and the fallback runs after a cancel.
        QTemporaryDir first;
        SetupController setup(fakeSetupDrivesync(first.path()), first.filePath("Drive"));
        setup.signIn();
        QTRY_VERIFY(QFile::exists(first.filePath("child.pid")) && QFile::exists(first.filePath("browser.pid")));
        const qint64 firstBrowser = readBytes(first.filePath("browser.pid")).trimmed().toLongLong();
        const auto noFirstBrowser = qScopeGuard([firstBrowser] { ::kill(static_cast<pid_t>(firstBrowser), SIGKILL); });
        QCOMPARE(calls(first.path()).first(), QString("signin --json"));
        setup.cancel();
        QTRY_VERIFY(calls(first.path()).contains("signin --abandon"));
    }

    // Step 5.2, Task 8: one Backend per account the resolver lists.
    void accountListMakesOneBackendPerAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", successfulRun(now.addSecs(-600), 480)));
        const auto list = accountListOver(roots);
        QCOMPARE(list->count(), 2);
        QVERIFY(list->several());
        QCOMPARE(list->backends().size(), 2);
        QCOMPARE(list->backends().at(0)->accountId(), QString("account1"));
        QCOMPARE(list->backends().at(1)->accountId(), QString("account2"));
        QCOMPARE(list->backends().at(1)->serviceUnit(), QString("drive-sync@account2.service"));
        QCOMPARE(list->accounts().at(0).toMap().value("label").toString(), QString("Drive"));
        QCOMPARE(list->accounts().at(1).toMap().value("label").toString(), QString("Drive2"));
        QCOMPARE(list->accounts().at(1).toMap().value("folder").toString(), dir.filePath("Drive2"));
        QCOMPARE(list->current(), list->backends().at(0));
        QCOMPARE(list->selected(), 0);
        QCOMPARE(list->currentLabel(), QString("Drive"));
        QSignalSpy selected(list.get(), &AccountList::selectedChanged);
        list->select(1);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(list->current(), list->backends().at(1));
        QCOMPARE(list->selected(), 1);
        QCOMPARE(list->currentLabel(), QString("Drive2"));
        // The same one again, or one out of range, changes nothing.
        list->select(1);
        list->select(5);
        list->select(-1);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(list->selected(), 1);
    }

    void accountListKeepsTheSelectionById()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", successfulRun(now.addSecs(-600), 480)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account4", "Drive4", successfulRun(now.addSecs(-600), 480)));
        const auto list = accountListOver(roots);
        list->select(0);
        Backend *two = list->current();
        QCOMPARE(two->accountId(), QString("account2"));
        QSignalSpy selected(list.get(), &AccountList::selectedChanged);
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account3", "Drive3", successfulRun(now.addSecs(-600), 480)));
        list->reload();
        QCOMPARE(list->current(), two);
        QCOMPARE(list->count(), 3);
        QStringList ids;
        for (Backend *backend : list->backends())
            ids << backend->accountId();
        QCOMPARE(ids, (QStringList{"account2", "account3", "account4"}));
        QCOMPARE(selected.size(), 0);

        // account2 stays selected when one appears before it.
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive1", successfulRun(now.addSecs(-600), 480)));
        list->reload();
        QCOMPARE(list->current(), two);
        QCOMPARE(list->selected(), 1);
        QCOMPARE(selected.size(), 1);  // its index moved
    }

    void accountListAdoptsInPlaceWithOneAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        const auto list = accountListOver(roots);
        QCOMPARE(list->count(), 1);
        QVERIFY(!list->several());
        Backend *view = list->current();
        QCOMPARE(view->accountId(), QString());
        QVERIFY(!view->managed());
        QSignalSpy selected(list.get(), &AccountList::selectedChanged);

        // A first setup: the view becomes account1, the same object.
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        list->reload();
        QCOMPARE(list->current(), view);
        QCOMPARE(list->backends(), QList<Backend *>{view});
        QCOMPARE(view->accountId(), QString("account1"));
        QVERIFY(view->managed());
        QCOMPARE(view->serviceUnit(), QString("drive-sync@account1.service"));
        QCOMPARE(list->currentLabel(), QString("Drive"));

        // Cancelled before its base: back to the view, still the same object.
        QVERIFY(QDir(roots.configRoot + "/accounts/account1").removeRecursively());
        list->reload();
        QCOMPARE(list->current(), view);
        QCOMPARE(view->accountId(), QString());
        QVERIFY(!view->managed());
        QCOMPARE(selected.size(), 0);
    }

    void accountListFollowsTheMove()
    {
        QTemporaryDir dir;
        MoveHome h = makeMoveHome(dir.path());
        QVERIFY(write(dir.filePath("drive-sync.log"), successfulRun(QDateTime::currentDateTime().addSecs(-600), 480).toUtf8()));
        const AccountRoots roots = h.roots;
        const auto list = accountListOver(roots);
        Backend *backend = list->current();
        QCOMPARE(backend->accountId(), QString("account1"));
        QCOMPARE(backend->serviceUnit(), QString("drive-sync.service"));
        QSignalSpy selected(list.get(), &AccountList::selectedChanged);

        // A live move caught halfway: the resolver sees no account at all,
        // and the list keeps account1 until it ends.
        QVERIFY(write(takeoverLockPath(roots.stateRoot), QByteArray::number(QCoreApplication::applicationPid()) + "\n"));
        QVERIFY(write(migrationJournalPath(roots.stateRoot), "{}\n"));
        QVERIFY(QDir().mkpath(roots.configRoot + "/accounts/account1"));
        list->reload();
        QTest::qWait(400);
        QCOMPARE(list->current(), backend);
        QCOMPARE(backend->accountId(), QString("account1"));
        QCOMPARE(backend->serviceUnit(), QString("drive-sync.service"));
        QVERIFY(backend->managed());
        QVERIFY(QDir(roots.configRoot + "/accounts").removeRecursively());
        QVERIFY(QFile::remove(migrationJournalPath(roots.stateRoot)));
        QVERIFY(QFile::remove(takeoverLockPath(roots.stateRoot)));

        const MoveOutcome outcome = moveToAccounts(h.roots, h.tools);
        QVERIFY2(outcome.result == "moved", qPrintable(outcome.result + ": " + outcome.reason));
        list->reload();
        QCOMPARE(list->current(), backend);
        QCOMPARE(list->backends(), QList<Backend *>{backend});
        QCOMPARE(backend->serviceUnit(), QString("drive-sync@account1.service"));
        QVERIFY(backend->managed());
        QCOMPARE(selected.size(), 0);
    }

    void accountListDropsAGoneAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", successfulRun(now.addSecs(-600), 480)));
        const auto list = accountListOver(roots);
        list->select(1);
        Backend *one = backendOf(*list, "account1");
        Backend *two = list->current();
        QCOMPARE(two->accountId(), QString("account2"));
        QStringList order;
        connect(list.get(), &AccountList::selectedChanged, this, [&] {
            // The window points "backend" at the new one here: it must not be
            // the one about to go.
            order << "selected:" + list->current()->accountId();
        });
        connect(two, &QObject::destroyed, this, [&] { order << "destroyed"; });
        QVERIFY(QDir(roots.configRoot + "/accounts/account2").removeRecursively());
        list->reload();
        QCOMPARE(list->current(), one);
        QCOMPARE(list->count(), 1);
        QTRY_COMPARE(order, (QStringList{"selected:account1", "destroyed"}));
    }

    void dotsFollowEachBackend()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2",
                                    successfulRun(now.addSecs(-900), 300) + failedRun(now.addSecs(-200))));
        const auto list = accountListOver(roots);
        QCOMPARE(list->selected(), 0);
        QSignalSpy accounts(list.get(), &AccountList::accountsChanged);
        // Until systemd answers, every headline is notLoaded.
        QTRY_COMPARE(list->accounts().at(1).toMap().value("status").toString(), QString("failed"));
        QCOMPARE(list->accounts().at(1).toMap().value("tone").toString(), QString("error"));
        QTRY_COMPARE(list->accounts().at(0).toMap().value("tone").toString(), QString("ok"));
        // A run that ends well reaches the dot of an account not shown.
        QFile log(dir.filePath("account2.log"));
        QVERIFY(log.open(QIODevice::Append));
        log.write(successfulRun(now.addSecs(-60), 50).toUtf8());
        log.close();
        QTRY_COMPARE(list->accounts().at(1).toMap().value("tone").toString(), QString("ok"));
        QVERIFY(accounts.size() >= 1);
        QCOMPARE(list->selected(), 0);
    }

    void selectWraps()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        for (const QString &id : {QString("account1"), QString("account2"), QString("account3")})
            QVERIFY(makeLabelledAccount(roots, dir.path(), id, "Drive" + id.right(1), successfulRun(now.addSecs(-600), 480)));
        const auto list = accountListOver(roots);
        list->selectPrevious();
        QCOMPARE(list->selected(), 2);
        list->selectNext();
        QCOMPARE(list->selected(), 0);
        list->selectNext();
        QCOMPARE(list->selected(), 1);
        list->selectPrevious();
        QCOMPARE(list->selected(), 0);

        // With one account there is nothing to move to.
        QTemporaryDir single;
        AccountRoots singleRoots = accountRoots(single.path());
        singleRoots.base.systemctl = fakeSystemctl(single.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(singleRoots, single.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        const auto one = accountListOver(singleRoots);
        QSignalSpy selected(one.get(), &AccountList::selectedChanged);
        one->selectNext();
        one->selectPrevious();
        QCOMPARE(one->selected(), 0);
        QCOMPARE(selected.size(), 0);
    }

    void openSelectsTheAccount()
    {
        QTemporaryDir dir;
        AccountRoots roots = accountRoots(dir.path());
        const QDateTime now = QDateTime::currentDateTime();
        roots.base.systemctl = fakeSystemctl(dir.path(), now.addSecs(-1400));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account1", "Drive", successfulRun(now.addSecs(-600), 480)));
        QVERIFY(makeLabelledAccount(roots, dir.path(), "account2", "Drive2", successfulRun(now.addSecs(-600), 480)));
        const auto list = accountListOver(roots);
        QVERIFY(list->selectId("account2"));
        QCOMPARE(list->current()->accountId(), QString("account2"));
        QVERIFY(!list->selectId("account9"));
        QCOMPARE(list->current()->accountId(), QString("account2"));

        // What "drive-sync open" checks before the window exists.
        const AccountSet set = resolveAccounts(roots);
        QCOMPARE(chooseOpenAccount({"--account", "account2"}, set).id, QString("account2"));
        QCOMPARE(chooseOpenAccount({"--account", "account9"}, set).error,
                 QString("no account account9; accounts: account1, account2"));
        const OpenChoice plain = chooseOpenAccount({"-platform", "offscreen"}, set);
        QVERIFY(plain.id.isEmpty() && plain.error.isEmpty());
    }
};

QTEST_MAIN(DrivesyncTests)
#include "drive-sync-tests.moc"
