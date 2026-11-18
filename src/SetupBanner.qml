import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Offers first setup when there is no drive-sync at all (spec section 6).
// Nothing changes until the user goes through the dialog that T opens.
Rectangle {
    id: banner

    signal setupRequested()

    readonly property color accent: theme.accent

    visible: takeoverSetup.state === "none" && !backend.managed
    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    implicitHeight: row.implicitHeight + 24

    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
        spacing: 12

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
            text: {
                translator.language
                return setupController.state === "settingUp"
                    ? qsTr("Setting up drive-sync…")
                    : qsTr("There is no drive-sync here yet. DriveSync can set up a two-way sync with Google Drive: sign in, pick a folder, and confirm a dry run before anything is copied.")
            }
        }
        Button {
            text: qsTr("Set up…")
            focusPolicy: Qt.NoFocus
            // Drawn by hand, like the other banners' buttons.
            background: Rectangle {
                implicitHeight: 34
                radius: Math.min(4, theme.radius)
                color: parent.down ? Qt.darker(banner.accent, 1.2) : banner.accent
            }
            contentItem: Label {
                text: parent.text
                textFormat: Text.PlainText
                color: "#101010"
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "T"
            onClicked: banner.setupRequested()
        }
    }
}
