import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1000
    height: 700
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "DriveSync"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property int page: 0  // 0: runs, 1: conflict copies
    // Relative times ("8 min ago") are computed against this clock.
    property double now: Date.now()

    Timer {
        interval: 15000
        running: true
        repeat: true
        onTriggered: win.now = Date.now()
    }
    Connections {
        target: backend
        function onChanged() { win.now = Date.now() }
        // A fix that reaches a question opens its dialog, unless another
        // dialog is in the way; one already waiting at start is only shown.
        function onFixChanged() {
            const step = backend.fixStep
            if (step !== win.lastFixStep && !win.dialogOpen) {
                if (step === "confirm")
                    fixDialog.openGuarded()
                else if (step === "questions")
                    fixQuestions.openGuarded()
            }
            // A fix gone: a cancelled first setup also took its config away,
            // and the window offers setup again.
            if (step === "" && win.lastFixStep !== "") {
                backend.reloadConfig()
                takeoverSetup.check()
            }
            win.lastFixStep = step
        }
    }
    property string lastFixStep: ""
    Component.onCompleted: win.lastFixStep = backend.fixStep

    function openFix() {
        if (backend.fixStep === "questions")
            fixQuestions.open()
        else
            fixDialog.open()
    }
    Connections {
        target: takeoverSetup
        function onChanged() {
            backend.reloadConfig()
            if (!win.takeoverArmed || takeoverSetup.state === "checking")
                return
            win.takeoverArmed = takeoverSetup.state === "blocked" && win.takeoverWaitsForRun
            if (takeoverSetup.state === "offer" && !win.dialogOpen)
                takeoverDialog.open()
        }
    }

    // First setup: once it started, its fix takes over (the dry run opens
    // the fix dialog by itself), so the dialog goes first. A sign-in again
    // ends with a run, which shows whether Google takes the new token.
    Connections {
        target: setupController
        function onSetupStarted() {
            setupDialog.close()
            takeoverSetup.check()
            backend.reloadConfig()
        }
        function onSignedIn() {
            if (!setupDialog.again)
                return
            setupDialog.close()
            if (backend.canSyncNow)
                backend.syncNow()
        }
    }
    // No drive-sync at all: T offers first setup instead of a takeover.
    readonly property bool setupOffered: takeoverSetup.state === "none" && !backend.managed
    // Google no longer takes the token of the drive-sync DriveSync runs.
    readonly property bool signInNeeded: backend.managed && (backend.status === "failed" || backend.status === "fixFailed")
                                         && backend.causeKind === "signIn"

    // Runs take about half of each 15 min tick, so the offer is often away
    // when the user looks. T during a run arms it: the dialog opens by itself
    // once the run ends and the check finds nothing else in the way.
    property bool takeoverArmed: false
    readonly property bool takeoverWaitsForRun: takeoverSetup.state === "blocked"
        && takeoverSetup.blockers.length === 1 && takeoverSetup.blockers[0] === "running"

    function move(step) {
        if (win.helpVisible)
            help.scroll(step)
        else if (settingsDialog.visible)
            settingsDialog.move(step)
        else if (deleteDialog.visible)
            deleteDialog.scroll(step)
        else if (fixQuestions.visible)
            fixQuestions.move(step)
        else if (fixDialog.visible)
            fixDialog.scroll(step)
        else if (win.page === 0)
            runsPage.move(step)
        else
            conflictsPage.move(step)
    }

    // "?" arrives with Shift on most layouts (US, ABNT2), and not every
    // platform plugin matches it against the plain "?".
    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible && !win.dialogOpen; onActivated: win.helpVisible = false }
    // The popups never take focus (their buttons are NoFocus), so their own
    // closePolicy never sees Esc: the window closes them.
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: pauseMenu.visible; onActivated: pauseMenu.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: takeoverDialog.visible; onActivated: takeoverDialog.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: fixDialog.visible; onActivated: fixDialog.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: fixQuestions.visible; onActivated: fixQuestions.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: deleteDialog.visible; onActivated: deleteDialog.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: setupDialog.visible; onActivated: setupDialog.close() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: settingsDialog.visible; onActivated: settingsDialog.close() }
    // Settings: "," like most apps' settings key, with Ctrl too. Its fields
    // take every other key for themselves (SettingsDialog.keepTyping).
    Shortcut {
        sequences: [",", "Ctrl+,"]
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && backend.managed
        onActivated: settingsDialog.open()
    }
    Shortcut { sequence: "Ctrl+1"; context: Qt.ApplicationShortcut; enabled: settingsDialog.visible; onActivated: settingsDialog.showPage(0) }
    Shortcut { sequence: "Ctrl+2"; context: Qt.ApplicationShortcut; enabled: settingsDialog.visible; onActivated: settingsDialog.showPage(1) }
    Shortcut { sequence: "Ctrl+3"; context: Qt.ApplicationShortcut; enabled: settingsDialog.visible; onActivated: settingsDialog.showPage(2) }
    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; enabled: settingsDialog.visible; onActivated: settingsDialog.save() }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.refresh() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: backend.openDriveFolder() }
    Shortcut {
        sequence: "B"
        context: Qt.ApplicationShortcut
        enabled: pluginSetup.state === "offer"
        onActivated: pluginSetup.accept()
    }
    Shortcut {
        sequence: "N"
        context: Qt.ApplicationShortcut
        enabled: pluginSetup.state === "offer" || pluginSetup.state === "blocked"
        onActivated: pluginSetup.decline()
    }
    // While a dialog is open its own keys win: two enabled shortcuts on the
    // same key cancel each other out.
    readonly property bool dialogOpen: pauseMenu.visible || takeoverDialog.visible || fixDialog.visible
                                       || fixQuestions.visible || deleteDialog.visible || setupDialog.visible
                                       || settingsDialog.visible
    // The newest run stopped at the max-delete guard and DriveSync runs
    // drive-sync: D reviews what it would delete (spec 5b). Also after a
    // fix whose final run stopped there. Never while a run is under way.
    readonly property bool deletesHeld: backend.deletesHeld
    Shortcut { sequences: ["Tab", "Backtab"]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: win.page = 1 - win.page }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: win.page = 0 }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: win.page = 1 }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: pauseMenu.visible; onActivated: pauseMenu.choose(1) }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: pauseMenu.visible; onActivated: pauseMenu.choose(2) }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; enabled: pauseMenu.visible; onActivated: pauseMenu.choose(3) }
    Shortcut {
        sequence: "S"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && backend.canSyncNow
        onActivated: backend.syncNow()
    }
    Shortcut {
        sequence: "P"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && backend.managed && !backend.paused
        onActivated: pauseMenu.open()
    }
    Shortcut {
        sequence: "U"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && backend.paused
        onActivated: backend.resume()
    }
    Shortcut {
        sequence: "T"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && (takeoverSetup.state === "offer" || win.takeoverWaitsForRun || win.setupOffered)
        onActivated: {
            if (win.setupOffered)
                setupDialog.openSetup()
            else if (takeoverSetup.state === "offer")
                takeoverDialog.open()
            else if (takeoverSetup.pending)
                takeoverSetup.cancelPending()
            else
                win.takeoverArmed = !win.takeoverArmed
        }
    }
    // F offers the fix, or opens the step that waits for the user. While its
    // unit works there is nothing to decide.
    Shortcut {
        sequence: "F"
        context: Qt.ApplicationShortcut
        // After --max-delete stopped a run, a fix would bring the deleted
        // files back (spec 5b): no F until a run passes, even when refused
        // runs (edited filters) came after the stop.
        enabled: !win.dialogOpen && backend.managed && backend.status !== "fixing"
                 && !(backend.status === "failed" && backend.guardStopHolds)
        onActivated: win.openFix()
    }
    Shortcut {
        sequence: "I"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && win.signInNeeded
        onActivated: setupDialog.openAgain()
    }
    Shortcut {
        sequence: "X"
        context: Qt.ApplicationShortcut
        enabled: setupDialog.visible && setupController.state === "signingIn"
        onActivated: setupController.cancel()
    }
    Shortcut {
        sequence: "X"
        context: Qt.ApplicationShortcut
        enabled: (fixDialog.visible && fixDialog.hasFix) || fixQuestions.visible
        onActivated: fixQuestions.visible ? fixQuestions.cancelFix() : fixDialog.cancelFix()
    }
    Shortcut {
        sequence: "D"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen && win.deletesHeld
        onActivated: deleteDialog.open()
    }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: fixQuestions.visible; onActivated: fixQuestions.choose("cloud") }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: fixQuestions.visible; onActivated: fixQuestions.choose("notebook") }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; onActivated: win.move(1) }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; onActivated: win.move(-1) }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: win.page === 1 && !win.dialogOpen
        onActivated: conflictsPage.openCurrent()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: takeoverDialog.visible
        onActivated: takeoverDialog.confirm()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: fixDialog.visible
        onActivated: fixDialog.accept()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: fixQuestions.visible
        onActivated: fixQuestions.send()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: deleteDialog.visible
        onActivated: deleteDialog.accept()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        context: Qt.ApplicationShortcut
        enabled: setupDialog.visible
        onActivated: setupDialog.accept()
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            StatusPanel {
                Layout.fillWidth: true
                now: win.now
                onSignInRequested: setupDialog.openAgain()
            }

            ControlsRow {
                Layout.fillWidth: true
                Layout.topMargin: -6
                onPauseRequested: pauseMenu.open()
            }

            FixBanner {
                Layout.fillWidth: true
                onFixRequested: win.openFix()
            }

            DeleteBanner {
                Layout.fillWidth: true
                deletesHeld: win.deletesHeld
                onReviewRequested: deleteDialog.open()
            }

            TakeoverBanner {
                Layout.fillWidth: true
                armed: win.takeoverArmed
                waitsForRun: win.takeoverWaitsForRun
                onConfirmRequested: takeoverDialog.open()
            }

            SetupBanner {
                Layout.fillWidth: true
                onSetupRequested: setupDialog.openSetup()
            }

            BarBanner {
                Layout.fillWidth: true
            }

            TabBar {
                id: tabs
                Layout.fillWidth: true
                currentIndex: win.page
                onCurrentIndexChanged: win.page = currentIndex
                Material.background: "transparent"

                TabButton {
                    text: qsTr("Runs")
                    focusPolicy: Qt.NoFocus
                }
                TabButton {
                    text: backend.conflicts.length > 0 ? qsTr("Conflict copies (%1)").arg(backend.conflicts.length)
                                                       : qsTr("Conflict copies")
                    focusPolicy: Qt.NoFocus
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.page

                RunsPage {
                    id: runsPage
                    now: win.now
                }
                ConflictsPage {
                    id: conflictsPage
                    now: win.now
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: backend.message
                    color: theme.accent
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("Space refreshes   ? shortcuts")
                    color: "#6f6f7a"
                    font.pixelSize: 12
                }
            }
        }

        HelpOverlay {
            id: help
            anchors.fill: parent
            visible: win.helpVisible
            onCloseRequested: win.helpVisible = false
        }

        PauseMenu { id: pauseMenu }
        TakeoverDialog { id: takeoverDialog }
        FixDialog { id: fixDialog }
        FixQuestions { id: fixQuestions; now: win.now }
        DeleteDialog { id: deleteDialog }
        SetupDialog { id: setupDialog }
        SettingsDialog { id: settingsDialog }
    }
}
