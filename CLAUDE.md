# CLAUDE.md - drivesync

Single source of truth for agents and humans working on this repository.
`AGENTS.md` only points here.

## Overview

DriveSync is an Omarchy desktop app (Qt 6, C++ and QML) that shows the state
of `drive-sync`, the rclone bisync of `~/Drive` with Google Drive that runs
from the `drive-sync.timer` user unit every 15 minutes, in a window, on the
Omarchy bar (its widget ships in the package) and through
`drive-sync status --json`. It reads the rclone log, asks systemd, and looks
for conflict copies. It writes its own state (`~/.local/state/drive-sync-app/`),
the link to its bar widget, and, when the user accepts, the bar layout through
`omarchy plugin`. Since step 2 it can take over a drive-sync made by
`drive-sync.sh` (`drive-sync takeover`): its packaged user units then run the
same rclone command from `~/.config/drive-sync/config.json`, and the window,
the bar and the command line can sync now, pause and resume. Since step 3 it
fixes a broken base (`F`, `drive-sync fix`): a resync with the folder in use,
run by `drive-sync-fix.service`, which only starts after the user confirms a
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
that name must never come back. Things named after the repository keep
`drivesync` until the repository is renamed: `drivesync.pro`,
`tests/drivesync_tests.*`, `fakeDrivesync` and the `action.drivesync` i18n key.

## Stack

- C++17, Qt 6 (core, gui, qml, quick, quickcontrols2, dbus), qmake.
- QML with the Material style, dark theme, Omarchy accent.
- Qt Test for tests, run offscreen.
- Packaged with a local `PKGBUILD` and `makepkg`. No other dependencies.

## Structure

```text
drivesync.pro           qmake project
bin/build               qmake6 + make into build/drive-sync
bin/test                builds and runs build/tests/drivesync_tests offscreen
bin/install             bin/build, then makepkg -fsi with BUILDDIR/PKGDEST under build/
bin/harness             builds tools/harness into build/harness and runs it with the given options
src/main.cpp            commands run headless (status, sync, pause, resume, takeover, fix, deletes, signin, setup, settings, folders, and run-condition, run, notify, watchdog, fix-run for the units); otherwise the window
src/synclog.{h,cpp}     pure parser of the rclone bisync log into SyncRun records
src/snapshot.{h,cpp}    buildSnapshot(): the one place the state is decided; tone; the JSON contract
src/backend.{h,cpp}     the window's state: log watch, systemctl calls, conflict scan, headline rules (summarize)
src/clockformat.{h,cpp}  12 or 24 hours, read from the Omarchy bar clock in shell.json
src/conflictcache.{h,cpp}  conflict count shared through ~/.local/state/drive-sync-app/conflicts.json
src/statuscommand.{h,cpp}  `drive-sync status [--json]`: same sources, read once, synchronously
src/syncconfig.{h,cpp}  the config a takeover writes; rclone arguments; filtersUsable (the md5 check)
src/pausestate.{h,cpp}  the pause mark (~/.local/state/drive-sync-app/paused.json) and "next 08:00"
src/controlcommands.{h,cpp}  sync, pause, resume, run-condition, run (execs rclone); runProcess
src/fixstate.{h,cpp}    the fix's state machine in ~/.local/state/drive-sync-app/fix.json (steps, questions)
src/fixcommand.{h,cpp}  `drive-sync fix` and `fix-run` (drive-sync-fix.service): dry run, base run, questions, final run
src/listingdiff.{h,cpp}  reads bisync's .lst listings (Go %q names) and compares both sides, as drive-sync.sh did
src/blockeddeletes.{h,cpp}  a max-delete stop: its deletions from the log, the one-run mark (allow-deletes.json), the raised guard, `drive-sync deletes`
src/legacymigration.{h,cpp}  moves the SyncWatch and drivesync folders to the drive-sync names (state to drive-sync-app); first thing main() does
src/notifier.{h,cpp}    notify (ExecStopPost) and watchdog: the port of drive-sync-aviso
src/takeover.{h,cpp}    detect, apply with rollback, undo; `drive-sync takeover`
src/oauthclient.{h,cpp}  shippedOAuthClient(): the Google client, compiled in only from the git-ignored src/oauthclient.local.h (template: src/oauthclient.local.h.example)
src/signin.{h,cpp}      `drive-sync signin [--abandon]`: rclone's browser flow creates the drive-sync remote, or refreshes the config's remote
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
src/StatusPanel.qml     headline, details, progress, warning chips
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
plugin/                 Omarchy bar widget (id drive-sync), installed to /usr/share/drive-sync/omarchy-plugin; reads `drive-sync status --json`
site/                   project page on GitHub Pages (cateim.github.io/drivesync): index.html, privacy.html, terms.html, style.css, i18n.js (pt-BR and the EN/PT switch)
.github/workflows/pages.yml  publishes site/, copying the icon and window-runs.png in from assets/
docs/superpowers/       git-ignored, local only: design (specs/) and step plans (plans/)
pkgbuild/               PKGBUILD, .desktop, units/ (drive-sync, drive-sync-watchdog and drive-sync-fix user units, /usr/lib/systemd/user)
assets/icons/           launcher icon, installed by the PKGBUILD, and the logo of the Google Cloud consent screen (no tile, dark outline; `google-cloud-logo-120.png` beside it is the 120 px upload, rendered with `rsvg-convert -w 120 -h 120 assets/icons/drive-sync.svg -o assets/icons/google-cloud-logo-120.png`, so redo it whenever the SVG changes)
assets/images/          README pictures: window-*.png from bin/harness over a made-up log; bar-*.png are real screens with no file names
```

