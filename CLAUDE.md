# CLAUDE.md - drive-sync

Single source of truth for agents and humans working on this repository.
`AGENTS.md` only points here.

## Overview

DriveSync is an Omarchy desktop app (Qt 6, C++ and QML) that shows the state
of `drive-sync`, the rclone bisync of `~/Drive` with Google Drive that runs
from a user timer every 15 minutes (`drive-sync@account1.timer` since step
5.1, `drive-sync.timer` for the single config of older versions until the
move to accounts), in a window, on the
Omarchy bar (its widget ships in the package) and through
`drive-sync status --json`. It reads the rclone log, asks systemd, and looks
for conflict copies. It writes its own state (`~/.local/state/drive-sync-app/`),
the link to its bar widget, and, when the user accepts, the bar layout through
`omarchy plugin`. Since step 2 it can take over a drive-sync made by
`drive-sync.sh` (`drive-sync takeover`): its packaged user units then run the
same rclone command (since step 5.1 as `account1`: config and filters with
their `.md5` in `~/.config/drive-sync/accounts/account1/`, the old
`ExecCondition` in `machine.json`, `drive-sync@account1.timer` turned on;
`takeover --undo` only from a pre-accounts takeover, refused and touching
nothing once `accounts/` or `disconnected/` exists), and the window,
the bar and the command line can sync now, pause and resume. Since step 3 it
fixes a broken base (`F`, `drive-sync fix`): a resync with the folder in use,
run by the account's fix unit (`drive-sync-fix@<id>.service`, the plain
`drive-sync-fix.service` in legacy mode), which only starts after the user confirms a
dry run and touches single synced files only as the fix's comparison or the
user's answers say. It never starts a resync or touches a synced file
otherwise. Since step 5b, after `--max-delete` stopped a run, it lists what
that run would delete (`D`, `drive-sync deletes`) and lets one run through
with the guard raised once the user types how many files that is. Since
step 4a it sets up a drive-sync where none exists (`T`, `drive-sync signin`
then `drive-sync setup --folder`): rclone's own browser sign-in with the
DriveSync OAuth client, the default filters, the config, and a fix whose dry
run the user confirms; and a token Google no longer accepts is the cause
`signIn`, with `I` to sign in again. Since step 4b it has Settings (`,`,
`drive-sync settings`, `drive-sync folders`): the interval (a drop-in for
the packaged timer) and the max-delete guard at once; the folder, the
filters and selective sync (a marked block at the top of the filters)
through the fix, undone byte for byte when that fix is cancelled before its
base; "late" is three intervals. The plan for growing it into the
owner of drive-sync is `docs/superpowers/specs/2026-11-07-drive-sync-manager-design.md`; `docs/` is git-ignored,
so that file exists only in the maintainer's working copy. The `drive-sync` scripts themselves live
in the DIY repository (`assets/omarchy/home/.local/bin/drive-sync.sh` and
`drive-sync-aviso`), not here.

Built following the `omarchy-app` skill (`~/.claude/skills/omarchy-app/`):
one window, one job, keyboard first, follows the Omarchy theme and language.

**Names.** The product is shown as "DriveSync". Its machine names are
`drive-sync` (package, `/usr/bin/drive-sync`, `/usr/share/drive-sync`, the
`.desktop` file and Hyprland class, the bar widget id, `~/.config/drive-sync`,
`~/.local/share/drive-sync`) and `drive-sync-app` for the state folder, because
`drive-sync` is also what it manages: the units, `drive-sync.log` and the old
`drive-sync.sh` and `drive-sync-aviso`. The AUR has an unrelated `drivesync`, so
that name must never come back. The repository was `drivesync` until
2026-11-20 and is now `drive-sync` (`github.com/CaTeIM/drive-sync`, the page
at `drive-sync.selflabs.org` since 2026-11-20); what was named after it followed:
`drive-sync.pro`, `tests/drive-sync-tests.*`, `fakeDriveSync` and the bar's
`action.open` i18n key. `drivesync` stays only where it names the app's old
name (the legacy migration, the plugin swap, `takeoverLockHeld` and their tests).

## Stack

- C++17, Qt 6 (core, gui, qml, quick, quickcontrols2, dbus), qmake.
- QML with the Material style, dark theme, Omarchy accent.
- Qt Test for tests, run offscreen.
- Packaged with a local `PKGBUILD` and `makepkg`. No other dependencies.

## Structure

