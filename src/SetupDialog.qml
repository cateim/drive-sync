import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// First setup (spec section 6): sign in to Google, pick the folder and the
// cloud folders that come down (selective sync, spec 8), start.
// Start writes the filters and the config and begins the fix, whose dry run
// opens in the fix dialog: nothing is copied before the user confirms it.
// With "again", only the sign-in, for a token Google no longer accepts.
Popup {
    id: dialog

    property bool again: false
    readonly property string phase: setupController.state
    readonly property bool signedIn: phase === "signedIn" || phase === "settingUp"
    readonly property bool canAccept: phase !== "signingIn" && phase !== "settingUp"
                                      && (again || !signedIn || field.text.trim() !== "")

    // The field takes the keys: Esc, Enter and X reach the window's shortcuts,
    // every other window shortcut is off while a dialog is open.
    focus: true
    modal: true
    // A click beside the dialog must not drop a sign-in that waits for the
    // browser: only Esc, Close or X end it.
    closePolicy: phase === "signingIn" ? Popup.NoAutoClose : Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(620, Overlay.overlay ? Overlay.overlay.width - 48 : 620)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    function openSetup() {
        again = false
        open()
    }
    function openAgain() {
        again = true
        open()
    }

    onOpened: {
        field.text = Format.home(setupController.folder)
        // A new user starts from the default groups; step 4 edits them.
        filtersPopup.reload()
        if (signedIn && !again) {
            field.forceActiveFocus()
            folderTree.reset([], "drive-sync:")
        }
    }
    // A sign-in that still waits for the browser goes with the dialog.
    onClosed: setupController.cancel()

    Connections {
        target: setupController
        function onSignedIn() {
            if (dialog.visible && !dialog.again) {
                field.forceActiveFocus()
                // DriveSync's own remote, before any config names it.
                folderTree.reset([], "drive-sync:")
            }
        }
    }

    // Step 4 in words: how many groups, rules and paths the filters hold.
    readonly property string filtersSummary: {
        translator.language
        let groups = 0
        for (const p of filterEditor.presets)
            if (p.ticked)
                ++groups
        return qsTr("%1 ready-made groups, %2 rules by name, %3 paths on this computer left out.")
            .arg(groups).arg(filterEditor.nameRules.length).arg(localTree.excluded.length)
    }

    function accept() {
        if (!canAccept)
            return
        if (again || !signedIn)
            setupController.signIn()
        else
            setupController.start(field.text.trim(), folderTree.skipped, filtersPopup.text())
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12

        Label {
            text: dialog.again ? qsTr("Sign in to Google again") : qsTr("Set up DriveSync")
            textFormat: Text.PlainText
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }

        // 1. The sign-in.
        Label {
            visible: !dialog.again
            text: dialog.signedIn ? qsTr("1. Signed in to Google ✓") : qsTr("1. Sign in to Google")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: dialog.signedIn ? theme.accent : "#e4e4e8"
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.signedIn || dialog.again
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: {
                translator.language
                if (dialog.phase === "signingIn")
                    return qsTr("Waiting for the browser. Finish the sign-in there, or cancel it here.")
                if (dialog.again)
                    return qsTr("Google no longer accepts DriveSync's sign-in, so nothing syncs. Your browser opens Google's sign-in page; after it, DriveSync starts a run.")
                return qsTr("Your browser opens Google's sign-in page for DriveSync. Google gives the access to rclone, which keeps it in its own config; DriveSync never sees your password.")
            }
        }
        // rclone's own sign-in address, for a browser that did not open.
        Label {
            Layout.fillWidth: true
            visible: dialog.phase === "signingIn" && setupController.link !== ""
            text: qsTr("If the browser did not open, open this address in it:")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        TextEdit {
            Layout.fillWidth: true
            visible: dialog.phase === "signingIn" && setupController.link !== ""
            text: setupController.link
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 12
            color: theme.accent
        }
        // Why the sign-in failed, or why setup refused: from rclone and the
        // command, so never markup.
        Label {
            Layout.fillWidth: true
            visible: setupController.detail !== "" && (dialog.phase === "failed" || dialog.phase === "signedIn")
            text: setupController.detail
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#ffb74d"
        }

        // 2. The folder.
        Label {
            visible: !dialog.again
            text: qsTr("2. Choose the folder")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: dialog.signedIn ? "#e4e4e8" : "#6f6f7a"
        }
        TextField {
            id: field
            visible: !dialog.again
            Layout.fillWidth: true
            enabled: dialog.phase === "signedIn"
            font.family: "monospace"
            selectByMouse: true
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.again
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: dialog.signedIn ? "#c4c4cc" : "#6f6f7a"
            text: qsTr("Files already in it stay and go to the cloud. Where both sides have a file at the same path, the newer copy wins on both; the dry run lists every copy, and nothing changes before you confirm it.")
        }

        // 3. The cloud folders that come down.
        Label {
            visible: !dialog.again
            text: qsTr("3. Choose the cloud folders")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: dialog.signedIn ? "#e4e4e8" : "#6f6f7a"
        }
        FolderTreeView {
            visible: !dialog.again && dialog.signedIn
            Layout.fillWidth: true
            Layout.preferredHeight: 150
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.again
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: dialog.signedIn ? "#c4c4cc" : "#6f6f7a"
            text: folderTree.error !== "" && dialog.signedIn
                  ? folderTree.error
                  : qsTr("Untick what should stay only in the cloud; Tab reaches the list, Space ticks, → opens a folder. Settings change this later.")
        }

        // 4. What does not sync: the default rules, or rules brought from an
        // earlier sync, which a folder already full of files usually needs.
        Label {
            visible: !dialog.again
            text: qsTr("4. Choose what does not sync")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: dialog.signedIn ? "#e4e4e8" : "#6f6f7a"
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.again
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: dialog.signedIn ? "#c4c4cc" : "#6f6f7a"
            text: dialog.filtersSummary
        }
        RowLayout {
            visible: !dialog.again
            spacing: 4
            Button {
                text: qsTr("Choose what does not sync…")
                flat: true
                enabled: dialog.phase === "signedIn"
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Ctrl+E"
                onClicked: filtersPopup.open()
            }
        }
        // Keyboard first: Ctrl+E opens the editor and closes it again.
        Shortcut {
            sequence: "Ctrl+E"
            context: Qt.ApplicationShortcut
            enabled: dialog.visible && !dialog.again && dialog.phase === "signedIn"
            onActivated: filtersPopup.visible ? filtersPopup.close() : filtersPopup.open()
        }

        // 5. Start.
        Label {
            visible: !dialog.again
            text: qsTr("5. Start")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: dialog.signedIn ? "#e4e4e8" : "#6f6f7a"
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.again
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: dialog.signedIn ? "#c4c4cc" : "#6f6f7a"
            text: dialog.phase === "settingUp"
                  ? qsTr("Setting up drive-sync…")
                  : qsTr("DriveSync writes its filters and config, turns on the timers and makes a dry run of the first sync. The fix dialog then lists every copy, and nothing is copied before you confirm.")
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Button {
                text: qsTr("Close")
                flat: true
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Esc"
                onClicked: dialog.close()
            }
            Button {
                visible: dialog.phase === "signingIn"
                text: qsTr("Cancel the sign-in")
                flat: true
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "X"
                onClicked: setupController.cancel()
            }
            Button {
                text: dialog.again || !dialog.signedIn ? qsTr("Sign in…") : qsTr("Start")
                enabled: dialog.canAccept
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    implicitHeight: 34
                    radius: Math.min(4, theme.radius)
                    color: !parent.enabled ? "#2e2e36" : parent.down ? Qt.darker(theme.accent, 1.2) : theme.accent
                }
                contentItem: Label {
                    text: parent.text
                    textFormat: Text.PlainText
                    color: parent.enabled ? "#101010" : "#6f6f7a"
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Enter"
                onClicked: dialog.accept()
            }
        }
    }

    // Step 4's editor, the same one Settings' Filters page shows, over the
    // dialog: it is too tall to sit inside it.
    Popup {
        id: filtersPopup
        function reload() { editorView.load(filterEditor.defaultText(), field.text) }
        function text() { return editorView.text() }
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: Math.min(760, Overlay.overlay ? Overlay.overlay.width - 48 : 760)
        height: Overlay.overlay ? Overlay.overlay.height - 48 : 640
        modal: true
        closePolicy: Popup.NoAutoClose
        padding: 24
        Material.roundedScale: theme.radius
        background: Rectangle {
            color: "#1c1c21"
            radius: theme.radius
            border.color: "#2e2e36"
        }
        // The tree shows the folder as it stands in step 2 when it opens.
        onOpened: {
            localTree.reset(field.text, localTree.excluded)
            editorView.focusFirst()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Label {
                text: qsTr("What does not sync")
                textFormat: Text.PlainText
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: "#f4f4f5"
            }
            FiltersEditor {
                id: editorView
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    text: qsTr("Back to the defaults")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: filtersPopup.reload()
                }
                Button {
                    text: qsTr("Done")
                    focusPolicy: Qt.NoFocus
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: "Ctrl+E"
                    onClicked: filtersPopup.close()
                }
            }
        }
    }
}
