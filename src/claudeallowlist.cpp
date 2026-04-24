#include "claudeallowlist.h"

#include "debuglog.h"
#include "secureio.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QRegularExpression>

// Valid top-level tool names recognized by Claude Code
static const QStringList s_validTools = {
    "Read", "Edit", "Write", "Glob", "Grep", "Bash",
    "WebFetch", "WebSearch", "Agent", "NotebookEdit",
    "Skill", "TaskCreate", "TaskUpdate", "TaskGet",
    "TaskList", "TaskOutput", "TaskStop",
    "ToolSearch", "Monitor", "EnterPlanMode", "ExitPlanMode",
    "EnterWorktree", "ExitWorktree",
    "CronCreate", "CronDelete", "CronList",
    "RemoteTrigger", "ScheduleWakeup",
};

static const QString s_sudoPrefix =
    QStringLiteral("SUDO_ASKPASS=/usr/libexec/ssh/ksshaskpass sudo -A ");

ClaudeAllowlistDialog::ClaudeAllowlistDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Claude Code - Edit Allowlist");
    setMinimumSize(550, 400);
    resize(650, 500);

    auto *mainLayout = new QVBoxLayout(this);

    // Info label
    auto *infoLabel = new QLabel(
        "Manage permission rules for Claude Code. "
        "Rules are saved to <code>.claude/settings.local.json</code> in the shell's working directory.",
        this);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: gray; font-size: 11px; margin-bottom: 4px;");
    mainLayout->addWidget(infoLabel);

    // Tabs for Allow / Deny / Ask
    m_tabs = new QTabWidget(this);

    m_allowList = new QListWidget(this);
    m_tabs->addTab(m_allowList, "Allow");

    m_denyList = new QListWidget(this);
    m_tabs->addTab(m_denyList, "Deny");

    m_askList = new QListWidget(this);
    m_tabs->addTab(m_askList, "Ask");

    mainLayout->addWidget(m_tabs, 1);

    // Rule input row
    auto *inputLayout = new QHBoxLayout();

    m_ruleInput = new QLineEdit(this);
    m_ruleInput->setPlaceholderText("e.g. Bash(git *), Read, Edit(src/**)");
    connect(m_ruleInput, &QLineEdit::returnPressed, this, &ClaudeAllowlistDialog::onAddRule);
    connect(m_ruleInput, &QLineEdit::textChanged, this, [this](const QString &text) {
        // Live preview of normalization
        if (text.trimmed().isEmpty()) {
            showValidationHint("");
            return;
        }
        QString normalized = normalizeRule(text.trimmed());
        if (normalized != text.trimmed()) {
            showValidationHint(QString("Will be corrected to: %1").arg(normalized));
        } else {
            // Check for subsumption
            QListWidget *list = currentList();
            QString subsumer = findSubsumingRule(list, normalized);
            if (!subsumer.isEmpty()) {
                showValidationHint(
                    QString("Already covered by: %1").arg(subsumer));
            } else {
                // Suggest generalization for specific Bash commands
                QString generalized = generalizeRule(normalized);
                if (!generalized.isEmpty() && generalized != normalized) {
                    showValidationHint(
                        QString("Tip: use %1 to cover all subcommands").arg(generalized));
                } else {
                    showValidationHint("");
                }
            }
        }
    });
    inputLayout->addWidget(m_ruleInput, 1);

    // Presets dropdown
    m_presetsCombo = new QComboBox(this);
    m_presetsCombo->addItem("Presets...");
    m_presetsCombo->addItem("Bash(git *)");
    m_presetsCombo->addItem("Bash(make *)");
    m_presetsCombo->addItem("Bash(cmake *)");
    m_presetsCombo->addItem("Bash(npm *)");
    m_presetsCombo->addItem("Bash(python3 *)");
    m_presetsCombo->addItem("Bash(pip *)");
    m_presetsCombo->addItem("Bash(cargo *)");
    m_presetsCombo->addItem("Bash(ls *)");
    m_presetsCombo->addItem("Bash(cat *)");
    m_presetsCombo->addItem("Bash(mkdir *)");
    m_presetsCombo->addItem("Bash(gh *)");
    m_presetsCombo->addItem("Read");
    m_presetsCombo->addItem("Edit");
    m_presetsCombo->addItem("Write");
    m_presetsCombo->addItem("Glob");
    m_presetsCombo->addItem("Grep");
    m_presetsCombo->addItem("WebFetch");
    m_presetsCombo->addItem("WebSearch");
    connect(m_presetsCombo, &QComboBox::activated, this, &ClaudeAllowlistDialog::onPresetSelected);
    inputLayout->addWidget(m_presetsCombo);

    mainLayout->addLayout(inputLayout);

    // Validation hint label (below input)
    m_validationLabel = new QLabel(this);
    m_validationLabel->setWordWrap(true);
    m_validationLabel->setStyleSheet("font-size: 11px; margin: 0; padding: 0;");
    m_validationLabel->hide();
    mainLayout->addWidget(m_validationLabel);

    // Add / Remove buttons
    auto *btnLayout = new QHBoxLayout();

    auto *addBtn = new QPushButton("Add Rule", this);
    connect(addBtn, &QPushButton::clicked, this, &ClaudeAllowlistDialog::onAddRule);
    btnLayout->addWidget(addBtn);

    auto *removeBtn = new QPushButton("Remove Selected", this);
    connect(removeBtn, &QPushButton::clicked, this, &ClaudeAllowlistDialog::onRemoveRule);
    btnLayout->addWidget(removeBtn);

    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // Dialog buttons
    auto *dialogBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
        this);
    connect(dialogBtns, &QDialogButtonBox::accepted, this, [this]() {
        if (!saveSettings()) {
            showValidationHint(
                QStringLiteral("Save failed — check permissions on %1").arg(m_settingsPath),
                /*isError=*/true);
            return;  // Don't accept() a failed save — user keeps the dialog open to retry
        }
        accept();
    });
    connect(dialogBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(dialogBtns->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &ClaudeAllowlistDialog::onApply);
    mainLayout->addWidget(dialogBtns);
}

