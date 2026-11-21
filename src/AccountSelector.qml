import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The accounts, one chip each, from two accounts on (spec 12, "What the user
// sees"): a dot in the account's tone and its label, the shown one with an
// accent border. A click shows that account. While a dialog or the help is
// open the chips take no click, as [ and ] take no key, so an answer typed
// there can never land on another account.
Flow {
    id: selector

    // A dialog or the help overlay is open.
    property bool locked: false

    visible: accountList.several
    spacing: 8

    Repeater {
        model: accountList.accounts
        delegate: Rectangle {
            id: chip
            required property var modelData
            required property int index
            readonly property bool shown: index === accountList.selected
            // The same colors as the headline's border and glyph.
            readonly property color tone: modelData.tone === "error" ? "#ff6b6b"
                                        : modelData.tone === "warning" ? "#ffb74d"
                                        : theme.accent

            radius: Math.min(4, theme.radius)
            color: shown ? "#1c1c22" : "#16161a"
            border.width: 1
            border.color: shown ? theme.accent : "#2e2e36"
            implicitWidth: row.implicitWidth + 22
            implicitHeight: row.implicitHeight + 12

            RowLayout {
                id: row
                anchors.centerIn: parent
                spacing: 8

                Rectangle {
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: chip.tone
                }
                Label {
                    text: chip.modelData.label
                    textFormat: Text.PlainText
                    color: chip.shown ? "#f4f4f5" : "#c4c4cc"
                    font.pixelSize: 13
                    font.weight: chip.shown ? Font.DemiBold : Font.Normal
                }
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                enabled: !selector.locked
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: accountList.select(chip.index)
            }

            ToolTip.visible: mouse.containsMouse
            ToolTip.delay: 600
            ToolTip.text: qsTr("Show %1. [ and ] switch accounts").arg(chip.modelData.label)
        }
    }
}
