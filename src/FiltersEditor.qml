import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// What does not sync, built on screen (step 4c): ready-made groups, the
// synced folder on this computer with a tick per path, rules by name, and
// the rest as rclone text. Settings' Filters page and first setup share it;
// filterEditor and localTree hold the state, and text() is the whole file.
ScrollView {
    id: view

    // What a field takes for itself; the dialog that holds this passes its own.
    property var keepTyping: function (event) {
        if (event.key !== Qt.Key_Escape && !(event.modifiers & Qt.ControlModifier))
            event.accepted = true
    }
    property string importError: ""
    property string folder: ""
    property string ruleError: ""
    readonly property var kinds: ["name", "folder", "extension", "path"]

    clip: true
    contentWidth: availableWidth

    function load(text, syncedFolder) {
        folder = syncedFolder
        importError = ""
        ruleError = ""
        filterEditor.load(text)
    }
    function text() { return filterEditor.text(localTree.excluded) }
    // An untouched page starts no fix: this compares meaning, not bytes.
    function edited() { return filterEditor.edited(localTree.excluded) }
    // Where typing lands when the page opens: a new rule by name.
    function focusFirst() { ruleField.forceActiveFocus() }

    function presetTitle(id) {
        switch (id) {
        case "git": return qsTr("Git folders")
        case "nodeModules": return qsTr("node_modules")
        case "python": return qsTr("Python environments and caches")
        case "builds": return qsTr("Build output")
        case "googleDocs": return qsTr("Google Docs shortcuts")
        case "systemJunk": return qsTr("Windows and macOS junk")
        case "temporary": return qsTr("Locks and temporary files")
        }
        return id
    }
    function sentence(kind, value) {
        switch (kind) {
        case "name": return qsTr("Nothing named %1, in any folder").arg(value)
        case "folder": return qsTr("No folder named %1, in any folder, with what it holds").arg(value)
        case "extension": return qsTr("No file ending in .%1").arg(value.replace(/^\./, ""))
        case "path": return value.endsWith("/") ? qsTr("Not the folder %1, from the top").arg(value)
                                                : qsTr("Not the file %1, from the top").arg(value)
        }
        return value
    }
    function kindLabel(kind) {
        switch (kind) {
        case "name": return qsTr("Name")
        case "folder": return qsTr("Folder")
        case "extension": return qsTr("Extension")
        case "path": return qsTr("Path")
        }
        return kind
    }
    function addRule() {
        ruleError = filterEditor.addNameRule(kinds[kindBox.currentIndex], ruleField.text)
        if (ruleError === "")
            ruleField.text = ""
    }

    Connections {
        target: filterEditor
        // A load or an import: the paths of the new text tick the tree.
        function onLoaded() {
            advanced.text = filterEditor.advanced
            localTree.reset(view.folder, filterEditor.localPaths)
        }
    }

    ColumnLayout {
        width: view.availableWidth
        spacing: 8

        // 1. Ready-made groups.
        Label {
            text: qsTr("Ready-made groups")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: "#e4e4e8"
        }
        Repeater {
            model: filterEditor.presets
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 8
                CheckBox {
                    // Tight rows: seven groups must fit above the tree.
                    topPadding: 0
                    bottomPadding: 0
                    implicitHeight: 32
                    checked: modelData.ticked
                    focusPolicy: Qt.NoFocus
                    onToggled: filterEditor.setPreset(modelData.id, checked)
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        text: { translator.language; return view.presetTitle(modelData.id) }
                        textFormat: Text.PlainText
                        color: "#e4e4e8"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.rules
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: "monospace"
                        font.pixelSize: 11
                        color: "#6f6f7a"
                    }
                }
            }
        }

        // 2. The synced folder on this computer.
        Label {
            Layout.topMargin: 8
            text: qsTr("On this computer")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: "#e4e4e8"
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Untick what stays out of the sync. rclone never syncs a link: untick one when the cloud has a file with its name, or that file comes down over the link.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        FolderTreeView {
            id: localView
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            tree: localTree
            loadingText: ""
            emptyText: qsTr("The folder is empty, or not made yet.")
        }
        // Everything left out, flat: an imported rule may name a path deep in
        // a closed folder, or one this computer does not have.
        Label {
            visible: localTree.excluded.length > 0
            text: qsTr("Left out")
            textFormat: Text.PlainText
            color: "#c4c4cc"
        }
        Repeater {
            model: localTree.excluded
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    // Paths come from the disk or an old file: never markup.
                    text: modelData
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.family: "monospace"
                    font.pixelSize: 12
                    color: "#e4e4e8"
                }
                Label {
                    visible: !localTree.exists(modelData)
                    text: qsTr("not on this computer")
                    textFormat: Text.PlainText
                    font.pixelSize: 11
                    color: "#6f6f7a"
                }
                Button {
                    text: qsTr("Remove")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: localTree.remove(modelData)
                }
            }
        }

        // 3. Rules by name.
        Label {
            Layout.topMargin: 8
            text: qsTr("By name")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: "#e4e4e8"
        }
        Repeater {
            model: filterEditor.nameRules
            delegate: RowLayout {
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        // Names come from the person or an old file: never markup.
                        text: { translator.language; return view.sentence(modelData.kind, modelData.value) }
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: "#e4e4e8"
                    }
                    Label {
                        text: modelData.line
                        textFormat: Text.PlainText
                        font.family: "monospace"
                        font.pixelSize: 11
                        color: "#6f6f7a"
                    }
                }
                Button {
                    text: qsTr("Remove")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: filterEditor.removeNameRule(index)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ComboBox {
                id: kindBox
                model: view.kinds.map(k => { translator.language; return view.kindLabel(k) })
                focusPolicy: Qt.NoFocus
            }
            TextField {
                id: ruleField
                Layout.fillWidth: true
                font.family: "monospace"
                selectByMouse: true
                placeholderText: {
                    translator.language
                    switch (view.kinds[kindBox.currentIndex]) {
                    case "name": return qsTr("e.g. notes.tmp")
                    case "folder": return qsTr("e.g. build")
                    case "extension": return qsTr("e.g. iso")
                    case "path": return qsTr("e.g. Photos/2019/ (a folder ends with /)")
                    }
                    return ""
                }
                Keys.onShortcutOverride: (event) => view.keepTyping(event)
                Keys.onReturnPressed: view.addRule()
                Keys.onEnterPressed: view.addRule()
            }
            Button {
                text: qsTr("Add")
                flat: true
                enabled: ruleField.text.trim() !== ""
                focusPolicy: Qt.NoFocus
                onClicked: view.addRule()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: view.ruleError !== ""
            text: view.ruleError
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#ffb74d"
        }

        // 4. Advanced: every rule written by hand, kept as it is.
        Label {
            Layout.topMargin: 8
            text: qsTr("Advanced")
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            color: "#e4e4e8"
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("rclone filter rules, kept as written and read before everything above, so a \"+\" rule here wins. The block of cloud folders belongs to the Cloud folders page.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#c4c4cc"
        }
        TextArea {
            id: advanced
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            Keys.onShortcutOverride: (event) => view.keepTyping(event)
            onTextChanged: filterEditor.setAdvanced(text)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: importField
                Layout.fillWidth: true
                font.family: "monospace"
                selectByMouse: true
                placeholderText: qsTr("Filters file of an earlier sync")
                Keys.onShortcutOverride: (event) => view.keepTyping(event)
                Keys.onReturnPressed: view.importError = filterEditor.importFile(text)
                Keys.onEnterPressed: view.importError = filterEditor.importFile(text)
            }
            Button {
                text: qsTr("Import")
                flat: true
                enabled: importField.text.trim() !== ""
                focusPolicy: Qt.NoFocus
                onClicked: view.importError = filterEditor.importFile(importField.text)
            }
        }
        Label {
            Layout.fillWidth: true
            visible: view.importError !== ""
            text: view.importError
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#ffb74d"
        }
    }
}
