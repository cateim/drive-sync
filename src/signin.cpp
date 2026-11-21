#include "signin.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <functional>
#include <signal.h>
#include <unistd.h>

#include "controlcommands.h"
#include "fixstate.h"
#include "machineguard.h"
#include "syncconfig.h"

namespace {

// listremotes, config dump, config delete and the lsjson check.
constexpr int kQuickTimeoutMs = 60 * 1000;

// The rclone a SIGTERM or SIGINT stops: the window's Cancel, the window
// closing, or Ctrl+C in a terminal. Only rclone is stopped, so the command
// itself lives on to remove the half-made remote; the signal never reaches
// the browser, which xdg-open may have started in the same process group.
volatile sig_atomic_t g_rclone = 0;
volatile sig_atomic_t g_stopped = 0;

extern "C" void stopRclone(int) {
    g_stopped = 1;
    const pid_t pid = g_rclone;
    if (pid > 0)
        ::kill(pid, SIGTERM);  // async-signal-safe
}

// Installs the handler for one sign-in and puts the previous ones back, since
// the tests run the command inside their own process.
class StopOnSignal {
public:
    StopOnSignal() {
        g_rclone = 0;
        g_stopped = 0;
        struct sigaction action {};
        action.sa_handler = stopRclone;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, &m_term);
        sigaction(SIGINT, &action, &m_int);
    }
    ~StopOnSignal() {
        sigaction(SIGTERM, &m_term, nullptr);
        sigaction(SIGINT, &m_int, nullptr);
        g_rclone = 0;
    }
    StopOnSignal(const StopOnSignal &) = delete;
    StopOnSignal &operator=(const StopOnSignal &) = delete;

private:
    struct sigaction m_term {}, m_int {};
};

bool stopped() { return g_stopped != 0; }

using LineSink = std::function<void(const QString &)>;

struct Ran {
    bool ended = false;  // exited by itself, within the time given
    int code = -1;
    QByteArray output;
    QString error;  // the last line rclone wrote on stderr, or why it did not run
};

// rclone with stdin on /dev/null, so every question rclone config would ask
// takes its default and only the browser is left (tried with rclone 1.75.2).
// Its stdout is kept in memory only: config create and update can print the
// finished section, client and token included. Each stderr line goes to
// onLine as it comes; a stoppable run is the one a signal stops.
Ran runRclone(const QString &rclone, const QStringList &arguments, int timeoutMs, bool stoppable = false,
              const LineSink &onLine = {}) {
    Ran ran;
    if (rclone.isEmpty()) {
        ran.error = "rclone was not found on PATH";
        return ran;
    }
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(rclone, arguments);
    if (!process.waitForStarted(timeoutMs)) {
        ran.error = "rclone did not start";
        return ran;
    }
    if (stoppable) {
        g_rclone = static_cast<sig_atomic_t>(process.processId());
        if (stopped())  // the signal came before the pid was known
            ::kill(static_cast<pid_t>(process.processId()), SIGTERM);
    }
    QByteArray pending;
    QString last;
    const auto drain = [&] {
        pending += process.readAllStandardError();
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(pending.left(newline)).trimmed();
            pending.remove(0, newline + 1);
            if (line.isEmpty())
                continue;
            last = line;
            if (onLine)
                onLine(line);
        }
    };
    QElapsedTimer clock;
    clock.start();
    bool timedOut = false;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(200);
        drain();
        if (process.state() != QProcess::NotRunning && clock.elapsed() > timeoutMs) {
            timedOut = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
    }
    if (stoppable)
        g_rclone = 0;
    drain();
    pending = pending.trimmed();
    if (!pending.isEmpty())
        last = QString::fromUtf8(pending);
    if (timedOut) {
        ran.error = "timeout";
        return ran;
    }
    ran.ended = process.exitStatus() == QProcess::NormalExit;
    ran.code = process.exitCode();
    ran.output = process.readAllStandardOutput();
    ran.error = last.isEmpty() ? QString("rclone exited with %1").arg(ran.code) : last;
    return ran;
}

bool succeeded(const Ran &ran) { return ran.ended && ran.code == 0; }

