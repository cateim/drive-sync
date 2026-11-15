#include "selectivesync.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>
#include <signal.h>
#include <sys/prctl.h>

#include "controlcommands.h"
#include "fixcommand.h"
#include "signin.h"
#include "syncconfig.h"

namespace {

const QString kGlobCharacters = "\\*?[]{}";
// rclone's exit code for "directory not found".
constexpr int kRcloneDirNotFound = 3;
// Google answers one level in seconds; a hung call must not hold the tree.
constexpr int kListTimeoutMs = 60 * 1000;

// Where the block is: [begin, end) including the blank line after it, when
// it has one, and the line end it uses.
struct BlockSpan {
    qsizetype begin = -1;
    qsizetype end = -1;
    bool blankAfter = true;
    QByteArray eol = "\n";
};

// A line without its "\r": a file saved with Windows line ends is the same
// file, and its block must be found, not doubled.
QByteArray withoutCr(const QByteArray &line) { return line.endsWith('\r') ? line.chopped(1) : line; }

// The block's lines and where it sits; nullopt with the reason when it has
// no end. No block: an empty span.
std::optional<BlockSpan> findBlock(const QByteArray &filters, QList<QByteArray> *rules, QString *error) {
    BlockSpan span;
    span.eol = filters.contains("\r\n") ? "\r\n" : "\n";
    qsizetype from = 0;
    bool inBlock = false;
    while (from < filters.size()) {
        qsizetype end = filters.indexOf('\n', from);
        if (end < 0)
            end = filters.size();
        const QByteArray line = withoutCr(filters.mid(from, end - from));
        const qsizetype next = end + 1;
        if (!inBlock && line == kSkipBlockBegin) {
            inBlock = true;
            span.begin = from;
            span.eol = filters.mid(from, end - from).endsWith('\r') ? "\r\n" : "\n";
        } else if (inBlock && line == kSkipBlockEnd) {
            span.end = qMin(next, filters.size());
            // The blank line after the block goes with it, and a block written
            // without one is rewritten without one.
            span.blankAfter = filters.mid(span.end).startsWith(span.eol);
            if (span.blankAfter)
                span.end += span.eol.size();
            return span;
        } else if (inBlock) {
            rules->append(line);
        }
        from = next;
    }
    if (inBlock) {
        *error = QString("the filters have \"%1\" with no \"%2\" after it; fix them in the filters editor")
                     .arg(kSkipBlockBegin, kSkipBlockEnd);
        return std::nullopt;
    }
    return span;
}

QString unescapeFilterPath(const QString &escaped) {
    QString plain;
    for (qsizetype i = 0; i < escaped.size(); ++i) {
        if (escaped.at(i) == '\\' && i + 1 < escaped.size())
            ++i;
        plain += escaped.at(i);
    }
    return plain;
}

QString folderProblem(const QString &folder) {
    if (folder.isEmpty())
        return "a folder to leave out has no name";
    if (folder.contains('\n') || folder.contains('\r'))
        return QString("\"%1\" has a line break in its name, which no filter rule can hold").arg(folder.simplified());
    if (folder.startsWith('/') || folder.endsWith('/') || folder.contains("//"))
        return QString("\"%1\" is not a folder path such as Work/Old").arg(folder);
    return QString();
}

bool insideFolder(const QString &path, const QString &folder) { return path.startsWith(folder + "/"); }

// The rclone a SIGTERM or SIGINT stops: the window's tree stops a listing it
// no longer needs, and the listing goes with it instead of running on.
volatile sig_atomic_t g_listing = 0;
volatile sig_atomic_t g_listingStopped = 0;

extern "C" void stopListing(int) {
    g_listingStopped = 1;
    const pid_t pid = g_listing;
    if (pid > 0)
        ::kill(pid, SIGTERM);  // async-signal-safe
}

// Installed for one listing, the previous handlers put back after it, since
// the tests run the command inside their own process.
class StopListingOnSignal {
public:
    StopListingOnSignal() {
        g_listing = 0;
        g_listingStopped = 0;
        struct sigaction action {};
        action.sa_handler = stopListing;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, &m_term);
        sigaction(SIGINT, &action, &m_int);
    }
    ~StopListingOnSignal() {
        sigaction(SIGTERM, &m_term, nullptr);
        sigaction(SIGINT, &m_int, nullptr);
        g_listing = 0;
    }
    StopListingOnSignal(const StopListingOnSignal &) = delete;
    StopListingOnSignal &operator=(const StopListingOnSignal &) = delete;

private:
    struct sigaction m_term {}, m_int {};
};

}  // namespace

