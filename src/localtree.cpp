#include "localtree.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

LocalTree::LocalTree(QObject *parent) : QObject(parent) {}

bool LocalTree::dirty() const {
    QStringList a = m_initial, b = m_excluded;
    a.sort();
    b.sort();
    return a != b;
}

void LocalTree::reset(const QString &root, const QStringList &excluded) {
    QString path = root.trimmed();
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    m_root = path;
    m_initial = m_excluded = excluded;
    m_error.clear();
    m_levels.clear();
    m_expanded.clear();
    // A folder setup has not made yet has nothing to list: no rows, no error.
    if (QFileInfo(m_root).isDir())
        load(QString());
    rebuild();
}

// One level, folders first and then files, each by name as a person sorts
// them. A link is never followed, so a linked folder does not open.
void LocalTree::load(const QString &path) {
    QDir dir(m_root + '/' + path);
    const QFileInfoList infos =
        dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QList<Entry> entries;
    for (const QFileInfo &info : infos) {
        Entry entry;
        entry.name = info.fileName();
        entry.link = info.isSymLink();
        entry.folder = info.isDir() && !entry.link;
        entry.path = path + entry.name + (entry.folder ? "/" : "");
        entries << entry;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        if (a.folder != b.folder)
            return a.folder;
        return collator.compare(a.name, b.name) < 0;
    });
    m_levels.insert(path, entries);
}

bool LocalTree::coveredByParent(const QString &path) const {
    for (const QString &other : m_excluded)
        if (other != path && other.endsWith('/') && path.startsWith(other))
            return true;
    return false;
}

void LocalTree::toggle(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    const QVariantMap r = m_rows.at(row).toMap();
    const QString path = r.value("path").toString();
    if (!r.value("tickable").toBool())
        return;
    if (m_excluded.contains(path)) {
        m_excluded.removeAll(path);
    } else {
        // What was left out inside it goes with it: the folder rule covers it.
        if (path.endsWith('/'))
            m_excluded.erase(std::remove_if(m_excluded.begin(), m_excluded.end(),
                                            [&](const QString &other) { return other.startsWith(path); }),
                             m_excluded.end());
        m_excluded << path;
    }
    rebuild();
}

bool LocalTree::exists(const QString &path) const {
    const QFileInfo info(m_root + '/' + path);
    return info.exists() || info.isSymLink();
}

void LocalTree::remove(const QString &path) {
    if (m_excluded.removeAll(path) > 0)
        rebuild();
}

void LocalTree::expand(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    const QVariantMap r = m_rows.at(row).toMap();
    if (!r.value("folder").toBool())
        return;
    const QString path = r.value("path").toString();
    if (!m_levels.contains(path))
        load(path);
    m_expanded.insert(path);
    rebuild();
}

void LocalTree::collapse(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    m_expanded.remove(m_rows.at(row).toMap().value("path").toString());
    rebuild();
}

void LocalTree::rebuild() {
    m_rows.clear();
    addRows(QString(), 0);
    emit changed();
}

void LocalTree::addRows(const QString &parent, int depth) {
    for (const Entry &entry : m_levels.value(parent)) {
        // A line break cannot be written in a filter rule: such a name is
        // shown, always synced, and never ticked off.
        const bool unsupported = entry.name.contains('\n') || entry.name.contains('\r');
        const bool covered = coveredByParent(entry.path);
        const bool expanded = m_expanded.contains(entry.path);
        m_rows << QVariantMap{{"path", entry.path},
                              {"name", entry.name},
                              {"depth", depth},
                              {"folder", entry.folder},
                              {"expanded", expanded},
                              {"loading", false},
                              {"loaded", m_levels.contains(entry.path)},
                              {"ticked", !covered && !m_excluded.contains(entry.path)},
                              {"tickable", !covered && !unsupported},
                              {"local", false},
                              {"unsupported", unsupported},
                              {"link", entry.link}};
        if (entry.folder && expanded)
            addRows(entry.path, depth + 1);
    }
}