## Commands

```sh
bin/build            # about 30 s from clean
bin/test             # 304 cases, about 24 s (twelve run real rclone on temp folders)
bin/install          # needs sudo (pacman -U); run it in a real terminal
bin/harness --scenario running --clock "h:mm AP" --out build/shot.png   # the window offscreen, made-up data; options atop tools/harness/harness.cpp
makepkg -f           # from pkgbuild/, with BUILDDIR and PKGDEST set as in bin/install, to check packaging without sudo
build/drive-sync status --json   # the state as the bar sees it (0.13 s with a fresh conflict cache)
(cd plugin && node --test tests/*.test.mjs)   # bar widget, 26 cases; the glob is required on Node 22
build/drive-sync takeover --check              # read-only: what a takeover would move, or what blocks it
bin/harness --scenario setup-offer --keys o --out build/shot.png   # first setup; also setup-signing-in, setup-folder, signin-needed
bin/harness --managed --keys ",w^2w>w" --out build/shot.png   # Settings, Cloud folders page; ^3 Filters, ~X types an X
omarchy plugin validate plugin                # needs the folder path, not the plugin id
```

## How the state is decided

`buildSnapshot()` (`src/snapshot.cpp`, pure, tested) is the only place: the
window's `Backend`, `drive-sync status` and, through its JSON, the bar widget
all show what it decides. It finds the running run, the last success, the
last run, `failingSince`, `canSyncNow` and the `tone` (`ok`, `busy`,
`warning`, `error`), and calls `Backend::summarize`, which picks one
headline, most urgent first:
`unavailable` (systemctl failed), `notLoaded`, `fixing`, `fixWaiting`, `fixFailed`,
`running`, `resync`, `paused`, `noLog`,
`failed`, `interrupted`, `skipped` (ExecCondition: emergency home), `timerOff`,
`stale` (no success for three intervals, 45 min at the default 15; the
interval comes from the config through `BackendPaths::intervalSeconds`), `noDrive`, `noRuns` (a log with no run:
new or rotated), `conflicts`, `ok`. What the
headline leaves out goes to `warnings`: `timerOff`, `timerDisabled`,
`conflicts`.

- A run is "running" when systemd says the service is active, when the resync
  pause mark holds a live `drive-sync.sh` pid, or when the open run wrote to
  the log in the last 150 s (the log is watched, systemd is only polled).
- systemd's `Result` counts only when no log run started after the service's
  `ExecMainStartTimestamp`: `drive-sync.sh resync` runs rclone outside the unit.
  A skip (`exec-condition`) has no main start, so it is the headline only
  when no log run started after its `InactiveEnterTimestamp`: the fix's
  final run, outside the unit, comes after the ticks it held back.
- Failure causes match `drive-sync-aviso`: lost listings, out of sync, else the
  first error that is not the generic "Bisync aborted" line.
- The JSON (`snapshotToJson`, version 2) always has the same 20 keys (version
  1 plus `managed`, `paused`, `pausedUntil`); times are Unix seconds, `0` when
  unknown. The widget rejects any other version, so a change to the keys means
  a new version and a widget that reads it, installed together.
- A pause is a mark file, not a stopped timer: `run-condition` skips each tick
  while it holds and clears it at the first tick after its end, so "paused"
  lasts until a run really starts and survives a reboot.
- A fix is a mark file too: `fix.json` is written before anything else and
  `run-condition` skips every tick while it exists, so the timer, `S`, `U` and
  the bar's sync wait for the whole fix (spec section 5). Its steps:
  `preparing` (wait for a run, dry run into `fix-dry-run.log`), `confirm`
  (the user), `base` (the resync, into the main log), `questions` (the
  user), `finishing` (answers, then a normal run through `runCommandPlan`),
  or `failed`. `fix-run` loops until a step waits for the user, fails or ends,
  and sends a card at once when it does (the watchdog repeats it hourly). It
  checks the config's `runOnlyWhen` before every step, `fix --start` also
  refuses after a run that `--max-delete` stopped (`causeKind` `maxDelete`,
  spec 5b), and a final run that failed and took the listings with it sends
  the retry back to `base`.
