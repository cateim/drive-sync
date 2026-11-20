#include "blockeddeletes.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringView>
#include <QTimeZone>
#include <limits>

#include "accounts.h"
#include "controlcommands.h"
#include "fixcommand.h"
#include "fixstate.h"
#include "listingdiff.h"
#include "pausestate.h"
#include "snapshot.h"
#include "synclog.h"

namespace {

constexpr int kAllowFileVersion = 1;
constexpr qint64 kAllowSeconds = 30 * 60;
constexpr int kBisyncDefaultMaxDelete = 50;  // rclone 1.75's --max-delete default for bisync

// "2026/11/12 18:41:19 INFO  : - Path2             File was deleted                            - a/b.txt".
// The path keeps what follows "- " whole: a leading space is part of it.
const QRegularExpression kDeleted(
    R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d [A-Z]+\s*: - Path([12])\s+File was deleted\s+- (.*)$)");
const QRegularExpression kGuard(R"(too many deletes \(>\d+%, (\d+) of (\d+)\))");

// The start of the line runPlanTakingMark writes when a confirmation did
// not get the raised guard, up to the note itself.
const QRegularExpression kKeptLine(R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d INFO\s*: $)");
constexpr QLatin1StringView kKeptNote("DriveSync kept --max-delete at ");

// The note right before the run at start (it is written before rclone
// starts, so before the run's "Synching Path1" line), as a reason word.
// A note with another run's start between it and this one is not this run's,
// and one inside a file name is not a note.
QString keptReasonBefore(const QString &text, qsizetype start) {
    if (start <= 0)
        return QString();
    const qsizetype hit = text.lastIndexOf(kKeptNote, start - 1);
    if (hit < 0 || SyncLog::newestRunStart(QStringView(text).mid(hit, start - hit)) >= 0)
        return QString();
    const qsizetype lineStart = text.lastIndexOf('\n', hit) + 1;
    if (!kKeptLine.matchView(QStringView(text).mid(lineStart, hit - lineStart)).hasMatch())
        return QString();
    const QStringView note = QStringView(text).mid(hit, text.indexOf('\n', hit) - hit);
    if (note.contains(QLatin1String("was dropped")))
        return "dropped";
    if (note.contains(QLatin1String("a dry run finds")))
        return "unconfirmed";
    if (note.contains(QLatin1String("still stopped")))
        return "stillStopped";
    if (note.contains(QLatin1String("wrote no run")))
        return "noRun";
    return "failed";
}

// rclone writes a name with " or \ in it the Go way, quotes included.
QString unquoted(const QString &path) {
    if (path.size() < 2 || !path.startsWith('"') || !path.endsWith('"'))
        return path;
    bool ok = false, notUtf8 = false;
    const QString name = unquoteGo(path.mid(1, path.size() - 2), &ok, &notUtf8);
    return ok ? name : path;
}

}  // namespace

BlockedDeletes deletionsInNewestRun(const QString &logText, int limit) {
    BlockedDeletes result;
    const qsizetype start = SyncLog::newestRunStart(logText);
    if (start < 0)
        return result;
    QList<BlockedDelete> cloud, notebook;
    for (QStringView line : QStringView(logText).mid(start).split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.contains(QLatin1String("File was deleted")))
            continue;
        const QRegularExpressionMatch m = kDeleted.matchView(line);
        if (!m.hasMatch())
            continue;
        const bool inCloud = m.capturedView(1) == QLatin1String("1");
        ++result.count;
        ++(inCloud ? result.cloudCount : result.notebookCount);
        QList<BlockedDelete> &side = inCloud ? cloud : notebook;
        if (side.size() < limit)
            side.append({unquoted(m.captured(2)), inCloud});
    }
    result.list = (cloud + notebook).mid(0, qMax(0, limit));
    return result;
}

bool guardStopHolds(const QList<SyncRun> &runs) {
    for (auto it = runs.crbegin(); it != runs.crend(); ++it) {
        if (it->outcome == "open" || it->phase.isEmpty())
            continue;
        return it->outcome == "failed" && it->causeKind == "maxDelete";
    }
    return false;
}

