import QtQuick
import Quickshell
import Quickshell.Io
import "DriveSyncModel.js" as Model

// Polls `drive-sync status --json --all` (every account, version 3) and runs
// the widget's actions through `drive-sync` too (sync, pause, resume), so the
// widget never decides what a command may do: the app refuses what does not
// make sense. Every command comes from a Model builder, which only ever puts
// a checked account id in argv.
Item {
  id: root

  property var settings: ({})

  readonly property string home: Quickshell.env("HOME") || ""
  readonly property string historyPath: localPath("open-history.sh")

  readonly property int refreshIntervalSec: Model.clampInt(setting("refreshIntervalSec", 30), 30, 10, 600)
  // While a run is going, poll fast so the end shows up within seconds.
  readonly property int pollMs: Model.anyBusy(all) || starting ? 5000 : refreshIntervalSec * 1000

  // Every account as the app printed it, and the one the icon shows.
  property var all: Model.parseStatus("")
  readonly property var status: Model.shownAccount(all)
  property bool starting: false
  property string actionError: ""
  // Whether the current poll delivered its output; see statusProcess.
  property bool streamed: false
  // The syncs still to start, one after another through startProcess.
  property var syncQueue: []
  // The first refusal of the current round of syncs, and whether any started.
  property string syncError: ""
  property bool syncStarted: false

  function localPath(name) {
    return decodeURIComponent(String(Qt.resolvedUrl(name)).replace(/^file:\/\//, ""))
  }

  function setting(name, fallback) {
    var value = settings ? settings[name] : undefined
    return value === undefined || value === null ? fallback : value
  }

  function refresh() {
    if (statusProcess.running) return
    streamed = false
    statusProcess.running = true
    watchdog.restart()
  }

  // Starts the given syncs in turn. The list is taken once, here: an
  // account's canSyncNow turns false as soon as its run starts, so working it
  // out again halfway would skip or repeat accounts.
  function startSyncs(commands) {
    if (startProcess.running || syncQueue.length > 0) return
    var queue = []
    for (var i = 0; commands && i < commands.length; i++) if (commands[i]) queue.push(commands[i])
    if (queue.length === 0) return
    actionError = ""
    syncError = ""
    syncStarted = false
    starting = true
    syncQueue = queue
    runNextSync()
  }

  function runNextSync() {
    if (syncQueue.length === 0) {
      actionError = syncError
      // Every account refused: nothing is starting, as with one account.
      if (!syncStarted) starting = false
      settle.restart()
      return
    }
    startProcess.command = root.syncQueue[0]
    syncQueue = syncQueue.slice(1)
    startProcess.running = true
  }

  // Every account that can sync now: the right click, S with no account under
  // the cursor, and the IPC.
  function syncAll() {
    startSyncs(Model.syncCommands(all))
  }

  function syncAccount(index) {
    var account = all.accounts[index]
    if (!account || !Model.canSyncNow(account)) return
    startSyncs([Model.syncCommand(account.id)])
  }

  function control(command) {
    if (!command || controlProcess.running) return
    actionError = ""
    controlProcess.command = command
    controlProcess.running = true
  }

  // One hour on the bar: a pause with no end is never one key away. The
  // window offers the other lengths. With several accounts: resume the paused
  // ones when any is, otherwise pause them all; with one, as before.
  function togglePauseAll() {
    if (Model.anyPaused(all)) control(Model.resumeAllCommand())
    else if (Model.anyCanPause(all)) control(Model.pauseAllCommand())
  }

  function togglePauseAccount(index) {
    var account = all.accounts[index]
    if (!account) return
    if (Model.canResume(account)) control(Model.resumeCommand(account.id))
    else if (Model.canPause(account)) control(Model.pauseCommand(account.id))
  }

  // The DriveSync window: focused when open, started otherwise, on the given
  // account ("" for none).
  function openWindow(id) {
    var args = Model.openArgs(root.historyPath, id)
    if (args) Quickshell.execDetached(args)
  }

  // The account's own folder, as the app printed it; ~/Drive only when no
  // account is known at all.
  function openFolder(folder) {
    var target = Model.folderToOpen(root.all, folder, root.home)
    if (target !== "") Quickshell.execDetached(["uwsm-app", "--", "nautilus", target])
  }

  Timer {
    interval: root.pollMs
    repeat: true
    running: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  Process {
    id: statusProcess
    // A fixed argument list: nothing from the shell or the settings reaches it.
    command: ["drive-sync", "status", "--json", "--all"]
    // Parse when the stream ends, not on exit: the two signals have no fixed order.
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: { root.streamed = true; root.applyStatus(text) }
    }
    onExited: function(exitCode) {
      if (exitCode !== 0) console.warn("[drive-sync] drive-sync status exited with " + exitCode)
    }
    // A missing binary fails to start: Quickshell then ends the process
    // without ending the stream or emitting exited, so this is the only signal.
    onRunningChanged: { if (!running) { watchdog.stop(); if (!root.streamed) root.applyStatus("") } }
  }

  // A poll that never ends (an old drive-sync opens its window instead of
  // answering) is stopped, and the bar says it does not know. Longer than a
  // cold conflict scan (30 s) plus a slow systemctl (5 s).
  Timer {
    id: watchdog
    interval: 45000
    onTriggered: {
      statusProcess.running = false
      root.applyStatus("")
    }
  }

  function applyStatus(raw) {
    var parsed = Model.parseStatus(raw)
    // A missing or old drive-sync (no version 3) shows as "unknown", never as
    // the last good answer.
    if (!parsed.ok) console.warn("[drive-sync] drive-sync status printed no version 3 status")
    all = parsed
    if (Model.anyBusy(parsed) && syncQueue.length === 0 && !startProcess.running) starting = false
  }

  Process {
    id: startProcess
    // Queues one run and returns at once; the run takes minutes.
    stderr: StdioCollector { id: startErr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        var error = Model.cleanText(startErr.text) || ("drive-sync sync exit " + exitCode)
        if (root.syncError === "") root.syncError = error
        console.warn("[drive-sync] drive-sync sync failed: " + error)
      } else {
        root.syncStarted = true
      }
      // The next account goes whatever this one answered.
      root.runNextSync()
    }
  }

  // Pause or resume. Its only way in is the function above, given a Model
  // builder's list.
  Process {
    id: controlProcess
    stderr: StdioCollector { id: controlErr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        root.actionError = Model.cleanText(controlErr.text) || ("drive-sync exit " + exitCode)
        console.warn("[drive-sync] " + root.actionError)
      }
      root.refresh()
    }
  }

  // systemd takes a moment to flip the unit to activating; look again soon,
  // and give up on "starting" if the run never showed up.
  Timer {
    id: settle
    interval: 1500
    onTriggered: {
      root.refresh()
      giveUp.restart()
    }
  }

  Timer {
    id: giveUp
    interval: 20000
    onTriggered: root.starting = false
  }
}