- A fix shows as `fixing` (busy; the JSON's `resync` is true), `fixWaiting`
  (confirm or questions) or `fixFailed`. A working step with
  `drive-sync-fix.service` inactive is interrupted (`causeKind`
  `fixInterrupted`), but only 60 s after `stepStarted`: `fix` writes the step
  before systemd starts the unit. The `systemctl show` call describes three
  units (service, timer, fix unit), and every reader requires all three.
- Every writer of `fix.json` (the `fix` command, `fix-run`, the watchdog's
  reminder) takes `FixLock` (an flock on `fix.lock`) around its read and
  write, never across a long step. The fix and the takeover find the bisync
  listings by `bisyncSessionName(remote, local)`, the name rclone gives them
  (checked against rclone 1.75.2), never "the only pair in the work dir".
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
  base step removes the mark.
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
  `HOME=<dir>`). `PluginSetup` swaps the `drivesync` widget for `drive-sync`
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
  `argv[0]`, and the legacy checks match by exact file name.

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
  hand; `drivesync.pro` and the test project define `HAVE_LOCAL_OAUTH` only
  when it exists, and qmake must run again after it appears (`bin/build`
  does).
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
- **Do not watch `~/.local/state` itself**: other programs write there all
  the time. The folder is watched only while the log does not exist.
- **The repo lives under `~/Drive`**, mirrored by drive-sync. Its filters skip
  `/GitHub/**/build/**`, so every build output (qmake, tests, makepkg) goes
  under `build/`. Do not let makepkg write `pkgbuild/src` or `pkgbuild/pkg`.
- **The bar widget depends on two names.** It runs `drive-sync status --json`
  from the PATH, and its `W` key (`plugin/open-history.sh`) focuses the window
  whose Hyprland class is exactly `drive-sync`. Renaming the binary or changing
  `setDesktopFileName` breaks it, so change the widget in the same commit.
- `QString::arg` reads `%1000` as placeholder `%10`: build microsecond values
  as numbers, never by appending zeros after `%1`.
- clangd shows "file not found" for every Qt header because there is no
  `compile_commands.json`; the qmake build is the truth.
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
  source file the window uses needs `tools/harness/harness.pro`.
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
  waits for drive-sync.service to be idle: a run that passed run-condition
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
- **The interval drop-in.** `~/.config/systemd/user/drive-sync.timer.d/drive-sync-interval.conf`
  resets the timer list with an empty `OnUnitActiveSec=` (systemd.timer(5):
  that resets every timer, so it repeats `OnBootSec=5min`), then sets the new
  value. At 15 minutes there is no drop-in at all, because the takeover
  refuses any drop-in on the units (`dropIn` blocker, `checkPackagedUnits`),
  and `undoTakeover` removes ours before anything moves (a removal that
  fails stops the undo; a later refusal writes it back). The config
  keeps `intervalMinutes` (optional in config version 1, 15 when absent);
  "late" reads it from there, never from systemd. Not verified on a live
  timer: whether daemon-reload alone re-arms it (the command also runs
  `try-restart drive-sync.timer`).
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
  `DriveSyncService` id. It is `syncNow()`, and it checks `canSyncNow` first.
- `drive-sync status --json` output is parsed in
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

## Deploy

Local only: `bin/install` installs the package with pacman, which puts
`/usr/bin/drive-sync`, the `.desktop` entry, the icon and the bar widget in
place, so it shows up in the Omarchy launcher. The DIY repository records it in
`docs/omarchy.md` (packages built from a local repository). Bump `pkgver` in
`pkgbuild/PKGBUILD` and `app.setApplicationVersion` in `src/main.cpp`
together, after closing the changelog.

The project page publishes itself: a push to `master` that touches `site/`,
`assets/icons/`, `assets/images/` or the workflow runs `pages.yml` (the repo's
Pages source is "GitHub Actions"). To preview it locally, copy the icon to
`site/favicon.svg` and `window-runs.png` to `site/images/` (both git-ignored)
and open the HTML file; delete the copies afterwards, since `site/` syncs to Drive.

- **`site/privacy.html` and `site/terms.html` are what Google's sign-in
  screen links to.** The Branding of the DriveSync Google Cloud project
  points at `https://cateim.github.io/drivesync/`, `.../privacy.html` and
  `.../terms.html`, with `cateim.github.io` as the authorized domain
  (verified in Search Console through the `cateim.github.io` user-site
  repository). A change in what the app reads, stores or sends changes the
  privacy page, its date and its pt-BR text in `site/i18n.js` in the same
  commit. Renaming either page or the repository moves the URL with no
  redirect, so it means updating the Branding links the same day.
- Every element with `data-i18n` (or `-content`, `-alt`) needs its pt-BR
  line in `site/i18n.js`; a missing one stays English and warns in the
  console.