bool fixWaitsForDeletes(const FixState &fix, bool guardStopHolds) {
    return fix.step == "failed" && fix.failedStep == "finishing" && guardStopHolds;
}

std::optional<BlockedDeletes> blockedDeletes(const QString &logText, int limit) {
    const qsizetype start = SyncLog::newestRunStart(logText);
    if (start < 0)
        return std::nullopt;
    // The parser also makes a run of an error with no run open (DriveSync's
    // refusal, rclone that did not start), so one run means nothing came after.
    const QList<SyncRun> runs = SyncLog::parse(logText.mid(start));
    if (runs.size() != 1 || runs.first().outcome != "failed" || runs.first().causeKind != "maxDelete")
        return std::nullopt;
    BlockedDeletes result = deletionsInNewestRun(logText, limit);
    result.runStart = runs.first().start;
    result.keptReason = keptReasonBefore(logText, start);
    const QRegularExpressionMatch guard = kGuard.match(runs.first().causeDetail);
    result.guardCount = guard.captured(1).toInt();
    result.guardTotal = guard.captured(2).toInt();
    return result;
}

QString allowDeletesPath(const QString &appState) { return appState + "/allow-deletes.json"; }

bool writeAllowDeletes(const QString &appState, const AllowDeletes &allow) {
    if (!QDir().mkpath(appState))
        return false;
    QSaveFile out(allowDeletesPath(appState));
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(QJsonObject{{"version", kAllowFileVersion},
                                        {"count", allow.count},
                                        {"cloudCount", allow.cloudCount},
                                        {"notebookCount", allow.notebookCount},
                                        {"total", allow.total},
                                        {"at", allow.at.isValid() ? allow.at.toSecsSinceEpoch() : 0},
                                        {"runStart", allow.runStart.isValid() ? allow.runStart.toSecsSinceEpoch() : 0}})
                  .toJson(QJsonDocument::Compact));
    return out.commit();
}

