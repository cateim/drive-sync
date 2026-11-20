// The DriveSync window, offscreen, against a made-up drive-sync. It loads the
// real Main.qml with a fake log, a fake systemctl, a fake omarchy and a fake
// drive-sync, presses keys, and saves what the window shows as a PNG. Nothing
// opens on the desktop and nothing of this machine is read (unless --real),
// so the pictures can go into the README.
//
//   bin/harness --out build/shot.png [options]   (build/shot.png by default)
//   --lang en|pt-BR                      the Omarchy language
//   --scenario ok|running|failed|clean|empty|maxdelete   clean: no conflict copies; empty: no run in the log;
//                                        maxdelete: the last run stopped at the guard, 2 + 1800 deletions;
//                                        guardfilters: that stop, then a run refused for edited filters;
//                                        keptguard: that stop, after a confirmation that kept the guard;
//                                        unconfirmed: the run let through deleted 2 files nobody confirmed;
//                                        setup-offer: no drive-sync at all (empty log, takeover "none");
//                                        setup-signing-in: that, with a sign-in that waits for the browser;
//                                        setup-folder: that, with a sign-in that ends at once;
//                                        signin-needed: the last run failed on a revoked token (implies --managed;
//                                        with --fix failed, the fix stopped on it)
//                                        config-error: account1's config cannot be read (implies --managed)
//                                        stale-config: account1 in accounts/, and an old config.json beside it
//                                        that is ignored (implies --managed)
//                                        move-stuck: the move to accounts stopped halfway (its journal says
//                                        stuck; implies --managed)
//   --clock "<Qt format>"                the bar clock; without it, the system locale's hours
//   --pause none|hour|forever|ended      a pause mark; --managed: DriveSync's own units
//   --takeover managed|none|offer|running|blocked|failed   managed by default: "none" means no
//                                        drive-sync at all, which offers first setup
//   --bar none|offer|blocked             what the fake omarchy says about the widget
//   --fix none|fixing|confirm|questions|failed   a fix at that step (implies --managed);
//                                        confirm comes with a dry run of 1200 copies
//   --fix-later                          writes that fix after the window is up, so it opens by itself
//   --keys <letters>                     t Tab, digits (1 and 2 the tabs), h ?, d Down, e Esc,
//                                        s Space, p P, o T, f F, D D, c C, l L, x X, r Enter,
//                                        b Backspace, w waits a second, I I, Q Q (typed into a field
//                                        it must not quit), "," the settings, ^ Ctrl with the next
//                                        key (^2 Ctrl+2, ^s Ctrl+S), > Right, < Left, ~ the next
//                                        character typed as it is (~J types a J)
//   --settings-interval N --settings-max-delete N   what the fake "settings --json" says (15 and 5)
//   --slow-settings                      that answer takes a second, as a slow disk or a busy CPU can
//   --width N --height N                 1000x700 by default
//   --real                               this machine's drive-sync: never for the README

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "accounts.h"
#include "backend.h"
#include "clockformat.h"
#include "fixstate.h"
#include "pausestate.h"
#include "pluginsetup.h"
#include "settingscontroller.h"
#include "filtereditor.h"
#include "foldertree.h"
#include "localtree.h"
#include "setupcontroller.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(path));
    f.write(data);
}

static QString L(const QDateTime &t, const QString &level, const QString &m) {
    return QString("%1 %2: %3\n").arg(t.toString("yyyy/MM/dd HH:mm:ss"), level.leftJustified(6), m);
}
static QString C(const QString &tag, const QString &what, const QString &path) {
    return QString("- %1%2 - %3").arg(tag.leftJustified(18), what.leftJustified(43), path);
}
static const QString kStart = R"(Synching Path1 "gdrive{ab12}:/" with Path2 "/home/someone/Drive/")";