QString escapeFilterPath(const QString &path) {
    QString escaped;
    for (const QChar c : path) {
        if (kGlobCharacters.contains(c))
            escaped += '\\';
        escaped += c;
    }
    return escaped;
}

std::optional<QStringList> skippedFolders(const QByteArray &filters, QString *error) {
    QList<QByteArray> rules;
    if (!findBlock(filters, &rules, error))
        return std::nullopt;
    QStringList folders;
    for (const QByteArray &rule : rules) {
        const QString line = QString::fromUtf8(rule);
        const QString middle = line.mid(3, line.size() - 6);
        // Only "- /<escaped folder>/**", as withSkippedFolders writes it.
        if (!line.startsWith("- /") || !line.endsWith("/**") || line.size() <= 6
            || escapeFilterPath(unescapeFilterPath(middle)) != middle) {
            *error = QString("the folders left out were edited by hand (\"%1\"); fix them in the filters editor")
                         .arg(line.left(120));
            return std::nullopt;
        }
        folders << unescapeFilterPath(middle);
    }
    return folders;
}

std::optional<QByteArray> withSkippedFolders(const QByteArray &filters, const QStringList &folders, QString *error) {
    QList<QByteArray> rules;
    const std::optional<BlockSpan> span = findBlock(filters, &rules, error);
    if (!span)
        return std::nullopt;
    if (!skippedFolders(filters, error))
        return std::nullopt;
    QStringList kept;
    for (const QString &folder : folders) {
        if (const QString problem = folderProblem(folder); !problem.isEmpty()) {
            *error = problem;
            return std::nullopt;
        }
        if (!kept.contains(folder))
            kept << folder;
    }
    std::sort(kept.begin(), kept.end());
    QStringList list;
    for (const QString &folder : std::as_const(kept))
        if (!std::any_of(kept.cbegin(), kept.cend(), [&](const QString &other) { return insideFolder(folder, other); }))
            list << folder;

    // In the file's own line ends; the blank line after the block as the
    // block had it, or one for a new block.
    QByteArray block;
    if (!list.isEmpty()) {
        block = QByteArray(kSkipBlockBegin) + span->eol;
        for (const QString &folder : std::as_const(list))
            block += "- /" + escapeFilterPath(folder).toUtf8() + "/**" + span->eol;
        block += QByteArray(kSkipBlockEnd) + span->eol;
        if (span->begin < 0 || span->blankAfter)
            block += span->eol;
    }
    QByteArray result = filters;
    if (span->begin >= 0)
        result.replace(span->begin, span->end - span->begin, block);
    else
        result.prepend(block);
    return result;
}

bool folderSkipped(const QStringList &skipped, const QString &path) {
    return std::any_of(skipped.cbegin(), skipped.cend(),
                       [&](const QString &folder) { return path == folder || insideFolder(path, folder); });
}

bool folderTickable(const QStringList &skipped, const QString &path) {
    return std::none_of(skipped.cbegin(), skipped.cend(), [&](const QString &folder) { return insideFolder(path, folder); });
}

QStringList toggledFolder(const QStringList &skipped, const QString &path) {
    if (!folderTickable(skipped, path))
        return skipped;
    if (skipped.contains(path)) {
        QStringList result = skipped;
        result.removeAll(path);
        return result;
    }
    QStringList result;
    for (const QString &folder : skipped)
        if (!insideFolder(folder, path))
            result << folder;
    result << path;
    std::sort(result.begin(), result.end());
    return result;
}

QStringList folderListArguments(const QString &remote, const QString &path, const QStringList &configFlags) {
    return QStringList{"lsjson", path.isEmpty() ? remote : remoteFile(remote, path), "--dirs-only"}
           + remoteFlags(configFlags);
}