std::optional<AllowDeletes> takeAllowDeletes(const QString &appState, const QDateTime &now) {
    QFile in(allowDeletesPath(appState));
    if (!in.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
    in.close();
    // Gone before anything else: a mark that could not be removed would
    // serve a second run.
    if (!in.remove())
        return std::nullopt;
    const QJsonObject object = document.object();
    AllowDeletes allow;
    allow.count = object.value("count").toInt();
    allow.cloudCount = object.value("cloudCount").toInt();
    allow.notebookCount = object.value("notebookCount").toInt();
    allow.total = object.value("total").toInt();
    const qint64 at = object.value("at").toInteger();
    allow.at = QDateTime::fromSecsSinceEpoch(at, QTimeZone::UTC);
    const qint64 runStart = object.value("runStart").toInteger();
    if (runStart > 0)
        allow.runStart = QDateTime::fromSecsSinceEpoch(runStart, QTimeZone::UTC);
    // A clock set back since then makes the age negative: refused too, the
    // safer side.
    const qint64 age = now.toSecsSinceEpoch() - at;
    if (object.value("version").toInt() != kAllowFileVersion || at <= 0 || age < 0 || age > kAllowSeconds
        || allow.count <= 0 || allow.total <= 0)
        return std::nullopt;
    return allow;
}

int maxDeletePercent(int largestSide, int total, int configured) {
    const qint64 percent = total > 0 ? (qint64(largestSide) * 100 + total - 1) / total : 100;
    return int(qMin<qint64>(100, qMax<qint64>(configured, percent)));
}

QStringList withMaxDelete(const QStringList &arguments, int percent) {
    QStringList result = arguments;
    bool found = false;
    for (qsizetype i = 0; i < result.size(); ++i) {
        if (result.at(i) == "--max-delete" && i + 1 < result.size()) {
            result[++i] = QString::number(percent);
            found = true;
        } else if (result.at(i).startsWith("--max-delete=")) {
            result[i] = QString("--max-delete=%1").arg(percent);
            found = true;
        }
    }
    if (!found)
        result << "--max-delete" << QString::number(percent);
    return result;
}

namespace {

constexpr int kListLimit = 500;

QJsonObject listJson(const std::optional<BlockedDeletes> &blocked) {
    const BlockedDeletes b = blocked.value_or(BlockedDeletes());
    QJsonArray list;
    for (const BlockedDelete &d : b.list)
        list.append(QJsonObject{{"path", d.path}, {"deletedInCloud", d.deletedInCloud}});
    return QJsonObject{{"ok", true},
                       {"error", QString()},
                       {"count", b.count},
                       {"cloudCount", b.cloudCount},
                       {"notebookCount", b.notebookCount},
                       {"guardCount", b.guardCount},
                       {"guardTotal", b.guardTotal},
                       {"runStart", b.runStart.isValid() ? b.runStart.toSecsSinceEpoch() : 0},
                       {"list", list}};
}

void printList(const std::optional<BlockedDeletes> &blocked, QTextStream &out) {
    if (!blocked) {
        out << "Nothing is held back: the last run did not stop at the max-delete guard.\n";
        return;
    }
    out << QString("The last run stopped at the max-delete guard (%1 of %2 on one side). It would delete %3 files:\n")
               .arg(QString::number(blocked->guardCount), QString::number(blocked->guardTotal),
                    QString::number(blocked->count));
    out << QString("  %1 gone from the cloud: deleted on the notebook, kept in a backup folder of DriveSync's state\n")
               .arg(blocked->cloudCount);
    out << QString("  %1 gone from the notebook: sent to the Drive trash\n").arg(blocked->notebookCount);
    bool cloudHeader = false, notebookHeader = false;
    for (const BlockedDelete &d : blocked->list) {
        bool &header = d.deletedInCloud ? cloudHeader : notebookHeader;
        if (!header)
            out << (d.deletedInCloud ? "Gone from the cloud:\n" : "Gone from the notebook:\n");
        header = true;
        out << "  " << d.path << "\n";
    }
    if (blocked->list.size() < blocked->count)
        out << QString("(the first %1 of %2)\n").arg(blocked->list.size()).arg(blocked->count);
    out << QString("To let one run delete them: drive-sync deletes --allow %1\n").arg(blocked->count);
}

// The checks, the mark, then the run that uses it.
QString allowDeletes(const BackendPaths &paths, const std::optional<BlockedDeletes> &blocked, int count, bool *stale) {
    if (!paths.managed)
        return "letting deletions through needs DriveSync to run drive-sync; take it over first";
    if (takeoverLockHeld(paths.globalState(), paths.proc))
        return "a takeover is running; try again when it ends";
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const PauseMark pause = readPause(pauseFilePath(paths.appState));
    if (pause.present && !pauseEnded(pause, now))
        return "drive-sync is paused; run drive-sync resume first";
    if (!blocked)
        return "nothing is held back: the last run did not stop at the max-delete guard";
    if (blocked->guardTotal <= 0)
        return "the max-delete line of the last run cannot be read";
    if (count != blocked->count)
        return QString("the last run would delete %1 files, not %2; see them with drive-sync deletes --list")
            .arg(blocked->count)
            .arg(count);
    AllowDeletes allow;
    allow.count = blocked->count;
    allow.cloudCount = blocked->cloudCount;
    allow.notebookCount = blocked->notebookCount;
    allow.total = blocked->guardTotal;
    allow.at = now;
    allow.runStart = blocked->runStart;

    // Under the fix's lock: a fix starting meanwhile would hold the run. None
    // while the move to account1 runs, and none at a place it just left.
    const AccountWriteLock lock(paths);
    if (!lock.refusal().isEmpty())
        return lock.refusal();
    if ((*stale = lock.stale()))
        return QString();
    const bool fixing = fixPresent(paths.appState);
    const FixState fix = fixing ? readFix(paths.appState) : FixState();
    // Only a fix whose final step failed with the guard stop still the
    // newest run (blocked says so): the confirmation is for that run, so the
    // fix goes back to it.
    if (fixing && !fixWaitsForDeletes(fix, blocked.has_value()))
        return "a fix is under way, and it holds every run until it ends";
    if (!writeAllowDeletes(paths.appState, allow))
        return QString("cannot write %1").arg(allowDeletesPath(paths.appState));
    const QString error = fixing ? retryFix(paths, fix) : startOneRun(paths);
    if (!error.isEmpty())
        QFile::remove(allowDeletesPath(paths.appState));  // no later run may take it
    return error;
}

}  // namespace

int runDeletesCommand(const QStringList &arguments, const BackendPaths &paths, QTextStream &out, QTextStream &err) {
    QStringList rest = arguments;
    const bool json = rest.removeAll("--json") > 0;
    const bool list = rest == QStringList{"--list"};
    bool countOk = false;
    const int count = rest.size() == 2 && rest.at(0) == "--allow" ? rest.at(1).toInt(&countOk) : 0;
    if (!list && (!countOk || count <= 0))
        return commandAnswer(json, "usage: drive-sync deletes --list | --allow <count> [--json]", out, err);
    // The window shows 500 paths; printed here, every one of them is.
    const auto blocked = blockedDeletes(SyncLog::readRecent(paths.log, kTailBytes),
                                        list && !json ? std::numeric_limits<int>::max() : kListLimit);
    if (!list) {
        bool stale = false;
        const QString error = allowDeletes(paths, blocked, count, &stale);
        if (stale)
            return kAccountPathsMoved;  // nothing printed: main runs it again
        return commandAnswer(json, error, out, err);
    }
    if (json)
        out << QJsonDocument(listJson(blocked)).toJson(QJsonDocument::Compact) << "\n";
    else
        printList(blocked, out);
    out.flush();
    return 0;
}

int configuredMaxDelete(const QStringList &arguments) {
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == "--max-delete" && i + 1 < arguments.size())
            return arguments.at(i + 1).toInt();
        if (arguments.at(i).startsWith("--max-delete="))
            return arguments.at(i).mid(13).toInt();
    }
    return kBisyncDefaultMaxDelete;
}