static QString okRun(const QDateTime &s, int secs, const QStringList &cloud, const QStringList &notebook) {
    QString log = L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
    log += L(s.addSecs(secs - 20), "INFO", "Path1 checking for diffs");
    for (const auto &p : cloud) log += L(s.addSecs(secs - 20), "INFO", C("Path1", "File changed: size (larger), time (newer)", p));
    log += L(s.addSecs(secs - 20), "INFO", QString("Path1:    %1 changes:    0 new,    %1 modified,    0 deleted").arg(cloud.size()));
    log += L(s.addSecs(secs - 20), "INFO", "Path2 checking for diffs");
    for (const auto &p : notebook) log += L(s.addSecs(secs - 20), "INFO", C("Path2", "File is new", p));
    log += L(s.addSecs(secs - 20), "INFO", QString("Path2:    %1 changes:    %1 new,    0 modified,    0 deleted").arg(notebook.size()));
    log += L(s.addSecs(secs - 19), "INFO", "Applying changes");
    log += L(s.addSecs(secs), "INFO", "Bisync successful");
    return log;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    auto opt = [&](const QString &n, const QString &d) { int i = args.indexOf(n); return i >= 0 && i + 1 < args.size() ? args[i + 1] : d; };
    const QString lang = opt("--lang", "en"), scenario = opt("--scenario", "ok"), keys = opt("--keys", ""), out = opt("--out", "build/shot.png");
    const int width = opt("--width", "1000").toInt(), height = opt("--height", "700").toInt();

    QTemporaryDir dir;
    put(dir.filePath("language"), lang.toUtf8() + "\n");
    // "~" in the window is this temp folder, never the real home: the
    // filters screen lists the synced folder, and pictures from here go in
    // the README. Unless --real, which reads this machine on purpose.
    const bool realMachine = args.contains("--real");
    if (!realMachine)
        qputenv("HOME", QFile::encodeName(dir.path()));
    const QDateTime now = QDateTime::currentDateTime();

    QString log;
    QStringList cloudFiles{"Notes/ideas.md", "Documents/budget-2026.ods"};
    QStringList localFiles{"GitHub/my-app/README.md", "GitHub/my-app/src/main.mjs", "Photos/trip/beach-01.jpg"};
    for (int i = 6; i >= 1; --i)
        log += okRun(now.addSecs(-i * 900 - 500), 470 + i * 7, i % 2 ? cloudFiles.mid(0, 1) : cloudFiles,
                     i == 1 ? localFiles : i == 3 ? QStringList{"Notes/groceries.md"} : QStringList{});
    QString active = "inactive";
    QDateTime confirmedAt;  // unconfirmed: when the run let through took its confirmation
    if (scenario == "failed") {
        const QDateTime s = now.addSecs(-200);
        log += L(s, "INFO", kStart);
        log += L(s, "ERROR", "Bisync critical error: cannot find prior Path1 or Path2 listings, likely due to critical error on prior run ");
        log += L(s, "ERROR", "Bisync aborted. Error is retryable without --resync due to --resilient mode.");
    } else if (scenario == "running") {
        const QDateTime s = now.addSecs(-250);
        log += L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
        log += L(now.addSecs(-10), "INFO", "");
        log += "Transferred:   \t 0 B / 0 B, -, 0 B/s, ETA -\nChecks:  41000 / 41000, 100%, Listed 41210\nElapsed time:  4m0.0s\n\n";
        active = "activating";
    } else if (scenario == "maxdelete" || scenario == "guardfilters" || scenario == "keptguard") {
        // What rclone 1.75.2 writes when --max-delete stops a run: each
        // deletion, both sides, then the abort for the side over the guard.
        const QDateTime s = now.addSecs(-200);
        if (scenario == "keptguard")
            log += L(s, "INFO", "DriveSync kept --max-delete at 5% for this run: a dry run finds 3 deletions not in the "
                                "list that was confirmed (gone from the cloud: 2, gone from the notebook: 1800)");
        log += L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
        log += L(s, "INFO", "Path1 checking for diffs");
        for (const QString &p : {QString("Notes/old-draft.md"), QString("Documents/budget-2025.ods")})
            log += L(s, "INFO", C("Path1", "File was deleted", p));
        log += L(s, "INFO", "Path1:    2 changes:    0 new,    0 modified,    2 deleted");
        log += L(s, "INFO", "Path2 checking for diffs");
        for (int i = 0; i < 1800; ++i)
            log += L(s, "INFO", C("Path2", "File was deleted", QString("Photos/2024/trip-%1.jpg").arg(i, 4, 10, QChar('0'))));
        log += L(s, "INFO", "Path2: 1800 changes:    0 new,    0 modified, 1800 deleted");
        log += L(s, "ERROR", R"(Safety abort: too many deletes (>5%, 1800 of 33411) on Path2 "/home/someone/Drive/". Run with --force if desired.)");
        log += L(s, "NOTICE", "Bisync aborted. Please try again.");
        log += L(s, "NOTICE", "Failed to bisync: too many deletes");
        if (scenario == "guardfilters")
            log += L(now.addSecs(-60), "CRITICAL", "DriveSync did not start rclone: /home/someone/.config/drive-sync/filters.txt "
                                                   "changed since the last resync, so bisync would lose its base");
    } else if (scenario == "signin-needed") {
        // What rclone 1.75 writes when Google refuses the refresh token.
        log += L(now.addSecs(-200), "ERROR",
                 "Failed to create file system for \"gdrive:\": couldn't find root directory ID: Get "
                 "\"https://www.googleapis.com/drive/v3/files/root\": couldn't fetch token: invalid_grant: maybe token "
                 "expired? - try refreshing with \"rclone config reconnect gdrive:\"");
    } else if (scenario == "unconfirmed") {
        // The run let through: one file on each side went after the dry run.
        const QDateTime s = now.addSecs(-120);
        confirmedAt = s.toUTC().addSecs(-5);
        log += L(s, "INFO", kStart) + L(s, "INFO", "Building Path1 and Path2 listings");
        log += L(s, "INFO", C("Path1", "File was deleted", "Photos/2024/trip-0001.jpg"));
        log += L(s, "INFO", C("Path2", "File was deleted", "Notes/old-draft.md"));
        log += L(s.addSecs(30), "INFO", "Bisync successful");
    }
    // No drive-sync at all: nothing has run yet.
    const bool firstSetup = scenario.startsWith("setup-");
    if (scenario == "empty" || firstSetup)
        log.clear();  // a new or just rotated log
    put(dir.filePath("drive-sync.log"), log.toUtf8());

    if (scenario != "clean" && scenario != "empty" && !firstSetup) {
        put(dir.filePath("Drive/Notes/ideas.md.conflict1"), QByteArray(2400, 'x'));
        put(dir.filePath("Drive/Spreadsheets/bills.xlsx.conflict1"), QByteArray(18300, 'x'));
    } else {
        QDir().mkpath(dir.filePath("Drive"));
    }
    // A made-up synced folder for the filters screen's tree, with a link.
    for (const char *file : {"Drive/Documents/budget-2026.ods", "Drive/GitHub/my-app/build/app.o",
                             "Drive/GitHub/my-app/README.md", "Drive/Photos/trip/beach-01.jpg", "Drive/notes.txt"})
        put(dir.filePath(file), "x");
    QFile::link(dir.filePath("Drive/notes.txt"), dir.filePath("Drive/GitHub/my-app/notes.txt"));

    const QString fix = opt("--fix", "none");
    const QString systemctl = dir.filePath("systemctl");
    // One show block per unit asked, starting with its Id, as systemd answers:
    // the window maps the answers by Id. list-timers names each timer asked.
    put(systemctl, QString(R"sh(#!/bin/sh
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
                       .arg(now.addSecs(380).toMSecsSinceEpoch() * 1000).arg(active).arg(now.addSecs(-1400).toSecsSinceEpoch())
                       .arg(fix == "fixing" ? "activating" : "inactive").toUtf8());
    QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QQuickStyle::setStyle("Material");
    Translator translator(dir.filePath("language"));
    app.installTranslator(&translator);
    Theme theme(dir.filePath("omarchy"));
    const bool real = args.contains("--real");
    BackendPaths fake{dir.filePath("drive-sync.log"), dir.filePath("Drive"), dir.filePath("state"),
                      dir.filePath("proc"), systemctl, QStandardPaths::findExecutable("find"), dir.filePath("app"),
                      QString(), QString()};
    fake.managed = args.contains("--managed") || fix != "none" || scenario == "signin-needed" || scenario == "config-error"
                   || scenario == "stale-config" || scenario == "move-stuck";
    // The resolver reads these made-up roots, as the window reads the home.
    AccountRoots roots;
    roots.configRoot = dir.filePath("config");
    roots.stateRoot = fake.appState;
    roots.userUnits = dir.filePath("units");
    roots.base = fake;
    // A valid config for account1, naming the made-up log and folder.
    auto writeConfig = [&](const QString &file) {
        const QJsonObject config{{"version", 1}, {"remote", "gdrive:"}, {"localPath", fake.drive},
                                 {"workDir", fake.appState + "/bisync"}, {"filtersFile", dir.filePath("config/filters.txt")},
                                 {"logFile", fake.log}, {"rcloneFlags", QJsonArray()}, {"runOnlyWhen", ""}};
        put(file, QJsonDocument(config).toJson());
    };
    if (scenario == "stale-config") {
        writeConfig(dir.filePath("config/accounts/account1/config.json"));
        writeConfig(dir.filePath("config/config.json"));
    }
    if (scenario == "move-stuck") {
        writeConfig(dir.filePath("config/config.json"));
        put(migrationJournalPath(roots.stateRoot),
            R"({"version": 1, "account": "account1", "moves": [], "created": [], "plainTimerEnabled": true, )"
            R"("plainTimerActive": true, "stuck": true, "stuckReason": "paused.json is at both places"})");
    }
    if (scenario == "config-error") {
        // An account whose config.json is no longer JSON: listed, managed,
        // and its headline says why it cannot be read.
        fake.id = "account1";
        fake.config = dir.filePath("config/config.json");
        QDir().mkpath(dir.filePath("config"));
        put(fake.config, "{\"version\": 1, \"remote\": ");
    }
    if (confirmedAt.isValid()) {
        ConfirmedRun run;
        run.at = confirmedAt;
        run.backupDir = "/home/someone/.local/state/drive-sync-app/allowed-backup/20261114-101500";
        writeConfirmedRun(fake.appState, run);
        checkConfirmedRun(fake.appState, fake.log);
    }
    const bool fixLater = args.contains("--fix-later");
    auto writeTheFix = [&] {
        FixState state;
        state.step = fix == "fixing" ? "base" : fix;
        state.started = QDateTime::currentDateTimeUtc().addSecs(-1500);
        state.stepStarted = QDateTime::currentDateTimeUtc().addSecs(-300);
        if (fix == "confirm") {
            // A big dry run: the dialog lists the first 500 and counts the rest.
            QByteArray dryRun =
                "2026/11/11 12:00:01 NOTICE: - Path1             Resync is copying files to                  - Path2\n"
                "2026/11/11 12:00:01 NOTICE: Notes/old-draft.md: Skipped copy as --dry-run is set (size 2400)\n"
                "2026/11/11 12:00:01 NOTICE: Documents/budget-2026.ods: Skipped copy as --dry-run is set (size 18300)\n"
                "2026/11/11 12:00:01 NOTICE: - Path2             Resync is copying files to                  - Path1\n";
            for (int i = 0; i < 1200; ++i)
                dryRun += QString("2026/11/11 12:00:01 NOTICE: Photos/2025/trip-%1.jpg: Skipped copy as --dry-run is set "
                                  "(size 2400000)\n").arg(i, 4, 10, QChar('0')).toUtf8();
            put(fixDryRunLogPath(fake.appState), dryRun);
            state.copies = 1202;
        }
        if (fix == "questions") {
            const qint64 t = QDateTime::currentSecsSinceEpoch();
            FixQuestion edited{"Notes/ideas.md", "differs", 2600, 2400, t - 1300, t - 5400, ""};
            FixQuestion added{"Documents/budget-2026.ods", "onlyCloud", 18300, -1, t - 900, 0, ""};
            FixQuestion sheet{"Spreadsheets/bills.xlsx", "differs", 19100, 18300, t - 1100, t - 86400, ""};
            state.questions = {added, edited, sheet};
        }
        if (fix == "failed") {
            state.failedStep = "base";
            state.error = scenario == "signin-needed"
                ? "Failed to create file system for \"gdrive:\": couldn't fetch token: invalid_grant: maybe token expired? - "
                  "try refreshing with \"rclone config reconnect gdrive:\""
                : "Bisync critical error: prior lock file found: ~/.local/state/rclone/bisync/gdrive_..lck";
        }
        writeFix(fake.appState, state);
    };
    if (fix != "none" && !fixLater)
        writeTheFix();
    const QString pause = opt("--pause", "none");
    if (pause != "none") {
        PauseMark mark;
        mark.present = true;
        mark.since = QDateTime::currentDateTimeUtc().addSecs(-1800);
        if (pause == "hour") mark.until = QDateTime::currentDateTimeUtc().addSecs(3000);
        if (pause == "ended") mark.until = QDateTime::currentDateTimeUtc().addSecs(-60);
        writePause(pauseFilePath(fake.appState), mark);
    }
    // The README pictures show a drive-sync DriveSync already runs; "none"
    // (no drive-sync at all) is what offers first setup.
    const QString takeover = firstSetup ? QString("none") : opt("--takeover", "managed");
    const QByteArray answer =
        takeover == "offer" ? R"({"ok":true,"error":"","state":"found","found":["/home/someone/.config/systemd/user/drive-sync.service","/home/someone/.config/systemd/user/drive-sync.timer","/home/someone/.local/bin/drive-sync.sh","/home/someone/.config/rclone/gdrive-filtros.txt"],"blockers":[]})"
        : takeover == "running" ? R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running"]})"
        : takeover == "blocked" ? R"({"ok":true,"error":"","state":"found","found":[],"blockers":["running","differentValue:Nice"]})"
        : takeover == "failed" ? R"({"ok":false,"error":"could not stop the old timers: Access denied","state":"","found":[],"blockers":[]})"
        : takeover == "none" ? R"({"ok":true,"error":"","state":"none","found":[],"blockers":[]})"
        : R"({"ok":true,"error":"","state":"managed","found":[],"blockers":[]})";
    put(dir.filePath("answer.json"), answer);
    // The fake drive-sync answers every child call with the takeover check,
    // and logs the calls, which are printed at the end. Its sign-in waits for
    // a browser that never answers, or, for setup-folder, ends at once. The
    // window's "signin --abandon" runs detached and can outlive the temp dir.
    const QString signinAnswer = scenario == "setup-folder"
        ? "echo '{\"ok\":true,\"error\":\"\"}'; exit 0"
        : "echo '{\"link\":\"http://127.0.0.1:53682/auth?state=k3Jv9QxT2mWb7LpA\"}'; exec sleep 30";
    // Settings and the cloud's folders: made up, never this machine's.
    const QJsonObject settings{{"ok", true}, {"error", ""}, {"folder", "/home/someone/Drive"}, {"remote", "gdrive:"},
                               {"intervalMinutes", opt("--settings-interval", "15").toInt()},
                               {"maxDelete", opt("--settings-max-delete", "5").toInt()},
                               {"filtersFile", "/home/someone/.config/drive-sync/filters.txt"},
                               {"filters", "# BEGIN DriveSync: cloud folders left out of this computer\n- /Archive/2019/**\n"
                                           "# END DriveSync: cloud folders left out of this computer\n\n"
                                           "# What DriveSync does not sync between your folder and Google Drive.\n"
                                           "- .git/**\n- node_modules/**\n- .venv/**\n- *.gdoc\n- desktop.ini\n"},
                               {"skipped", QJsonArray{"Archive/2019"}}, {"skipError", ""}};
    put(dir.filePath("settings.json"), QJsonDocument(settings).toJson(QJsonDocument::Compact));
    auto level = [](const QStringList &paths, const QStringList &local) {
        QJsonArray list;
        for (const QString &path : paths)
            list.append(QJsonObject{{"name", path.section('/', -1)}, {"path", path}, {"local", local.contains(path)},
                                    {"unsupported", false}});
        return QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"folders", list}}).toJson(QJsonDocument::Compact);
    };
    put(dir.filePath("folders-root.json"), level({"Archive", "Documents", "Notes", "Photos", "Spreadsheets"},
                                                 {"Archive", "Documents", "Notes", "Photos", "Spreadsheets"}));
    put(dir.filePath("folders-Archive.json"), level({"Archive/2019", "Archive/2020"}, {"Archive/2019", "Archive/2020"}));
    put(dir.filePath("drive-sync"),
        QString("#!/bin/sh\necho \"$*\" >> '%2'\n[ \"$*\" = 'signin --json' ] && { %3; }\n"
                "case \"$1\" in\n"
                "  settings) if [ \"$2\" = --json ]; then %5cat '%4/settings.json'; else echo '{\"ok\":true,\"error\":\"\",\"fixStarted\":false}'; fi; exit 0 ;;\n"
                "  accounts) echo '%6'; exit %7 ;;\n"
                "  folders) case \" $* \" in *\" -- \"*) for last in \"$@\"; do :; done; cat \"%4/folders-$last.json\" 2>/dev/null || echo '{\"ok\":true,\"error\":\"\",\"folders\":[]}' ;; *) cat '%4/folders-root.json' ;; esac; exit 0 ;;\n"
                "esac\n"
                "cat '%1' 2>/dev/null\n")
            .arg(dir.filePath("answer.json"), dir.filePath("calls.log"), signinAnswer, dir.path(),
                 args.contains("--slow-settings") ? QString("sleep 1; ") : QString(),
                 // "accounts --migrate --json": the move to account1, which
                 // only the move-stuck scenario sees stop halfway.
                 scenario == "move-stuck"
                     ? QString(R"({"ok":false,"error":"the move to accounts stopped halfway","mode":"legacy","accounts":[],)"
                               R"("moveBlocker":"","notices":[],"move":{"result":"stuck","reason":"paused.json is at both places"}})")
                     : QString(R"({"ok":true,"error":"","mode":"accounts","accounts":[],"moveBlocker":"","notices":[],)"
                               R"("move":{"result":"notNeeded","reason":""}})"),
                 scenario == "move-stuck" ? QString("1") : QString("0"))
            .toUtf8());
    QFile::setPermissions(dir.filePath("drive-sync"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    fake.self = dir.filePath("drive-sync");
    roots.base.self = fake.self;
    // As main does: the window follows the resolver over the roots. With no
    // account there, the made-up paths stand in for one, so every older
    // scenario (a managed drive-sync with no config file) looks as before.
    const AccountRoots realRoots = real ? AccountRoots::defaults() : AccountRoots();
    const std::function<WindowAccount()> resolve = [&]() -> WindowAccount {
        if (real)
            return windowAccount(realRoots);
        WindowAccount account = windowAccount(roots);
        if (account.paths.id.isEmpty())
            account.paths = fake;
        return account;
    };
    Backend backend(resolve().paths);
    backend.setResolver(resolve);
    // The move runs as a child of the fake drive-sync, never with --real,
    // where it would be this machine's move.
    if (!real && (readAccountLayout(roots.configRoot).mode == "legacy" || moveInProgress(roots.stateRoot)))
        backend.startMove();
    TakeoverSetup takeoverSetup(dir.filePath("drive-sync"));
    takeoverSetup.check();
    SetupController setupController(dir.filePath("drive-sync"), dir.filePath("Drive"));
    SettingsController settingsController(dir.filePath("drive-sync"));
    FolderTree folderTree(dir.filePath("drive-sync"));
    LocalTree localTree;
    FilterEditor filterEditor;

    // none: the widget is already on the bar, so no banner shows.
    const QString bar = opt("--bar", "none");
    const QByteArray list = bar == "offer" ? "[]" : R"([{"id":"drive-sync","enabled":true}])";
    put(dir.filePath("list.json"), list);
    put(dir.filePath("omarchy"), QString("#!/bin/sh\ncase \"$*\" in\n  'plugin list --json') cat '%1' ;;\nesac\n")
                                     .arg(dir.filePath("list.json")).toUtf8());
    QFile::setPermissions(dir.filePath("omarchy"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir().mkpath(dir.filePath("shipped"));
    if (bar == "blocked")
        QDir().mkpath(dir.filePath("plugins/drive-sync"));
    PluginSetup pluginSetup({dir.filePath("shipped"), dir.filePath("plugins"), dir.filePath("omarchy"), QString(), QString(), QString()});
    pluginSetup.check();

    // The bar clock lives in shell.json; without --clock the bar has none.
    const QString clock = opt("--clock", "");
    QJsonObject clockEntry{{"id", "omarchy.clock"}, {"format", clock}};
    QJsonObject layout;
    layout["center"] = QJsonArray{clockEntry};
    QJsonObject barConfig;
    barConfig["layout"] = layout;
    QJsonObject shell;
    shell["bar"] = barConfig;
    put(dir.filePath("shell.json"), clock.isEmpty() ? QByteArray("{}") : QJsonDocument(shell).toJson());
    ClockFormat clockFormat(ClockPaths{{dir.filePath("shell.json")}, QLocale::system()});
    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("clockFormat", &clockFormat);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("pluginSetup", &pluginSetup);
    engine.rootContext()->setContextProperty("takeoverSetup", &takeoverSetup);
    engine.rootContext()->setContextProperty("setupController", &setupController);
    engine.rootContext()->setContextProperty("settingsController", &settingsController);
    engine.rootContext()->setContextProperty("folderTree", &folderTree);
    engine.rootContext()->setContextProperty("localTree", &localTree);
    engine.rootContext()->setContextProperty("filterEditor", &filterEditor);
    // Qt.quit() from a shortcut: said, so a key that must not quit is caught.
    QObject::connect(&engine, &QQmlEngine::quit, [] { printf("quit asked\n"); });
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    window->resize(width, height);
    QTest::qWait(1500);
    if (fix != "none" && fixLater) {
        writeTheFix();
        QTest::qWait(400);
    }

    bool ctrl = false, literal = false;
    for (const QChar k : keys) {
        if (literal) {
            QTest::keyClick(window, k.toLatin1());
            literal = false;
        } else if (k == '~') { literal = true; continue; }
        else if (k == '^') { ctrl = true; continue; }
        else if (ctrl) {
            QTest::keyClick(window, k.isDigit() ? Qt::Key(Qt::Key_0 + k.digitValue()) : Qt::Key(Qt::Key_A + (k.toLower().toLatin1() - 'a')),
                            Qt::ControlModifier);
            ctrl = false;
        }
        else if (k == ',') QTest::keyClick(window, Qt::Key_Comma);
        else if (k == '>') QTest::keyClick(window, Qt::Key_Right);
        else if (k == '<') QTest::keyClick(window, Qt::Key_Left);
        else if (k == 't') QTest::keyClick(window, Qt::Key_Tab);
        else if (k == 'h') QTest::keyClick(window, Qt::Key_Question, Qt::ShiftModifier);
        else if (k == 'd') QTest::keyClick(window, Qt::Key_Down);
        else if (k == 'e') QTest::keyClick(window, Qt::Key_Escape);
        else if (k == 's') QTest::keyClick(window, Qt::Key_Space);
        else if (k == 'p') QTest::keyClick(window, Qt::Key_P);
        else if (k == 'o') QTest::keyClick(window, Qt::Key_T);
        else if (k == 'f') QTest::keyClick(window, Qt::Key_F);
        else if (k == 'D') QTest::keyClick(window, Qt::Key_D);
        else if (k == 'b') QTest::keyClick(window, Qt::Key_Backspace);
        else if (k == 'c') QTest::keyClick(window, Qt::Key_C);
        else if (k == 'l') QTest::keyClick(window, Qt::Key_L);
        else if (k == 'x') QTest::keyClick(window, Qt::Key_X);
        else if (k == 'r') QTest::keyClick(window, Qt::Key_Return);
        else if (k == 'w') QTest::qWait(1100);
        else if (k == 'I') QTest::keyClick(window, Qt::Key_I);
        else if (k == 'Q') QTest::keyClick(window, Qt::Key_Q, Qt::NoModifier);
        else if (k.isDigit()) QTest::keyClick(window, Qt::Key(Qt::Key_0 + k.digitValue()));
        QTest::qWait(250);
    }
    QTest::qWait(400);
    window->grabWindow().save(out);
    printf("status=%s runs=%lld conflicts=%lld -> %s\n", qPrintable(backend.status()), (long long)backend.runs().size(),
           (long long)backend.conflicts().size(), qPrintable(out));
    // What the filters editor holds, when it is on screen: typing into it
    // must reach it and nothing else.
    if (QObject *editor = window->findChild<QObject *>("filtersEditor"); editor && keys.contains('~'))
        printf("filters editor: %s\nwindow page=%d help=%d\n",
               qPrintable(QString::fromUtf8(QJsonDocument(QJsonArray{filterEditor.text(localTree.excluded())}).toJson(QJsonDocument::Compact))),
               window->property("page").toInt(), window->property("helpVisible").toBool());
    if (!folderTree.rows().isEmpty())
        printf("tree left out: %s\n", qPrintable(folderTree.skipped().join(", ")));
    QFile calls(dir.filePath("calls.log"));
    if (calls.open(QIODevice::ReadOnly))
        printf("drive-sync calls:\n%s", calls.readAll().constData());
    return 0;
}