void ClaudeAllowlistDialog::setSettingsPath(const QString &path) {
    m_settingsPath = path;
    loadSettings();
}

void ClaudeAllowlistDialog::prefillRule(const QString &rule) {
    m_tabs->setCurrentIndex(0); // Switch to Allow tab

    // Normalize and generalize the rule before prefilling
    QString normalized = normalizeRule(rule);
    QString generalized = generalizeRule(normalized);
    QString finalRule = generalized.isEmpty() ? normalized : generalized;

    m_ruleInput->setText(finalRule);
    m_ruleInput->selectAll();

    if (finalRule.isEmpty()) return;

    // Pre-check for duplicates / subsumption BEFORE auto-adding so the
    // user always sees feedback when the rule is already present.
    // Without the hint, prefill would silently drop the auto-add (Add
    // button became a no-op) and the user couldn't tell whether the
    // rule was saved or already existed. User spec 2026-04-18: "the
    // pop-up window should check for duplicate commands first before
    // adding any command."
    const int preCount = m_allowList->count();
    if (isDuplicate(m_allowList, finalRule)) {
        showValidationHint(
            QStringLiteral("Already in the Allow list: %1").arg(finalRule), false);
        return;
    }
    QString subsumer = findSubsumingRule(m_allowList, finalRule);
    if (!subsumer.isEmpty()) {
        showValidationHint(
            QStringLiteral("Already covered by existing rule: %1").arg(subsumer),
            false);
        return;
    }
    addRuleWithCompanion(m_allowList, finalRule);
    if (m_allowList->count() > preCount) {
        showValidationHint(
            QStringLiteral("Added to Allow list. Click Save to persist."),
            false);
    }
}

