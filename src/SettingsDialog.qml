import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "format.js" as Format

// Settings (spec section 8), keyboard first: Ctrl+1, Ctrl+2 and Ctrl+3 pick
// a page, Ctrl+S saves the page shown, Esc closes. The interval and the
// guard apply at once. The folder, the cloud folders and the filters change
// what bisync compares, so each page says before saving that the fix
// rebuilds the base, whose dry run waits for the user's confirmation; the
// dialog closes when that fix starts, and the fix dialog takes over. Save
// closes the dialog too, once the change applied (or at once when the page
// had nothing to save), unless another page still holds unsaved edits: a
// Save that seemed to do nothing was the complaint (2026-11-18).
Popup {
    id: dialog

    property int page: 0
    // The choices, plus the config's own value when the command line set
    // another one, so showing it never changes it.
    property var intervals: [5, 10, 15, 30, 60, 120, 240]
    // Only what the user changed is saved: a value the dialog cannot show as
    // it is (a guard above 50 set by hand) must not be rewritten.
    property bool intervalTouched: false
    property bool guardTouched: false
    // The tree is read from the cloud only once its page is shown, and its
    // ticks start from the settings as last read; until then it cannot save,
    // or it would rewrite the block from an empty or old list.
    property bool treeShown: false
    property bool treeFromRead: false
    // What the last read put in the editor and in the tree, so a later read
    // (after a General save) keeps edits made since, unless the file changed
    // underneath them.
    property string loadedFilters: ""
    property var treeBase: []
    readonly property bool busy: settingsController.state !== "idle"
    // The fields show the settings as read; set again after every read.
    property bool fieldsSet: false
    // Save closes the dialog once its change applied, unless another page
    // still has edits; note says why it stayed open.
    property bool closeAfterSave: false
    property bool savedWithOthers: false
    property string note: ""

    focus: true
    modal: true
    closePolicy: Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(760, Overlay.overlay ? Overlay.overlay.width - 48 : 760)
    height: Math.min(620, Overlay.overlay ? Overlay.overlay.height - 48 : 620)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#2e2e36"
    }

    onOpened: {
        fieldsSet = false
        closeAfterSave = false
        savedWithOthers = false
        note = ""
        loadedFilters = ""
        treeShown = false
        treeFromRead = false
        settingsController.load()
        showPage(0)
    }

    function showPage(index) {
        page = index
        note = ""
        savedWithOthers = false
        if (index === 1 && !treeShown) {
            treeShown = true
            if (fieldsSet)
                resetTree()
        }
        focusPage()
    }
    function resetTree() {
        treeBase = settingsController.skipped.slice()
        folderTree.reset(settingsController.skipped, "")
        treeFromRead = true
    }
    function sameList(a, b) {
        return a.length === b.length && a.every((item, i) => item === b[i])
    }
    function focusPage() {
        if (page === 0)
            folderField.forceActiveFocus()
        else if (page === 1)
            tree.forceActiveFocus()
        else
            filtersView.focusFirst()
    }
    function setFields() {
        folderField.text = Format.home(settingsController.folder)
        const choices = [5, 10, 15, 30, 60, 120, 240]
        if (choices.indexOf(settingsController.intervalMinutes) < 0 && settingsController.intervalMinutes > 0)
            choices.push(settingsController.intervalMinutes)
        intervals = choices.sort((a, b) => a - b)
        intervalBox.currentIndex = intervals.indexOf(settingsController.intervalMinutes)
        guardBox.value = settingsController.maxDelete
        intervalTouched = false
        guardTouched = false
        // The editor and the ticks keep unsaved edits when the filters on disk
        // are still the ones they started from; else they start over from the
        // file, since edits made on an older file would undo the newer one.
        const firstRead = !fieldsSet
        if (firstRead || !filtersView.edited() || settingsController.filters !== loadedFilters)
            filtersView.load(settingsController.filters, settingsController.folder)
        loadedFilters = settingsController.filters
        fieldsSet = true
        if (treeShown && (firstRead || !folderTree.dirty || !sameList(treeBase, settingsController.skipped)))
            resetTree()
    }
    Connections {
        target: settingsController
        function onChanged() {
            if (dialog.visible && settingsController.loaded && settingsController.state === "idle"
                    && (!dialog.fieldsSet || settingsController.saved))
                dialog.setFields()
            if (dialog.closeAfterSave && settingsController.state === "idle") {
                dialog.closeAfterSave = false
                if (settingsController.saved && settingsController.error === "")
                    dialog.close()
            }
        }
        function onFixStarted() { dialog.close() }
    }

    // The arrows the window hands over when no field has them.
    function move(step) {
        if (page === 1)
            tree.move(step)
    }

    // What each page would save: General's three fields, the ticks once read
    // from the settings, the filters by meaning.
    function intervalEdited() {
        return intervalTouched && intervals[intervalBox.currentIndex] !== settingsController.intervalMinutes
    }
    function guardEdited() {
        return guardTouched && guardBox.value !== settingsController.maxDelete
    }
    function folderEdited() {
        return folderField.text.trim() !== Format.home(settingsController.folder)
    }
    function pageEdited(index) {
        if (index === 0)
            return intervalEdited() || guardEdited() || folderEdited()
        if (index === 1)
            return treeFromRead && folderTree.dirty && settingsController.skipError === ""
        return filtersView.edited()
    }
    function otherPagesEdited() {
        return [0, 1, 2].some(index => index !== page && pageEdited(index))
    }

    function save() {
        if (busy || !settingsController.loaded)
            return
        note = ""
        savedWithOthers = false
        if (!pageEdited(page)) {
            if (otherPagesEdited())
                note = qsTr("Nothing to save on this page; another page still has changes.")
            else
                close()
            return
        }
        // A fix closes the dialog by itself (fixStarted); a change that
        // applies at once closes it when its command answers well.
        closeAfterSave = !otherPagesEdited()
        savedWithOthers = !closeAfterSave
        if (page === 0) {
            if (intervalEdited())
                settingsController.setInterval(intervals[intervalBox.currentIndex])
            if (guardEdited())
                settingsController.setMaxDelete(guardBox.value)
            // Last: it starts a fix, and the dialog closes with it.
            if (folderEdited())
                settingsController.setFolder(folderField.text.trim())
        } else if (page === 1) {
            settingsController.setSkipped(folderTree.skipped)
        } else {
            settingsController.setFilters(filtersView.text())
        }
    }

    // What a field takes for itself while it has the focus: everything but
    // Esc and the Ctrl shortcuts of this dialog, so typing a J, a Q, a
    // digit, Space or an arrow never reaches a window shortcut.
    function keepTyping(event) {
        if (event.key !== Qt.Key_Escape && !(event.modifiers & Qt.ControlModifier))
            event.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("Settings")
            textFormat: Text.PlainText
            font.pixelSize: 18
            font.weight: Font.DemiBold
            color: "#f4f4f5"
        }

        TabBar {
            Layout.fillWidth: true
            currentIndex: dialog.page
            Material.background: "transparent"
            TabButton { text: qsTr("General"); focusPolicy: Qt.NoFocus; onClicked: dialog.showPage(0) }
            TabButton { text: qsTr("Cloud folders"); focusPolicy: Qt.NoFocus; onClicked: dialog.showPage(1) }
            TabButton { text: qsTr("Filters"); focusPolicy: Qt.NoFocus; onClicked: dialog.showPage(2) }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: dialog.page

            // General: the folder, the interval and the guard.
            ColumnLayout {
                spacing: 8

                Label {
                    text: qsTr("Local folder")
                    textFormat: Text.PlainText
                    font.weight: Font.DemiBold
                    color: "#e4e4e8"
                }
                TextField {
                    id: folderField
                    Layout.fillWidth: true
                    font.family: "monospace"
                    selectByMouse: true
                    Keys.onShortcutOverride: (event) => dialog.keepTyping(event)
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Changing the folder builds a new base for it: the fix first makes a dry run, and nothing is copied before you confirm it. The current folder stays as it is, outside the sync.")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#c4c4cc"
                }

                Label {
                    Layout.topMargin: 8
                    text: qsTr("Sync interval")
                    textFormat: Text.PlainText
                    font.weight: Font.DemiBold
                    color: "#e4e4e8"
                }
                ComboBox {
                    id: intervalBox
                    Layout.preferredWidth: 200
                    model: dialog.intervals.map(m => m < 60 || m % 60 !== 0 ? qsTr("Every %1 min").arg(m)
                                                                            : qsTr("Every %1 h").arg(m / 60))
                    onActivated: dialog.intervalTouched = true
                    // Its own keys, and letters (it searches by them): Q must
                    // not quit while it has the focus.
                    Keys.onShortcutOverride: (event) => {
                        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
                            return
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down || event.key === Qt.Key_Space
                                || (event.key >= Qt.Key_A && event.key <= Qt.Key_Z))
                            event.accepted = true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Applies at once. The sync shows as late after three intervals with no success: %1 min.")
                              .arg(3 * dialog.intervals[Math.max(0, intervalBox.currentIndex)])
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#c4c4cc"
                }

                Label {
                    Layout.topMargin: 8
                    text: qsTr("Max-delete guard (percent)")
                    textFormat: Text.PlainText
                    font.weight: Font.DemiBold
                    color: "#e4e4e8"
                }
                SpinBox {
                    id: guardBox
                    from: 1
                    // A guard above 50 set by hand shows as it is; the
                    // command takes 50 at most.
                    to: Math.max(50, settingsController.maxDelete)
                    editable: true
                    onValueModified: dialog.guardTouched = true
                    Keys.onShortcutOverride: (event) => {
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down || (event.key >= Qt.Key_0 && event.key <= Qt.Key_9))
                            event.accepted = true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Applies at once. A run that would delete more than this share of the files on either side stops, deletes nothing and lists them for you to review.")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#c4c4cc"
                }
                Item { Layout.fillHeight: true }
            }

            // Cloud folders: selective sync.
            ColumnLayout {
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Ticked folders come down to this computer; unticked ones stay whole in the cloud. Saving changes the filters, so the fix rebuilds the base: its dry run comes first, and nothing changes before you confirm it.")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#c4c4cc"
                }
                FolderTreeView {
                    id: tree
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                Label {
                    Layout.fillWidth: true
                    visible: settingsController.skipError !== ""
                    // From the command, about the user's own file: never markup.
                    text: settingsController.skipError
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#ffb74d"
                }
                Label {
                    Layout.fillWidth: true
                    visible: folderTree.error !== ""
                    text: folderTree.error
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#ffb74d"
                }
                Label {
                    Layout.fillWidth: true
                    visible: folderTree.newlySkippedLocal.length > 0
                    text: qsTr("Unticked, these stay on this computer as they are, outside the sync; delete them yourself if you no longer need them: %1")
                              .arg(folderTree.newlySkippedLocal.join(", "))
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#ffb74d"
                }
                Label {
                    Layout.fillWidth: true
                    visible: folderTree.reticked.length > 0
                    text: qsTr("Ticked again, these send their copy on this computer back to the cloud, files deleted in the cloud meanwhile included; the dry run lists every copy: %1")
                              .arg(folderTree.reticked.join(", "))
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#ffb74d"
                }
            }

            // Filters: the whole file.
            ColumnLayout {
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("What does not sync. Saving checks the rules with rclone, then the fix rebuilds the base with them: its dry run comes first, and runs wait until it ends.")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#c4c4cc"
                }
                FiltersEditor {
                    id: filtersView
                    objectName: "filtersEditor"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    keepTyping: dialog.keepTyping
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: settingsController.error !== ""
            text: settingsController.error
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: "#ffb74d"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: {
                    translator.language
                    if (settingsController.state === "loading")
                        return qsTr("Reading the settings…")
                    if (settingsController.state === "saving")
                        return qsTr("Saving…")
                    if (dialog.note !== "")
                        return dialog.note
                    return settingsController.saved && dialog.savedWithOthers
                            ? qsTr("Saved; another page still has changes.") : ""
                }
                textFormat: Text.PlainText
                color: theme.accent
            }
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
                text: qsTr("Save")
                enabled: !dialog.busy && settingsController.loaded
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
                ToolTip.text: "Ctrl+S"
                onClicked: dialog.save()
            }
        }
    }
}
