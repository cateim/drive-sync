#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <optional>

#include "backend.h"

// Selective sync (spec 8): the cloud folders the user leaves out of this
// computer become rules in a marked block at the very top of the filters
// file. Rules are first-match, so a rule there wins over any later "+" of
// the user's own. A folder left out stays whole in the cloud, and its local
// copy, when it was synced before, stays on disk outside the sync (both
// tried with rclone 1.75.2). Changing the block changes the filters, so it
// goes through the fix like any filters edit.

inline constexpr char kSkipBlockBegin[] = "# BEGIN DriveSync: cloud folders left out of this computer";
inline constexpr char kSkipBlockEnd[] = "# END DriveSync: cloud folders left out of this computer";

// A folder path as a literal rclone glob: \ * ? [ ] { } get a backslash.
QString escapeFilterPath(const QString &path);
// The other way: a backslash keeps the character after it.
QString unescapeFilterPath(const QString &escaped);

// The folders the block leaves out, paths relative to the remote's root
// ("Work/Old"); an empty list when there is no block. nullopt, with the
// reason, when the block has no end or holds a line this code never writes
// (edited by hand): the window then sends the user to the filters editor.
std::optional<QStringList> skippedFolders(const QByteArray &filters, QString *error);

// The filters with the block leaving out exactly these folders, sorted, with
// no duplicate and nothing inside another listed folder. An empty list
// removes the block and the blank line after it, so the old bytes come back.
// Refuses an empty name, a leading or trailing "/", an empty segment, a
// newline or carriage return (a filter line cannot hold one), and a block it
// cannot read.
std::optional<QByteArray> withSkippedFolders(const QByteArray &filters, const QStringList &folders, QString *error);

// The tree's rules. Left out: the folder or one above it is in the list.
// Tickable: no folder above it is in the list (a child of a folder left out
// follows its parent). Toggling a tickable folder adds it (dropping the
// listed folders inside it) or removes it; anything else changes nothing.
bool folderSkipped(const QStringList &skipped, const QString &path);
bool folderTickable(const QStringList &skipped, const QString &path);
QStringList toggledFolder(const QStringList &skipped, const QString &path);

// rclone's listing of one level of the cloud: lsjson <remote/path>
// --dirs-only, with the config's --config and --drive-* flags.
QStringList folderListArguments(const QString &remote, const QString &path, const QStringList &configFlags);

// "drive-sync folders [--remote REMOTE] [--json] [[--] PATH]": the folders one
// level below PATH in the cloud, as {"ok", "error", "folders": [{"name",
// "path", "local", "unsupported"}]}, sorted by name ignoring case. "path" is
// relative to the remote's root, "local" says the synced folder holds it,
// "unsupported" marks a name no filter line can hold. The remote is
// --remote, else the config's, else drive-sync: (first setup, before any
// config). Nothing is copied.
int runFoldersCommand(const QStringList &arguments, const BackendPaths &paths, const QString &rclone, QTextStream &out,
                      QTextStream &err);
