#include "filterrules.h"

#include <QSet>

#include "selectivesync.h"

namespace {

const QByteArray kPresetsBegin = "# BEGIN DriveSync: ready-made groups";
const QByteArray kPresetsEnd = "# END DriveSync: ready-made groups";
const QByteArray kPathsBegin = "# BEGIN DriveSync: paths on this computer";
const QByteArray kPathsEnd = "# END DriveSync: paths on this computer";
const QByteArray kNamesBegin = "# BEGIN DriveSync: rules by name";
const QByteArray kNamesEnd = "# END DriveSync: rules by name";
const QByteArray kGroupPrefix = "# group: ";

// rclone drops the spaces at the end of a rule line, escaped or not (tried
// with rclone 1.75.2 on 2026-11-17), so a name ending in a space writes that
// last space as the class "[ ]", which rclone matches.
const QString kFinalSpace = "[ ]";

QString escapeValue(const QString &value) {
    QString escaped = escapeFilterPath(value);
    if (escaped.endsWith(' '))
        escaped = escaped.chopped(1) + kFinalSpace;
    return escaped;
}

// The value an escaped text stands for, or a null string when it holds a
// glob character of its own (the screen never writes one).
QString plainValue(const QString &escaped) {
    const bool finalSpace = escaped.endsWith(kFinalSpace);
    const QString base = finalSpace ? escaped.chopped(kFinalSpace.size()) : escaped;
    if (escapeFilterPath(unescapeFilterPath(base)) != base)
        return QString();
    return unescapeFilterPath(base) + (finalSpace ? " " : "");
}

// One line of a block, back into a rule; nullopt for anything the screen
// never writes.
std::optional<NameRule> ruleFromLine(const QString &line) {
    if (!line.startsWith("- ") || line.size() <= 2)
        return std::nullopt;
    const QString rest = line.mid(2);
    NameRule rule;
    QString escaped;
    if (rest.startsWith('/')) {
        rule.kind = "path";
        const bool folder = rest.endsWith("/**");
        escaped = rest.mid(1, rest.size() - 1 - (folder ? 3 : 0));
        rule.value = plainValue(escaped);
        if (escaped.isEmpty() || rule.value.isNull())
            return std::nullopt;
        rule.value += folder ? "/" : "";
    } else if (rest.startsWith("*.")) {
        rule.kind = "extension";
        escaped = rest.mid(2);
        rule.value = plainValue(escaped);
        if (escaped.isEmpty() || escaped.contains('/') || rule.value.isNull())
            return std::nullopt;
    } else if (rest.endsWith("/**")) {
        rule.kind = "folder";
        escaped = rest.chopped(3);
        rule.value = plainValue(escaped);
        if (escaped.isEmpty() || escaped.contains('/') || rule.value.isNull())
            return std::nullopt;
    } else {
        rule.kind = "name";
        rule.value = plainValue(rest);
        if (rest.contains('/') || rule.value.isNull())
            return std::nullopt;
    }
    // Only what nameRuleLine writes the same way counts.
    if (nameRuleLine(rule) != line)
        return std::nullopt;
    return rule;
}

// The lines of a file, each with its own line end, so text outside the blocks
// is put back byte for byte.
QList<QByteArray> linesWithEnds(const QByteArray &bytes) {
    QList<QByteArray> lines;
    qsizetype from = 0;
    while (from < bytes.size()) {
        const qsizetype end = bytes.indexOf('\n', from);
        const qsizetype next = end < 0 ? bytes.size() : end + 1;
        lines << bytes.mid(from, next - from);
        from = next;
    }
    return lines;
}

QByteArray bare(QByteArray line) {
    while (line.endsWith('\n') || line.endsWith('\r'))
        line.chop(1);
    return line;
}

// Reads one block's lines into the model; false (model untouched) when a
// line is not one the screen writes.
bool takeBlock(const QByteArray &begin, const QList<QByteArray> &lines, FilterModel *model) {
    if (begin == kPresetsBegin) {
        QStringList ids;
        QString id;
        QStringList rules;
        auto close = [&]() {
            if (id.isNull())
                return rules.isEmpty();
            for (const FilterPreset &preset : filterPresets())
                if (preset.id == id && preset.rules == rules && !ids.contains(id)) {
                    ids << id;
                    return true;
                }
            return false;
        };
        for (const QByteArray &raw : lines) {
            const QByteArray line = bare(raw);
            if (line.startsWith(kGroupPrefix)) {
                if (!close())
                    return false;
                id = QString::fromUtf8(line.mid(kGroupPrefix.size()));
                rules.clear();
            } else {
                rules << QString::fromUtf8(line);
            }
        }
        if (!close())
            return false;
        // In the screen's order, whatever order the block had.
        for (const FilterPreset &preset : filterPresets())
            if (ids.contains(preset.id))
                model->presets << preset.id;
        return true;
    }
    QList<NameRule> rules;
    for (const QByteArray &raw : lines) {
        const std::optional<NameRule> rule = ruleFromLine(QString::fromUtf8(bare(raw)));
        if (!rule || (begin == kPathsBegin && rule->kind != "path"))
            return false;
        rules << *rule;
    }
    if (begin == kPathsBegin) {
        for (const NameRule &rule : rules)
            model->localPaths << rule.value;
    } else {
        model->nameRules += rules;
    }
    return true;
}

QByteArray block(const QByteArray &begin, const QByteArray &end, const QByteArray &body) {
    return begin + '\n' + body + end + '\n';
}

}  // namespace