namespace {

// How long after its record the run let through may start: rclone writes
// its start line within a second of drive-sync run's exec.
constexpr qint64 kConfirmedStartSeconds = 5 * 60;

// The start of every whole "Synching Path1" line, oldest first.
QList<qsizetype> runStartsIn(const QString &text) {
    static const QRegularExpression synching(R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d [A-Z]+\s*: Synching Path1 )",
                                             QRegularExpression::MultilineOption);
    QList<qsizetype> starts;
    for (auto it = synching.globalMatch(text); it.hasNext();)
        starts << it.next().capturedStart();
    return starts;
}

QString confirmedRunPath(const QString &appState) { return appState + "/allowed-run.json"; }
QString unconfirmedPath(const QString &appState) { return appState + "/unconfirmed-deletes.json"; }

bool writeJson(const QString &appState, const QString &file, const QJsonObject &object) {
    if (!QDir().mkpath(appState))
        return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return out.commit();
}

}  // namespace

bool writeConfirmedRun(const QString &appState, const ConfirmedRun &run) {
    return writeJson(appState, confirmedRunPath(appState),
                     QJsonObject{{"version", kAllowFileVersion},
                                 {"at", run.at.isValid() ? run.at.toSecsSinceEpoch() : 0},
                                 {"backupDir", run.backupDir},
                                 {"confirmed", QJsonArray::fromStringList(run.confirmed)}});
}

