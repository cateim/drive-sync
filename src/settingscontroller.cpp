#include "settingscontroller.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>

SettingsController::SettingsController(const QString &program, QObject *parent)
    : QObject(parent), m_program(program) {}

SettingsController::~SettingsController() {
    if (!m_child)
        return;
    m_child->disconnect();
    // A change takes seconds, and one stopped between the fix's mark and its
    // unit would hold the runs: it finishes on its own. A read just stops.
    if (m_state == "saving") {
        // The editor's bytes reach its stdin before the window goes.
        m_child->waitForBytesWritten(2000);
        m_child->setParent(nullptr);
    }
    else
        m_child->kill();
}

void SettingsController::load() { enqueue({"settings", "--json"}); }

void SettingsController::setInterval(int minutes) {
    enqueue({"settings", "--interval", QString::number(minutes), "--json"});
}

void SettingsController::setMaxDelete(int percent) {
    enqueue({"settings", "--max-delete", QString::number(percent), "--json"});
}

void SettingsController::setFolder(const QString &folder) {
    QString path = folder.trimmed();
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    enqueue({"settings", "--folder", path, "--json"});
}

void SettingsController::setFilters(const QString &text) {
    // On stdin: the child needs nothing of the window's once it started,
    // even when the window closes before it read the text.
    enqueue({"settings", "--filters", "-", "--json"}, text.toUtf8());
}

void SettingsController::setSkipped(const QStringList &folders) {
    QStringList arguments{"settings"};
    for (const QString &folder : folders)
        arguments << "--skip" << folder;
    if (folders.isEmpty())
        arguments << "--skip-none";
    enqueue(arguments << "--json");
}

void SettingsController::enqueue(const QStringList &arguments, const QByteArray &input) {
    // A read already waiting covers another one.
    if (arguments == QStringList{"settings", "--json"}
        && std::any_of(m_queue.cbegin(), m_queue.cend(), [&](const Pending &p) { return p.arguments == arguments; }))
        return;
    m_queue << Pending{arguments, input};
    if (!m_child)
        runNext();
}

void SettingsController::runNext() {
    if (m_queue.isEmpty())
        return;
    const Pending next = m_queue.takeFirst();
    const QStringList arguments = next.arguments;
    const bool read = arguments == QStringList{"settings", "--json"};
    m_state = read ? "loading" : "saving";
    if (!read) {
        m_error.clear();
        m_saved = false;
    }
    emit changed();
    auto *process = new QProcess(this);
    m_child = process;
    connect(process, &QProcess::finished, this, [this, process, arguments] {
        if (m_child == process) {
            m_child = nullptr;
            finished(arguments, process->readAllStandardOutput());
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, arguments](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // A crash or a kill also ends in finished().
        if (m_child == process) {
            m_child = nullptr;
            finished(arguments, {});
        }
        process->deleteLater();
    });
    process->start(m_program, arguments);
    // Written before the event loop runs again; QProcess sends it as the
    // child reads, and the closed channel ends its stdin.
    process->write(next.input);
    process->closeWriteChannel();
}

void SettingsController::finished(const QStringList &arguments, const QByteArray &output) {
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed().split('\n').last());
    const QJsonObject answer = document.object();
    const bool ok = document.isObject() && answer.value("ok").toBool();
    const bool read = arguments == QStringList{"settings", "--json"};
    bool fixNow = false;
    if (!ok) {
        m_error = answer.value("error").toString();
        if (m_error.isEmpty())
            m_error = read ? tr("DriveSync could not read its settings.") : tr("DriveSync could not change the setting.");
        // What was queued after a refused change was meant to follow it.
        m_queue.clear();
        m_fixStarted = false;
    } else if (read) {
        m_loaded = true;
        m_folder = answer.value("folder").toString();
        m_remote = answer.value("remote").toString();
        m_intervalMinutes = answer.value("intervalMinutes").toInt();
        m_maxDelete = answer.value("maxDelete").toInt();
        m_filters = answer.value("filters").toString();
        m_skipped.clear();
        for (const QJsonValue &folder : answer.value("skipped").toArray())
            m_skipped << folder.toString();
        m_skipError = answer.value("skipError").toString();
    } else {
        m_fixStarted = m_fixStarted || answer.value("fixStarted").toBool();
        if (m_queue.isEmpty()) {
            // The last change of this save: read the settings back, and say
            // how the change took effect.
            fixNow = m_fixStarted;
            m_fixStarted = false;
            m_saved = !fixNow;
            m_queue << Pending{{"settings", "--json"}, {}};
        }
    }
    m_state = "idle";
    emit changed();
    if (fixNow)
        emit fixStarted();
    runNext();
}
