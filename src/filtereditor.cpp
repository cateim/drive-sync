#include "filtereditor.h"

#include <QDir>
#include <QFile>

FilterEditor::FilterEditor(QObject *parent) : QObject(parent) {}

QVariantList FilterEditor::presets() const {
    QVariantList list;
    for (const FilterPreset &preset : filterPresets()) {
        QStringList rules;
        for (const QString &rule : preset.rules)
            rules << rule.mid(2);  // "- " off: the screen shows the pattern
        list << QVariantMap{
            {"id", preset.id}, {"ticked", m_model.presets.contains(preset.id)}, {"rules", rules.join("  ")}};
    }
    return list;
}

QVariantList FilterEditor::nameRules() const {
    QVariantList list;
    for (const NameRule &rule : m_model.nameRules)
        list << QVariantMap{{"kind", rule.kind}, {"value", rule.value}, {"line", nameRuleLine(rule)}};
    return list;
}

void FilterEditor::load(const QString &text) {
    m_model = m_loaded = readFilterModel(text.toUtf8());
    emit changed();
    emit loaded();
}

QString FilterEditor::importFile(const QString &path) {
    QString file = path.trimmed();
    if (file == "~" || file.startsWith("~/"))
        file = QDir::homePath() + file.mid(1);
    QFile in(file);
    // A filters file is a few KB; anything far bigger is the wrong file.
    constexpr qint64 kMaxBytes = 1024 * 1024;
    if (!in.open(QIODevice::ReadOnly))
        return tr("Cannot read %1: %2").arg(file, in.errorString());
    if (in.size() > kMaxBytes)
        return tr("%1 is too big for a filters file.").arg(file);
    m_model = importedFilterModel(in.readAll());
    // An import is an edit: the file on disk is not what it shows.
    m_loaded = FilterModel();
    emit changed();
    emit loaded();
    return QString();
}

void FilterEditor::setPreset(const QString &id, bool ticked) {
    if (ticked == m_model.presets.contains(id))
        return;
    if (ticked)
        m_model.presets << id;
    else
        m_model.presets.removeAll(id);
    emit changed();
}

QString FilterEditor::addNameRule(const QString &kind, const QString &value) {
    const NameRule rule{kind, value.trimmed()};
    const QString line = nameRuleLine(rule);
    if (line.isEmpty())
        return kind == "path" || value.trimmed().isEmpty()
                   ? tr("Type a name first.")
                   : tr("A name, a folder or an extension cannot hold \"/\": use a path for that.");
    for (const NameRule &other : m_model.nameRules)
        if (nameRuleLine(other) == line)
            return tr("That rule is already in the list.");
    m_model.nameRules << rule;
    emit changed();
    return QString();
}

void FilterEditor::removeNameRule(int index) {
    if (index < 0 || index >= m_model.nameRules.size())
        return;
    m_model.nameRules.removeAt(index);
    emit changed();
}

void FilterEditor::setAdvanced(const QString &text) {
    const QByteArray bytes = text.toUtf8();
    if (bytes == m_model.advanced)
        return;
    m_model.advanced = bytes;
    emit changed();
}

bool FilterEditor::edited(const QStringList &localPaths) const {
    FilterModel now = m_model, before = m_loaded;
    now.localPaths = localPaths;
    now.presets.sort();
    before.presets.sort();
    now.localPaths.sort();
    before.localPaths.sort();
    return now.presets != before.presets || now.localPaths != before.localPaths
           || now.nameRules != before.nameRules || now.advanced != before.advanced;
}

QString FilterEditor::text(const QStringList &localPaths) const {
    FilterModel model = m_model;
    model.localPaths = localPaths;
    return QString::fromUtf8(writeFilterModel(model));
}