UnconfirmedDeletes checkConfirmedRun(const QString &appState, const QString &logFile) {
    QFile in(confirmedRunPath(appState));
    if (!in.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject object = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    // Checked once, whatever the result: the next run is a normal one.
    in.remove();
    const qint64 at = object.value("at").toInteger();
    if (object.value("version").toInt() != kAllowFileVersion || at <= 0)
        return {};
    bool truncated = false;
    const QString text = SyncLog::readRecent(logFile, kTailBytes, nullptr, &truncated);
    const QList<SyncRun> runs = SyncLog::parse(text, kStateRuns, truncated);
    // The run that took the mark is the first one to reach bisync after it
    // (the log keeps whole seconds). None means rclone stopped before bisync
    // this time.
    qsizetype first = -1;
    for (qsizetype i = 0; i < runs.size() && first < 0; ++i)
        if (!runs.at(i).phase.isEmpty() && runs.at(i).start.toSecsSinceEpoch() >= at - 2)
            first = i;
    if (first < 0)
        return {};
    const SyncRun &run = runs.at(first);
    // drive-sync run writes the record just before rclone starts, and rclone
    // logs the run's start at once: a later first run is an ordinary one
    // after a record whose run never started (the machine died in between).
    if (run.start.toSecsSinceEpoch() > at + kConfirmedStartSeconds)
        return {};
    // A run that stopped before "Applying changes" (the guard again, with
    // more files gone since the dry run) only listed what it would delete.
    // One that reached it counts even with runs after it: a fix unit killed
    // during its final run has no ExecStopPost to check it then.
    if (run.outcome != "success" && run.phase != "applying" && run.phase != "updating")
        return {};
    // That run's own lines: the text up to the start of the run after it.
    qsizetype ordinal = 0;
    for (qsizetype i = 0; i < first; ++i)
        if (!runs.at(i).phase.isEmpty())
            ++ordinal;
    const QList<qsizetype> starts = runStartsIn(text);
    const qsizetype end = ordinal + 1 < starts.size() ? starts.at(ordinal + 1) : text.size();
    QSet<QString> confirmed;
    for (const QJsonValue &item : object.value("confirmed").toArray())
        confirmed.insert(item.toString());
    UnconfirmedDeletes result;
    result.runStart = run.start;
    result.backupDir = object.value("backupDir").toString();
    const BlockedDeletes found = deletionsInNewestRun(text.left(end), std::numeric_limits<int>::max());
    for (const BlockedDelete &d : found.list)
        if (!confirmed.contains(QString(d.deletedInCloud ? "1:" : "2:") + d.path))
            ++(d.deletedInCloud ? result.cloudCount : result.notebookCount);
    if (result.cloudCount + result.notebookCount == 0)
        return result;
    // The window reads the file when the log line arrives.
    writeJson(appState, unconfirmedPath(appState),
              QJsonObject{{"version", kAllowFileVersion},
                          {"runStart", result.runStart.toSecsSinceEpoch()},
                          {"cloudCount", result.cloudCount},
                          {"notebookCount", result.notebookCount},
                          {"backupDir", result.backupDir}});
    appendToLog(logFile, "NOTICE",
                QString("DriveSync: the run let through deleted %1 files that were not in the confirmed list "
                        "(gone from the cloud, deleted on the notebook and kept in %2: %3; gone from the notebook, "
                        "sent to the Drive trash: %4)")
                    .arg(QString::number(result.cloudCount + result.notebookCount), result.backupDir,
                         QString::number(result.cloudCount), QString::number(result.notebookCount)));
    return result;
}

std::optional<UnconfirmedDeletes> readUnconfirmedDeletes(const QString &appState) {
    QFile in(unconfirmedPath(appState));
    if (!in.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject object = QJsonDocument::fromJson(in.readAll()).object();
    const qint64 runStart = object.value("runStart").toInteger();
    if (object.value("version").toInt() != kAllowFileVersion || runStart <= 0)
        return std::nullopt;
    UnconfirmedDeletes result;
    result.runStart = QDateTime::fromSecsSinceEpoch(runStart, QTimeZone::UTC);
    result.cloudCount = object.value("cloudCount").toInt();
    result.notebookCount = object.value("notebookCount").toInt();
    result.backupDir = object.value("backupDir").toString();
    return result;
}
