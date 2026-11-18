import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sync now, pause and resume, and the Drive folder; Settings on the right.
// The keys live in the "?" overlay, and each button names its key on hover.
// Pause, resume and Settings need DriveSync to run drive-sync, so they show
// only after a takeover.
RowLayout {
    id: row

    signal pauseRequested()
    signal settingsRequested()

    spacing: 8

    component KeyButton: Button {
        id: button
        property string key
        signal activated()
        flat: true
        focusPolicy: Qt.NoFocus
        Material.foreground: theme.accent
        ToolTip.visible: hovered && key !== ""
        ToolTip.delay: 600
        ToolTip.text: key
        onClicked: button.activated()
    }

    KeyButton {
        text: qsTr("Sync now")
        key: "S"
        enabled: backend.canSyncNow
        opacity: enabled ? 1 : 0.4
        onActivated: backend.syncNow()
    }
    KeyButton {
        visible: backend.managed && !backend.paused
        text: qsTr("Pause…")
        key: "P"
        onActivated: row.pauseRequested()
    }
    KeyButton {
        visible: backend.paused
        text: qsTr("Resume")
        key: "U"
        onActivated: backend.resume()
    }
    KeyButton {
        text: qsTr("Open folder")
        key: "O"
        onActivated: backend.openDriveFolder()
    }
    Item { Layout.fillWidth: true }
    KeyButton {
        visible: backend.managed
        text: qsTr("Settings…")
        key: ","
        onActivated: row.settingsRequested()
    }
}
