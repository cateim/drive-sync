#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "filterrules.h"

// The window's side of the filters screen (step 4c): the groups, the rules
// by name and the advanced text of a FilterModel, edited live; the paths come
// from LocalTree, which the screen hands over when it asks for the text.
// Nothing here writes a file: the text goes to "drive-sync settings
// --filters -" or "drive-sync setup --filters -", which rebuild the base.
class FilterEditor : public QObject {
    Q_OBJECT
    // {id, ticked, rules} per group, in filterPresets() order; rules is the
    // rclone lines, one per line, for the screen to show under the box.
    Q_PROPERTY(QVariantList presets READ presets NOTIFY changed)
    // {kind, value, line} per rule, in the order added.
    Q_PROPERTY(QVariantList nameRules READ nameRules NOTIFY changed)
    // The paths the loaded text leaves out, for LocalTree.reset().
    Q_PROPERTY(QStringList localPaths READ localPaths NOTIFY loaded)
    Q_PROPERTY(QString advanced READ advanced NOTIFY changed)

public:
    explicit FilterEditor(QObject *parent = nullptr);

    QVariantList presets() const;
    QVariantList nameRules() const;
    QStringList localPaths() const { return m_model.localPaths; }
    QString advanced() const { return QString::fromUtf8(m_model.advanced); }

    // What a new user starts with (defaultFilters()), for first setup.
    Q_INVOKABLE QString defaultText() const { return QString::fromUtf8(defaultFilters()); }
    // Loads a filters file's text; loaded() follows.
    Q_INVOKABLE void load(const QString &text);
    // Reads a filters file from an earlier sync ("~" is the home), lighting
    // up what the screen can show. Empty when read, otherwise why not.
    Q_INVOKABLE QString importFile(const QString &path);
    Q_INVOKABLE void setPreset(const QString &id, bool ticked);
    // Empty when added, otherwise why not (nothing changes then).
    Q_INVOKABLE QString addNameRule(const QString &kind, const QString &value);
    Q_INVOKABLE void removeNameRule(int index);
    Q_INVOKABLE void setAdvanced(const QString &text);
    // Whether these paths and the editor's state differ from what was loaded,
    // by meaning, not by bytes: an untouched Save must start no fix.
    Q_INVOKABLE bool edited(const QStringList &localPaths) const;
    // The whole filters text, with these paths (LocalTree.excluded) left out.
    Q_INVOKABLE QString text(const QStringList &localPaths) const;

signals:
    void changed();
    void loaded();

private:
    FilterModel m_model;
    FilterModel m_loaded;  // as load() or importFile() left it
};
