import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// After --max-delete stopped a run (spec 5b): D shows what the run would
// delete, and lets one run through once the user types how many files
// that is. A fix is never offered here, since it would bring them back.
// After the run let through, it says which deletions nobody confirmed.
Rectangle {
    id: banner

    signal reviewRequested()

    readonly property color accent: theme.accent

    readonly property int unconfirmed: backend.unconfirmedCloudCount + backend.unconfirmedNotebookCount

    visible: deletesHeld || unconfirmed > 0
    Material.roundedScale: theme.radius
    color: "#16161a"
    radius: theme.radius
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    implicitHeight: row.implicitHeight + 24

    // Main.qml passes the same rule the D key follows.
    property bool deletesHeld: false

    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
        spacing: 12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            // The run the user let through stopped again: why it had the
            // usual guard, which drive-sync run wrote to the log before it.
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: "#ffb74d"
                text: {
                    switch (backend.blockedKeptReason) {
                    case "dropped": return qsTr("The confirmation was dropped: the run it was for was no longer the last one, so this run had the usual guard.")
                    case "unconfirmed": return qsTr("The run you let through kept the usual guard: its dry run found deletions that were not in the list you confirmed.")
                    case "stillStopped": return qsTr("The run you let through kept the usual guard: even with the raised guard its dry run stopped, so more files would go.")
                    case "noRun": return qsTr("The run you let through kept the usual guard: its dry run did not reach bisync.")
                    case "failed": return qsTr("The run you let through kept the usual guard: its dry run failed.")
                    default: return ""
                    }
                }
            }
            // The run let through went past what the user saw: how many,
            // and where they are now.
            Label {
                Layout.fillWidth: true
                visible: banner.unconfirmed > 0
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: "#ffb74d"
                text: qsTr("The run you let through deleted %Ln files that were not in the list you confirmed.", "", banner.unconfirmed)
                      + (backend.unconfirmedCloudCount > 0
                         ? " " + qsTr("The ones deleted on the notebook are kept in %1.").arg(backend.unconfirmedBackupDir) : "")
                      + (backend.unconfirmedNotebookCount > 0
                         ? " " + qsTr("The ones deleted in the cloud are in the Drive trash.") : "")
            }
            Label {
                Layout.fillWidth: true
                visible: banner.deletesHeld
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: "#c4c4cc"
                text: qsTr("Review… lists what the run would delete, and lets one run through after you type how many files that is.")
            }
        }
        Button {
            visible: banner.deletesHeld
            text: qsTr("Review…")
            focusPolicy: Qt.NoFocus
            // Drawn by hand, like the bar banner's button.
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
            ToolTip.text: "D"
            onClicked: banner.reviewRequested()
        }
    }
}
