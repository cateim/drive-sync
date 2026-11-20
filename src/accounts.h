#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <functional>

#include "backend.h"
#include "fixstate.h"

// The accounts drive-sync syncs, and every path and unit name of each one
// (spec 12, "One resolver"). Nothing else builds an account's path or the
// name of one of its units: a caller asks resolveAccounts() and reads the
// BackendPaths it gets.
//
// Two modes. Legacy: neither <configRoot>/accounts/ nor
// <configRoot>/disconnected/ exists and <configRoot>/config.json does; there
// is then one account, account1, with the paths and the plain units DriveSync
// had before accounts. Accounts: everything else; the accounts are the
// accounts/<id>/ folders, named by a valid id, that hold a config.json, and
// there may be none.

// "account" followed by a number from 1 to 99, with no leading zero: the only
// form an id takes, so a unit name or a path never needs escaping.
bool validAccountId(const QString &id);

// Where the resolver looks. Tests point every root at a temp folder.
struct AccountRoots {
    QString configRoot;  // ~/.config/drive-sync
    QString stateRoot;   // ~/.local/state/drive-sync-app
    QString userUnits;   // ~/.config/systemd/user
    // The paths before any config is applied: the old log, ~/Drive,
    // drive-sync.sh's state folder, /proc, systemctl, find, appState (the
    // state root) and this program.
    BackendPaths base;

    static AccountRoots defaults();
};

struct AccountLayout {
    QString mode;      // "legacy" | "accounts"
    QStringList ids;   // by the number in the id; legacy mode has account1
    // Accounts mode with a config.json at the old place, which is ignored.
    bool staleConfig = false;
};

AccountLayout readAccountLayout(const QString &configRoot);

// Something about the set of accounts the user should hear of. Kinds:
// "staleConfig" (path is the ignored old config) and "moveStuck" (path is
// the move's journal).
struct AccountNotice {
    QString kind;
    QString path;
};

struct AccountSet {
    QString mode;  // as AccountLayout
    QList<BackendPaths> accounts;
    QList<AccountNotice> notices;
};

// Every account, each with its paths and units, and with its config applied
// when it loads. An account whose config cannot be read is still listed.
AccountSet resolveAccounts(const AccountRoots &roots);

// What the window shows: the first account (the view with no account when
// there is none) and the notices about the set. In 5.1 there is one account;
// 5.2 lets the user pick another.
struct WindowAccount {
    BackendPaths paths;
    QList<AccountNotice> notices;
};

WindowAccount windowAccount(const AccountRoots &roots);

// The paths a new account <id> gets in the accounts layout (first setup):
// its config, state folder and log, whether or not the config exists yet.
BackendPaths newAccountPaths(const AccountRoots &roots, const QString &id);

// The view with no account at all: base with no config, no id, the plain
// units and managed false. An old drive-sync that was not taken over is
// shown through it.
BackendPaths noAccountPaths(const AccountRoots &roots);

// What the window, the bar and the cards call an account: the base name of
// its folder, or, when another account's folder has the same base name, its
// path under home (the absolute path outside it).
QString accountLabel(const BackendPaths &account, const QList<BackendPaths> &all, const QString &home);

// An account's three units, in this order: service, timer, fix unit.
QStringList accountUnits(const BackendPaths &paths);

// <stateRoot>/migration.json, the journal of the move to account1, which
// exists only while the move runs or after one that stopped halfway.
QString migrationJournalPath(const QString &stateRoot);
bool moveInProgress(const QString &stateRoot);
// Neither accounts/, disconnected/ nor the single config.json under
// configRoot: the layout a single config left when it vanished (and a
// machine that never had a drive-sync).
bool singleConfigGone(const QString &configRoot);

// What the journal's stuckReason names; empty when it names nothing or the
// journal cannot be read.
QString migrationStuckReason(const QString &journal);

// The writers' lock rule (spec 12, "Writers during the move"). A writer of
// an account's state resolves its paths, then takes the account's FixLock;
// with it held it refuses while the move's journal exists, and when the
// paths it resolved before waiting are no longer the account's (the move
// ran meanwhile) it exits with kAccountPathsMoved before printing anything,
// and dispatchToAccounts resolves again and runs it once more.
inline constexpr int kAccountPathsMoved = 75;

// Whether paths still are what the resolver gives for paths.id: the same
// config and state folder, or, with no id, still no account at all. A new
// account's paths (accounts layout, not managed: newAccountPaths before its
// config exists, as first setup gets them) are current while the accounts
// layout still lists no account and no move is under way. Paths built by
// hand (no configRoot or no stateRoot) always are.
bool accountPathsCurrent(const BackendPaths &paths);

class AccountWriteLock {
public:
    explicit AccountWriteLock(const BackendPaths &paths, bool wait = true);
    // The FixLock is held (false only when its file cannot be opened, which
    // writers have always gone on through, or with wait false and another
    // holder).
    bool held() const { return m_lock.held(); }
    // The paths moved while this waited: run the command again.
    bool stale() const { return m_stale; }
    // Why this writer must not write now: the move is under way.
    QString refusal() const { return m_refusal; }

private:
    FixLock m_lock;
    QString m_refusal;
    bool m_stale = false;
};

// Takes "--account <id>" when they are the first two arguments, so that a
// value of settings, setup or folders (a cloud folder named --account) stays
// a value. *id is empty when there is none. False when "--account" comes
// first with no id after it.
bool takeAccountOption(QStringList *arguments, QString *id);

// The accounts a command acts on, or why it acts on none (exit code 1):
// - an id the resolver does not list is an error;
// - status, sync, pause and resume act on every account;
// - fix, deletes, settings, folders and signin act on the only account,
//   and want --account when there are several;
// - run-condition, run, notify and fix-run (what the units call) take no
//   --account in legacy mode, where they mean account1, and need one in
//   accounts mode, so a plain unit never runs an instance's account;
// - setup, takeover, watchdog and accounts take no --account.
// With no account at all, a command that acts on one gets noAccountPaths().
struct AccountChoice {
    QList<BackendPaths> targets;
    QString error;
    int exitCode = 0;
};

AccountChoice chooseAccounts(const AccountSet &set, const QString &command, const QString &id,
                             const AccountRoots &roots);

// What main does with a command's arguments before it runs: run-condition
// skips (1) while the move to account1 is under way, before any config is
// read, since one may be gone halfway; then --account is taken and the
// accounts chosen. A refusal is answered once (as JSON with "--json") and
// returns its exit code; otherwise run gets the arguments left and the
// accounts, and its exit code is returned. A run that answers
// kAccountPathsMoved is chosen and run once more over a fresh resolve.
using AccountCommand = std::function<int(const QStringList &arguments, const QList<BackendPaths> &accounts)>;
int dispatchToAccounts(const QString &command, const QStringList &arguments, const AccountSet &set,
                       const AccountRoots &roots, QTextStream &out, QTextStream &err, const AccountCommand &run);