```text
drive-sync.pro          qmake project
bin/build               qmake6 + make into build/drive-sync
bin/test                builds and runs build/tests/drive-sync-tests offscreen
bin/install             bin/build, then makepkg -fsi with BUILDDIR/PKGDEST under build/
bin/harness             builds tools/harness into build/harness and runs it with the given options
src/accountlist.{h,cpp}  the window's accounts: one Backend per account the resolver lists (or the view with no account), the selection, in-place adoption with one; QML's "backend" is current()
src/accounts.{h,cpp}    the one resolver: legacy or accounts mode, every account's BackendPaths (paths and unit names), notices, labels; --account and chooseAccounts (which accounts a command acts on)
src/accountmove.{h,cpp}  the move of the single config to account1: plannedMoves, moveBlocker, moveToAccounts with its journal and rollback; `drive-sync accounts [--migrate]`
src/main.cpp            commands run headless (status, sync, pause, resume, takeover, fix, deletes, signin, setup, settings, folders, accounts, and run-condition, run, notify, watchdog, fix-run for the units); otherwise the window
src/synclog.{h,cpp}     pure parser of the rclone bisync log into SyncRun records
src/snapshot.{h,cpp}    buildSnapshot(): the one place the state is decided; tone; the JSON contract
src/backend.{h,cpp}     the window's state: log watch, systemctl calls, conflict scan, headline rules (summarize); follows the resolver (setResolver), starts the move (startMove)
src/clockformat.{h,cpp}  12 or 24 hours, read from the Omarchy bar clock in shell.json
src/conflictcache.{h,cpp}  conflict count shared through <appState>/conflicts.json (the account's state folder: ~/.local/state/drive-sync-app/accounts/<id>/, or the state root in legacy mode)
src/statuscommand.{h,cpp}  `drive-sync status [--json]`: same sources, read once, synchronously
src/syncconfig.{h,cpp}  the config a takeover writes; rclone arguments; filtersUsable (the md5 check)
src/pausestate.{h,cpp}  the pause mark (<appState>/paused.json) and "next 08:00"
src/controlcommands.{h,cpp}  sync, pause, resume, run-condition, run (execs rclone); runProcess
src/fixstate.{h,cpp}    the fix's state machine in <appState>/fix.json (steps, questions)
src/fixcommand.{h,cpp}  `drive-sync fix` and `fix-run` (the account's fix unit, drive-sync-fix@<id>.service): dry run, base run, questions, final run
src/listingdiff.{h,cpp}  reads bisync's .lst listings (Go %q names) and compares both sides, as drive-sync.sh did
src/machineguard.{h,cpp}  the runOnlyWhen check of the machine: the config's in legacy mode, machine.json in accounts mode
src/blockeddeletes.{h,cpp}  a max-delete stop: its deletions from the log, the one-run mark (allow-deletes.json), the raised guard, `drive-sync deletes`
src/legacymigration.{h,cpp}  moves the SyncWatch and drivesync folders to the drive-sync names (state to drive-sync-app); first thing main() does
src/notifier.{h,cpp}    notify (ExecStopPost) and watchdog: the port of drive-sync-aviso
src/takeover.{h,cpp}    detect, apply with rollback, undo; `drive-sync takeover`; checkPackagedUnits and foreignDropIns (packaged units and template instances, by Id); findOldDriveSync (an old drive-sync next to accounts)
src/oauthclient.{h,cpp}  shippedOAuthClient(): the Google client, compiled in only from the git-ignored src/oauthclient.local.h (template: src/oauthclient.local.h.example)
src/signin.{h,cpp}      `drive-sync signin [--abandon]`: rclone's browser flow creates the drive-sync remote, or refreshes the config's remote
src/signin-page.{html,qrc}  the page rclone shows after Google (a Go html/template with @MARKER@ text slots), compiled in with the logo; signInPage() fills it
src/firstsetup.{h,cpp}  `drive-sync setup --folder`: refusals, default filters, the new user's config, startFix, then the timers
src/setupcontroller.{h,cpp}  the window's side of signin and setup; Cancel sends SIGTERM to the signin child alone
src/settingscommand.{h,cpp}  `drive-sync settings`: show; interval (config + timer drop-in) and guard at once; folder, filters and --skip through startFix with the settings-change mark
src/selectivesync.{h,cpp}  the block of cloud folders left out (escape, parse, rewrite), the tick rules, `drive-sync folders` (rclone lsjson --dirs-only)
src/settingscontroller.{h,cpp}  the window's side of Settings: a queue of `drive-sync settings ... --json` children
src/foldertree.{h,cpp}  the cloud folder tree for Settings and first setup, a level at a time through `drive-sync folders`
src/filterrules.{h,cpp}  the filters as the screen builds them: groups, paths, rules by name, advanced text; read, write, import; defaultFilters()
src/filtereditor.{h,cpp}  the window's side of the filters screen (groups, rules by name, advanced, import); text() is the whole file
src/localtree.{h,cpp}  the synced folder on this computer as a tree with ticks (QDir, never follows a link)
src/takeoversetup.{h,cpp}  the window's side of a takeover, through a child process
src/pluginsetup.{h,cpp} links the shipped bar widget and enables it through `omarchy plugin`
src/theme.{h,cpp}       Omarchy accent and Hyprland rounding (from the skill template)
src/translator.{h,cpp}  Omarchy language, JSON catalogs (from the skill template)
src/Main.qml            window, shortcuts, layout
src/AccountSelector.qml one chip per account (dot in its tone, label) from two accounts on; a click, [ or ] shows another, never while a dialog or the help is open
src/StatusPanel.qml     headline, details, the resolver's notices, progress, warning chips
src/RunsPage.qml        run list; RunDetail.qml shows the picked run
src/ConflictsPage.qml   conflict copies
src/HelpOverlay.qml     "?" overlay
src/BarBanner.qml       offer to put the widget on the bar (B), or why it could not
src/ControlsRow.qml     sync now (S), pause (P), resume (U), open folder (O); Settings (,) on the right
src/PauseMenu.qml       the three pause lengths (1, 2, 3)
src/TakeoverBanner.qml  offer to take over (T), or what blocks it; TakeoverDialog.qml confirms
src/SetupBanner.qml     offer first setup (T) when there is no drive-sync at all
src/SetupDialog.qml     sign in, the folder, the cloud folders, start; with "again", only the sign-in (I)
src/SettingsDialog.qml  Settings (,): General, Cloud folders and Filters pages (Ctrl+1/2/3), Ctrl+S saves the page shown
src/FolderTreeView.qml  the tree with ticks, over folderTree or localTree; takes its own keys (arrows, J K, Space, Left, Right) while it has the focus
src/FiltersEditor.qml   what does not sync: groups, the local tree, rules by name, advanced and import; Settings' Filters page and setup's step 4 (Ctrl+E)
src/FixBanner.qml       offer to fix (F) after lost listings, out of sync or changed filters
src/FixDialog.qml       the fix's start, the dry run to confirm (Enter) and a stopped step (retry; X cancels)
src/FixQuestions.qml    the files that changed in the cloud during the base run: C cloud, L notebook, Enter sends
src/DeleteBanner.qml    offer to review the deletions (D) after a max-delete stop
src/DeleteDialog.qml    those deletions per side, and the typed count that lets one run through (Enter)
src/format.js           relative times, durations, sizes
src/i18n/*.json         en (plural forms only) and pt-BR
tests/                  Qt Test suite
tools/harness/          offscreen harness: the real Main.qml over a made-up drive-sync, saved as a PNG
plugin/                 Omarchy bar widget (id drive-sync), installed to /usr/share/drive-sync/omarchy-plugin; reads `drive-sync status --json --all` (version 3)
site/                   project page on GitHub Pages (drive-sync.selflabs.org): index.html, privacy.html, terms.html, style.css, i18n.js (pt-BR and the EN/PT switch)
.github/workflows/pages.yml  publishes site/, copying the icon and window-runs.png in from assets/
docs/superpowers/       git-ignored, local only: design (specs/) and step plans (plans/)
pkgbuild/               PKGBUILD, .desktop, units/ (/usr/lib/systemd/user: the plain drive-sync, drive-sync-watchdog and drive-sync-fix user units, and the templates drive-sync@.service, drive-sync@.timer and drive-sync-fix@.service, one instance per account)
assets/icons/           launcher icon, installed by the PKGBUILD, and the logo of the Google Cloud consent screen (no tile, dark outline; `google-cloud-logo-120.png` beside it is the 120 px upload, rendered with `rsvg-convert -w 120 -h 120 assets/icons/drive-sync.svg -o assets/icons/google-cloud-logo-120.png`, so redo it whenever the SVG changes)
assets/images/          README pictures: window-*.png from bin/harness over a made-up log; bar-*.png are real screens with no file names
```

## Commands

```sh
bin/build            # about 30 s from clean
bin/test             # 421 cases, about 38 s (fourteen run real rclone on temp folders)
bin/install          # needs sudo (pacman -U); run it in a real terminal
bin/harness --scenario running --clock "h:mm AP" --out build/shot.png   # the window offscreen, made-up data; options atop tools/harness/harness.cpp
makepkg -f           # from pkgbuild/, with BUILDDIR and PKGDEST set as in bin/install, to check packaging without sudo
build/drive-sync status --json --all   # the state as the bar sees it, every account, version 3 (0.13 s with a fresh conflict cache)
build/drive-sync status --account account1 --json   # one account; --account <id> comes first, on every command but setup, takeover, watchdog, accounts
drive-sync open --account account2   # the window on that account (an id not listed: exit 1, chooseAccounts' message); the bar's rows run it
(cd plugin && node --test tests/*.test.mjs)   # bar widget, 51 cases; the glob is required on Node 22
build/drive-sync takeover --check              # read-only: what a takeover would move, or what blocks it
drive-sync accounts [--json]                   # read-only: mode, accounts, and whether the move to account1 can go (installed binary only; --migrate moves)
bin/harness --scenario setup-offer --keys o --out build/shot.png   # first setup; also setup-signing-in, setup-folder, signin-needed
bin/harness --managed --keys ",w^2w>w" --out build/shot.png   # Settings, Cloud folders page; ^3 Filters, ~X types an X
bin/harness --scenario two-accounts --keys "]" --out build/shot.png   # the selector, account2 shown; [ the previous, --select <id> before the keys
omarchy plugin validate plugin                # needs the folder path, not the plugin id
```

## How the state is decided

Which drive-sync is read comes first: `resolveAccounts()` (`src/accounts.cpp`)
is the only place that builds an account's paths or unit names (spec 12, step
5.1). Legacy mode (neither `~/.config/drive-sync/accounts/` nor `disconnected/`
exists, and `config.json` does) has one account, `account1`, with exactly the
old paths and the plain units. Accounts mode (everything else) lists the
`accounts/<id>/` folders named by a valid id (`account1` to `account99`) that
hold a `config.json`, each with its state in
`~/.local/state/drive-sync-app/accounts/<id>/`, no drive-sync.sh state folder
and the units `drive-sync@<id>.service`, `.timer` and `drive-sync-fix@<id>.service`;
there may be none, and an old `config.json` is then ignored and reported
(`staleConfig`), never moved. A config that does not load keeps its account.
`BackendPaths::defaults()` is the first account, else `noAccountPaths` (no
config, plain units, not managed). The takeover lock and the move's
`migration.json` live in the state root (`BackendPaths::globalState()`).
The resolver also names each account (`BackendPaths::label`, `accountLabel`
over the whole set with `AccountRoots::home`: the folder's base name, or its
path under the home when two folders share one), so the cards, the JSON, the
status text and the window read one label; `noAccountPaths` and
`newAccountPaths` take their folder's base name, and paths built by hand
have none. `windowAccounts(roots)` lists every account the window can show
(the view with no account alone when there is none), and
`chooseOpenAccount` reads `drive-sync open --account <id>`.

`buildSnapshot()` (`src/snapshot.cpp`, pure, tested) is the only place: the
window's `Backend`, `drive-sync status` and, through its JSON, the bar widget
all show what it decides. It finds the running run, the last success, the
last run, `failingSince`, `canSyncNow` (false while `migration.json`
exists, `SnapshotSources::moving`, since `run-condition` skips every tick
then) and the `tone` (`ok`, `busy`, `warning`, `error`), and calls `Backend::summarize`, which picks one
headline, most urgent first:
`unavailable` (systemctl failed), `configError` (an account whose config
does not load, detail the loader's reason; never for a missing config, which a
takeover undone leaves), `notLoaded`, `fixing`, `fixWaiting`, `fixFailed`,
`running`, `resync`, `paused`, `noLog`,
`failed`, `interrupted`, `skipped` (ExecCondition: emergency home; `causeKind`
`oldDriveSync` when an old drive-sync holds the account), `timerOff`,
`stale` (no success for three intervals, 45 min at the default 15; the
interval comes from the config through `BackendPaths::intervalSeconds`), `noDrive`, `noRuns` (a log with no run:
new or rotated), `conflicts`, `ok`. What the
headline leaves out goes to `warnings`: `timerOff`, `timerDisabled`,
`conflicts`, `oldDriveSync`.