void ClaudeAllowlistDialog::loadSettings() {
    m_allowList->clear();
    m_denyList->clear();
    m_askList->clear();

    if (m_settingsPath.isEmpty()) return;

    QFile file(m_settingsPath);
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    QJsonObject perms = doc.object().value("permissions").toObject();

    auto loadList = [](QListWidget *list, const QJsonArray &arr) {
        for (const QJsonValue &v : arr) {
            if (v.isString())
                list->addItem(v.toString());
        }
    };

    loadList(m_allowList, perms.value("allow").toArray());
    loadList(m_denyList, perms.value("deny").toArray());
    loadList(m_askList, perms.value("ask").toArray());
}

bool ClaudeAllowlistDialog::saveSettings() {
    if (m_settingsPath.isEmpty()) return false;

    // Read existing file to preserve non-permission keys (model, editor
    // prefs, custom hooks, ...). If the file EXISTS but fails to parse,
    // refuse to save — otherwise the write below would produce a file
    // containing only {"permissions": {...}} and silently destroy every
    // other key the user had. Rotate the corrupt file aside first so
    // the user can hand-recover. 0.7.12 /indie-review fix — the same
    // silent-data-loss pattern Config::load had.
    QJsonObject root;
    {
        QFile file(m_settingsPath);
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly)) {
                ANTS_LOG(DebugLog::Claude,
                         "allowlist saveSettings: %s exists but cannot be "
                         "read — refusing to save (would risk clobbering "
                         "non-permission keys)",
                         qUtf8Printable(m_settingsPath));
                return false;
            }
            const QByteArray raw = file.readAll();
            file.close();
            QJsonParseError err{};
            QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
            if (doc.isObject()) {
                root = doc.object();
            } else {
                const QString backup = rotateCorruptFileAside(m_settingsPath);
                ANTS_LOG(DebugLog::Claude,
                         "allowlist saveSettings: %s failed to parse at "
                         "offset %d: %s — rotated to %s, refusing to save",
                         qUtf8Printable(m_settingsPath),
                         err.offset,
                         qUtf8Printable(err.errorString()),
                         qUtf8Printable(backup.isEmpty()
                                        ? QStringLiteral("(backup FAILED)")
                                        : backup));
                return false;
            }
        }
        // else: fresh install, no settings file yet — root stays empty
        // object, save proceeds and writes the file for the first time.
    }

    // Build permissions object (skip empty arrays to keep file clean)
    auto listToArray = [](QListWidget *list) {
        QJsonArray arr;
        for (int i = 0; i < list->count(); ++i)
            arr.append(list->item(i)->text());
        return arr;
    };

    QJsonObject perms;
    QJsonArray allow = listToArray(m_allowList);
    QJsonArray deny = listToArray(m_denyList);
    QJsonArray ask = listToArray(m_askList);

    if (!allow.isEmpty()) perms["allow"] = allow;
    if (!deny.isEmpty()) perms["deny"] = deny;
    if (!ask.isEmpty()) perms["ask"] = ask;
    root["permissions"] = perms;

    // Create .claude directory if needed
    if (!QDir().mkpath(QFileInfo(m_settingsPath).absolutePath()))
        return false;

    // Atomic write with 0600 permissions.
    //
    // Belt-and-suspenders perms: setOwnerOnlyPerms(file) sets 0600 on
    // the QSaveFile temp fd, and setOwnerOnlyPerms(m_settingsPath) sets
    // it again on the final path after commit. Most local filesystems
    // carry fd permissions across rename, but FAT/exFAT on removable
    // media, some SMB/NFS mounts, and edge cases where Qt falls back to
    // copy+unlink instead of rename do not. This file can hold API keys
    // (Claude Code bearer tokens in ~/.claude/settings.json); a
    // world-readable final file on those filesystems would leak them
    // to any UID on the host. Post-commit chmod closes the gap.
    QSaveFile file(m_settingsPath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    setOwnerOnlyPerms(file);
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) return false;
    setOwnerOnlyPerms(m_settingsPath);
    return true;
}

