#include "settingscontroller.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>

namespace {

// "settings" with "--account <id>" right after it when id is set, then rest.
QStringList settingsCommand(const QString &account, const QStringList &rest) {
    QStringList arguments{"settings"};
    if (!account.isEmpty())
        arguments << "--account" << account;
    return arguments + rest;
}

// The account a command names, empty when it names none.
QString accountOf(const QStringList &arguments) {
    return arguments.value(1) == "--account" ? arguments.value(2) : QString();
}

// The read is "settings --json", whichever account it names.
bool isRead(const QStringList &arguments) {
    QStringList rest = arguments;
    if (rest.value(1) == "--account")
        rest.remove(1, 2);
    return rest == QStringList{"settings", "--json"};
}

}  // namespace

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

void SettingsController::setAccount(const QString &account) {
    if (account == m_account)
        return;
    m_account = account;
    // What was read is the other account's: nothing shows, and no field is
    // saved from it, until this account is read.
    m_loaded = false;
    m_folder.clear();
    m_remote.clear();
    m_filters.clear();
    m_skipError.clear();
    m_skipped.clear();
    m_intervalMinutes = 0;
    m_maxDelete = 0;
    emit changed();
}

void SettingsController::load() { enqueue(settingsCommand(m_account, {"--json"})); }

void SettingsController::setInterval(int minutes) {
    enqueue(settingsCommand(m_account, {"--interval", QString::number(minutes), "--json"}));
}

void SettingsController::setMaxDelete(int percent) {
    enqueue(settingsCommand(m_account, {"--max-delete", QString::number(percent), "--json"}));
}

void SettingsController::setFolder(const QString &folder) {
    QString path = folder.trimmed();
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    enqueue(settingsCommand(m_account, {"--folder", path, "--json"}));
}

void SettingsController::setFilters(const QString &text) {
    // On stdin: the child needs nothing of the window's once it started,
    // even when the window closes before it read the text.
    enqueue(settingsCommand(m_account, {"--filters", "-", "--json"}), text.toUtf8());
}

void SettingsController::setSkipped(const QStringList &folders) {
    QStringList arguments = settingsCommand(m_account, {});
    for (const QString &folder : folders)
        arguments << "--skip" << folder;
    if (folders.isEmpty())
        arguments << "--skip-none";
    enqueue(arguments << "--json");
}

void SettingsController::enqueue(const QStringList &arguments, const QByteArray &input) {
    // A read already waiting covers another one of the same account.
    if (isRead(arguments)
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
    const bool read = isRead(arguments);
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
    const bool read = isRead(arguments);
    bool fixNow = false;
    if (!ok) {
        m_error = answer.value("error").toString();
        if (m_error.isEmpty())
            m_error = read ? tr("DriveSync could not read its settings.") : tr("DriveSync could not change the setting.");
        // What was queued after a refused change was meant to follow it.
        m_queue.clear();
        m_fixStarted.clear();
    } else if (read && accountOf(arguments) != m_account) {
        // Another account's read (the account changed while it ran): its
        // values are not the ones shown now.
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
        // A change keeps the account it was queued with, and so does the fix
        // it started: another account's commands queued meanwhile (a read
        // after "]") neither hold it nor take it.
        const QString account = accountOf(arguments);
        if (answer.value("fixStarted").toBool())
            m_fixStarted.insert(account);
        const bool moreChanges = std::any_of(m_queue.cbegin(), m_queue.cend(), [&](const Pending &p) {
            return !isRead(p.arguments) && accountOf(p.arguments) == account;
        });
        if (!moreChanges) {
            // The last change of this account's save: read its settings
            // back, and say how the change took effect. Only the account
            // shown hears how: the signal closes Settings, and "saved" makes
            // it take its fields again. For another account "saved" stays
            // false, as runNext() left it.
            const bool fixed = m_fixStarted.remove(account);
            if (account == m_account) {
                fixNow = fixed;
                m_saved = !fixed;
            }
            // The read names the account the change did, which the
            // account shown may no longer be.
            const QStringList readBack = settingsCommand(account, {"--json"});
            if (std::none_of(m_queue.cbegin(), m_queue.cend(), [&](const Pending &p) { return p.arguments == readBack; }))
                m_queue << Pending{readBack, {}};
        }
    }
    m_state = "idle";
    emit changed();
    if (fixNow)
        emit fixStarted();
    runNext();
}
