#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

// The synced folder on this computer as a tree with ticks, for the filters
// screen: a path unticked becomes a "- /<path>" rule (a folder ends with
// "/"). It reads one level at a time with QDir when a folder opens, and
// never follows a link: rclone does not sync links (it skips them), so a link
// shows with a warning, since a cloud file of the same name would come down
// over it unless its path is left out. Rows match FolderTree's, so
// FolderTreeView shows either.
class LocalTree : public QObject {
    Q_OBJECT
    // Visible rows, depth first: {path, name, depth, folder, expanded,
    // loading, loaded, ticked, tickable, local, unsupported, link}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // The paths left out, from the top; a folder ends with "/".
    Q_PROPERTY(QStringList excluded READ excluded NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool loading READ loading CONSTANT)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)

public:
    explicit LocalTree(QObject *parent = nullptr);

    QVariantList rows() const { return m_rows; }
    QStringList excluded() const { return m_excluded; }
    QString error() const { return m_error; }
    bool loading() const { return false; }
    bool dirty() const;

    // Starts over at root ("~" is the home) with these paths left out.
    Q_INVOKABLE void reset(const QString &root, const QStringList &excluded);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void expand(int row);
    Q_INVOKABLE void collapse(int row);
    // For the flat list of what is left out: is that path on this computer
    // (an imported rule may name one that is not), and taking one back in.
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE void remove(const QString &path);

signals:
    void changed();

private:
    struct Entry {
        QString name, path;  // path from the top; a folder's ends with "/"
        bool folder = false;
        bool link = false;
    };
    void load(const QString &path);
    void rebuild();
    void addRows(const QString &parent, int depth);
    bool coveredByParent(const QString &path) const;

    QString m_root;
    QStringList m_initial;
    QStringList m_excluded;
    QString m_error;
    QHash<QString, QList<Entry>> m_levels;  // by the parent's path ("" is the top)
    QSet<QString> m_expanded;
    QVariantList m_rows;
};
