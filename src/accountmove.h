#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "accounts.h"
#include "backend.h"
#include "takeover.h"

// The move of the single drive-sync (legacy mode) to account1 of the
// accounts layout, with no resync (spec 12, "Moving the single account to
// account1"). Only config.json and the state files move; every path the
// config names (folder, remote, work dir, filters with their .md5, log) stays
// where it is, so bisync sees the same command line.
//
// The order: check everything, touching nothing; take the takeover lock and
// the FixLock; write the journal <stateRoot>/migration.json; disable the
// plain timer; check that no run started meanwhile; move the state files and
// the interval drop-in, rewrite the paths in the moved JSON files, write
// machine.json, then make <configRoot>/accounts/account1 and move config.json
// last (legacy mode holds until then); daemon-reload and check the instance
// units; enable and start drive-sync@account1.timer as the plain one was;
// remove the journal. Any failure, in this run or after a crash in an
// earlier one, rolls back from the journal; a rollback that cannot get back
// to legacy mode keeps the journal with "stuck": true and every timer off.

// What a move ended in: "moved", "notNeeded" (already the accounts layout),
// "deferred" (nothing was touched, or what was is back), "rolledBack" (a
// journal was undone) or "stuck"; reason says why for all but "moved".
struct MoveOutcome {
    QString result;
    QString reason;
};

// The renames of the move, in order, as {from, to} absolute paths: each
// state entry present in <stateRoot> into <stateRoot>/accounts/account1/,
// then the plain timer's interval drop-in to the instance's when present,
// then config.json to <configRoot>/accounts/account1/config.json.
QList<QPair<QString, QString>> plannedMoves(const AccountRoots &roots, const BackendPaths &legacy);

// Step 1, read only: empty when the move can go now, else why not. Checks
// that the mode is legacy and its config loads, that the machine guard
// passes, that the takeover lock and the FixLock are free, that no target of
// the move exists, that the three account1 instances load from the package
// with no drop-in, that no foreign drop-in reaches them, that no old
// drive-sync is found, and that the plain drive-sync.service and
// drive-sync-fix.service are inactive.
QString moveBlocker(const AccountRoots &roots, const TakeoverPaths &tools);

// Runs the move, or rolls back the journal of one that stopped halfway
// (then it stops there: the move is tried again at a later trigger). A
// journal another live drive-sync is working on is left alone.
MoveOutcome moveToAccounts(const AccountRoots &roots, const TakeoverPaths &tools);

// "drive-sync accounts [--migrate] [--json]": the mode, each account's id,
// label and folder, and whether the move can go; --migrate runs it first.
int runAccountsCommand(const QStringList &arguments, const AccountRoots &roots, const TakeoverPaths &tools,
                       QTextStream &out, QTextStream &err);
