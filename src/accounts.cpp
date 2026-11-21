#include "accounts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

#include "controlcommands.h"

namespace {

const QRegularExpression &accountIdPattern() {
    static const QRegularExpression pattern("^account([1-9][0-9]?)$");
    return pattern;
}

int accountNumber(const QString &id) { return accountIdPattern().match(id).captured(1).toInt(); }

// The paths of account <id> in the accounts layout, before its config is read.
BackendPaths accountsLayoutPaths(const AccountRoots &roots, const QString &id) {
    BackendPaths paths = roots.base;
    paths.id = id;
    paths.config = roots.configRoot + "/accounts/" + id + "/config.json";
    paths.appState = roots.stateRoot + "/accounts/" + id;
    // drive-sync.sh never ran an account of the new layout: no resync mark to read.
    paths.stateDir.clear();
    paths.serviceUnit = "drive-sync@" + id + ".service";
    paths.timerUnit = "drive-sync@" + id + ".timer";
    paths.fixUnit = "drive-sync-fix@" + id + ".service";
    paths.configRoot = roots.configRoot;
    paths.stateRoot = roots.stateRoot;
    paths.accountsMode = true;
    return paths;
}

// Managed, then the config's log, folder and interval when it loads. A config
// that cannot be read keeps its account, with the fallback log and folder,
// so a broken account shows as one instead of hiding.
void applyAccountConfig(BackendPaths *paths, const QString &fallbackLog, const QString &fallbackDrive) {
    paths->managed = true;
    if (const auto config = loadSyncConfig(paths->config, nullptr)) {
        applySyncConfig(paths, *config);
        return;
    }
    paths->log = fallbackLog;
    paths->drive = fallbackDrive;
}

// The base name of a folder, as a label for paths with no set around them.
QString folderName(const QString &drive) { return QFileInfo(QDir::cleanPath(drive)).fileName(); }

bool journalStuck(const QString &journal) {
    QFile file(journal);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return QJsonDocument::fromJson(file.readAll()).object().value("stuck").toBool(false);
}

}  // namespace

bool validAccountId(const QString &id) { return accountIdPattern().match(id).hasMatch(); }

bool singleConfigGone(const QString &configRoot) {
    const QDir root(configRoot);
    return !QFileInfo::exists(root.filePath("accounts")) && !QFileInfo::exists(root.filePath("disconnected"))
           && !QFileInfo::exists(root.filePath("config.json"));
}

AccountRoots AccountRoots::defaults() {
    // Built by hand: BackendPaths::defaults() asks the resolver, so it
    // cannot be the base.
    const QString home = QDir::homePath();
    AccountRoots roots;
    roots.configRoot = home + "/.config/drive-sync";
    roots.stateRoot = home + "/.local/state/drive-sync-app";
    roots.userUnits = home + "/.config/systemd/user";
    roots.home = home;
    BackendPaths &base = roots.base;
    base.log = home + "/.local/state/drive-sync.log";
    base.drive = home + "/Drive";
    base.stateDir = home + "/.local/state/drive-sync";
    base.proc = "/proc";
    base.systemctl = QStandardPaths::findExecutable("systemctl");
    base.find = QStandardPaths::findExecutable("find");
    base.appState = roots.stateRoot;
    // "drive-sync open --account <id>" resolves before the window's
    // application exists, and only compares ids: no program path then,
    // rather than Qt's warning.
    base.self = QCoreApplication::instance() ? QCoreApplication::applicationFilePath() : QString();
    return roots;
}

