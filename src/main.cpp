// drive-sync - shows how drive-sync, the rclone bisync of ~/Drive, is doing.

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include "accountlist.h"
#include "accountmove.h"
#include "accounts.h"
#include "backend.h"
#include "blockeddeletes.h"
#include "clockformat.h"
#include "controlcommands.h"
#include "firstsetup.h"
#include "fixcommand.h"
#include "legacymigration.h"
#include "notifier.h"
#include "pluginsetup.h"
#include "selectivesync.h"
#include "settingscommand.h"
#include "settingscontroller.h"
#include "filtereditor.h"
#include "foldertree.h"
#include "localtree.h"
#include "setupcontroller.h"
#include "signin.h"
#include "statuscommand.h"
#include "takeover.h"
#include "takeoversetup.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Before anything else: the conflict cache, the pause mark and the bar
    // offer all create their folder, and a new folder made first would leave
    // the live config behind in the old one.
    for (const QString &problem : migrateFromLegacyNames(QDir::homePath()))
        QTextStream(stderr) << "drive-sync: " << problem << "\n";

    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Commands that need no display run without a GUI application, so the
    // bar and the units can call them without touching Wayland.
    static const QStringList headless{"status", "sync", "pause", "resume", "run-condition", "run", "notify",
                                      "watchdog", "takeover", "fix", "fix-run", "deletes", "signin", "setup", "settings", "folders",
                                      "accounts"};
    if (headless.contains(command)) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("drive-sync");
        app.setApplicationVersion("0.1.0");
        QTextStream out(stdout);
        QTextStream err(stderr);
        const QStringList arguments = app.arguments().mid(2);
        // Resolved once; every command then takes "--account <id>" as its
        // first two arguments and acts on the accounts chooseAccounts gives.
        const AccountRoots roots = AccountRoots::defaults();
        const AccountSet set = resolveAccounts(roots);
        return dispatchToAccounts(
            command, arguments, set, roots, out, err,
            [&](const QStringList &rest, const QList<BackendPaths> &accounts) -> int {
                // Every command but status, sync, pause and resume gets one account.
                const BackendPaths &paths = accounts.first();
                // An old drive-sync next to accounts is looked for in the
                // real home here, never by a reader on its own.
                if (command == "status")
                    return runStatusCommand(rest, accounts, set.notices, out, err, 5000,
                                            oldDriveSyncCheck(TakeoverPaths::defaults()));
                if (command == "sync")
                    return runSyncCommand(rest, accounts, out, err);
                if (command == "pause")
                    return runPauseCommand(rest, accounts, QDateTime::currentDateTimeUtc(), out, err);
                if (command == "resume")
                    return runResumeCommand(rest, accounts, out, err);
                if (command == "run")
                    return runRunCommand(paths, err);
                // The move to account1 by hand; the window runs it as a child too.
                if (command == "accounts")
                    return runAccountsCommand(rest, roots, TakeoverPaths::defaults(), out, err);
                if (command == "takeover")
                    return runTakeoverCommand(rest, TakeoverPaths::defaults(), paths, out, err);
                if (command == "fix")
                    return runFixCommand(rest, paths, out, err);
                if (command == "deletes")
                    return runDeletesCommand(rest, paths, out, err);
                if (command == "signin") {
                    // The page the browser shows after Google is read by a
                    // person, in the Omarchy language, like the cards.
                    Translator translator;
                    app.installTranslator(&translator);
                    SignInTools tools = SignInTools::defaults();
                    tools.language = translator.language();
                    return runSignInCommand(rest, paths, tools, out, err);
                }
                // With nothing configured, setup writes account1 in the
                // accounts layout.
                if (command == "setup")
                    return runSetupCommand(rest, setupAccountPaths(roots, paths), TakeoverPaths::defaults(), out, err);
                if (command == "folders")
                    return runFoldersCommand(rest, paths, QStandardPaths::findExecutable("rclone"), out, err);
                if (command == "settings")
                    return runSettingsCommand(rest, paths, TakeoverPaths::defaults(), out, err);
                if (command == "notify" || command == "watchdog" || command == "fix-run") {
                    // The cards are read by a person, in the Omarchy language.
                    Translator translator;
                    app.installTranslator(&translator);
                    NotifierPaths tools = NotifierPaths::defaults(Translator::formatLocale(translator.language()));
                    // The bare 5.1 titles go once, and only with one account:
                    // each is part of every account's labelled title, so with
                    // two it would close both. The watchdog decides it after
                    // its own resolve.
                    if (command != "watchdog")
                        tools.bareTitlesMark =
                            set.accounts.size() == 1 ? paths.globalState() + "/notify-bare-titles.done" : QString();
                    const QDateTime now = QDateTime::currentDateTimeUtc();
                    if (command == "notify")
                        return runNotifyCommand(qEnvironmentVariable("SERVICE_RESULT"), paths, tools, now);
                    // The watchdog makes the move first, then resolves every
                    // account itself.
                    if (command == "watchdog")
                        return runWatchdogCommand(roots, TakeoverPaths::defaults(), tools, now);
                    QString error;
                    const auto config = loadSyncConfig(paths.config, &error);
                    return runFixUnit(paths, config.value_or(SyncConfig()), FixTools::defaults(), tools,
                                      config ? QString() : error);
                }
                // run-condition: 255 is not a skip for systemd but a failure,
                // which is what a missing or broken config is.
                QString error;
                const auto config = loadSyncConfig(paths.config, &error);
                if (!config) {
                    err << "drive-sync: " << error << "\n";
                    return 255;
                }
                const bool oldDriveSyncHold =
                    paths.accountsMode && oldDriveSyncHolds(readOldDriveSync(TakeoverPaths::defaults()), config->localPath);
                return runConditionCommand(paths, *config, QDateTime::currentDateTimeUtc(), oldDriveSyncHold);
            });
    }
    // Asked for, the usage goes to stdout and is not an error. Checked before
    // the dash rule below, which would open the window instead.
    if (command == "--help" || command == "-h" || command == "help") {
        QTextStream(stdout) << usageText();
        return 0;
    }
    // No argument or "open" starts the window. Arguments that start with a
    // dash are Qt's own (-platform, -style) and go to the window too.
    if (!command.isEmpty() && command != "open" && !command.startsWith('-')) {
        QTextStream(stderr) << usageText();
        return 1;
    }
    // "open --account <id>" opens the window on that account. Checked before
    // the window exists, so a wrong id fails at once, with no display, and
    // with chooseAccounts' words, as every command that takes --account.
    QString openAccount;
    if (command == "open") {
        QStringList afterOpen;
        for (int i = 2; i < argc; ++i)
            afterOpen << QString::fromLocal8Bit(argv[i]);
        const OpenChoice choice = chooseOpenAccount(afterOpen, resolveAccounts(AccountRoots::defaults()));
        if (!choice.error.isEmpty()) {
            QTextStream(stderr) << "drive-sync: " << choice.error << "\n";
            return 1;
        }
        openAccount = choice.id;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("drive-sync");
    app.setApplicationVersion("0.1.0");

    // Matches the window to drive-sync.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("drive-sync");
    app.setWindowIcon(QIcon::fromTheme("drive-sync"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    // Clock times show 12 or 24 hours like the clock on the Omarchy bar.
    ClockFormat clockFormat;
    // One Backend per account the resolver lists, and the one the window
    // shows; it follows the resolver, so a first setup, a takeover, the move
    // to account1 or an account added or gone needs no restart.
    const AccountRoots roots = AccountRoots::defaults();
    const OldDriveSyncCheck oldDriveSync = oldDriveSyncCheck(TakeoverPaths::defaults());
    AccountList accountList([roots] { return windowAccounts(roots); }, [oldDriveSync](const BackendPaths &paths) {
        auto *backend = new Backend(paths);
        // Each window Backend looks for an old drive-sync next to accounts.
        backend->setOldDriveSyncCheck(oldDriveSync);
        return backend;
    });
    if (!openAccount.isEmpty())
        accountList.selectId(openAccount);
    // The move to account1, in a child, when there is something to move or a
    // journal to roll back; the watchdog and "drive-sync accounts --migrate"
    // start it too. Once, whatever the number of accounts.
    if (readAccountLayout(roots.configRoot).mode == "legacy" || moveInProgress(roots.stateRoot))
        accountList.current()->startMove();
    // Links the bar widget shipped in the package; enabling it waits for the user.
    PluginSetup pluginSetup;
    pluginSetup.check();
    // Asks this same program, in a child process, whether drive-sync can be taken over.
    TakeoverSetup takeoverSetup(QCoreApplication::applicationFilePath());
    takeoverSetup.check();
    // First setup and signing in again, through the same program as children.
    SetupController setupController(QCoreApplication::applicationFilePath(), QDir::homePath() + "/Drive");
    // Settings and the tree of cloud folders, also through child processes.
    SettingsController settingsController(QCoreApplication::applicationFilePath());
    FolderTree folderTree(QCoreApplication::applicationFilePath());
    // The filters screen: the synced folder on this computer, and the rules.
    LocalTree localTree;
    FilterEditor filterEditor;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("clockFormat", &clockFormat);
    // QML reads the account shown as "backend": moved to the selected one,
    // connected before the load so it runs before QML's own handlers.
    engine.rootContext()->setContextProperty("accountList", &accountList);
    engine.rootContext()->setContextProperty("backend", accountList.current());
    QObject::connect(&accountList, &AccountList::selectedChanged, &engine, [&engine, &accountList] {
        engine.rootContext()->setContextProperty("backend", accountList.current());
    });
    engine.rootContext()->setContextProperty("pluginSetup", &pluginSetup);
    engine.rootContext()->setContextProperty("takeoverSetup", &takeoverSetup);
    engine.rootContext()->setContextProperty("setupController", &setupController);
    engine.rootContext()->setContextProperty("settingsController", &settingsController);
    engine.rootContext()->setContextProperty("folderTree", &folderTree);
    engine.rootContext()->setContextProperty("localTree", &localTree);
    engine.rootContext()->setContextProperty("filterEditor", &filterEditor);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
