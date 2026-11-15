#include "foldertree.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include "selectivesync.h"

FolderTree::FolderTree(const QString &program, QObject *parent) : QObject(parent), m_program(program) {}

FolderTree::~FolderTree() { stopChildren(); }

void FolderTree::stopChildren() {
    // Listings only read: stopping one halfway loses nothing. SIGTERM first,
    // which "drive-sync folders" passes to its rclone; SIGKILL only when it
    // does not end (its rclone then gets SIGTERM as its parent dies).
    for (QProcess *process : std::as_const(m_children)) {
        process->disconnect();
        process->terminate();
        if (!process->waitForFinished(1000)) {
            process->kill();
            process->waitForFinished(500);
        }
        process->deleteLater();
    }
    m_children.clear();
}

bool FolderTree::dirty() const {
    QStringList a = m_initial, b = m_skipped;
    a.sort();
    b.sort();
    return a != b;
}

QStringList FolderTree::newlySkippedLocal() const {
    QStringList found;
    for (const QString &folder : m_skipped)
        if (!folderSkipped(m_initial, folder) && m_local.value(folder))
            found << folder;
    return found;
}

QStringList FolderTree::reticked() const {
    QStringList found;
    // Not listed yet counts as having a copy: the warning is the safe side.
    for (const QString &folder : m_initial)
        if (!folderSkipped(m_skipped, folder) && m_local.value(folder, true))
            found << folder;
    return found;
}

void FolderTree::reset(const QStringList &skipped, const QString &remote) {
    stopChildren();
    m_remote = remote;
    m_initial = skipped;
    m_skipped = skipped;
    m_error.clear();
    m_levels.clear();
    m_local.clear();
    m_expanded.clear();
    rebuild();
    load(QString());
}

void FolderTree::toggle(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    const QVariantMap item = m_rows.at(row).toMap();
    if (!item.value("tickable").toBool())
        return;
    m_skipped = toggledFolder(m_skipped, item.value("path").toString());
    rebuild();
}

void FolderTree::expand(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    const QString path = m_rows.at(row).toMap().value("path").toString();
    m_expanded.insert(path);
    if (!m_levels.contains(path))
        load(path);
    rebuild();
}

void FolderTree::collapse(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    m_expanded.remove(m_rows.at(row).toMap().value("path").toString());
    rebuild();
}

void FolderTree::load(const QString &path) {
    if (m_children.contains(path))
        return;
    QStringList arguments{"folders"};
    if (!m_remote.isEmpty())
        arguments << "--remote" << m_remote;
    arguments << "--json";
    // "--" first: a folder may be called "--json".
    if (!path.isEmpty())
        arguments << "--" << path;
    auto *process = new QProcess(this);
    m_children.insert(path, process);
    connect(process, &QProcess::finished, this, [this, process, path] {
        if (m_children.value(path) == process) {
            m_children.remove(path);
            listed(path, process->readAllStandardOutput(), true);
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, path](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        if (m_children.value(path) == process) {
            m_children.remove(path);
            listed(path, {}, false);
        }
        process->deleteLater();
    });
    process->start(m_program, arguments);
    emit changed();
}

void FolderTree::listed(const QString &path, const QByteArray &output, bool started) {
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed().split('\n').last());
    const QJsonObject answer = document.object();
    if (!started || !document.isObject() || !answer.value("ok").toBool()) {
        m_error = answer.value("error").toString();
        if (m_error.isEmpty())
            m_error = tr("DriveSync could not list the folders in the cloud.");
        m_expanded.remove(path);
        rebuild();
        return;
    }
    QList<Node> level;
    for (const QJsonValue &value : answer.value("folders").toArray()) {
        const QJsonObject folder = value.toObject();
        Node node{folder.value("name").toString(), folder.value("path").toString(), folder.value("local").toBool(),
                  folder.value("unsupported").toBool()};
        // Only a folder right below the one listed: one named after the
        // level itself would nest the tree in itself without end.
        if (node.name.isEmpty() || node.path != (path.isEmpty() ? node.name : path + "/" + node.name))
            continue;
        m_local.insert(node.path, node.local);
        level << node;
    }
    m_levels.insert(path, level);
    m_error.clear();
    rebuild();
}

void FolderTree::addRows(const QString &parent, int depth) {
    for (const Node &node : m_levels.value(parent)) {
        const bool expanded = m_expanded.contains(node.path);
        m_rows << QVariantMap{{"path", node.path},
                              {"name", node.name},
                              {"depth", depth},
                              {"expanded", expanded},
                              {"loading", m_children.contains(node.path)},
                              {"loaded", m_levels.contains(node.path)},
                              {"ticked", !folderSkipped(m_skipped, node.path)},
                              {"tickable", !node.unsupported && folderTickable(m_skipped, node.path)},
                              {"local", node.local},
                              {"unsupported", node.unsupported}};
        if (expanded)
            addRows(node.path, depth + 1);
    }
}

void FolderTree::rebuild() {
    m_rows.clear();
    addRows(QString(), 0);
    emit changed();
}