QListWidget *ClaudeAllowlistDialog::currentList() const {
    switch (m_tabs->currentIndex()) {
    case 0: return m_allowList;
    case 1: return m_denyList;
    case 2: return m_askList;
    default: return m_allowList;
    }
}

// --- Rule validation and correction ---

QString ClaudeAllowlistDialog::normalizeRule(const QString &raw) {
    QString rule = raw.trimmed();
    if (rule.isEmpty()) return rule;

    // Fix Bash(cmd:*) → Bash(cmd *) (colon format is invalid)
    static QRegularExpression colonGlob(R"(^Bash\((.+?):(\*)\)$)");
    auto m = colonGlob.match(rule);
    if (m.hasMatch()) {
        rule = QStringLiteral("Bash(") + m.captured(1) + " " + m.captured(2) + ")";
    }

    // Fix Bash(cmd: *) → Bash(cmd *)
    static QRegularExpression colonSpaceGlob(R"(^Bash\((.+?):\s+(\*)\)$)");
    m = colonSpaceGlob.match(rule);
    if (m.hasMatch()) {
        rule = QStringLiteral("Bash(") + m.captured(1) + " " + m.captured(2) + ")";
    }

    // Convert bare sudo to SUDO_ASKPASS form
    // Matches: Bash(sudo cmd ...) or Bash(sudo -X cmd ...)
    static QRegularExpression bareSudo(R"(^Bash\(sudo\s+(.+)\)$)");
    m = bareSudo.match(rule);
    if (m.hasMatch() && !rule.contains("SUDO_ASKPASS")) {
        QString rest = m.captured(1);
        // Strip any existing -A flag to avoid doubling
        rest.remove(QRegularExpression(R"(^-A\s+)"));
        rule = QStringLiteral("Bash(") + s_sudoPrefix + rest + ")";
    }

    // Bash commands without wildcard: Bash(git) → Bash(git *)
    // Only for commands that would typically take arguments
    static QRegularExpression bashNoWild(R"(^Bash\(([a-zA-Z0-9_./-]+)\)$)");
    m = bashNoWild.match(rule);
    if (m.hasMatch()) {
        QString cmd = m.captured(1);
        // Single-word commands that are typically used with args
        if (cmd != "make" && cmd != "env") {
            rule = QStringLiteral("Bash(") + cmd + " *)";
        }
    }

    // WebFetch domain without prefix: WebFetch(example.com) → WebFetch(domain:example.com)
    static QRegularExpression webNoDomain(R"(^WebFetch\(([a-zA-Z0-9][\w.-]+\.\w+)\)$)");
    m = webNoDomain.match(rule);
    if (m.hasMatch()) {
        rule = QStringLiteral("WebFetch(domain:") + m.captured(1) + ")";
    }

    // Read/Edit/Write with absolute path missing // prefix: Read(/etc/foo) → Read(//etc/foo)
    static QRegularExpression absPath(R"(^(Read|Edit|Write)\(/([^/].*)\)$)");
    m = absPath.match(rule);
    if (m.hasMatch()) {
        rule = m.captured(1) + "(//" + m.captured(2) + ")";
    }

    // Validate tool name exists (for rules with parenthesized args)
    static QRegularExpression toolParen(R"(^(\w+)\(.+\)$)");
    m = toolParen.match(rule);
    if (m.hasMatch()) {
        QString tool = m.captured(1);
        // Allow mcp__ prefixed tools and known tools
        if (!tool.startsWith("mcp__") && !s_validTools.contains(tool)) {
            // Unknown tool — pass through but it may not work
        }
    }

    return rule;
}