const QList<FilterPreset> &filterPresets() {
    static const QList<FilterPreset> presets{
        {"git", {"- .git/**", "- .git"}},
        {"nodeModules", {"- node_modules/**"}},
        {"python", {"- .venv/**", "- venv/**", "- __pycache__/**", "- *.pyc"}},
        {"builds", {"- build/**", "- dist/**", "- target/**", "- .pio/**", "- .next/**", "- .nuxt/**",
                    "- .svelte-kit/**"}},
        {"googleDocs", {"- *.gdoc", "- *.gsheet", "- *.gslides", "- *.gdraw", "- *.gform", "- *.gscript",
                        "- *.gsite", "- *.gmap", "- *.gjam", "- *.glink", "- *.gvid"}},
        {"systemJunk", {"- desktop.ini", "- Thumbs.db", "- .DS_Store"}},
        {"temporary", {"- .~lock.*#", "- .tmp.drivedownload/**", "- .tmp.driveupload/**"}},
    };
    return presets;
}

QStringList defaultPresetIds() { return {"git", "nodeModules", "python", "googleDocs", "systemJunk", "temporary"}; }

QString nameRuleLine(const NameRule &rule) {
    // Never trimmed here: a path from the tree may begin or end with a space,
    // and trimming it would leave out another path. Typed values are trimmed
    // where they are typed (FilterEditor::addNameRule).
    QString value = rule.value;
    if (value.trimmed().isEmpty() || value.contains('\n') || value.contains('\r'))
        return QString();
    if (rule.kind == "extension") {
        if (value.startsWith('.'))
            value.remove(0, 1);
        if (value.isEmpty() || value.contains('/'))
            return QString();
        return "- *." + escapeValue(value);
    }
    if (rule.kind == "name")
        return value.contains('/') ? QString() : "- " + escapeValue(value);
    if (rule.kind == "folder") {
        if (value.endsWith('/'))
            value.chop(1);
        if (value.isEmpty() || value.contains('/'))
            return QString();
        return "- " + escapeFilterPath(value) + "/**";
    }
    if (rule.kind == "path") {
        while (value.startsWith('/'))
            value.remove(0, 1);
        const bool folder = value.endsWith('/');
        if (folder)
            value.chop(1);
        if (value.isEmpty())
            return QString();
        return "- /" + (folder ? escapeFilterPath(value) + "/**" : escapeValue(value));
    }
    return QString();
}

