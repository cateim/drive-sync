import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "DriveSyncModel.js" as Model

// DriveSync's bar widget for drive-sync. The icon says at a glance how the
// sync is doing (the account that needs you most); the panel says when, what
// failed, and runs one now. From two accounts on, it lists each one. The
// state comes from the app (`drive-sync status --json --all`), so the bar and
// the window can never disagree.
Panel {
  id: root
  moduleName: "drive-sync"
  ipcTarget: "drive-sync"
  manageIpc: false

  readonly property var all: sync.all
  // The account the icon shows; with one account, that account.
  readonly property var status: sync.status
  readonly property var accounts: root.all.ok ? root.all.accounts : []
  // With one account (or none) the panel is the one-account panel of 5.1.
  readonly property bool several: root.accounts.length >= 2
  readonly property var targets: Model.panelTargets(root.all)
  readonly property string syncState: Model.barState(status)
  readonly property bool canSync: Model.anyCanSync(root.all) && !sync.starting
  readonly property string causeText: causeFor(status)
  // The folder the folder button, O and the middle click open, as shown.
  readonly property string shownFolder: Model.displayFolder(Model.folderToOpen(root.all, root.status.folder, sync.home), sync.home)

  // Every cause the app names gets a sentence, as in the window; any other
  // error shows its log line, and the line is never blank.
  function causeFor(status) {
    switch (status.causeKind) {
      case "lostListings": return strings.t("cause.lostListings")
      case "outOfSync": return strings.t("cause.outOfSync")
      case "timeout": return strings.t("cause.timeout")
      case "systemd": return strings.t("cause.systemd").arg(status.causeDetail)
      case "filtersChanged": return strings.t("cause.filtersChanged")
      case "fixInterrupted": return strings.t("cause.fixInterrupted")
      case "maxDelete": return strings.t("cause.maxDelete")
      case "signIn": return strings.t("cause.signIn")
      case "firstSyncPending": return strings.t("cause.firstSyncPending")
      case "oldDriveSync": return strings.t("cause.oldDriveSync")
      case "otherAccount": return strings.t("cause.otherAccount")
      case "unchecked": return strings.t("cause.unchecked")
    }
    return status.causeDetail !== "" ? status.causeDetail : strings.t("cause.unknown")
  }

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  // The theme has no warning color; this is the window's (StatusPanel.qml),
  // so a warning reads the same on the bar and in the window.
  readonly property color warning: "#ffb74d"
  // Urgent states take the bar's urgent color, warnings the orange.
  readonly property color stateColor: Model.isUrgent(syncState) ? urgent
                                    : Model.isWarning(syncState) ? warning : foreground
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  function colorFor(state) {
    return Model.isUrgent(state) ? urgent : Model.isWarning(state) ? warning : foreground
  }

  // Clock times show 12 or 24 hours like the clock on the bar, the same rule
  // as the window's. A right click on the clock rewrites shell.json, so the
  // user's file is watched.
  readonly property string timeFormat: Model.clockTimeFormat(
    Model.barClockHours([userShellFile.text(), defaultShellFile.text()], bar ? bar.vertical : false),
    Qt.locale().timeFormat(Locale.ShortFormat))
  property FileView userShellFile: FileView {
    path: (Quickshell.env("HOME") || "") + "/.config/omarchy/shell.json"
    watchChanges: true
    printErrors: false
    blockLoading: true
    onFileChanged: reload()
  }
  property FileView defaultShellFile: FileView {
    path: (Quickshell.env("OMARCHY_PATH") || "/usr/share/omarchy") + "/config/omarchy/shell.json"
    printErrors: false
    blockLoading: true
  }

  // The cursor walks root.targets (Model.panelTargets).
  property int actionIndex: 0
  property bool cursorActive: false
  // "?" swaps the row of key names, which did not fit, for a list.
  property bool showKeys: false
  // A poll can drop accounts while the cursor sits past the new end.
  onTargetsChanged: if (root.actionIndex >= root.targets.length) root.actionIndex = 0

  readonly property Item panelFlick: panel.loadedContent ? panel.loadedContent.panelFlick : null

  readonly property string headline: headlineFor(status)

  function headlineFor(s) {
    if (!s.ok) return strings.t("status.unknown")
    switch (s.status) {
      case "ok": return strings.t("status.ok").arg(clock(s.lastSuccess))
      case "stale": return strings.t("status.stale").arg(ago(s.lastSuccess))
      case "failed": return strings.t("status.failed").arg(clock(s.failingSince || s.lastRunStart))
      case "paused":
        if (!s.pausedUntil) return strings.t("status.paused")
        // The mark holds until the next tick clears it.
        return s.pausedUntil <= s.now ? strings.t("status.pausedEnding") : strings.t("status.pausedUntil").arg(clock(s.pausedUntil))
      default: return strings.t("status." + s.status)
    }
  }

  // Epoch seconds as a clock time, with the date when it is not today.
  function clock(sec) {
    if (!sec) return "-"
    var when = new Date(sec * 1000)
    var locale = Qt.locale(Commons.I18n.formatLocale)
    var today = new Date((status.now || sec) * 1000)
    var sameDay = when.toDateString() === today.toDateString()
    return sameDay ? when.toLocaleTimeString(locale, root.timeFormat) : when.toLocaleString(locale, "d MMM " + root.timeFormat)
  }

  function ago(sec) {
    var parts = Model.ageParts((status.now || sec) - sec)
    if (parts.unit === "now") return strings.t("age.now")
    return strings.tn("age." + parts.unit, parts.value).arg(Commons.I18n.formatNumber(parts.value))
  }

  function duration(seconds) {
    if (seconds < 60) return strings.tn("duration.seconds", seconds).arg(Commons.I18n.formatNumber(seconds))
    var minutes = Math.round(seconds / 60)
    return strings.tn("duration.minutes", minutes).arg(Commons.I18n.formatNumber(minutes))
  }

  function lastSuccessText(s) {
    return s.lastSuccess ? strings.t("row.lastSuccessValue").arg(clock(s.lastSuccess)).arg(ago(s.lastSuccess)) : "-"
  }

  function lastRunText(s) {
    if (s.running && s.currentRunStart) return strings.t("row.runningSince").arg(clock(s.currentRunStart))
    if (!s.lastRunStart) return "-"
    var took = Model.lastRunSeconds(s)
    return took > 0 ? strings.t("row.lastRunValue").arg(clock(s.lastRunStart)).arg(duration(took)) : clock(s.lastRunStart)
  }

  // The app prints 0 when the timer has no next run (off, or a resync paused it).
  function nextRunText(s) {
    if (s.resync || !s.nextRun) return "-"
    return clock(s.nextRun)
  }

  // Whether the cursor sits on this target (account index, or -1 for the
  // general row and the one-account row).
  function hasCursorOn(account, action) {
    if (!root.cursorActive) return false
    var target = root.targets[root.actionIndex]
    return !!target && target.account === account && target.action === action
  }

  function runTarget(target) {
    if (!target) return
    if (target.account >= 0) runAccountAction(target.account, target.action)
    else runAction(target.action)
  }

  // The general row, and the one-account row of 5.1: every account at once.
  function runAction(name) {
    if (name === "sync") sync.syncAll()
    else if (name === "pause") sync.togglePauseAll()
    else if (name === "log") { sync.openWindow(""); close() }
    else if (name === "folder") { sync.openFolder(root.status.folder); close() }
  }

  function runAccountAction(index, name) {
    var account = root.accounts[index]
    if (!account) return
    if (name === "open") { sync.openWindow(account.id); close() }
    else if (name === "sync") sync.syncAccount(index)
    else if (name === "pause") sync.togglePauseAccount(index)
    else if (name === "folder") { sync.openFolder(account.folder); close() }
  }

  // S, P, W and O act on the account under the cursor, else on every account
  // (O: the folder of the account the icon shows; W: the window as it is).
  function runKey(name) {
    var index = Model.cursorAccount(root.targets, root.cursorActive, root.actionIndex)
    if (index >= 0) runAccountAction(index, name === "log" ? "open" : name)
    else runAction(name)
  }

  function scrollItemIntoView(item) {
    if (!panelFlick || !item) return
    Qt.callLater(function() {
      if (!item || !panelFlick) return
      var margin = Style.space(6)
      var point = item.mapToItem(panelFlick.contentItem, 0, 0)
      var top = point.y
      var bottom = top + item.height
      var viewTop = panelFlick.contentY
      var viewBottom = viewTop + panelFlick.height
      var maxY = Math.max(0, panelFlick.contentHeight - panelFlick.height)
      if (top < viewTop + margin) panelFlick.contentY = Math.max(0, top - margin)
      else if (bottom > viewBottom - margin) panelFlick.contentY = Math.min(maxY, bottom + margin - panelFlick.height)
    })
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: if (opened) {
    cursorActive = false
    showKeys = false
    if (panelFlick) panelFlick.contentY = 0
    sync.refresh()
    Qt.callLater(function() { if (panel.loadedContent) panel.loadedContent.forceActiveFocus() })
  }

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  DriveSyncService {
    id: sync
    settings: root.settings
  }

  ShellIpc {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { sync.refresh(); return "ok" }
    // Every account that can sync now.
    function syncNow(): string {
      var accepted = Model.anyCanSync(root.all) && !sync.starting
      sync.syncAll()
      return accepted ? "started" : "busy"
    }
    // With several accounts: the paused ones resume when any is paused,
    // otherwise every account pauses for an hour. With one, as before.
    function togglePause(): string {
      var answer = Model.anyPaused(root.all) ? "resumed" : Model.anyCanPause(root.all) ? "paused" : "unavailable"
      sync.togglePauseAll()
      return answer
    }
    // Answers scripts, so it stays English: the state name, not the headline.
    function status(): string { return root.syncState }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: Model.glyph(root.syncState)
    active: Model.isUrgent(root.syncState) || Model.isWarning(root.syncState)
    activeColor: root.stateColor
    dimmed: Model.isDimmed(root.syncState)
    // The label is a folder's name: it goes in through fillText, never arg().
    tooltipText: root.status.label !== "" ? Model.fillText(strings.t("tooltip.account"), [root.status.label, root.headline])
                                          : Model.fillText(strings.t("tooltip"), [root.headline])
    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) sync.syncAll()
      else if (buttonCode === Qt.MiddleButton) sync.openFolder(root.status.folder)
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(340))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(480))

    content: PanelKeyCatcher {
      property alias panelFlick: panelFlick
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onMoveRequested: function(dx, dy) {
        if (!root.cursorActive) { root.cursorActive = true; return }
        var delta = dx !== 0 ? dx : dy
        if (delta !== 0) root.actionIndex = Model.stepTarget(root.actionIndex, delta, root.targets.length)
      }
      onActivateRequested: if (root.cursorActive) root.runTarget(root.targets[root.actionIndex])
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        var key = String(t).toLowerCase()
        if (key === "s") root.runKey("sync")
        else if (key === "p") root.runKey("pause")
        else if (key === "w") root.runKey("log")
        else if (key === "o") root.runKey("folder")
        else if (key === "r") sync.refresh()
        else if (key === "?") root.showKeys = !root.showKeys
      }

      // Eight accounts do not fit in the panel's height: the list scrolls.
      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: column
          width: panelFlick.width
          spacing: Style.space(12)

          PanelHero {
            width: parent.width
            title: strings.t("title")
            meta: root.several ? Model.fillText(strings.t("hero.account"), [root.status.label, root.headline]) : root.headline
            foreground: root.foreground
            fontFamily: root.fontFamily
            iconOpacity: Model.isDimmed(root.syncState) ? 0.5 : 1.0
            iconComponent: Component {
              Text {
                textFormat: Text.PlainText
                text: Model.glyph(root.syncState)
                color: root.stateColor
                font.family: root.fontFamily
                font.pixelSize: Style.font.display
              }
            }
          }

          // From two accounts on, each account's block says its own error.
          Text {
            textFormat: Text.PlainText
            visible: text !== ""
            width: parent.width
            text: sync.actionError !== "" ? sync.actionError
                : !root.several && (root.syncState === "failing" || root.status.status === "fixFailed") ? root.causeText : ""
            color: root.urgent
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
            wrapMode: Text.WordWrap
          }

          // An old drive-sync next to DriveSync: only removing it by hand helps.
          Text {
            textFormat: Text.PlainText
            visible: Model.hasWarning(root.status, "oldDriveSync")
            width: parent.width
            text: strings.t("warning.oldDriveSync")
            color: root.warning
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
            wrapMode: Text.WordWrap
          }

          // One account (or none): 5.1's rows.
          Column {
            visible: !root.several
            width: parent.width
            spacing: Style.spacing.labelGap

            InfoPair {
              label: strings.t("row.lastSuccess")
              value: root.lastSuccessText(root.status)
            }
            InfoPair { label: strings.t("row.lastRun"); value: root.lastRunText(root.status) }
            InfoPair { label: strings.t("row.nextRun"); value: root.nextRunText(root.status) }
            InfoPair {
              visible: root.status.failingSince > 0
              label: strings.t("row.failingSince")
              value: root.clock(root.status.failingSince)
            }
            InfoPair {
              visible: root.status.conflicts > 0
              label: strings.t("row.conflicts")
              value: Commons.I18n.formatNumber(root.status.conflicts)
            }
          }

          // Two accounts or more: one block per account, in the app's order.
          Column {
            visible: root.several
            width: parent.width
            spacing: Style.space(12)

            Repeater {
              model: root.accounts

              delegate: Column {
                id: block
                required property var modelData
                required property int index
                readonly property var account: modelData
                readonly property string accountState: Model.barState(account)
                readonly property bool failing: accountState === "failing" || account.status === "fixFailed"

                width: parent ? parent.width : 0
                spacing: Style.spacing.labelGap

                // The header opens the window on this account.
                CursorSurface {
                  id: accountHeader
                  width: parent.width
                  implicitHeight: header.implicitHeight + Style.space(8)
                  hasCursor: root.hasCursorOn(block.index, "open")
                  foreground: root.foreground
                  onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(accountHeader)

                  RowLayout {
                    id: header
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Style.space(4)
                    anchors.rightMargin: Style.space(4)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Style.space(8)

                    Rectangle {
                      Layout.alignment: Qt.AlignVCenter
                      implicitWidth: Style.space(8)
                      implicitHeight: Style.space(8)
                      radius: width / 2
                      color: root.colorFor(block.accountState)
                      opacity: Model.isDimmed(block.accountState) ? 0.5 : 1.0
                    }
                    Text {
                      textFormat: Text.PlainText
                      Layout.maximumWidth: header.width / 2
                      text: block.account.label
                      color: root.foreground
                      font.family: root.fontFamily
                      font.pixelSize: Style.font.bodySmall
                      font.bold: true
                      elide: Text.ElideRight
                    }
                    Text {
                      textFormat: Text.PlainText
                      Layout.fillWidth: true
                      horizontalAlignment: Text.AlignRight
                      text: root.headlineFor(block.account)
                      color: root.colorFor(block.accountState)
                      font.family: root.fontFamily
                      font.pixelSize: Style.font.bodySmall
                      elide: Text.ElideRight
                    }
                  }

                  MouseArea {
                    id: headerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.runAccountAction(block.index, "open")
                  }

                  PanelToolTip {
                    visible: headerMouse.containsMouse
                    text: Model.fillText(strings.t("action.openAccount"), [block.account.label])
                    fontFamily: root.fontFamily
                  }
                }

                InfoPair { label: strings.t("row.lastSuccess"); value: root.lastSuccessText(block.account) }
                InfoPair { label: strings.t("row.nextRun"); value: root.nextRunText(block.account) }

                Text {
                  textFormat: Text.PlainText
                  visible: block.failing
                  width: parent.width
                  text: block.failing ? root.causeFor(block.account) : ""
                  color: root.urgent
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.bodySmall
                  wrapMode: Text.WordWrap
                }

                RowLayout {
                  width: parent.width
                  spacing: Style.space(8)

                  PanelActionButton {
                    id: accountSync
                    readonly property bool can: Model.canSyncNow(block.account) && !sync.starting
                    iconText: String.fromCodePoint(0xF0450)
                    tooltipText: can ? Model.fillText(strings.t("action.syncAccount"), [block.account.label])
                                     : strings.t("action.syncBusy")
                    enabled: can
                    hasCursor: root.hasCursorOn(block.index, "sync")
                    foreground: root.foreground
                    fontFamily: root.fontFamily
                    onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(accountSync)
                    onClicked: root.runAccountAction(block.index, "sync")
                  }
                  PanelActionButton {
                    id: accountPause
                    readonly property bool resumes: Model.canResume(block.account)
                    iconText: String.fromCodePoint(resumes ? 0xF040A : 0xF03E4)  // play, pause
                    tooltipText: resumes ? Model.fillText(strings.t("action.resumeAccount"), [block.account.label])
                               : Model.canPause(block.account) ? Model.fillText(strings.t("action.pauseAccount"), [block.account.label])
                               : strings.t("action.pauseUnavailable")
                    enabled: resumes || Model.canPause(block.account)
                    hasCursor: root.hasCursorOn(block.index, "pause")
                    foreground: root.foreground
                    fontFamily: root.fontFamily
                    onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(accountPause)
                    onClicked: root.runAccountAction(block.index, "pause")
                  }
                  PanelActionButton {
                    id: accountFolder
                    readonly property string folder: Model.folderToOpen(root.all, block.account.folder, sync.home)
                    iconText: String.fromCodePoint(0xF0770)
                    tooltipText: Model.fillText(strings.t("action.openFolder"), [Model.displayFolder(folder, sync.home)])
                    enabled: folder !== ""
                    hasCursor: root.hasCursorOn(block.index, "folder")
                    foreground: root.foreground
                    fontFamily: root.fontFamily
                    onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(accountFolder)
                    onClicked: root.runAccountAction(block.index, "folder")
                  }
                  Item { Layout.fillWidth: true }
                }
              }
            }
          }

          PanelSeparator { foreground: root.foreground }

          // One account (or none): 5.1's row of icons.
          RowLayout {
            visible: !root.several
            width: parent.width
            spacing: Style.space(8)

            PanelActionButton {
              iconText: String.fromCodePoint(0xF0450)
              tooltipText: root.canSync ? strings.t("action.sync") : strings.t("action.syncBusy")
              enabled: root.canSync
              hasCursor: root.hasCursorOn(-1, "sync")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.runAction("sync")
            }
            PanelActionButton {
              readonly property bool resumes: Model.canResume(root.status)
              iconText: String.fromCodePoint(resumes ? 0xF040A : 0xF03E4)  // play, pause
              tooltipText: resumes ? strings.t("action.resume")
                         : Model.canPause(root.status) ? strings.t("action.pause") : strings.t("action.pauseUnavailable")
              enabled: resumes || Model.canPause(root.status)
              hasCursor: root.hasCursorOn(-1, "pause")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.runAction("pause")
            }
            PanelActionButton {
              iconText: String.fromCodePoint(0xF021A)
              tooltipText: strings.t("action.open")
              hasCursor: root.hasCursorOn(-1, "log")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.runAction("log")
            }
            PanelActionButton {
              iconText: String.fromCodePoint(0xF0770)
              tooltipText: Model.fillText(strings.t("action.openFolder"), [root.shownFolder])
              enabled: root.shownFolder !== ""
              hasCursor: root.hasCursorOn(-1, "folder")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.runAction("folder")
            }

            Text {
              textFormat: Text.PlainText
              Layout.fillWidth: true
              horizontalAlignment: Text.AlignRight
              text: strings.t("keys.toggle")
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
              elide: Text.ElideRight
            }
          }

          // Two accounts or more: every account at once, and the window.
          RowLayout {
            visible: root.several
            width: parent.width
            spacing: Style.space(8)

            PanelActionButton {
              id: syncAllButton
              iconText: String.fromCodePoint(0xF0450)
              tooltipText: root.canSync ? strings.t("action.syncAll") : strings.t("action.syncBusy")
              enabled: root.canSync
              hasCursor: root.hasCursorOn(-1, "sync")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(syncAllButton)
              onClicked: root.runAction("sync")
            }
            PanelActionButton {
              id: pauseAllButton
              readonly property bool resumes: Model.anyPaused(root.all)
              iconText: String.fromCodePoint(resumes ? 0xF040A : 0xF03E4)  // play, pause
              tooltipText: resumes ? strings.t("action.resumeAll")
                         : Model.anyCanPause(root.all) ? strings.t("action.pauseAll") : strings.t("action.pauseUnavailable")
              enabled: resumes || Model.anyCanPause(root.all)
              hasCursor: root.hasCursorOn(-1, "pause")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(pauseAllButton)
              onClicked: root.runAction("pause")
            }
            PanelActionButton {
              id: openButton
              iconText: String.fromCodePoint(0xF021A)
              tooltipText: strings.t("action.open")
              hasCursor: root.hasCursorOn(-1, "log")
              foreground: root.foreground
              fontFamily: root.fontFamily
              onHasCursorChanged: if (hasCursor) root.scrollItemIntoView(openButton)
              onClicked: root.runAction("log")
            }

            Text {
              textFormat: Text.PlainText
              Layout.fillWidth: true
              horizontalAlignment: Text.AlignRight
              text: strings.t("keys.toggle")
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
              elide: Text.ElideRight
            }
          }

          Column {
            visible: root.showKeys
            width: parent.width
            spacing: Style.spacing.labelGap

            // Not L for the window: lowercase h, j, k and l move the cursor
            // (PanelKeyCatcher), so it would only arrive with Shift.
            InfoPair { label: "S"; value: root.several ? strings.t("keys.syncSeveral") : strings.t("keys.sync") }
            InfoPair { label: "P"; value: root.several ? strings.t("keys.pauseSeveral") : strings.t("keys.pause") }
            InfoPair { label: "W"; value: root.several ? strings.t("keys.historySeveral") : strings.t("keys.history") }
            InfoPair {
              label: "O"
              value: root.several ? strings.t("keys.folderSeveral") : Model.fillText(strings.t("keys.openFolder"), [root.shownFolder])
            }
            InfoPair { label: "R"; value: strings.t("keys.refresh") }
            InfoPair { label: strings.t("keys.arrowsLabel"); value: strings.t("keys.arrows") }
            InfoPair { label: "Esc"; value: strings.t("keys.close") }
          }
        }
      }
    }
  }

  component InfoPair: Item {
    property string label: ""
    property string value: ""

    width: parent ? parent.width : 0
    implicitHeight: Math.max(labelText.implicitHeight, valueText.implicitHeight)

    Text {
      id: labelText
      textFormat: Text.PlainText
      anchors.left: parent.left
      text: parent.label
      color: root.foreground
      opacity: 0.6
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
    }

    Text {
      id: valueText
      textFormat: Text.PlainText
      anchors.right: parent.right
      anchors.left: labelText.right
      anchors.leftMargin: Style.space(8)
      horizontalAlignment: Text.AlignRight
      text: parent.value
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }
  }
}