An old drive-sync next to accounts (spec 12; accounts mode only, since
before the move it is what a takeover moves aside): `findOldDriveSync`
(`src/takeover.cpp`) lists drive-sync.sh's units, scripts and the filters
its unit names, and a plain `drive-sync.service` or `.timer` systemd loads
from outside `/usr/lib/systemd/user`; `oldDriveSyncHolds` holds the account
whose folder the old unit names (equal, inside or containing,
`foldersOverlap`), and every account when an old `drive-sync.service` names
no folder it can read. Readers never look by themselves: `main` hands
`status` and the window an `OldDriveSyncCheck` over the real home, and the
plain units ride in their one `show` call; tests and the harness give none.
The hold skips in `run-condition` (after the move and fix checks), shows as
`skipped` with `causeKind` `oldDriveSync` (the detail lists what was found)
and the warning `oldDriveSync` (chip in the window, a line in the bar's
panel); the watchdog sends one card per new list, kept in
`~/.local/state/drive-sync-app/notify-accounts.json`. DriveSync never stops
or edits the old units.

The watchdog (`runWatchdogCommand(roots, takeover, notifier, now)`,
`drive-sync-watchdog.timer`, hourly) runs `moveToAccounts` first (the move,
or the rollback of an orphan journal), then resolves, then runs the machine
guard once (silent when it says no). With no journal it watches each
account (`watchAccount`: config error, an unreadable `machine.json`, or
`timerProblem` over the account's own units; the pause and fix reminders),
each with its `notify-watchdog.json` in the account's state folder, written
and removed under `AccountWriteLock`. While `migration.json` exists (a live
move, or one stuck) it checks no account and writes no account file: a
"timer off" card would be wrong, and a file at the old place would make the
rollback stuck. Cards about every account live in `notify-accounts.json`
by kind: `moveStuck` and `staleConfig` repeat hourly like any problem (the
latter is left alone while a journal exists, since the old config is then a
step of the move) and go silently once fixed; `oldDriveSync` stays one card
per list. The `moveStuck` card says to clear what the journal's
`stuckReason` names (it adds the reason, `migrationStuckReason`) and run
`drive-sync accounts --migrate`, which rolls back from the journal again;
the window's notice and `status`'s stderr say the same. `moveRolledBack`
comes from the outcome of the watchdog's own `moveToAccounts`: hourly
while it keeps ending in `rolledBack` (with the reason, an orphan journal's
rollback included), gone silently on `moved` or `notNeeded`, left alone on
`deferred` or `stuck`; the window's `startMove` child shows nothing, since
the next watchdog round tries again. `configGone`: with no account, no
journal and `singleConfigGone` (no `accounts/`, no `disconnected/`, no
`config.json`), a plain `drive-sync.timer` loaded from the package and
enabled or active means the single config vanished (the card pre-5.1
watchdogs gave, "config cannot be read"); a timer that is off (a new
machine) or a user unit of that name (an undone takeover) says nothing.
Since 5.2 every per-account card, and every title a close names, is
`<title> (<label>)` (`accountCardTitle`, through `tr("%1 (%2)")`: the
account's label, else its folder's base name): the first card, the
reminder, the recovery, the timer, the pause and fix reminders, the fix's
cards and the unconfirmed deletions. The cards about every account (an old
drive-sync, the move stuck or undone, a stale config, the config gone) keep
their bare titles. The 14 bare 5.1 titles (`accountCardBareTitles`) are
dismissed once, before the first labelled card, and only while the resolver
lists exactly one account: `notify` and `fix-run` (in `main`) and the
watchdog (after its resolve) set `NotifierPaths::bareTitlesMark` to
`<stateRoot>/notify-bare-titles.done` then, and `dismissBareTitlesOnce`
writes that mark first and dismisses only when the write succeeded, never
while the move's journal exists. With two accounts each bare title is part
of both accounts' labelled ones, so it is never dismissed.

The window holds one `Backend` per account through `AccountList`
(`src/accountlist.cpp`, step 5.2) over `windowAccounts(roots)` (every
account, else `noAccountPaths`, and the notices). QML reads the account
shown as the context property `backend`, which `main` (and the harness)
sets again to `accountList.current()` on `selectedChanged`, connected
before the load so it runs before QML's handlers (Task 1 of 5.2 checked
that bindings and `Connections` follow the swap); `Main.qml` then takes the
new account's fix step as seen, so a waiting fix shows its banner and never
opens its dialog by itself. `drive-sync open --account <id>` selects that
account at start (an id not listed exits 1 before the window exists).
From two accounts on (`accountList.several`), `AccountSelector` shows one
chip per account over `accountList.accounts` (rebuilt on every `Backend`'s
`changed`, so each dot follows its own account); a click, `[` or `]` (both
wrap) changes the selection, and all three are off while a dialog or the
help overlay is open, so a typed count, a fix confirmation or a Settings
save stays with the account it was opened for. Settings' title then names
`accountList.currentLabel` (its own `currentLabelChanged`). With one
account none of it shows.
Each `Backend` gets a resolver from the list that resolves the whole set,
returns its own entry (by id; with one `Backend` and one entry, that entry
whatever the id, so a first setup, a takeover, a cancelled setup and the
move keep the same object) and queues one coalesced `reconcile()`, which
makes a `Backend` for an account that appeared (through a factory: `main`
gives each the old drive-sync check), drops the one of an account that
went (selecting the first and emitting `selectedChanged` before
`deleteLater`), stores each label and waits like the `Backend` during a
live move. A `Backend`'s resolver, asked by `reloadConfig` first: another id,
config, state folder or unit means the window drops every watch (log,
state folder, config folder) and adopts the new paths, so a first setup, a
takeover, a cancelled first setup and the move need no restart. Besides the
config's folder it watches the config root (`~/.config/drive-sync`), where
`accounts/` appears and the old `config.json` leaves; each recompute (the
30 s poll) looks for one that appeared since (a setup or a takeover run from
a terminal on a home with no `~/.config/drive-sync`) and asks the resolver
once when it adds a watch. While a move is live
(`migration.json` and no `moveStuck` notice) it keeps what it shows and
asks again each second: between `mkdir accounts/account1` and the config's
rename the resolver sees no account at all. A systemd answer, or a `find`,
started for the paths it left is dropped and asked again. `notices`
(`{kind, path}`) are lines in `StatusPanel`. When the layout is legacy or a
journal exists, `main` calls `startMove()` once, on the selected `Backend`
(never per `Backend`): a child `drive-sync accounts
--migrate --json` with no parent, no timeout and its output to
`/dev/null`, since a move killed halfway waits for the next trigger to
roll back; then `reloadConfig`. Its exit code is never shown: a stuck move
shows through the notice. Every other command the window runs names the
account it shows (`Backend.accountId`) right after the command word.

- A run is "running" when systemd says the service is active, when the resync
  pause mark holds a live `drive-sync.sh` pid, or when the open run wrote to
  the log in the last 150 s (the log is watched, systemd is only polled).
- systemd's `Result` counts only when no log run started after the service's
  `ExecMainStartTimestamp`: `drive-sync.sh resync` runs rclone outside the unit.
  A skip (`exec-condition`) has no main start, so it is the headline only
  when no log run started after its `InactiveEnterTimestamp`: the fix's
  final run, outside the unit, comes after the ticks it held back.
- A resync (rclone 1.75.2) writes no "PathN: N changes" summary: its counts
  come from the "Copied (new)" and "Copied (replaced existing)" lines after
  "- PathN  Resync is copying files to  - PathM", which names the side.
- Failure causes match `drive-sync-aviso`: lost listings, out of sync, else the
  first error that is not the generic "Bisync aborted" line.
- The JSON (`snapshotToJson`, version 2) always has the same 20 keys (version
  1 plus `managed`, `paused`, `pausedUntil`); times are Unix seconds, `0` when
  unknown. The widget accepts one version only (version 3, through `--all`,
  since 5.2), so a change to the keys means a new version and a widget that
  reads it, installed together. With several
  accounts, `status` reads them all with one `show` and one `list-timers`
  (`gatherAccounts`), `--json` prints the worst one (`worstAccount`: error,
  warning, busy, ok, the first on a tie), and the text prints a block per
  account from two on, `<label> (<id>)` then the line indented by two
  spaces (one account, or the view with no account: the old single line,
  byte for byte). `--json --all` (`statusListToJson`, version 3; `--all`
  needs `--json`) has exactly `version`, `now`, `tone`, `worst` (the worst
  account's id), `canSyncNow` (any account's) and `accounts`, each built by
  `accountToJson`: the 18 keys version 2 has besides `version` and `now`,
  plus `id`, `label` and `folder`. Both build those 18 keys through one
  helper, so plain `--json` keeps version 2's bytes for an older widget
  (`versionTwoIsUnchanged`); the 5.2 widget reads `--all`. With no account,
  the view with no account is listed (empty `id`, `managed` false) unless
  its headline is `noLog`: then `accounts` is empty, `tone` `error` and
  `worst` empty. The resolver's notices (`staleConfig`, `moveStuck`) go to
  stderr only, so the bar never reads them.