FilterModel readFilterModel(const QByteArray &filters) {
    FilterModel model;
    const QList<QByteArray> lines = linesWithEnds(filters);
    const QList<QPair<QByteArray, QByteArray>> markers{
        {kPresetsBegin, kPresetsEnd}, {kPathsBegin, kPathsEnd}, {kNamesBegin, kNamesEnd}};
    QList<QByteArray> kept;
    bool sawBlock = false, textAfterBlocks = false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QByteArray line = bare(lines.at(i));
        QByteArray end;
        for (const auto &marker : markers)
            if (line == marker.first)
                end = marker.second;
        qsizetype close = -1;
        if (!end.isEmpty())
            for (qsizetype j = i + 1; j < lines.size() && close < 0; ++j)
                if (bare(lines.at(j)) == end)
                    close = j;
        if (close < 0 || !takeBlock(line, lines.mid(i + 1, close - i - 1), &model)) {
            if (sawBlock && !line.trimmed().isEmpty())
                textAfterBlocks = true;
            kept << lines.at(i);
            continue;
        }
        sawBlock = true;
        // writeFilterModel puts one empty line before each block.
        if (!kept.isEmpty() && bare(kept.last()).isEmpty())
            kept.removeLast();
        i = close;
    }
    model.advanced = kept.join();
    // DriveSync writes its blocks after every other byte. Text after one
    // means a hand edit: then the blocks stay text, so a save changes no byte
    // and a "+" rule written after them keeps losing, as it does now.
    if (textAfterBlocks) {
        FilterModel whole;
        whole.advanced = filters;
        return whole;
    }
    return model;
}

QByteArray writeFilterModel(const FilterModel &model) {
    QByteArray out = model.advanced;
    auto add = [&](const QByteArray &begin, const QByteArray &end, const QByteArray &body) {
        if (body.isEmpty())
            return;
        if (!out.isEmpty() && !out.endsWith('\n'))
            out += '\n';
        if (!out.isEmpty())
            out += '\n';
        out += block(begin, end, body);
    };
    QByteArray presets;
    for (const FilterPreset &preset : filterPresets())
        if (model.presets.contains(preset.id))
            presets += kGroupPrefix + preset.id.toUtf8() + '\n' + preset.rules.join('\n').toUtf8() + '\n';
    add(kPresetsBegin, kPresetsEnd, presets);
    QByteArray paths;
    for (const QString &path : model.localPaths)
        if (const QString line = nameRuleLine({"path", path}); !line.isEmpty())
            paths += line.toUtf8() + '\n';
    add(kPathsBegin, kPathsEnd, paths);
    QByteArray names;
    for (const NameRule &rule : model.nameRules)
        if (const QString line = nameRuleLine(rule); !line.isEmpty())
            names += line.toUtf8() + '\n';
    add(kNamesBegin, kNamesEnd, names);
    return out;
}

FilterModel importedFilterModel(const QByteArray &filters) {
    FilterModel model = readFilterModel(filters);
    const QList<QByteArray> lines = linesWithEnds(model.advanced);
    QSet<QString> present;
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(bare(raw)).trimmed();
        if (line.startsWith('+') || line.startsWith('!'))
            return model;
        present.insert(line);
    }
    QSet<QString> taken;
    for (const FilterPreset &preset : filterPresets()) {
        if (model.presets.contains(preset.id))
            continue;
        bool all = true;
        for (const QString &rule : preset.rules)
            all = all && present.contains(rule);
        if (!all)
            continue;
        model.presets << preset.id;
        for (const QString &rule : preset.rules)
            taken.insert(rule);
    }
    // Back in the screen's order.
    QStringList ordered;
    for (const FilterPreset &preset : filterPresets())
        if (model.presets.contains(preset.id))
            ordered << preset.id;
    model.presets = ordered;
    QList<QByteArray> kept;
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(bare(raw)).trimmed();
        if (taken.contains(line))
            continue;
        if (const std::optional<NameRule> rule = ruleFromLine(line)) {
            if (rule->kind == "path") {
                if (!model.localPaths.contains(rule->value))
                    model.localPaths << rule->value;
            } else if (!model.nameRules.contains(*rule)) {
                model.nameRules << *rule;
            }
            continue;
        }
        kept << raw;
    }
    model.advanced = kept.join();
    return model;
}

QByteArray defaultFilters() {
    FilterModel model;
    model.presets = defaultPresetIds();
    // The header is the person's text: the groups follow it as DriveSync's
    // block, which the filters screen shows as ticked boxes.
    model.advanced = "# What DriveSync does not sync between your folder and Google Drive.\n"
                     "# rclone bisync filter rules: \"- pattern\" leaves it out. A rule with no \"/\"\n"
                     "# at the start applies in every folder. DriveSync's Settings edit this file\n"
                     "# on screen. Edited by hand instead? Open DriveSync and use Fix…, which\n"
                     "# rebuilds the base with the new rules: bisync needs it, and runs wait.\n";
    return writeFilterModel(model);
}
