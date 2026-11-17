import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A tree with a tick per row, read a level at a time by its model: the
// cloud's folders (folderTree, selective sync) or the synced folder on this
// computer (localTree, the filters screen). Keyboard first: with the focus here, ↑ ↓ J K move,
// Space ticks, → opens and ← closes a folder; the window's shortcuts on
// those keys stand down while it has the focus.
FocusScope {
    id: view

    property int current: 0
    // FolderTree or LocalTree: the same rows and calls.
    property var tree: folderTree
    property string loadingText: qsTr("Listing the folders in the cloud…")
    property string emptyText: qsTr("The cloud has no folders.")
    readonly property var rows: tree.rows

    activeFocusOnTab: true
    implicitHeight: 200

    function move(step) {
        if (rows.length === 0)
            return
        current = Math.max(0, Math.min(rows.length - 1, current + step))
        list.positionViewAtIndex(current, ListView.Contain)
    }
    function toggleCurrent() { tree.toggle(current) }
    function expandCurrent() {
        if (current < rows.length && !rows[current].expanded)
            tree.expand(current)
    }
    function collapseCurrent() {
        if (current >= rows.length)
            return
        if (rows[current].expanded) {
            tree.collapse(current)
            return
        }
        // On a closed folder, ← goes up to the folder that holds it.
        const depth = rows[current].depth
        for (let i = current - 1; i >= 0; --i)
            if (rows[i].depth < depth) {
                current = i
                list.positionViewAtIndex(current, ListView.Contain)
                return
            }
    }
    onRowsChanged: current = Math.max(0, Math.min(current, rows.length - 1))

    readonly property var ownKeys: [Qt.Key_Up, Qt.Key_Down, Qt.Key_J, Qt.Key_K, Qt.Key_Space, Qt.Key_Left, Qt.Key_Right]
    // Letters and digits too, used or not: Q must not quit, nor any other
    // window letter fire, while the tree has the focus.
    Keys.onShortcutOverride: (event) => {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
            return
        if (ownKeys.indexOf(event.key) >= 0 || (event.key >= Qt.Key_A && event.key <= Qt.Key_Z)
                || (event.key >= Qt.Key_0 && event.key <= Qt.Key_9))
            event.accepted = true
    }
    Keys.onPressed: (event) => {
        if (event.modifiers & Qt.ControlModifier)
            return
        if (event.key === Qt.Key_Up || event.key === Qt.Key_K) move(-1)
        else if (event.key === Qt.Key_Down || event.key === Qt.Key_J) move(1)
        else if (event.key === Qt.Key_Space) toggleCurrent()
        else if (event.key === Qt.Key_Right) expandCurrent()
        else if (event.key === Qt.Key_Left) collapseCurrent()
        else return
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "#16161a"
        radius: Math.min(4, theme.radius)
        border.color: view.activeFocus ? theme.accent : "#2e2e36"
    }

    Label {
        anchors.centerIn: parent
        visible: view.rows.length === 0
        text: view.tree.loading ? view.loadingText : view.tree.error !== "" ? "" : view.emptyText
        textFormat: Text.PlainText
        color: "#8a8a94"
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: 4
        clip: true
        model: view.rows
        currentIndex: view.current
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width
            height: 26
            radius: Math.min(4, theme.radius)
            color: index === view.current && view.activeFocus ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                 : index === view.current ? "#24242a" : "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6 + row.modelData.depth * 18
                anchors.rightMargin: 6
                spacing: 6

                Label {
                    // ▸ closed, ▾ open, … being listed.
                    text: row.modelData.loading ? "…" : row.modelData.expanded ? "▾" : "▸"
                    textFormat: Text.PlainText
                    color: "#8a8a94"
                    Layout.preferredWidth: 12
                    // A file has nothing to open; the cloud tree lists only folders.
                    opacity: row.modelData.folder === false ? 0 : 1
                    MouseArea {
                        anchors.fill: parent
                        enabled: row.modelData.folder !== false
                        onClicked: {
                            view.current = row.index
                            row.modelData.expanded ? view.tree.collapse(row.index) : view.tree.expand(row.index)
                        }
                    }
                }
                Rectangle {
                    // The tick: drawn, since the font's ☑ is an emoji and ☐ is not.
                    implicitWidth: 14
                    implicitHeight: 14
                    radius: Math.min(3, theme.radius)
                    color: row.modelData.ticked && row.modelData.tickable ? theme.accent : "transparent"
                    border.color: !row.modelData.tickable ? "#55555e" : row.modelData.ticked ? theme.accent : "#c4c4cc"
                    Label {
                        anchors.centerIn: parent
                        visible: row.modelData.ticked
                        text: "✓"
                        textFormat: Text.PlainText
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        color: row.modelData.tickable ? "#101010" : "#55555e"
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            view.current = row.index
                            view.tree.toggle(row.index)
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    // Folder names come from the cloud: never markup.
                    text: row.modelData.name
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: row.modelData.ticked ? "#e4e4e8" : "#8a8a94"
                }
                Label {
                    visible: row.modelData.unsupported
                    text: qsTr("name with a line break: always synced")
                    textFormat: Text.PlainText
                    font.pixelSize: 11
                    color: "#ffb74d"
                }
                Label {
                    // rclone skips links, so a cloud file of the same name
                    // would come down over this one unless it is left out.
                    visible: row.modelData.link === true
                    text: qsTr("link: rclone does not sync it")
                    textFormat: Text.PlainText
                    font.pixelSize: 11
                    color: "#ffb74d"
                }
                Label {
                    visible: row.modelData.local && !row.modelData.unsupported
                    text: qsTr("on this computer")
                    textFormat: Text.PlainText
                    font.pixelSize: 11
                    color: "#6f6f7a"
                }
            }
        }
        ScrollBar.vertical: ScrollBar {}
    }
}
