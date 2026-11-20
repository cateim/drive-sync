#include "machineguard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr int kGuardTimeoutMs = 15000;

// The file guards accounts mode, and the view with no account under a known
// config root: setup and a first sign-in have no config whose runOnlyWhen
// could guard them, and a machine whose accounts were all disconnected keeps
// its machine.json.
bool guardFromFile(const BackendPaths &paths) {
    return paths.accountsMode || (paths.config.isEmpty() && !paths.configRoot.isEmpty());
}

}  // namespace

QString machineGuardPath(const QString &configRoot) { return configRoot + "/machine.json"; }

std::optional<QString> readMachineGuard(const QString &file, QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    QFile source(file);
    if (!source.exists())
        return QString();
    if (!source.open(QIODevice::ReadOnly)) {
        *error = source.errorString();
        return std::nullopt;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error = "it is not a JSON object";
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    if (object.value("version").toInt() != 1) {
        *error = QString("its version is %1, and DriveSync reads version 1")
                     .arg(object.value("version").toVariant().toString());
        return std::nullopt;
    }
    const QJsonValue command = object.value("runOnlyWhen");
    if (!command.isUndefined() && !command.isString()) {
        *error = "its runOnlyWhen is not text";
        return std::nullopt;
    }
    return command.toString();
}

bool writeMachineGuard(const QString &file, const QString &command, QString *error) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile target(file);
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"runOnlyWhen", command}}).toJson();
    if (target.open(QIODevice::WriteOnly) && target.write(data) == data.size() && target.commit())
        return true;
    if (error)
        *error = QString("cannot write %1: %2").arg(file, target.errorString());
    return false;
}

QString machineGuardCommand(const BackendPaths &paths, const SyncConfig &config, QString *error) {
    if (!guardFromFile(paths))
        return config.runOnlyWhen;
    const QString file = machineGuardPath(paths.configRoot);
    QString why;
    const std::optional<QString> command = readMachineGuard(file, &why);
    if (command)
        return *command;
    if (error)
        *error = QString("%1 cannot be read: %2").arg(file, why);
    return QString();
}

int runMachineGuard(const QString &command) {
    if (command.isEmpty())
        return 0;
    QProcess check;
    check.start(QStandardPaths::findExecutable("sh"), {"-c", command});
    if (!check.waitForStarted(kGuardTimeoutMs) || !check.waitForFinished(kGuardTimeoutMs)) {
        check.kill();
        check.waitForFinished(1000);
        return 255;
    }
    if (check.exitStatus() != QProcess::NormalExit || check.exitCode() == 255)
        return 255;
    return check.exitCode() == 0 ? 0 : 1;
}

QString machineGuardRefusal(const BackendPaths &paths, const SyncConfig &config) {
    QString error;
    const QString command = machineGuardCommand(paths, config, &error);
    if (!error.isEmpty())
        return error;
    if (runMachineGuard(command) == 0)
        return QString();
    // Where it says no, as on the emergency home, the folder is not the one
    // to sync, and a resync or a new sync would push it over the cloud.
    return QString("the runOnlyWhen check says no (%1), so this is not the home drive-sync runs on").arg(command);
}
