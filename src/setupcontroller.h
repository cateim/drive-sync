#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

// The window's side of first setup and of signing in again. It runs
// "drive-sync signin --json" and "drive-sync setup --folder <path> --json" as
// child processes, like TakeoverSetup, so the browser wait never blocks the
// window. Cancelling sends SIGTERM to "drive-sync signin" alone: it stops the
// rclone that waits for the browser on port 53682 and removes the half-made
// remote itself, and "drive-sync signin --abandon" runs after it as a
// fallback. The browser is never signalled.
class SetupController : public QObject {
    Q_OBJECT
    // "idle" | "signingIn" | "signedIn" | "settingUp" | "failed" (the sign-in
    // failed). A setup that fails goes back to "signedIn" with the reason in
    // detail, since the sign-in still holds.
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    // The folder the dialog offers first, then the one setup was started with.
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    // rclone's local sign-in address while the browser is awaited, for a
    // browser that xdg-open could not open; empty otherwise.
    Q_PROPERTY(QString link READ link NOTIFY changed)

public:
    SetupController(const QString &program, const QString &defaultFolder, QObject *parent = nullptr);
    ~SetupController() override;

    QString state() const { return m_state; }
    QString detail() const { return m_detail; }
    QString folder() const { return m_folder; }
    QString link() const { return m_link; }

    Q_INVOKABLE void signIn();
    // Only a sign-in waiting for the browser can be cancelled; a setup takes
    // seconds and stopping it halfway would leave a config with no fix.
    Q_INVOKABLE void cancel();
    // skipped: the cloud folders left out from the start (selective sync).
    // filters: the whole filters text built on screen (step 4), sent over
    // standard input; empty keeps setup's default rules. rclone checks it
    // before setup writes anything.
    Q_INVOKABLE void start(const QString &folder, const QStringList &skipped = {}, const QString &filters = {});

signals:
    void changed();
    // The browser sign-in ended well (or an earlier one had already).
    void signedIn();
    // Setup answered ok: the fix runs from here, and its dry run waits for
    // the user in the fix dialog.
    void setupStarted();

private:
    void run(const QStringList &arguments, const QByteArray &input = {});
    void finished(const QByteArray &output);
    void stopSignIn(bool thenAbandon);

    QString m_program;
    QString m_state = "idle";
    QString m_detail;
    QString m_folder;
    QString m_link;
    QProcess *m_child = nullptr;
    QByteArray m_output;  // what the running child printed so far
};