// rclone's errors can quote the client it was given; they reach the window
// and the terminal only without it.
QString scrubbed(QString text, const std::optional<OAuthClient> &client) {
    if (client) {
        if (!client->id.isEmpty())
            text.replace(client->id, "<client id>");
        if (!client->secret.isEmpty())
            text.replace(client->secret, "<client secret>");
    }
    return text;
}

// "--config <file>" (or "--config=<file>") from the config's flags: the
// remote lives in that rclone.conf, not in the default one.
QStringList configFlags(const QStringList &flags) {
    for (qsizetype i = 0; i < flags.size(); ++i) {
        if (flags.at(i) == "--config" && i + 1 < flags.size())
            return {"--config", flags.at(i + 1)};
        if (flags.at(i).startsWith("--config="))
            return {"--config", flags.at(i).mid(9)};
    }
    return {};
}

// The section of one remote in "rclone config dump", read in memory only:
// it holds the token.
QJsonObject remoteSection(const QString &rclone, const QString &name, QString *error) {
    const Ran dump = runRclone(rclone, {"config", "dump"}, kQuickTimeoutMs);
    if (!succeeded(dump)) {
        *error = QString("rclone config dump failed: %1").arg(dump.error);
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(dump.output);
    if (!document.isObject()) {
        *error = "rclone config dump did not print a JSON object";
        return {};
    }
    return document.object().value(name).toObject();
}

// A remote DriveSync made: Google Drive with this build's client. Anything
// else of that name is the user's own and is never taken or removed.
bool madeByDriveSync(const QJsonObject &section, const std::optional<OAuthClient> &client) {
    return client && section.value("type").toString() == "drive"
           && section.value("client_id").toString() == client->id;
}

int answer(bool json, const QString &error, const QJsonObject &extra, QTextStream &out, QTextStream &err) {
    if (!json)
        return commandAnswer(false, error, out, err);
    QJsonObject object{{"ok", error.isEmpty()}, {"error", error}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        object.insert(it.key(), it.value());
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << "\n";
    out.flush();
    return error.isEmpty() ? 0 : 1;
}

// After a sign-in, one listing of the top folder proves that the token works
// and that the Drive API answers for this client.
QString checkToken(const SignInTools &tools, const QString &name, const QStringList &flags = {}) {
    const Ran list = runRclone(tools.rclone, QStringList{"lsjson", name + ":", "--max-depth", "1", "--dirs-only"} + flags,
                               kQuickTimeoutMs);
    if (succeeded(list))
        return QString();
    return QString("Google accepted the sign-in, but Drive did not answer: %1").arg(scrubbed(list.error, tools.client));
}

QString signInFailure(const Ran &ran, const SignInTools &tools) {
    if (stopped())
        return "the sign-in was stopped before it finished";
    if (ran.error == "timeout")
        return QString("the browser sign-in did not finish within %1 minutes").arg(tools.timeoutMs / 60000.0, 0, 'g', 3);
    return QString("the Google sign-in did not finish: %1").arg(scrubbed(ran.error, tools.client));
}

// The browser flow on an existing remote, with the client it was made with.
QString signInAgain(const SignInTools &tools, const QString &name, const QStringList &flags, const QString &page,
                    const LineSink &onLine) {
    const Ran ran = runRclone(tools.rclone, refreshTokenArguments(name, page) + flags, tools.timeoutMs, true, onLine);
    if (!succeeded(ran) || stopped())
        return signInFailure(ran, tools);
    return checkToken(tools, name, flags);
}

QString deleteRemote(const SignInTools &tools, const QString &name) {
    const Ran ran = runRclone(tools.rclone, {"config", "delete", name}, kQuickTimeoutMs);
    return succeeded(ran) ? QString() : QString("could not remove the half-made remote %1: %2").arg(name, ran.error);
}

// A new user: the kNewRemoteName remote, created through the browser. Any
// failure, a kill at the timeout or a stop by signal included, removes the
// section rclone wrote before it opened the browser, so the next sign-in
// starts clean. A remote of that name DriveSync made before is used when its
// token works, signed in again when it does not, and made again when it has
// no token at all (a sign-in cut short by SIGKILL).
QString create(const SignInTools &tools, const QString &page, QJsonObject *extra, const LineSink &onLine) {
    if (!tools.client)
        return "this build has no Google client, so it cannot sign in; DriveSync has to be built with "
               "src/oauthclient.local.h (see src/oauthclient.local.h.example)";
    const QString name = kNewRemoteName;
    const Ran remotes = runRclone(tools.rclone, {"listremotes"}, kQuickTimeoutMs);
    if (!succeeded(remotes))
        return QString("rclone listremotes failed: %1").arg(remotes.error);
    if (QString::fromUtf8(remotes.output).split('\n').contains(name + ":")) {
        QString error;
        const QJsonObject section = remoteSection(tools.rclone, name, &error);
        if (!error.isEmpty())
            return error;
        if (!madeByDriveSync(section, tools.client)) {
            extra->insert("reason", "remoteExists");
            return QString("rclone already has a remote named %1 that DriveSync did not make, and DriveSync never "
                           "takes or overwrites it; to sign in with DriveSync, remove it with: rclone config delete %1")
                .arg(name);
        }
        if (!section.value("token").toString().isEmpty())
            return checkToken(tools, name).isEmpty() ? QString() : signInAgain(tools, name, {}, page, onLine);
        if (const QString cleanup = deleteRemote(tools, name); !cleanup.isEmpty())
            return cleanup;
    }
    const Ran ran = runRclone(tools.rclone, createRemoteArguments(name, *tools.client, page), tools.timeoutMs, true, onLine);
    QString error = succeeded(ran) && !stopped() ? checkToken(tools, name) : signInFailure(ran, tools);
    if (error.isEmpty())
        return QString();
    if (const QString cleanup = deleteRemote(tools, name); !cleanup.isEmpty())
        error += "; " + cleanup;
    return error;
}

// What a sign-in cut short leaves: DriveSync's own section without a token.
// Only before DriveSync runs drive-sync; after that the remote is the
// config's.
QString abandon(const BackendPaths &paths, const SignInTools &tools) {
    if (!paths.config.isEmpty() && QFile::exists(paths.config))
        return QString();
    const QString name = kNewRemoteName;
    QString error;
    const QJsonObject section = remoteSection(tools.rclone, name, &error);
    if (!error.isEmpty())
        return error;
    if (!madeByDriveSync(section, tools.client) || !section.value("token").toString().isEmpty())
        return QString();
    return deleteRemote(tools, name);
}

QString tr(const char *text) { return QCoreApplication::translate("SignIn", text); }

// The page for one sign-in, in a private file (0600) that lives as long as
// the command: rclone reads it when the browser comes back. When it cannot
// be written, path() is empty and rclone shows its own page; a missing file
// would stop rclone before the browser opens.
class SignInPageFile {
public:
    explicit SignInPageFile(const QString &language)
        : m_file(QDir::tempPath() + "/drive-sync-signin-XXXXXX.html") {
        const QByteArray page = signInPage(language);
        if (page.isEmpty() || !m_file.open() || m_file.write(page) != page.size() || !m_file.flush())
            return;
        m_path = m_file.fileName();
    }
    QString path() const { return m_path; }

private:
    QTemporaryFile m_file;
    QString m_path;
};

// rclone's "go to the following link: http://127.0.0.1:53682/auth?state=..."
// line, for a browser xdg-open could not open.
QString browserLink(const QString &line) {
    static const QString marker = "go to the following link: ";
    const qsizetype at = line.indexOf(marker);
    return at < 0 ? QString() : line.mid(at + marker.size()).trimmed();
}

}  // namespace

QString remoteName(const QString &remote) { return remote.section(':', 0, 0); }

QStringList createRemoteArguments(const QString &name, const OAuthClient &client, const QString &page) {
    QStringList arguments{"config", "create", name, "drive", "client_id", client.id, "client_secret", client.secret, "scope", "drive"};
    if (!page.isEmpty())
        arguments << "config_template_file" << page;
    return arguments;
}

QStringList refreshTokenArguments(const QString &name, const QString &page) {
    QStringList arguments{"config", "update", name, "config_refresh_token", "true"};
    if (!page.isEmpty())
        arguments << "config_template_file" << page;
    return arguments;
}

QByteArray signInPage(const QString &language) {
    QFile templateFile(":/signin-page.html");
    QFile logoFile(":/signin-logo.svg");
    if (!templateFile.open(QIODevice::ReadOnly) || !logoFile.open(QIODevice::ReadOnly))
        return {};
    // Comments go, so the served page carries none of the notes for
    // maintainers; the logo's <desc> is one of those too.
    static const QRegularExpression comment("<!--.*?-->\\s*", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression description("<desc>.*?</desc>\\s*", QRegularExpression::DotMatchesEverythingOption);
    QString page = QString::fromUtf8(templateFile.readAll()).remove(comment);
    const QString logo = QString::fromUtf8(logoFile.readAll()).remove(comment).remove(description).trimmed();
    const auto text = [](const QString &value) {
        return value.toHtmlEscaped().replace('{', "&#123;").replace('}', "&#125;");
    };
    page.replace("@LANG@", text(language));
    page.replace("@OK_TITLE@", text(tr("You can go back to DriveSync")));
    page.replace("@OK_TEXT@", text(tr("DriveSync finishes the sign-in and shows how it went.")));
    page.replace("@CLOSE_TAB@", text(tr("You can close this tab.")));
    page.replace("@ERROR_TITLE@", text(tr("Google did not complete the sign-in")));
    page.replace("@ERROR_TEXT@", text(tr("Go back to DriveSync and try again.")));
    // Last, so no marker inside the logo could be taken for one of ours.
    page.replace("@LOGO@", logo);
    return page.toUtf8();
}

SignInTools SignInTools::defaults() {
    SignInTools tools;
    tools.rclone = QStandardPaths::findExecutable("rclone");
    tools.client = shippedOAuthClient();
    return tools;
}

int runSignInCommand(const QStringList &arguments, const BackendPaths &paths, const SignInTools &tools, QTextStream &out,
                     QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    const bool abandoning = rest.removeAll("--abandon") > 0;
    if (!rest.isEmpty()) {
        err << "usage: drive-sync signin [--abandon] [--json]\n";
        return 1;
    }
    if (abandoning)
        return answer(json, abandon(paths, tools), {}, out, err);
    // A step of a fix at work reads and writes the remote; a sign-in in the
    // middle would change the token under it. A fix that waits for the user,
    // or stopped (on this very token, say), lets it through.
    if (fixPresent(paths.appState) && fixWorking(readFix(paths.appState).step))
        return answer(json, "a fix is under way; sign in when it ends", {}, out, err);
    if (takeoverLockHeld(paths.globalState(), paths.proc))
        return answer(json, "a takeover is running; sign in when it ends", {}, out, err);
    // On a machine whose check says no (the emergency home), a sign-in would
    // set up or change the remote of a folder that is not the one to sync.
    const auto current = QFile::exists(paths.config) ? loadSyncConfig(paths.config, nullptr) : std::nullopt;
    if (const QString refusal = machineGuardRefusal(paths, current.value_or(SyncConfig())); !refusal.isEmpty())
        return answer(json, refusal, {}, out, err);
    // The link goes out as soon as rclone prints it, before the final answer:
    // the window shows it, so a browser that did not open is no dead wait.
    const LineSink onLine = [&](const QString &line) {
        const QString link = scrubbed(browserLink(line), tools.client);
        if (link.isEmpty())
            return;
        if (json) {
            out << QJsonDocument(QJsonObject{{"link", link}}).toJson(QJsonDocument::Compact) << "\n";
            out.flush();
        } else {
            err << "drive-sync: if the browser did not open, go to " << link << "\n";
            err.flush();
        }
    };
    const StopOnSignal stopOnSignal;
    const SignInPageFile page(tools.language);
    QJsonObject extra;
    QString error;
    if (!paths.config.isEmpty() && QFile::exists(paths.config)) {
        const auto config = loadSyncConfig(paths.config, &error);
        if (config)
            error = signInAgain(tools, remoteName(config->remote), configFlags(config->rcloneFlags), page.path(), onLine);
    } else {
        error = create(tools, page.path(), &extra, onLine);
    }
    return answer(json, error, extra, out, err);
}