- A pause is a mark file, not a stopped timer: `run-condition` skips each tick
  while it holds and clears it at the first tick after its end, so "paused"
  lasts until a run really starts and survives a reboot.
- `runOnlyWhen` guards the machine, not an account (`src/machineguard.cpp`).
  Legacy mode reads the config's; accounts mode, and the view with no
  account (setup, a first sign-in), read `~/.config/drive-sync/machine.json`,
  `{"version": 1, "runOnlyWhen": "<shell command>"}`, and ignore any left in
  a config (Settings drops it at its next save there). A missing file or an
  empty command is no guard; a file that cannot be read holds (`run-condition`
  255, the others refuse naming it, the watchdog reports it). It runs as
  systemd reads an ExecCondition (0 passes, 255 or a crash fails, else no)
  and holds `run-condition`, `fix --start`, every `fix-run` step, `signin`
  (not `--abandon`) and `setup`; the watchdog says nothing while it says no.
- A fix is a mark file too: `fix.json` is written before anything else and
  `run-condition` skips every tick while it exists, so the timer, `S`, `U` and
  the bar's sync wait for the whole fix (spec section 5). Its steps:
  `preparing` (wait for a run, dry run into `fix-dry-run.log`), `confirm`
  (the user), `base` (the resync, into the main log), `questions` (the
  user), `finishing` (answers, then a normal run through `runCommandPlan`),
  or `failed`. `fix-run` loops until a step waits for the user, fails or ends,
  and sends a card at once when it does (the watchdog repeats it hourly). It
  checks the machine's `runOnlyWhen` before every step, `fix --start` also
  refuses after a run that `--max-delete` stopped (`causeKind` `maxDelete`,
  spec 5b), and a final run that failed and took the listings with it sends
  the retry back to `base`.
- A fix shows as `fixing` (busy; the JSON's `resync` is true), `fixWaiting`
  (confirm or questions) or `fixFailed`. A working step with the account's
  fix unit inactive is interrupted (`causeKind`
  `fixInterrupted`), but only 60 s after `stepStarted`: `fix` writes the step
  before systemd starts the unit. The `systemctl show` call names the
  account's three units (`accountUnits`: service, timer, fix unit, from the
  resolver) with `-p Id,...`, and every reader maps the blocks by `Id`
  (`Backend::parseShowById`), never by position, and requires all three: a
  missing one is `unavailable`. `list-timers` names the account's timer and
  is read by its `unit`. No command, card or window line hardcodes a unit
  name; the texts take it as an argument (`Backend.serviceUnit`,
  `timerUnit`, `fixUnit`), and the interval drop-in is
  `intervalDropInPath(userUnits, timerUnit)`. The takeover keeps the plain
  names of the old units it reads.
- Every writer of `fix.json` (the `fix` command, `fix-run`, the watchdog's
  reminder) takes `FixLock` (an flock on `fix.lock`) around its read and
  write, never across a long step. The fix and the takeover find the bisync
  listings by `bisyncSessionName(remote, local)`, the name rclone gives them
  (checked against rclone 1.75.2), never "the only pair in the work dir".
- The writers of an account's state that run outside a unit (`fix`,
  `setup`, `settings`, `deletes --allow`, `pause`, `resume`, and the
  watchdog's reminders and its `notify-watchdog.json` for their write) take
  `AccountWriteLock`
  (`src/accounts.h`): the account's `FixLock` first, then, with it held, a
  refusal while `migration.json` exists ("DriveSync is moving to accounts;
  try again in a minute"), then `accountPathsCurrent`: when the paths it
  resolved before waiting are no longer the account's, it exits 75
  (`kAccountPathsMoved`) before printing anything, and `dispatchToAccounts`
  resolves again and runs it once more. Paths with no `configRoot` or
  `stateRoot` (built by hand, as in many tests) are always current. Never
  take one while holding a `FixLock` in the same process. `status`, the
  window's conflict scan and its answers file write nothing while the
  journal exists.
- Runs refused for the filters' md5 have `causeKind` `filtersChanged` (the
  refusal text ends in `kFiltersChangedReason`): a normal card says "press F"
  instead of the critical "stopped syncing" one.
- A run whose errors say Google refused the token (`isTokenError`:
  `invalid_grant`, `invalid_client`, `unauthorized_client`, `token expired`,
  any case) has `causeKind` `signIn` with no detail: the critical card says
  "Open DriveSync and sign in again", the window offers `I` when it runs
  drive-sync, and nothing repeats rclone's `rclone config reconnect`. A fix
  that failed on such an error is `fixFailed` with `causeKind` `signIn` too.
  `couldn't fetch token` alone is no sign-in: rclone 1.75.2 writes it for a
  refused connection or a 5xx too (tried 2026-11-14 with a token URL nobody
  answers).
