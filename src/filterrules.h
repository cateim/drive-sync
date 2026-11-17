#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// The filters a person builds on screen (step 4c): ready-made groups, paths
// on this computer, rules by name, and the rest as text. Each of the first
// three is a marked block at the end of the filters file, after every byte
// the person wrote (the "advanced" text, the cloud-folders block of selective
// sync included): rclone takes the first rule that matches, so a "+" rule
// written by hand keeps winning over the excludes built here.

struct FilterPreset {
    QString id;  // "git", "nodeModules", "python", "builds", "googleDocs", "systemJunk", "temporary"
    QStringList rules;
};
// In the order the screen shows them.
const QList<FilterPreset> &filterPresets();
// What a new user starts with: every group but "builds".
QStringList defaultPresetIds();

// kind: "name" (a file or folder with this name, in any folder), "folder"
// (a folder with this name, in any folder, and everything in it),
// "extension" (files ending in ".<value>"), "path" (one file, or one folder
// when the value ends with "/", from the top of the synced folder).
struct NameRule {
    QString kind;
    QString value;
    bool operator==(const NameRule &other) const { return kind == other.kind && value == other.value; }
};
// The rclone line for a rule, glob characters in the value escaped; empty
// for a rule that cannot be written (an empty value, a line break, a "/"
// inside a name, folder or extension).
QString nameRuleLine(const NameRule &rule);

struct FilterModel {
    QStringList presets;        // the ids ticked, in filterPresets() order
    QStringList localPaths;     // from the top of the synced folder; a folder ends with "/"
    QList<NameRule> nameRules;  // in the order added
    QByteArray advanced;        // every byte outside DriveSync's blocks, as it is
};

// The blocks parsed back, and everything else kept byte for byte. A block
// with a line the screen never writes stays in the advanced text whole, so
// nothing written by hand is ever dropped.
FilterModel readFilterModel(const QByteArray &filters);
// The advanced text as it is, then the three blocks, each only when it has
// rules. readFilterModel(writeFilterModel(m)) gives m back.
QByteArray writeFilterModel(const FilterModel &model);
// A filters file from an earlier sync, read like readFilterModel, then the
// plain lines of its text that the screen can show move there: a group whose
// every rule is present, rules by name, paths from the top. Nothing moves when
// the text has a "+" or "!" rule, since moving excludes below it could change
// what matches first.
FilterModel importedFilterModel(const QByteArray &filters);

// What a new user starts with: a short header for the person, then the
// default groups as DriveSync's block.
QByteArray defaultFilters();