int runFoldersCommand(const QStringList &arguments, const BackendPaths &paths, const QString &rclone, QTextStream &out,
                      QTextStream &err) {
    // Options first; "--" ends them, so a folder named "--json" can be listed.
    bool json = false;
    bool valid = true;
    QString remote;
    QStringList positional;
    bool optionsEnded = false;
    for (qsizetype i = 0; i < arguments.size() && valid; ++i) {
        const QString &argument = arguments.at(i);
        if (optionsEnded || !argument.startsWith("--"))
            positional << argument;
        else if (argument == "--")
            optionsEnded = true;
        else if (argument == "--json")
            json = true;
        else if (argument == "--remote" && i + 1 < arguments.size())
            remote = arguments.at(++i);
        else
            valid = false;
    }
    if (!valid || positional.size() > 1)
        return commandAnswer(json, "usage: drive-sync folders [--remote REMOTE] [--json] [[--] PATH]", out, err);
    QString path = positional.value(0);
    while (path.startsWith('/'))
        path.remove(0, 1);
    while (path.endsWith('/'))
        path.chop(1);

    const auto config = loadSyncConfig(paths.config, nullptr);
    // The config's flags say how to reach its own remote; another remote
    // (first setup's, before any config) is listed with the flags a new
    // sync gets, so the tree shows what that sync will see (no shortcuts).
    const bool configRemote = config && (remote.isEmpty() || remote == config->remote);
    if (remote.isEmpty())
        remote = config ? config->remote : QString(kNewRemoteName) + ":";
    if (rclone.isEmpty())
        return commandAnswer(json, "rclone was not found", out, err);

    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    // A command killed outright (SIGKILL) takes its rclone along too.
    process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    const StopListingOnSignal stopOnSignal;
    process.start(rclone, folderListArguments(remote, path, configRemote ? config->rcloneFlags : newUserFlags()));
    g_listing = static_cast<sig_atomic_t>(process.processId());
    if (g_listingStopped && process.processId() > 0)  // the signal came before the pid was known
        ::kill(static_cast<pid_t>(process.processId()), SIGTERM);
    if (!process.waitForStarted(kListTimeoutMs) || !process.waitForFinished(kListTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return commandAnswer(json, QString("rclone did not list %1 in time").arg(path.isEmpty() ? remote : path), out, err);
    }
    if (g_listingStopped)
        return commandAnswer(json, "the listing was stopped", out, err);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == kRcloneDirNotFound)
            return commandAnswer(json, QString("%1 is no longer in the cloud").arg(path.isEmpty() ? remote : path), out, err);
        const QString why = QString::fromUtf8(process.readAllStandardError()).trimmed().section('\n', -1);
        return commandAnswer(json, QString("rclone could not list the cloud: %1").arg(why.left(200)), out, err);
    }
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput());
    if (!document.isArray())
        return commandAnswer(json, "rclone's listing is not a JSON list", out, err);

    QList<QJsonObject> folders;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value("Name").toString();
        if (!entry.value("IsDir").toBool() || name.isEmpty())
            continue;
        const QString full = path.isEmpty() ? name : path + "/" + name;
        folders << QJsonObject{{"name", name},
                               {"path", full},
                               {"local", config && QFileInfo(config->localPath + "/" + full).isDir()},
                               {"unsupported", !folderProblem(full).isEmpty()}};
    }
    std::sort(folders.begin(), folders.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const int order = a.value("name").toString().compare(b.value("name").toString(), Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a.value("name").toString() < b.value("name").toString();
    });
    if (json) {
        QJsonArray list;
        for (const QJsonObject &folder : std::as_const(folders))
            list.append(folder);
        out << QJsonDocument(QJsonObject{{"ok", true}, {"error", QString()}, {"folders", list}})
                   .toJson(QJsonDocument::Compact)
            << "\n";
    } else {
        for (const QJsonObject &folder : std::as_const(folders))
            out << folder.value("path").toString() << "\n";
    }
    out.flush();
    return 0;
}