QString ClaudeAllowlistDialog::generalizeRule(const QString &rule) {
    // Already a wildcard pattern — nothing useful to do.
    if (rule.endsWith(" *)") || rule.endsWith(" **)"))
        return {};

    // Extract Bash(...) content. Non-Bash rules (Read/Write/Edit/WebFetch/
    // etc.) have no shell-operator generalisation path.
    static QRegularExpression bashWrap(R"(^Bash\((.*)\)$)");
    auto bm = bashWrap.match(rule);
    if (!bm.hasMatch()) return {};
    const QString content = bm.captured(1);

    // Split on shell operators (&& || ; |), preserving them.
    //
    // Regex rule: the operator must be followed by whitespace (\s+), but
    // the preceding-whitespace is optional (\s*). This catches the common
    // idiomatic form `cmd1; cmd2` (no space before ;, one after) without
    // false-positively splitting quoted operators like `grep 'a|b'` (no
    // whitespace after the |) — the `\s+` trailing requirement keeps
    // quoted/embedded operators safe. Trade-off: rare form `cmd1 ;cmd2`
    // (space before, none after) also won't split, but it's uncommon
    // enough in permission-prompt scrapes that the safety win is worth
    // it.
    //
    // Order matters within the alternation: `||` and `&&` must come
    // before the bare `|` (otherwise `|` captures half of `||`).
    static QRegularExpression opRe(R"(\s*(&&|\|\||;|\|)\s+)");

    QStringList segments;
    QStringList operators;
    qsizetype pos = 0;
    auto it = opRe.globalMatch(content);
    while (it.hasNext()) {
        auto m = it.next();
        segments << content.mid(pos, m.capturedStart() - pos);
        operators << m.captured(1);
        pos = m.capturedEnd();
    }
    segments << content.mid(pos);

    // Safety denylist — any segment that is rm, bare sudo, or a
    // SUDO_* variable assignment (other than SUDO_ASKPASS — that's the
    // project-standard form handled separately below) means we refuse to
    // generalise. Over-broad rm/sudo rules are the kind of thing we
    // don't want the button offering with a single click.
    for (const QString &seg : std::as_const(segments)) {
        const QString s = seg.trimmed();
        if (s == "rm" || s.startsWith("rm ")) return {};
        if (s == "sudo" || s.startsWith("sudo ")) return {};
        if (s.startsWith("SUDO_") && !s.startsWith("SUDO_ASKPASS=")) return {};
    }

    // Project-standard SUDO_ASKPASS form — a single-segment rule of
    // shape `SUDO_ASKPASS=… sudo -A <cmd> <args>` generalises to the
    // same prefix with wildcarded args on the specific subcommand.
    if (segments.size() == 1) {
        static QRegularExpression sudoSpecific(
            R"(^SUDO_ASKPASS=\S+\s+sudo\s+-A\s+(\S+)\s+.+$)");
        auto m = sudoSpecific.match(content);
        if (m.hasMatch()) {
            const QString cmd = m.captured(1);
            return QStringLiteral("Bash(") + s_sudoPrefix + cmd + " *)";
        }
    }

    // Generalise each segment independently: base command + " *" if the
    // segment carries args, otherwise keep the bare command. This is
    // how the user's own settings.local.json encodes compound allowlist
    // entries (e.g. `Bash(cd * && git *)`, `Bash(cat * | *)`).
    static QRegularExpression segCmd(R"(^([A-Za-z_][\w./-]*)(\s+.+)?$)");
    QStringList genSegments;
    for (const QString &seg : std::as_const(segments)) {
        const QString s = seg.trimmed();
        auto m = segCmd.match(s);
        if (!m.hasMatch()) {
            // Can't parse a leading command — leave as-is so a partial
            // generalisation is still offered rather than returning
            // empty for the whole rule.
            genSegments << s;
            continue;
        }
        const QString base = m.captured(1);
        const bool hasArgs = m.capturedLength(2) > 0;
        genSegments << (hasArgs ? base + " *" : base);
    }

    // Reassemble with original operators, space-padded to match the
    // format the user's settings.local.json uses throughout.
    QString result = genSegments.value(0);
    for (int i = 0; i < operators.size(); ++i) {
        result += " " + operators[i] + " " + genSegments.value(i + 1);
    }

    // If nothing actually changed (e.g. single-segment Bash(make) with
    // no args), there's no useful generalisation to offer.
    if (result == content) return {};

    return QStringLiteral("Bash(") + result + ")";
}

