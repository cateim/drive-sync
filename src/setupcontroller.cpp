#include "setupcontroller.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <signal.h>

SetupController::SetupController(const QString &program, const QString &defaultFolder, QObject *parent)
    : QObject(parent), m_program(program), m_folder(defaultFolder) {}

SetupController::~SetupController() {
    if (!m_child)
        return;
    if (m_state == "signingIn") {
        // The window closes while the browser waits: the command stops its
        // rclone and removes the half-made remote on its own.
        stopSignIn(false);
        return;
    }
    // A setup takes seconds, and stopping it between the config and the fix
    // would leave a config with no fix: it finishes on its own. Without a
    // parent the QProcess is never destroyed, so nothing kills it at quit.
    m_child->disconnect();
    m_child->setParent(nullptr);
}

void SetupController::signIn() {
    if (m_state == "signingIn" || m_state == "settingUp")
        return;
    m_state = "signingIn";
    m_detail.clear();
    emit changed();
    run({"signin", "--json"});
}

void SetupController::cancel() {
    if (m_state != "signingIn")
        return;
    stopSignIn(true);
    m_state = "idle";
    m_detail.clear();
    m_link.clear();
    emit changed();
}

void SetupController::start(const QString &folder, const QStringList &skipped) {
    if (m_state != "signedIn")
        return;
    // The dialog shows the home as "~", as the rest of the window does.
    QString path = folder.trimmed();
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    m_folder = path;
    m_state = "settingUp";
    m_detail.clear();
    emit changed();
    QStringList arguments{"setup", "--folder", path};
    for (const QString &cloudFolder : skipped)
        arguments << "--skip" << cloudFolder;
    run(arguments << "--json");
}

void SetupController::stopSignIn(bool thenAbandon) {
    if (!m_child)
        return;
    QProcess *child = m_child;
    m_child = nullptr;
    child->disconnect();
    // Only "drive-sync signin" gets the signal: it stops its rclone and runs
    // the cleanup itself. The process group is left alone, because the
    // browser xdg-open started can live in it (omarchy-launch-browser).
    if (const qint64 pid = child->processId(); pid > 0)
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    // No parent from here: deleting a QProcess kills its child, which would
    // cut that cleanup short. It goes once the command has ended; if the
    // window quits first, the command finishes on its own.
    child->setParent(nullptr);
    const QString program = m_program;
    QObject::connect(child, &QProcess::finished, child, [child, program, thenAbandon] {
        // A fallback for a command that could not clean up (killed hard).
        if (thenAbandon)
            QProcess::startDetached(program, {"signin", "--abandon"});
        child->deleteLater();
    });
}

void SetupController::run(const QStringList &arguments) {
    auto *process = new QProcess(this);
    m_child = process;
    // "signin --json" prints {"link": ...} as soon as rclone has its local
    // sign-in address, then the answer on the last line.
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (m_child != process)
            return;
        while (process->canReadLine()) {
            const QByteArray line = process->readLine().trimmed();
            m_output += line + "\n";
            const QJsonObject object = QJsonDocument::fromJson(line).object();
            if (object.contains("link") && m_state == "signingIn") {
                m_link = object.value("link").toString();
                emit changed();
            }
        }
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (m_child == process) {
            m_child = nullptr;
            m_output += process->readAllStandardOutput();
            finished(m_output);
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        if (m_child == process) {
            m_child = nullptr;
            finished({});
        }
        process->deleteLater();
    });
    m_output.clear();
    process->start(m_program, arguments);
}

void SetupController::finished(const QByteArray &output) {
    // The answer is the last line; a link line may come before it.
    const QList<QByteArray> lines = output.trimmed().split('\n');
    const QJsonDocument document = QJsonDocument::fromJson(lines.last());
    const QJsonObject answer = document.object();
    const bool ok = document.isObject() && answer.value("ok").toBool();
    m_link.clear();
    if (m_state == "signingIn") {
        // A remote of that name the user made is refused by the command, with
        // the reason in the error: it is never adopted here.
        if (ok) {
            m_state = "signedIn";
            m_detail.clear();
            emit changed();
            emit signedIn();
            return;
        }
        m_state = "failed";
        m_detail = answer.value("error").toString();
        if (m_detail.isEmpty())
            m_detail = tr("DriveSync could not sign in to Google.");
        emit changed();
        return;
    }
    if (m_state == "settingUp") {
        if (ok) {
            m_state = "idle";
            m_detail.clear();
            emit changed();
            emit setupStarted();
            return;
        }
        m_state = "signedIn";
        m_detail = answer.value("error").toString();
        if (m_detail.isEmpty())
            m_detail = tr("DriveSync could not start the setup.");
        emit changed();
    }
}
