#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QProcess;

// The tree of cloud folders for selective sync, read one level at a time
// with "drive-sync folders [PATH] --json" in child processes (nothing is
// copied), and the ticks the user makes on it. The rules of what can be
// ticked are selectivesync.h's. Settings and first setup share one.
class FolderTree : public QObject {
    Q_OBJECT
    // The visible rows, depth first: {path, name, depth, expanded, loading,
    // loaded, ticked, tickable, local, unsupported}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QStringList skipped READ skipped NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    // The ticks differ from the ones the tree started with.
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    // Left out now and not before, with a copy in the synced folder: that
    // copy stays on this computer, outside the sync (spec 8).
    Q_PROPERTY(QStringList newlySkippedLocal READ newlySkippedLocal NOTIFY changed)
    // Left out before and ticked now, with a copy in the synced folder (or
    // not listed yet): the base is a union, so that copy goes back up, files
    // deleted in the cloud meanwhile included.
    Q_PROPERTY(QStringList reticked READ reticked NOTIFY changed)

public:
    explicit FolderTree(const QString &program, QObject *parent = nullptr);
    ~FolderTree() override;

    QVariantList rows() const { return m_rows; }
    QStringList skipped() const { return m_skipped; }
    QString error() const { return m_error; }
    bool loading() const { return !m_children.isEmpty(); }
    bool dirty() const;
    QStringList newlySkippedLocal() const;
    QStringList reticked() const;

    // Starts over from the cloud's root with these folders left out. An
    // empty remote is the config's.
    Q_INVOKABLE void reset(const QStringList &skipped, const QString &remote);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void expand(int row);
    Q_INVOKABLE void collapse(int row);

signals:
    void changed();

private:
    struct Node {
        QString name, path;
        bool local = false;
        bool unsupported = false;
    };
    void load(const QString &path);
    void listed(const QString &path, const QByteArray &output, bool started);
    void rebuild();
    void addRows(const QString &parent, int depth);
    void stopChildren();

    QString m_program;
    QString m_remote;
    QStringList m_initial;
    QStringList m_skipped;
    QString m_error;
    QHash<QString, QList<Node>> m_levels;  // loaded levels, by the parent's path ("" is the root)
    QHash<QString, bool> m_local;          // every folder seen: does the synced folder hold it
    QSet<QString> m_expanded;
    QHash<QString, QProcess *> m_children;  // levels being listed
    QVariantList m_rows;
};