bool ClaudeAllowlistDialog::ruleSubsumes(const QString &broad, const QString &narrow) {
    if (broad == narrow) return true;

    // Exact tool match (bare tool covers all uses): "Read" subsumes "Read(path)"
    if (!broad.contains('(') && narrow.startsWith(broad + "("))
        return true;

    // Bash glob: Bash(git *) subsumes Bash(git status *).
    //
    // Claude Code's allowlist matcher evaluates compound commands
    // (chained with &&, ||, ;, |) segment-by-segment — a simple
    // "Bash(cmd *)" rule therefore covers a compound narrow rule only
    // when *every* segment starts with "cmd ". A flat prefix check
    // (the pre-0.6.23 behavior) would wrongly claim "Bash(make *)"
    // subsumes "Bash(make x | tail y && ctest z | tail w)" because the
    // whole string starts with "make "; the dialog would then block the
    // user from adding the compound rule Claude Code actually needs and
    // keep re-prompting on every invocation.
    //
    // generalizeRule() already splits on the same splitter set when
    // producing compound rules; subsumption must use matching semantics.
    static const QRegularExpression bashGlob(R"(^Bash\((.+?)(\s+\*)\)$)");
    auto bm = bashGlob.match(broad);

    if (bm.hasMatch()) {
        QString broadPrefix = bm.captured(1);

        static const QRegularExpression bashInner(R"(^Bash\((.+)\)$)");
        auto ni = bashInner.match(narrow);
        if (!ni.hasMatch()) return false;

        const QString narrowInner = ni.captured(1);

        // Same splitter set as generalizeRule(): &&, ||, ;, |.
        static const QRegularExpression splitter(R"(\s*(?:&&|\|\||;|\|)\s*)");

        // A compound broadPrefix would need segment-wise matching against
        // the narrow in both directions, which the current matcher
        // semantics don't model cleanly. Return false rather than risk a
        // false positive — duplicate compound rules in the Allow list are
        // benign; a false-positive "already covered" warning blocks a
        // rule the user actually needs.
        if (broadPrefix.contains(splitter)) return false;

        const QStringList segments =
            narrowInner.split(splitter, Qt::SkipEmptyParts);
        if (segments.isEmpty()) return false;

        for (const QString &segRaw : segments) {
            const QString seg = segRaw.trimmed();
            if (!(seg.startsWith(broadPrefix + " ") || seg == broadPrefix))
                return false;
        }
        return true;
    }

    // SUDO_ASKPASS: Bash(SUDO...sudo -A zypper *) subsumes Bash(SUDO...sudo -A zypper install *)
    // This is handled by the Bash glob logic above

    // Read/Edit/Write path globs: Read(//etc/**) subsumes Read(//etc/hosts)
    static QRegularExpression pathGlob(R"(^(Read|Edit|Write)\((.+?)(\*\*)\)$)");
    auto pm = pathGlob.match(broad);
    if (pm.hasMatch()) {
        QString tool = pm.captured(1);
        QString pathPrefix = pm.captured(2);
        static QRegularExpression pathInner(R"(^(Read|Edit|Write)\((.+)\)$)");
        auto pi = pathInner.match(narrow);
        if (pi.hasMatch() && pi.captured(1) == tool) {
            if (pi.captured(2).startsWith(pathPrefix))
                return true;
        }
    }

    return false;
}

bool ClaudeAllowlistDialog::isDuplicate(QListWidget *list, const QString &rule) const {
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text() == rule)
            return true;
    }
    return false;
}

QString ClaudeAllowlistDialog::findSubsumingRule(QListWidget *list, const QString &rule) const {
    for (int i = 0; i < list->count(); ++i) {
        QString existing = list->item(i)->text();
        if (existing != rule && ruleSubsumes(existing, rule))
            return existing;
    }
    return {};
}

