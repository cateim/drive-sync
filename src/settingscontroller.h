#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

// The window's side of Settings. Every read and change is "drive-sync
// settings ... --json" in a child process, like SetupController, so the
// window never writes the config, the filters or a unit itself, and the
// offscreen harness reads nothing of this machine. Changes queue and run in
// order; a failure drops the rest.
class SettingsController : public QObject {
    Q_OBJECT
    // "idle" | "loading" | "saving"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    // Why the last command failed, from the command: never markup.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // A change that applied at once (interval, guard): the window says so.
    Q_PROPERTY(bool saved READ saved NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    Q_PROPERTY(QString remote READ remote NOTIFY changed)
    Q_PROPERTY(int intervalMinutes READ intervalMinutes NOTIFY changed)
    Q_PROPERTY(int maxDelete READ maxDelete NOTIFY changed)
    Q_PROPERTY(QString filters READ filters NOTIFY changed)
    Q_PROPERTY(QStringList skipped READ skipped NOTIFY changed)
    // The selective-sync block cannot be read (edited by hand): why.
    Q_PROPERTY(QString skipError READ skipError NOTIFY changed)

public:
    explicit SettingsController(const QString &program, QObject *parent = nullptr);
    ~SettingsController() override;

    QString state() const { return m_state; }
    QString error() const { return m_error; }
    bool saved() const { return m_saved; }
    bool loaded() const { return m_loaded; }
    QString folder() const { return m_folder; }
    QString remote() const { return m_remote; }
    int intervalMinutes() const { return m_intervalMinutes; }
    int maxDelete() const { return m_maxDelete; }
    QString filters() const { return m_filters; }
    QStringList skipped() const { return m_skipped; }
    QString skipError() const { return m_skipError; }

    Q_INVOKABLE void load();
    Q_INVOKABLE void setInterval(int minutes);
    Q_INVOKABLE void setMaxDelete(int percent);
    // "~" is the home, as the window shows it.
    Q_INVOKABLE void setFolder(const QString &folder);
    // The editor's text goes to the command through a file of its own.
    Q_INVOKABLE void setFilters(const QString &text);
    Q_INVOKABLE void setSkipped(const QStringList &folders);

signals:
    void changed();
    // A change went through the fix: its dry run waits for the user.
    void fixStarted();

private:
    // A command and what it reads on stdin (the filters editor's bytes).
    struct Pending {
        QStringList arguments;
        QByteArray input;
    };
    void enqueue(const QStringList &arguments, const QByteArray &input = {});
    void runNext();
    void finished(const QStringList &arguments, const QByteArray &output);

    QString m_program;
    QString m_state = "idle";
    QString m_error;
    bool m_saved = false;
    bool m_loaded = false;
    QString m_folder, m_remote, m_filters, m_skipError;
    int m_intervalMinutes = 0;
    int m_maxDelete = 0;
    QStringList m_skipped;
    QList<Pending> m_queue;
    QProcess *m_child = nullptr;
    bool m_fixStarted = false;
};