AccountLayout readAccountLayout(const QString &configRoot) {
    AccountLayout layout;
    const QDir root(configRoot);
    const bool accountsFolder = QFileInfo::exists(root.filePath("accounts"));
    const bool disconnectedFolder = QFileInfo::exists(root.filePath("disconnected"));
    const bool oldConfig = QFileInfo::exists(root.filePath("config.json"));
    if (!accountsFolder && !disconnectedFolder && oldConfig) {
        layout.mode = "legacy";
        layout.ids = {"account1"};
        return layout;
    }
    layout.mode = "accounts";
    layout.staleConfig = oldConfig;
    const QDir accounts(root.filePath("accounts"));
    for (const QString &name : accounts.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (validAccountId(name) && QFileInfo::exists(accounts.filePath(name + "/config.json")))
            layout.ids << name;
    std::sort(layout.ids.begin(), layout.ids.end(),
              [](const QString &a, const QString &b) { return accountNumber(a) < accountNumber(b); });
    return layout;
}

AccountSet resolveAccounts(const AccountRoots &roots) {
    AccountSet set;
    const AccountLayout layout = readAccountLayout(roots.configRoot);
    set.mode = layout.mode;
    if (layout.mode == "legacy") {
        // Exactly the paths and units DriveSync used before accounts.
        BackendPaths paths = roots.base;
        paths.id = "account1";
        paths.config = roots.configRoot + "/config.json";
        paths.appState = roots.stateRoot;
        paths.configRoot = roots.configRoot;
        paths.stateRoot = roots.stateRoot;
        applyAccountConfig(&paths, roots.base.log, roots.base.drive);
        set.accounts << paths;
    } else {
        for (const QString &id : layout.ids) {
            BackendPaths paths = accountsLayoutPaths(roots, id);
            applyAccountConfig(&paths, paths.appState + "/drive-sync.log", roots.base.drive);
            set.accounts << paths;
        }
    }
    // Named only now: a label depends on every other account's folder.
    const QList<BackendPaths> unlabelled = set.accounts;
    for (BackendPaths &account : set.accounts)
        account.label = accountLabel(account, unlabelled, roots.home);
    if (layout.staleConfig)
        set.notices << AccountNotice{"staleConfig", roots.configRoot + "/config.json"};
    // A journal nobody is working on (no live drive-sync holds the lock) is a
    // move that stopped halfway, as is one the move itself marked stuck.
    const QString journal = migrationJournalPath(roots.stateRoot);
    if (QFileInfo::exists(journal) && (journalStuck(journal) || !takeoverLockHeld(roots.stateRoot, roots.base.proc)))
        set.notices << AccountNotice{"moveStuck", journal};
    return set;
}

WindowAccount windowAccount(const AccountRoots &roots) {
    const WindowAccounts window = windowAccounts(roots);
    return {window.accounts.first(), window.notices};
}

WindowAccounts windowAccounts(const AccountRoots &roots) {
    const AccountSet set = resolveAccounts(roots);
    return {set.accounts.isEmpty() ? QList<BackendPaths>{noAccountPaths(roots)} : set.accounts, set.notices};
}

OpenChoice chooseOpenAccount(const QStringList &afterOpen, const AccountSet &set) {
    QStringList rest = afterOpen;
    QString id;
    if (!takeAccountOption(&rest, &id))
        return {QString(), "usage: drive-sync open [--account <id>]"};
    if (id.isEmpty())
        return {};
    QStringList ids;
    for (const BackendPaths &account : set.accounts) {
        if (account.id == id)
            return {id, QString()};
        ids << account.id;
    }
    return {QString(), QString("no account %1; accounts: %2").arg(id, ids.isEmpty() ? QString("none") : ids.join(", "))};
}

BackendPaths newAccountPaths(const AccountRoots &roots, const QString &id) {
    BackendPaths paths = accountsLayoutPaths(roots, id);
    paths.log = paths.appState + "/drive-sync.log";
    // Not managed until its config exists, whatever the base says:
    // accountPathsCurrent tells a new account's paths apart by it.
    paths.managed = false;
    if (const auto config = loadSyncConfig(paths.config, nullptr))
        applySyncConfig(&paths, *config);
    paths.label = folderName(paths.drive);
    return paths;
}

BackendPaths noAccountPaths(const AccountRoots &roots) {
    BackendPaths paths = roots.base;
    paths.config.clear();
    paths.id.clear();
    paths.serviceUnit = "drive-sync.service";
    paths.timerUnit = "drive-sync.timer";
    paths.fixUnit = "drive-sync-fix.service";
    paths.configRoot = roots.configRoot;
    paths.stateRoot = roots.stateRoot;
    paths.accountsMode = false;
    paths.managed = false;
    paths.label = folderName(paths.drive);
    return paths;
}

QString accountLabel(const BackendPaths &account, const QList<BackendPaths> &all, const QString &home) {
    const QString folder = QDir::cleanPath(account.drive);
    const QString name = QFileInfo(folder).fileName();
    bool shared = false;
    for (const BackendPaths &other : all)
        if (other.id != account.id && QFileInfo(QDir::cleanPath(other.drive)).fileName() == name)
            shared = true;
    if (!shared)
        return name;
    // Roots built by hand have no home: the absolute path then.
    const QString cleanHome = QDir::cleanPath(home);
    if (!cleanHome.isEmpty() && folder.startsWith(cleanHome + "/"))
        return folder.mid(cleanHome.size() + 1);
    return folder;
}

QStringList accountUnits(const BackendPaths &paths) { return {paths.serviceUnit, paths.timerUnit, paths.fixUnit}; }

QString migrationJournalPath(const QString &stateRoot) { return stateRoot + "/migration.json"; }

bool moveInProgress(const QString &stateRoot) { return QFileInfo::exists(migrationJournalPath(stateRoot)); }

QString migrationStuckReason(const QString &journal) {
    QFile file(journal);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QJsonDocument::fromJson(file.readAll()).object().value("stuckReason").toString();
}

bool takeAccountOption(QStringList *arguments, QString *id) {
    id->clear();
    if (arguments->value(0) != "--account")
        return true;
    if (arguments->size() < 2)
        return false;
    *id = arguments->at(1);
    arguments->remove(0, 2);
    return true;
}

AccountChoice chooseAccounts(const AccountSet &set, const QString &command, const QString &id,
                             const AccountRoots &roots) {
    AccountChoice choice;
    const auto refuse = [&choice](const QString &error) {
        choice.targets.clear();
        choice.error = error;
        choice.exitCode = 1;
        return choice;
    };
    QStringList ids;
    for (const BackendPaths &account : set.accounts)
        ids << account.id;
    // A command that acts on an account, with none at all, shows the view
    // with no account, as before accounts.
    const QList<BackendPaths> every = set.accounts.isEmpty() ? QList<BackendPaths>{noAccountPaths(roots)} : set.accounts;

    static const QStringList noAccountOption{"setup", "takeover", "watchdog", "accounts"};
    static const QStringList everyAccount{"status", "sync", "pause", "resume"};
    static const QStringList onlyAccount{"fix", "deletes", "settings", "folders", "signin"};
    static const QStringList unitCommands{"run-condition", "run", "notify", "fix-run"};
    if (noAccountOption.contains(command)) {
        if (!id.isEmpty())
            return refuse(QString("usage: drive-sync %1 takes no --account").arg(command));
        choice.targets = every;
        return choice;
    }
    // Decided on the mode before the id: legacy mode lists account1 too, and
    // an instance unit must never run the account the plain units run.
    if (unitCommands.contains(command)) {
        if (set.mode == "legacy" && !id.isEmpty())
            return refuse(QString("%1 --account is for the accounts layout; this machine still runs %2 with no "
                                  "--account")
                              .arg(command, roots.configRoot + "/config.json"));
        // The single config gone (no accounts/, no disconnected/, no
        // config.json) with the plain unit still ticking: before accounts
        // that was 255, a failure systemd records, not a skip. Any other
        // plain tick in the accounts layout stays a skip.
        if (command == "run-condition" && set.mode != "legacy" && id.isEmpty() && set.accounts.isEmpty()
            && singleConfigGone(roots.configRoot)) {
            QString error;
            loadSyncConfig(roots.configRoot + "/config.json", &error);
            refuse(error);
            choice.exitCode = 255;
            return choice;
        }
        if (set.mode != "legacy" && id.isEmpty())
            return refuse(QString("%1 needs --account <id> in the accounts layout; accounts: %2")
                              .arg(command, ids.isEmpty() ? QString("none") : ids.join(", ")));
    }
    if (!id.isEmpty()) {
        for (const BackendPaths &account : set.accounts)
            if (account.id == id) {
                choice.targets = {account};
                return choice;
            }
        return refuse(QString("no account %1; accounts: %2").arg(id, ids.isEmpty() ? QString("none") : ids.join(", ")));
    }
    if (everyAccount.contains(command) || unitCommands.contains(command)) {
        choice.targets = every;
        return choice;
    }
    if (onlyAccount.contains(command)) {
        if (set.accounts.size() > 1)
            return refuse(QString("several accounts (%1); pick one with --account").arg(ids.join(", ")));
        choice.targets = every;
        return choice;
    }
    return refuse(QString("unknown command %1").arg(command));
}

bool accountPathsCurrent(const BackendPaths &paths) {
    // The resolver needs both roots, and gives both to every account it
    // builds: paths without them were built by hand, and nothing moves them.
    if (paths.configRoot.isEmpty() || paths.stateRoot.isEmpty())
        return true;
    AccountRoots roots;
    roots.configRoot = paths.configRoot;
    roots.stateRoot = paths.stateRoot;
    roots.base = paths;
    const AccountSet set = resolveAccounts(roots);
    if (paths.id.isEmpty())
        return set.accounts.isEmpty();
    // A new account's paths (newAccountPaths before its config existed, as
    // first setup gets them) name an id the resolver does not list yet. They
    // stay current while the layout is still the accounts one with no
    // account at all and no move under way; any account that appeared while
    // the writer waited (a move that ended, another setup) means it starts
    // over and sees it. Step 5.4 (adding beside existing accounts) revisits
    // this.
    if (paths.accountsMode && !paths.managed)
        return set.mode == "accounts" && set.accounts.isEmpty() && !moveInProgress(paths.stateRoot);
    for (const BackendPaths &account : set.accounts)
        if (account.id == paths.id)
            return account.config == paths.config && account.appState == paths.appState;
    return false;
}

AccountWriteLock::AccountWriteLock(const BackendPaths &paths, bool wait) : m_lock(paths.appState, wait) {
    // Checked with the lock held: the move takes the same lock before its
    // journal, and holds it until the journal is gone.
    if (moveInProgress(paths.globalState()))
        m_refusal = "DriveSync is moving to accounts; try again in a minute";
    else if (!accountPathsCurrent(paths))
        m_stale = true;
}

int dispatchToAccounts(const QString &command, const QStringList &arguments, const AccountSet &set,
                       const AccountRoots &roots, QTextStream &out, QTextStream &err, const AccountCommand &run) {
    // A skip, and before anything is read: the move renames the config.
    if (command == "run-condition" && moveInProgress(roots.stateRoot))
        return 1;
    QStringList rest = arguments;
    QString id;
    const bool json = rest.contains("--json");
    if (!takeAccountOption(&rest, &id))
        return commandAnswer(json, "usage: --account needs an account id", out, err);
    // Twice at most: a writer whose account moved while it waited for the
    // lock says so (kAccountPathsMoved) before it prints anything.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const AccountChoice choice = chooseAccounts(attempt == 0 ? set : resolveAccounts(roots), command, id, roots);
        if (!choice.error.isEmpty()) {
            commandAnswer(json, choice.error, out, err);
            return choice.exitCode;
        }
        const int code = run(rest, choice.targets);
        if (code != kAccountPathsMoved)
            return code;
    }
    return commandAnswer(json, "DriveSync's accounts changed twice while this waited; try again", out, err);
}
