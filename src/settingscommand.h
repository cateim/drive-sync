#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "backend.h"
#include "takeover.h"

// "drive-sync settings": what the window's Settings view shows and changes
// (spec section 8). The interval and the max-delete guard apply at once.
// The folder, the filters and the cloud folders left out (selective sync)
// change what bisync compares, so they go through the fix: the new config or
// filters are written after the fix's mark and before its unit starts, and
// its dry run is the confirmation. Cancelling that fix before its base puts
// the old bytes back (settingsChangeMarkPath in fixstate.h).

// The drop-in that sets the packaged timer's interval:
// <userUnits>/drive-sync.timer.d/drive-sync-interval.conf.
QString intervalDropInPath(const QString &userUnits);
// Empty when rclone reads every rule of these filters (checked on an empty
// folder, so nothing is listed or copied); otherwise why, with rclone's words.
QString filtersRefusal(const QString &rclone, const QByteArray &filters);
// Its text. The empty OnUnitActiveSec= resets every timer of the unit
// (systemd.timer(5)), OnBootSec included, so it repeats the packaged value.
QByteArray intervalDropIn(int minutes);

// The guard a user may set, in percent: above this it stops guarding.
inline constexpr int kMaxSettableMaxDelete = 50;

// "drive-sync settings [--interval N | --max-delete N | --folder PATH |
// --filters FILE | --skip FOLDER... | --skip-none] [--json]". No change:
// the settings, as {"ok", "error", "folder", "remote", "intervalMinutes",
// "maxDelete", "filtersFile", "filters", "skipped", "skipError"}. A change:
// {"ok", "error", "fixStarted"}. Refused unless DriveSync runs drive-sync,
// and while a fix or a takeover is under way.
// --filters - reads the new filters from input (stdin when null), so the
// window hands the editor's bytes over without a file of its own. Values are
// taken as they are: a folder named "--json" is a folder.
int runSettingsCommand(const QStringList &arguments, const BackendPaths &paths, const TakeoverPaths &takeover,
                       QTextStream &out, QTextStream &err, QIODevice *input = nullptr);