- A refused run with no filters `.md5` and no listings has `causeKind`
  `firstSyncPending` (the refusal ends in `kFirstSyncPendingReason`): no base
  was ever made, so the window, a normal card and the bar say to press `F`,
  never "the filters changed". `drive-sync setup` writes
  `<appState>/first-setup.json` (what it wrote); `fix --cancel` with that
  mark and no base undoes the setup (timers off, its config and filters
  removed, its empty folders removed) and the window offers setup again. The
  base step removes the mark. Since step 5.1 setup writes `account1` in the
  accounts layout (`setupAccountPaths`: with no account listed, main hands
  it `newAccountPaths(roots, "account1")`): config and filters in
  `~/.config/drive-sync/accounts/account1/`, work dir and log in its state
  folder, `drive-sync-fix@account1.service` started and
  `drive-sync@account1.timer` turned on. It refuses when any account is
  listed (`DriveSync already runs drive-sync (<ids>)`), in legacy mode by the
  single config's name, and while `~/.config/drive-sync/filters.txt`
  exists. Those paths name an id the resolver does not list yet, so
  `accountPathsCurrent` takes a new account's paths (accounts layout, not
  managed) as current while the layout lists no account and no journal
  exists: an account that appears while setup waits on the lock sends it
  round again (75), where it refuses. The cancel leaves the state folder
  (it holds `fix.lock` and the dry run's log).
- A run `--max-delete` stopped is `causeKind` `maxDelete` (spec 5b). Its
  deletions come from its own log lines (`blockedDeletes`, only when it is
  the newest run), so the headline, the card and `D` count both sides.
  `drive-sync deletes --allow <count>` (the count of that list, typed by the
  user) writes `allow-deletes.json`; the next `drive-sync run` (or the fix's
  final run, when that is what stopped) takes it, removes it, and dry-runs
  the plan with `--max-delete` raised to the largest of
  `ceil(100 * side deletions / that side's files in its .lst listing)`
  (the abort line's total when the listings cannot be read), never below
  the config's guard, into `allowed-dry-run.log`. The mark names the stopped
  run's start, and it is dropped unless that run is still the newest one;
  the real run gets the raised guard only when the dry run reached bisync
  (a whole "Synching Path1" line in its log), ended well, and every deletion
  it finds, by side and path, is in that run's list. Otherwise the config's
  own guard stops it again with the new list, and the "DriveSync kept
  --max-delete" note before it becomes `BlockedDeletes.keptReason` (the
  banner says why; the card comes at once). A mark older than 30 minutes is
  dropped. Never `--force`. `guardStopHolds` looks past open runs and
  records with no phase, which never reached bisync (a refusal), to the stop
  before them: the fix's refusal, the card, the headline, `FixBanner` and
  `F` use it, so filters edited after a stop say "undo the edit first",
  never "press F".
- The run that gets the raised guard also gets `--backup-dir2
  <appState>/allowed-backup/<stop's start>`, and `allowed-run.json` keeps
  its confirmed list: `drive-sync run` execs rclone, so `checkConfirmedRun`
  runs after it in `drive-sync notify` (and in the fix's final step, and
  when `fix-run` starts, for a fix unit killed during that run), only for
  the first run after the record, when it started within 5 minutes of it
  and reached "Applying changes" (a guard stop only lists what it would
  delete), even with later runs after it. A
  deletion not in the list gives a NOTICE line (never ERROR: after
  "Bisync successful" the parser would make it a failed run),
  `unconfirmed-deletes.json` (the banner, while that run is the newest) and
  a critical card. DriveSync never empties the backup folder.
- `D`, its banner and its dialog follow `Backend.deletesHeld`: a guard stop,
  the headline `failed` or `fixFailed` (no run under way), and no fix or
  `fixWaitsForDeletes` (fix.json failed at `finishing` with the stop still
  holding, never the error text), the rule `deletes --allow` uses too. The
  dialog closes when it goes false.
- A guard stop's card repeats hourly like any problem: each tick stops again
  with a new start, so a stop is new (card at once) only when its per-side
  counts differ from the last card's (`key` in `notify-run.json`) or it is
  the run that took a confirmation.
- Every reader of the log uses `SyncLog::readRecent`: the tail, extended back
  (at most 64 MB) until it holds a whole "Synching Path1" log line
  (`SyncLog::newestRunStart`, so a file named like one does not count),
  because a stop of tens of thousands of deletions writes more than the 3 MB
  tail, and its start would be cut off. Each step back reads only the bytes
  before what it has, and when the start lands near the cut, 64 KB more:
  the "DriveSync kept" note sits just above it.

## Conventions

- C++ owns logic and anything that can fail; QML owns layout and keys. Every
  shortcut is a `Shortcut` with `Qt.ApplicationShortcut`.
- No key is printed beside a button or in a footer, nor named in a sentence
  ("Use Fix… to rebuild it", never "Press F") (Omarchy's apps do the
  same): `?` lists every key (`HelpOverlay.qml` and the README table, kept
  together), and each button names its key in a `ToolTip` on hover.
- Every visible string goes through `qsTr()`/`tr()` with `%1` placeholders and
  `%n`/`%Ln` plurals; add each new one to `src/i18n/pt-BR.json` (and to
  `en.json` when it is plural). Bindings that format times read
  `translator.language`.
- Clock times show 12 or 24 hours like the clock on the Omarchy bar, never
  by the language: the `omarchy.clock` entry of `~/.config/omarchy/shell.json`
  (`format`, or `verticalFormat` on a side bar), with Omarchy's defaults file
  when the user's has no layout and the system locale when the bar has no
  clock. The window watches the file (`ClockFormat`, passed to `format.js` as
  `clockFormat.timeFormat`), the notifications read it once, and the widget
  reads it with a `FileView`. The rule exists twice, `src/clockformat.cpp` and
  `plugin/DriveSyncModel.js`: change both together.
- Corners use `theme.radius`; small elements `Math.min(4, theme.radius)`.
- Comments explain why, in full sentences, in English.
- No em or en dashes anywhere (house rule of the owner).

## Gotchas

- **The app was SyncWatch until 2026-11-11 and drivesync until 2026-11-14.**
  `main()` first calls `migrateFromLegacyNames()`, which renames
  `~/.config/{drivesync,syncwatch}` and `~/.local/share/{drivesync,syncwatch}`
  to `drive-sync`, and `~/.local/state/{drivesync,syncwatch}` to
  `drive-sync-app`, and rewrites the paths inside `config.json` and every
  `taken-over/*/manifest.json`. The newer old name wins when both exist.
  So running `build/drive-sync` with any argument migrates the real home:
  never do it while the installed binary is still the old `drivesync`, which
  would then lose its config (test in a copied HOME instead: copy the three
  folders into a temp dir, rewrite the home in the copied JSON, run with
  `HOME=<dir>`). Since step 5.1 a copied HOME is not enough for `watchdog`
  and `accounts --migrate`: they change user units, and `systemctl --user`
  reaches the real user manager whatever HOME says. The move takes
  `systemctl` from its paths, which the tests fake; by hand it is tried
  only through the installed package. Opening the window counts too:
  `build/drive-sync` with no argument starts `accounts --migrate` on a
  legacy home (or one with a journal), run by itself (`build/drive-sync`);
  look at the window with `bin/harness` instead. `PluginSetup` swaps the `drivesync` widget for `drive-sync`
  on the bar by itself (`enable drive-sync --after drivesync`, then `disable
  drivesync`, then it removes the dangling link). The PKGBUILD
  `conflicts`/`replaces` `syncwatch` only: `drivesync` was dropped on
  2026-11-18, because that name belongs to another project in the AUR.
  Keep the old names only in those places, in `takeoverLockHeld` and in the
  tests of them.
- **`~/.local/state/drive-sync` is the old drive-sync.sh's**, not the app's: it
  holds the resync pause mark (`BackendPaths::stateDir`), and a takeover moves
  it aside whole. The app's own state therefore lives in
  `~/.local/state/drive-sync-app`; never put app files in the other one.
- **"drive-sync" is a substring of the legacy script names** (`drive-sync.sh`,
  `drive-sync-aviso`, the units' command lines). Never match the app by
  `contains("drive-sync")`: `takeoverLockHeld` reads the exact name of
  `argv[0]`, and the legacy checks match by exact file name. The same holds
  for units since step 5.1: `drive-sync@account1.service`,
  `drive-sync@.timer` and `drive-sync-fix@account1.service` all contain
  `drive-sync`, as do the plain units and `drive-sync-watchdog.timer`. Name
  a unit exactly (`accountUnits(paths)`,
  `plainDriveSyncUnits()`), and map `systemctl show` answers by `Id`.

- **A bisync run with the filters' `.md5` missing or stale destroys its base.**
  Tried with rclone 1.75 on 2026-11-08: the run stops with a critical error and
  renames the listings to `-err`, and from then on even the right filters fail
  until a resync. So `drive-sync run` checks `filtersUsable` before it execs
  rclone, the takeover copies the filters with their `.md5`, and no byte of the
  filters (comments included) may change outside a flow that resyncs. The fix
  is that flow: edit `filters.txt`, then `F`; runs are refused until it ends.
- **A bisync lock written without `--max-lock` never expires.** Tried with
  rclone 1.75.2 on 2026-11-11: a base run killed with SIGKILL and no
  `--max-lock` leaves `Expires at 0001-01-01`, so every later run fails with
  "prior lock file found". The fix's dry run and base run pass
  `--max-lock 2m` (the final run keeps the config's own), and a lock
  line in what a run just wrote (never an older line of the log) is waited
  out once, 130 s.
- **A user unit beats a packaged one of the same name.** The package ships
  `drive-sync.service` and `.timer`; they stay inert while the old ones exist in
  `~/.config/systemd/user`, which is why the takeover moves those aside and then
  checks `FragmentPath`. Rollback and undo disable one set before enabling the
  other, so there is never a second scheduler.
- `unix` is a predefined macro with GCC on Linux: a variable or lambda named
  `unix` fails to compile.
- moc reads a `'` inside a raw string literal as a char literal: a test with
  `R"(q'uote)"` made moc generate nothing ("No relevant classes found") and
  the link fail on the vtable. Use a plain escaped string there. A `\"`
  inside a raw string, or a raw string followed by another literal
  (`R"(...)" "\n"`) inside a macro, breaks moc the same way ("missing ')' in
  macro usage" on a line far below).
- **The max-delete guard, as rclone 1.75.2 does it** (tried 2026-11-12):
  each side is checked against its own count in the last listing, Path1
  first, after both sides' "File was deleted" lines are logged; the
  percentage is compared as a real number (11 of 21 stops at 52 and passes at
  53; 11 of 20 passes at 55); an abort leaves the listings as they were (no
  `-err`); a `--dry-run` writes the deletion lines as NOTICE and leaves the
  `.lst` files alone (it writes `-dry` ones). A name with `"` or `\` is
  written Go-quoted in those lines. `--backup-dir2 <dir outside Path2>` is
  accepted with local paths, also with `--dry-run` (tried 2026-11-14): every
  notebook file the run deletes (gone from the cloud) or overwrites (changed
  in the cloud) is moved there ("Moved into backup dir"); a file gone from
  the notebook is deleted in the cloud and never goes there.

- **Google sign-in goes through rclone, never through DriveSync** (tried with
  rclone 1.75.2 on 2026-11-14, scratch `RCLONE_CONFIG`, fake client, fake
  `xdg-open`). `rclone config create <name> drive client_id <id>
  client_secret <secret> scope drive` with stdin at EOF writes the section
  first, opens the browser through `xdg-open`, prints `Waiting for code...`
  and waits on port 53682; the token is written by rclone. `rclone config
  update <name> config_refresh_token true` runs the same flow on an existing
  remote (sign in again, with that remote's own client, and the config's
  `--config` when it has one). stderr carries "go to the following link:
  http://127.0.0.1:53682/auth?state=...", which `signin` relays (a
  `{"link"}` line with `--json`). `rclone config reconnect` needs a terminal (`Failed to read line: EOF`): never use it, and
  never show its advice from the log. A create killed while waiting leaves the
  section without a token, which `drive-sync signin --abandon` removes (only
  with no DriveSync config, and only DriveSync's own: type drive with the
  shipped client id). `signin` reuses such a remote (signed in again when
  lsjson refuses its token, made again when it has none) and never takes a
  `drive-sync` remote with another client. `bisync --resync --filters-file F` writes
  `F.md5`, so a new user's base run makes the md5 the normal runs need; until
  then `drive-sync run` refuses (cause `filtersChanged`, which offers `F`).
- **The OAuth client is never printed.** It reaches the code only through
  `shippedOAuthClient()`; `signin` replaces it in rclone's errors with
  `<client id>` and `<client secret>`, keeps rclone's stdout (which can hold
  the section and the token) in memory, and the tests use a made-up client.
  `src/oauthclient.local.h` is git-ignored and filled by the maintainer by
  hand; `drive-sync.pro` and the test project define `HAVE_LOCAL_OAUTH` only
  when it exists, and qmake must run again after it appears (`bin/build`
  does).
- **The page after Google is DriveSync's, through rclone** (tried with
  rclone 1.75.2 on 2026-11-20). `config create` and `config update ...
  config_refresh_token true` both take `config_template_file <file>`, and
  rclone serves that file at `http://127.0.0.1:53682/` when the browser comes
  back; the option is not written into `rclone.conf`. `signin` passes
  `signInPage()` (`src/signin-page.html`, its text through `tr()` in the
  Omarchy language, escaped with braces as `&#123;`, comments and the logo's
  `<desc>` stripped) from a 0600 `QTemporaryFile` that lives as long as the
  command; when it cannot be written the option is left out and rclone shows
  its own page. rclone renders it with Go's html/template, so what Google's
  error redirect carries comes out escaped: `.Name` is always rclone's
  "Auth Error", `.Description` is "No code returned by remote server:
  <error>: <error_description>" in rclone's English, and `.Code` and
  `.HelpURL` were empty.
  The page shows with `.OK` true as soon as Google returns a code, before
  rclone trades it for the token, so its text never says the sign-in
  worked: DriveSync shows that. A path that does not exist stops rclone
  before the browser opens, and a template that does not parse (a stray
  double brace, even inside an HTML comment) crashes rclone when the browser
  comes back. The real-rclone tests set `RCLONE_DRIVE_TOKEN_URL` to a closed
  local port (create honours it too), so no test reaches Google.
- **Every test reaches rclone in a sandbox.** `initTestCase` points
  `RCLONE_CONFIG` at a temp file and puts a fake `xdg-open` first on `PATH`
  for the whole binary. An rclone `alias` remote names bisync's listings after
  its target, not after the alias, so the end-to-end tests point
  `config.remote` at a local folder instead.
- **Cancelling a sign-in signals one pid.** `xdg-open` runs
  `omarchy-launch-browser`, and the browser can stay in rclone's process
  group, so a SIGTERM to the group would close it. `SetupController` sends
  SIGTERM to `drive-sync signin` alone; its handler (async-signal-safe: a
  flag and `kill()` of the rclone pid) stops rclone, and the command removes
  the half-made remote itself (Ctrl+C in a terminal too). The QProcess is
  unparented first, because deleting it would SIGKILL that cleanup.
- **Never use local-time `QDateTime` in hot paths.** On this machine each
  local-time conversion or comparison costs about 0.2 ms (Qt resolves the
  zone every time). `QDateTime::fromString` per log line made opening the app
  take 14 s of CPU. `synclog.cpp` builds fixed-offset times by hand, and
  `Backend` compares against `currentDateTimeUtc()`. Parsing the 3 MB tail of
  the real log takes about 90 ms.
- **`omarchy notification dismiss` matches any substring of a title on
  screen.** `/usr/bin/omarchy-notification-dismiss` passes its argument to
  `omarchy-shell -q notifications dismiss`, whose handler
  (`/usr/share/omarchy/shell/plugins/notifications/Service.qml`, checked
  2026-11-20) closes every toast whose summary contains it: case
  sensitive, plain substring, all at once. So a label always sits between
  fixed text (`<title> (<label>)`): put first, `Drive stopped syncing` would
  close `My Drive stopped syncing`. A new per-account title must not be a
  substring of another one (`cardTitlesNeverMatchAnotherAccount`).
- **Do not watch `~/.local/state` itself**: other programs write there all
  the time. The folder is watched only while the log does not exist.
- **The repo lives under `~/Drive`**, mirrored by drive-sync. Its filters skip
  `/GitHub/**/build/**`, so every build output (qmake, tests, makepkg) goes
  under `build/`. Do not let makepkg write `pkgbuild/src` or `pkgbuild/pkg`.
- **The bar widget depends on two names.** It runs `drive-sync status --json
  --all` from the PATH, and its `W` key (`plugin/open-history.sh`) focuses the
  window whose Hyprland class is exactly `drive-sync`, or starts it, with
  `drive-sync open --account <id>` when a click names an account. Renaming
  the binary, the `open` command or its `--account` option, or changing
  `setDesktopFileName` breaks it, so change the widget in the same commit.
- `QString::arg` reads `%1000` as placeholder `%10`: build microsecond values
  as numbers, never by appending zeros after `%1`.
- clangd shows "file not found" for every Qt header because there is no
  `compile_commands.json`; the qmake build is the truth.
- **After the repository moves, `make -B` once in `build/tests`.** The test
  object compiled before the 2026-11-20 rename kept the old `SRCDIR`
  (`.../GitHub/drivesync/...`): qmake's new `DEFINES` do not make `make`
  rebuild it, so 8 cases failed on paths until
  `make -B -j"$(nproc)"` there. A failure naming the old folder is that.
- **QML follows a swapped context property.** The window switches accounts
  by setting `backend` again to another `Backend` (`setContextProperty` on
  `selectedChanged`): every binding and `Connections` in every QML file
  then reads the new object and stops hearing the old one (checked on Qt
  6.12 by `contextPropertySwapRebinds`, and in `qrc:/Main.qml` through the
  harness's `--select`). That connection is made before `engine.load`, so
  it runs before QML's own `onSelectedChanged` handlers, which therefore
  already see the new `backend`. Never delete the old `Backend` before the
  swap: `AccountList` emits `selectedChanged` first, then `deleteLater`.
- Looking at the window without touching the desktop: `bin/harness` loads
  `qrc:/Main.qml` offscreen with fake `BackendPaths` (a made-up log, Drive,
  state dir, a fake `systemctl`), a fake `omarchy`, a fake `drive-sync` for
  the takeover check and a fake shell.json clock, presses `--keys`, and
  saves `window->grabWindow()`; QML warnings go to stderr. It is how the
  README window pictures are made, so it must never read this machine:
  `--real` exists for local checks only. Its takeover answer defaults to
  `managed`; `--takeover none` (and the `setup-*` scenarios) is a machine
  with no drive-sync at all, which shows the setup banner. A new context property in
  `main.cpp` needs the same line in `tools/harness/harness.cpp`, and a new
  source file the window uses needs `tools/harness/harness.pro`. Like
  `main`, it builds an `AccountList` (context property `accountList`, and
  `backend` moved to `current()` on `selectedChanged`) over made-up roots
  in its temp folder (`config/`, `app/`, `units/`, the fake paths as the
  base, `home` the temp folder it exports as `HOME`): an account the roots
  hold wins (`config-error`, `stale-config`, `move-stuck`, `two-accounts`),
  and with none the fake paths stand in, so the older scenarios (a managed
  drive-sync with no config file) look as they did. `--select <id>` selects
  that account after the load and the first wait, through the same swap a
  click or `]` makes (`--keys` sends `[` and `]` too); the status line
  prints the shown account's headline and `account=<id> accounts=<count>`.
  The harness has no pointer, so a chip's click and its tooltip are seen
  only in the real window. Its fake `drive-sync` logs each call as
  made, then drops `--account <id>` after the command before it answers;
  its `settings --json` answer is the same made-up folder for every
  account.
  It calls `startMove()` once, as `main` does, and the fake `drive-sync` answers
  `accounts --migrate --json` (`stuck` for `move-stuck`, else
  `notNeeded`); never with `--real`, where `self` would move this machine.
- The Material `Button` background (an elevation effect) does not render in
  the offscreen harness; `BarBanner.qml` draws its button's background itself.

- **Settings changes that need a resync (step 4b).** `drive-sync settings`
  writes nothing of the user's: `startFix(paths, apply, undo)` writes
  `fix.json` (every tick held from here), then `apply` writes
  `<appState>/settings-change.json` with the old and the new bytes
  (previousConfig/newConfig, previousFilters/newFilters, base64, only what
  changes), "fix" (fix.json's start; a mark serves that fix only, and
  `fix --start` clears one left behind) and "applied". The fix's preparing
  step writes the new bytes (`applySettingsChange`) only after its loop that
  waits for the account's service to be idle: a run that passed run-condition
  before the mark would otherwise read new filters against the old `.md5`,
  abort and take the base with it. `fix --cancel` at `preparing`, `confirm`,
  or `failed` from `preparing` writes the old bytes back when "applied",
  which keeps the base: tried with rclone 1.75.2 on 2026-11-15,
  `bisync --resync --dry-run` leaves the listings and the filters `.md5`
  alone (it writes `-dry` listings). From the base on the new bytes stay, and
  the unit drops the mark. New filters are checked with
  `rclone lsf <empty dir> --filter-from <file>` first ("malformed rule ..."),
  and bytes equal to the file's start nothing. The window sends the editor's
  bytes on stdin (`--filters -`). After a folder change, `takeover --undo` is
  refused until the folder is set back (the manifest records "localPath").
- **Option values are taken as they are** in `settings`, `setup` and
  `folders` (a cloud folder may be called `--json`); `folders` takes its path
  after `--`, and the window always passes it so.
- **`--account <id>` is taken only as the first two arguments** after the
  command (`takeAccountOption`), so a cloud folder named `--account` stays a
  value. `main` resolves once and hands everything to `dispatchToAccounts`:
  `run-condition` skips (1) while `migration.json` exists, before any config
  is read; then `chooseAccounts` decides. `status`, `sync`, `pause` and
  `resume` act on every account (their `--json` keeps `ok` and `error`, the
  first account's that failed, and adds `accounts`; `resume` with several
  leaves an account that is not paused alone); `fix`, `deletes`,
  `settings`, `folders` and `signin` act on the only one. What the units call
  (`run-condition`, `run`, `notify`, `fix-run`) takes no `--account` in
  legacy mode and needs one in accounts mode, the mode checked before the
  id: legacy mode lists `account1` too, and an instance must never run the
  account the plain units run. One exception keeps the pre-5.1 rule: a
  plain `run-condition` with no account at all and `singleConfigGone`
  exits 255 with the loader's error, a failure systemd records, not a skip
  (`configGone` is its card). An instance whose account is not listed
  still skips (1). Since 5.2 every child the window runs names its account
  at index 1 (`sync --account account1 --json`): `Backend::control` (sync,
  pause, resume, fix, deletes), `SettingsController` (built when queued, so
  a queued change and the read after it keep their account; its `account`
  is set when Settings opens, and a read of another account fills nothing),
  `FolderTree` in Settings and `SetupController::signIn(account)` for a
  sign-in again (no `signin --abandon` after cancelling one). The view with
  no account (empty id) passes none, and `accounts --migrate`, `takeover`,
  `setup`, and the first setup's `signin`, `signin --abandon` and
  `folders --remote drive-sync:` never carry one.
- **The move to accounts (step 5.1, `src/accountmove.cpp`).** `accounts/`
  under `~/.config/drive-sync` decides the mode, so `config.json` moves last
  and `<configRoot>/accounts/account1` is made just before it: until then
  the resolver stays in legacy mode. The order: every check with nothing
  touched (`moveBlocker`: legacy, guard, takeover lock, FixLock probed
  without creating the file, no target already there, `machine.json`
  absent, the three `account1` instances packaged with no drop-in, no
  foreign drop-in, no old drive-sync, plain service and fix unit idle);
  the takeover lock, then the FixLock (no wait), held to the end; the
  journal `<stateRoot>/migration.json` (every planned move, what was made,
  the plain timer's enabled and active); `disable --now drive-sync.timer`;
  the plain service and fix unit idle again (a tick in between defers);
  the renames (`renameat2` with `RENAME_NOREPLACE`, so nothing is ever
  overwritten); the moved JSON files rewritten through an exact map
  (each moved path and the config, never `<stateRoot>` as a prefix, so
  `<stateRoot>/bisync` stays), keeping their compact or indented layout;
  `machine.json`; the config; `daemon-reload` and `checkPackagedUnits` with
  the moved interval file allowed; the instance timer enabled and started
  as the plain one was; the old `conflicts.json` deleted only then; the
  journal removed. Each folder or file it makes goes into the journal
  before it is made. Any failure, and any journal found by a later trigger
  with no live drive-sync holding the takeover lock, goes through one
  rollback that reads only the journal: the instance timer off, each move
  back in reverse, the inverse rewrite, what it made removed (folders while
  empty), `daemon-reload` (a failure tolerated), the plain timer as
  recorded, the journal removed. It counts as done only when the resolver
  reads legacy mode again; otherwise (two copies of a file, a file gone)
  the journal stays with `"stuck": true`, no timer comes back, and the
  resolver reports `moveStuck`. The rollback of an orphan journal takes the
  takeover lock and then holds the FixLock as the move does, on `fix.lock`
  wherever it is now (the new place, else the old one), opened without
  creating anything (`OrphanFixLock`); a held one defers. `fix.lock` carries no data: one at both
  ends (a writer that waited on the old path made it again) loses the
  source copy, and an empty one already at the target is replaced. A
  plain timer that will not come back still ends the rollback (legacy
  mode, timer off, which the watchdog reports) rather than holding every
  tick behind a journal; what the timer was goes to
  `<stateRoot>/plain-timer.json` (outside the moved entries), which the
  next move ORs into its journal, so `drive-sync@account1.timer` comes on
  as `drive-sync.timer` was, and which a move that holds or a rollback that
  puts the timer back removes. `takeTakeoverLock` makes its check, the
  removal of a stale pid file and the new one a single step under an
  flock on the state root folder (no file of its own); the pid file is
  still the lock every reader and an older drive-sync check. Task 1 (2026-11-19) found that a started timer
  whose `OnBootSec` passed fires at once, and that a start `ExecCondition`
  skips still gives `OnUnitActiveSec` its base, so the move starts no
  extra run.
- **The interval drop-in.** `intervalDropInPath(userUnits, timerUnit)`:
  `~/.config/systemd/user/drive-sync@<id>.timer.d/drive-sync-interval.conf`
  for an account in accounts mode (the move carries it there), and
  `drive-sync.timer.d/` for the single config of legacy mode. It resets the timer list with an empty `OnUnitActiveSec=` (systemd.timer(5):
  that resets every timer, so it repeats `OnBootSec=5min`), then sets the new
  value. At 15 minutes there is no drop-in at all, because the takeover
  refuses any drop-in on the units (`dropIn` blocker, `checkPackagedUnits`),
  and `undoTakeover` removes ours before anything moves (a removal that
  fails stops the undo; a later refusal writes it back). The config
  keeps `intervalMinutes` (optional in config version 1, 15 when absent);
  "late" reads it from there, never from systemd. Not verified on a live
  timer: whether daemon-reload alone re-arms it (the command also runs
  `try-restart` on the account's timer).
- **Selective sync rules** (tried with rclone 1.75.2): the block sits at the
  top because filter rules are first-match; each line is
  `- /<folder>/**` with `\ * ? [ ] { }` escaped by a backslash; a name with a
  line break cannot be left out. A folder left out after it was synced keeps
  its local copy (bisync ignores it on both sides from then on); ticking it
  again is a union, so its local copy goes back up, files deleted in the cloud
  meanwhile included. A block line the code never writes (a `+` rule) makes
  the block "edited by hand", and the tree refuses to change it. A block keeps
  its line ends (CRLF too) and its blank line, or lack of one, after `# END`.
  The tree keeps only listing entries right below the folder listed: one
  named after the folder itself would nest the tree in itself (a crash seen
  with a wrong fake). `drive-sync folders` passes SIGTERM to its rclone and
  sets PR_SET_PDEATHSIG on it; the window stops a listing with SIGTERM.
- **The filters screen (step 4c) owns three blocks at the end of the
  filters file**: `# BEGIN DriveSync: ready-made groups` (a `# group: <id>`
  line before each group's exact rules), `paths on this computer` and
  `rules by name`, written in that order after every other byte. rclone
  takes the first match, so text written by hand (the advanced part, the
  cloud-folders block at the top included) keeps winning over them. A block
  with a line the screen never writes is read as advanced text, whole, so
  nothing is dropped. An import moves a plain line into the screen only
  when the text has no `+` or `!` rule, since moving an exclude below an
  include could change what matches first. `defaultFilters()` is the
  default groups' block under a short header. The blocks are read only when
  nothing but blank lines follows them; text after one (a hand edit) makes
  the whole file advanced text, so nothing moves. The Filters page saves
  only when `FilterEditor::edited()` (by meaning, never by bytes) says so,
  since a save starts a resync. Values are never trimmed except where they
  are typed: a tree path may begin or end with a space. rclone 1.75.2 drops
  the spaces at the end of a rule line, escaped or not (tried 2026-11-17),
  so a final space is written as `[ ]`; a name with a line break cannot be
  a rule, and the local tree shows it always synced.
- **The harness sets HOME to its temp folder** (unless `--real`), with a
  made-up synced folder and a link in it: the filters screen lists the
  folder in the setup field, and `~/Drive` must never be this machine's.
- moc also breaks on JSON in raw string literals inside a test macro ("missing
  ')' in macro usage" at the end of the file): build test JSON with
  `QJsonObject`.

### Bar plugin (`plugin/`)

- Omarchy loads third-party plugins only from `~/.config/omarchy/plugins/<id>/`
  (`PluginRegistry.qml` scans `"$dir"/*/`, which follows a symlink). A pacman
  package cannot install there, so `PluginSetup` links
  `~/.config/omarchy/plugins/drive-sync` to the package's copy and, after the
  user agrees (`B`), runs `omarchy plugin enable drive-sync`. A folder or a
  foreign link at that path is never touched.
  `N` says no and is remembered in `~/.local/state/drive-sync-app/bar-declined`:
  no offer and no new link afterwards, so a removal stays removed. `N` also
  hides the "blocked" notice (someone else's folder or link at that path)
  the same way. A failed `omarchy plugin list` before the user pressed `B`
  is state `unchecked`, with no banner; after `B` it is `failed`.
- A package upgrade reaches the bar only after a shell reload
  (`omarchy restart shell`): the plugin watcher's inotify does not follow the
  link into `/usr/share`.
- `omarchy plugin validate ~/.config/omarchy/plugins/drive-sync` rejects the
  link ("symlinks are not allowed inside a plugin folder"); validate
  `plugin/`, or the link with a trailing slash.
- Never link `plugin/` from the repo by hand before installing the matching
  binary: an older `drive-sync` has no `status` command and opens its window
  instead of answering, every poll, until the 45 s watchdog stops it.
- Any file written under `~/.config/omarchy/plugins/` hot-reloads the plugin;
  for a few seconds the icon shows the unknown state. Every reload also logs
  "ShellIpc ... another handler is registered"; IPC keeps answering.
- The IPC function cannot be named `sync`: it collides with the
  `DriveSyncService` id. It is `syncNow()`: it answers `started` when any
  account can sync now (`anyCanSync`) and runs one sync per such account.
- The widget polls `drive-sync status --json --all` and accepts version 3
  only (`parseStatus` in `DriveSyncModel.js`); plain `--json` stays version
  2 for an older widget still loaded until the shell reloads. The icon shows
  the account the JSON names `worst` (`shownAccount`); an empty list is
  `noLog`. With one account the panel is 5.1's; from two on, a `Repeater`
  lists each account with its own sync, pause or resume and folder, and a
  general row syncs every account that can, pauses all or resumes the paused
  ones, and opens the window. The cursor walks `panelTargets`; `S`, `P`, `O`
  and `W` act on the account under it (`cursorAccount`), else on all. The
  quick sync (right click, `S`, IPC `syncNow()`) runs one `drive-sync sync
  --account <id>` per account whose `canSyncNow` is true, in turn, because
  the plain `sync` stops at the first account that refuses.
- Untrusted JSON on its way to argv: every command comes from a builder in
  `DriveSyncModel.js` (`syncCommand`, `pauseCommand`, `resumeCommand`,
  `openArgs`...), which puts `--account <id>` right after the command word
  and returns null for an id that does not match `^account([1-9][0-9]?)$`;
  an entry with such an id is dropped whole by `parseAccount`. A folder is
  kept only when absolute with no control character, and the folder button,
  `O` and the middle click open the account's own `folder` from the JSON
  (`folderToOpen`): `$HOME/Drive` only when no account is known at all, and
  nothing for an account whose folder was refused. Labels (folder names) go
  into translated text through `fillText`, never chained `arg()`, which
  would fill a `%2` inside a label. A source-level test checks that every
  `command` in `DriveSyncService.qml` comes from those builders.
- `open-history.sh` takes one optional argument, the account to open the
  window on: only `^account[1-9][0-9]?$` passes, as one argument to
  `drive-sync open --account`; anything else starts the window with no
  account. An open window is only focused, whatever the argument. Its node
  test runs it with nothing but fakes on `PATH`, so a missing fake can never
  reach the real `uwsm-app` and open this machine's window.
- `drive-sync status --json --all` output is parsed in
  `StdioCollector.onStreamFinished`, never in `onExited`: the two have no
  fixed order. A binary that fails to start ends the process without ending
  the stream or emitting `exited`, so `onRunningChanged` applies an empty
  answer when nothing streamed, and a 45 s watchdog stops a poll that never
  ends: both show unknown, never the last good snapshot. Only a source-level
  test covers this; the live check is opening the panel with `drive-sync`
  missing.
- Every QML `Text` sets `textFormat: Text.PlainText` (a test enforces it):
  `causeDetail` comes from the log, which is untrusted, and `cleanText` caps
  it at 200 characters on one line.
- `open-history.sh` finds the window by exact class `drive-sync`, because
  `omarchy-launch-or-focus` also matches titles.
- A catalog key the installed widget reads never changes meaning: a new
  meaning is a new key. Until `omarchy restart shell` the running shell keeps
  the old widget, but Omarchy's `Translations.qml` reloads `plugin/i18n/*.json`
  as soon as the package replaces them, so a key whose text changed (`Open
  %1` where 5.1 read `Open ~/Drive`) shows a bare `%1` there. The node test
  `keys the installed widget reads keep their text` compares every key the
  widget at HEAD reads with its HEAD text in both catalogs.

## Deploy

Local only: `bin/install` installs the package with pacman, which puts
`/usr/bin/drive-sync`, the `.desktop` entry, the icon and the bar widget in
place, so it shows up in the Omarchy launcher. The DIY repository records it in
`docs/omarchy.md` (packages built from a local repository). Bump `pkgver` in
`pkgbuild/PKGBUILD` and `app.setApplicationVersion` in `src/main.cpp`
together, after closing the changelog.

The first install of step 5.1 moves the maintainer's single drive-sync to
`account1` (the watchdog, the window at start, or `drive-sync accounts
--migrate`): it turns `drive-sync.timer` off and `drive-sync@account1.timer`
on, and moves the interval drop-in. That is a notebook customization, so
the DIY repository follows in the same session (its `docs/omarchy.md`
drive-sync row and Parte 5, its `[Unreleased]`); the plan of step 5.1 keeps
the drafts and the checks to run by hand (its Task 16).

The project page publishes itself: a push to `master` that touches `site/`,
`assets/icons/`, `assets/images/` or the workflow runs `pages.yml` (the repo's
Pages source is "GitHub Actions"). To preview it locally, copy the icon to
`site/favicon.svg` and `window-runs.png` to `site/images/` (both git-ignored)
and open the HTML file; delete the copies afterwards, since `site/` syncs to Drive.

- **`site/privacy.html` and `site/terms.html` are what Google's sign-in
  screen links to.** The Branding of the DriveSync Google Cloud project
  points at `https://drive-sync.selflabs.org/`, `.../privacy.html` and
  `.../terms.html`, with `selflabs.org` as the authorized domain (the
  brand was never approved on `cateim.github.io`, so on 2026-11-20 the page
  moved to its own subdomain). The page is GitHub Pages with a custom
  domain: the repo's Pages `cname` is `drive-sync.selflabs.org` (set
  through the API, no CNAME file, since the workflow publishes), Cloudflare
  has a DNS-only CNAME to `cateim.github.io` (never proxied, or GitHub's
  certificate fails), and `selflabs.org` is a verified Pages domain of the
  CaTeIM account (TXT `_github-pages-challenge-cateim`), which keeps anyone
  else from taking the subdomain. `cateim.github.io/drive-sync/` redirects
  there. A change in what the app reads, stores or sends changes the
  privacy page, its date and its pt-BR text in `site/i18n.js` in the same
  commit. Renaming either page moves the URL with no redirect, so it means
  updating the Branding links the same day.
- Every element with `data-i18n` (or `-content`, `-alt`) needs its pt-BR
  line in `site/i18n.js`; a missing one stays English and warns in the
  console.