void ClaudeAllowlistDialog::showValidationHint(const QString &msg, bool isError) {
    if (msg.isEmpty()) {
        m_validationLabel->hide();
        return;
    }
    m_validationLabel->setText(msg);
    m_validationLabel->setStyleSheet(
        isError ? "color: #F38BA8; font-size: 11px; margin: 0; padding: 0;"
                : "color: #A6ADC8; font-size: 11px; margin: 0; padding: 0;");
    m_validationLabel->show();
}

void ClaudeAllowlistDialog::addRuleWithCompanion(QListWidget *list, const QString &rule) {
    // Add the primary rule if not already covered
    bool primaryAdded = false;
    if (!isDuplicate(list, rule) && findSubsumingRule(list, rule).isEmpty()) {
        // Remove narrower rules that this new rule would subsume
        for (int i = list->count() - 1; i >= 0; --i) {
            if (ruleSubsumes(rule, list->item(i)->text()))
                delete list->item(i);
        }
        list->addItem(rule);
        primaryAdded = true;
    }

    // For Bash(cmd *) rules, also add Bash(cd * && cmd *) companion
    // Claude Code often uses "cd path && cmd args" form
    static QRegularExpression bashWild(R"(^Bash\(([a-zA-Z0-9_./-]+) \*\)$)");
    auto m = bashWild.match(rule);
    if (m.hasMatch()) {
        QString cmd = m.captured(1);
        // Skip dangerous commands and cd itself
        if (cmd != "cd" && cmd != "rm" && cmd != "sudo") {
            QString companion = QStringLiteral("Bash(cd * && ") + cmd + " *)";
            if (!isDuplicate(list, companion) && findSubsumingRule(list, companion).isEmpty()) {
                list->addItem(companion);
            }
        }
    }

    if (primaryAdded)
        list->scrollToBottom();
}

void ClaudeAllowlistDialog::onAddRule() {
    QString raw = m_ruleInput->text().trimmed();
    if (raw.isEmpty()) return;

    QString rule = normalizeRule(raw);

    // Validate: must be a known tool name or tool(args) pattern
    static QRegularExpression validFormat(R"(^(\w+)(\(.+\))?$)");
    // Also allow mcp__* tools
    static QRegularExpression mcpFormat(R"(^mcp__\w+__\w+$)");
    if (!validFormat.match(rule).hasMatch() && !mcpFormat.match(rule).hasMatch()) {
        showValidationHint("Invalid rule format. Use: ToolName or ToolName(pattern)", true);
        return;
    }

    QListWidget *list = currentList();

    // Check exact duplicate
    if (isDuplicate(list, rule)) {
        showValidationHint("Rule already exists in the list.", true);
        return;
    }

    // Check if already covered by a broader rule
    QString subsumer = findSubsumingRule(list, rule);
    if (!subsumer.isEmpty()) {
        showValidationHint(
            QString("Already covered by existing rule: %1").arg(subsumer), true);
        return;
    }

    addRuleWithCompanion(list, rule);
    m_ruleInput->clear();
    showValidationHint("");
}

void ClaudeAllowlistDialog::onRemoveRule() {
    QListWidget *list = currentList();
    auto *item = list->currentItem();
    if (item) delete item;
}

void ClaudeAllowlistDialog::onPresetSelected(int index) {
    if (index <= 0) return; // Skip "Presets..." header
    m_ruleInput->setText(m_presetsCombo->currentText());
    m_presetsCombo->setCurrentIndex(0);
}

void ClaudeAllowlistDialog::onApply() {
    if (saveSettings()) {
        showValidationHint(QStringLiteral("Saved."), /*isError=*/false);
    } else {
        showValidationHint(
            QStringLiteral("Save failed — check permissions on %1").arg(m_settingsPath),
            /*isError=*/true);
    }
}
