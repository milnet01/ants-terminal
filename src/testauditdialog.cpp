// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testauditdialog.h"

#include "briefdispatch.h"
#include "config.h"
#include "secureio.h"
#include "sessionmemoryengine.h"

#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString systemPrompt(const QStringList &dims) {
    return QStringLiteral(
               "You are a test-suite auditor. Review the inlined test files "
               "(verbatim — you have no Read tool) across these dimensions: "
               "%1. Report each issue on its own line as "
               "`- [SEV] file:line — description` (SEV ∈ "
               "CRITICAL/HIGH/MED/LOW), grouped under `## <Dimension>` "
               "section headers so a histogram parser can read them. Do not "
               "invent file:line citations.")
        .arg(dims.join(QStringLiteral(", ")));
}

QString jsonArrayBlock(const QString &title, const QJsonArray &arr) {
    if (arr.isEmpty()) return {};
    QString s = QStringLiteral("\n## ") + title + QStringLiteral("\n\n");
    for (const QJsonValue &v : arr) {
        s += QStringLiteral("- ")
             + QString::fromUtf8(QJsonDocument(v.toObject()).toJson(
                   QJsonDocument::Compact))
             + QChar('\n');
    }
    return s;
}

// ANTS-1836 — atomic, owner-only report write (was a plain QFile with a
// discarded write() return: a torn report on disk-full, no 0600, silent
// failure). QSaveFile gives temp+rename atomicity; the verbatim model
// reports may quote source the user keeps private, so 0600 like the
// audit-cache pattern. Returns false on any failure so the caller can tell.
bool writeReport(const QString &path, const QString &body) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray bytes = body.toUtf8();
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) return false;
    setOwnerOnlyPerms(path);
    return true;
}

}  // namespace

TestAuditDialog::TestAuditDialog(QString projectCwd, QWidget *parent,
                                Config *config)
    : ReviewDialogBase(std::move(projectCwd), parent, config) {
    setWindowTitle(tr("Test-suite Audit"));
    buildControls();
    runPartition();   // derivePartition() → setLanes()
}

void TestAuditDialog::buildControls() {
    auto *panel = new QWidget(this);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);

    lay->addWidget(new QLabel(tr("Dimensions (CSV; empty = auto):"), panel));
    m_dimsEdit = new QLineEdit(panel);
    m_dimsEdit->setPlaceholderText(tr("flakiness,determinism,isolation,…"));
    connect(m_dimsEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_dimsEdit) setDimensionsCsv(m_dimsEdit->text().trimmed());
    });
    lay->addWidget(m_dimsEdit);

    m_resultsView = new QPlainTextEdit(panel);
    m_resultsView->setReadOnly(true);
    m_resultsView->setPlaceholderText(
        tr("Synthesis summary appears here after dispatch."));
    lay->addWidget(m_resultsView, 1);

    auto *foldBox = new QGroupBox(tr("Fold-in mode"), panel);
    auto *foldLay = new QVBoxLayout(foldBox);
    auto *narrativeRadio =
        new QRadioButton(tr("Narrative (prose, no IDs)"), foldBox);
    m_perFindingRadio =
        new QRadioButton(tr("Per-finding (allocate ROADMAP IDs)"), foldBox);
    narrativeRadio->setChecked(true);
    m_foldInMode = FoldInMode::Narrative;
    connect(m_perFindingRadio, &QRadioButton::toggled, this, [this](bool on) {
        setFoldInMode(on ? FoldInMode::PerFinding : FoldInMode::Narrative);
    });
    foldLay->addWidget(narrativeRadio);
    foldLay->addWidget(m_perFindingRadio);
    m_narrativeEdit = new QPlainTextEdit(foldBox);
    m_narrativeEdit->setPlaceholderText(
        tr("Narrative fold-in text (Closed-inline / Deferred / "
           "False-positives)…"));
    connect(m_narrativeEdit, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_narrativeEdit) setNarrativeText(m_narrativeEdit->toPlainText());
    });
    // Narrative is the default mode here, so the editor starts visible; hide
    // it when the user switches to Per-finding (mirrors cold-eyes).
    m_narrativeEdit->setVisible(narrativeRadio->isChecked());
    connect(narrativeRadio, &QRadioButton::toggled, m_narrativeEdit,
            &QWidget::setVisible);
    foldLay->addWidget(m_narrativeEdit);
    lay->addWidget(foldBox);

    setResultsWidget(panel);
}

QString TestAuditDialog::reportsRelDir() const {
    return QStringLiteral(".audit_cache/test_audit_") + m_token;
}

void TestAuditDialog::setDimensionsCsv(const QString &csv) {
    m_dimensionsCsv = csv.isEmpty() ? QString()
                                    : QStringLiteral("csv:") + csv;
    runPartition();
}

void TestAuditDialog::runEnginePartition() {
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd  = projectCwd();
    preq.scope      = QStringLiteral("auto");
    preq.dimensions = m_dimensionsCsv.isEmpty() ? QStringLiteral("auto")
                                                : m_dimensionsCsv;
    preq.chunkSize  = 12;
    const auto pr = TestAuditEngine::partition(preq);
    if (!pr.ok) {
        if (statusLabel())
            statusLabel()->setText(tr("Partition failed: %1").arg(pr.error));
        m_chunks.clear();
        m_token.clear();
        m_dimensionsActive.clear();
        m_framework.clear();
        rebuildPanel();
        return;
    }
    m_token            = pr.partitionToken;
    m_chunks           = pr.chunks;
    m_dimensionsActive = pr.dimensionsActive;
    m_framework        = pr.framework;
    if (statusLabel())
        statusLabel()->setText(tr("Framework: %1 · %2 chunks · %3 dimensions")
                                   .arg(m_framework)
                                   .arg(m_chunks.size())
                                   .arg(m_dimensionsActive.size()));
    rebuildPanel();
}

QList<ReviewLane> TestAuditDialog::derivePartition() {
    runEnginePartition();
    QList<ReviewLane> out;
    out.reserve(m_chunks.size());
    for (const TestAuditEngine::Chunk &c : m_chunks)
        out << ReviewLane{ c.id, c.id, c.paths.join(QChar('\n')) };
    return out;
}

void TestAuditDialog::prepareDispatch() {
    // ANTS-1843 — re-derive the partition once up-front. runPartition()
    // refreshes m_token + m_chunks (repopulating the in-process engine
    // cache after a restart) AND rebuilds the lane set via setLanes(), so
    // the base startDispatch loop enqueues lanes that match the live token.
    // This pre-empts briefFor()'s own stale_partition recovery, which kept
    // firing mid-loop and could diverge the token/lanes from enqueued jobs.
    // briefFor() retains that recovery for direct callers (INV-6).
    runPartition();
}

TestAuditEngine::BriefResult TestAuditDialog::briefFor(const QString &chunkId) {
    TestAuditEngine::BriefRequest br;
    br.callerCwd      = projectCwd();
    br.chunkId        = chunkId;
    br.partitionToken = m_token;
    auto b = TestAuditEngine::brief(br);
    if (!b.ok && b.code == QStringLiteral("stale_partition")) {
        // Post-restart in-process cache miss. Spec ANTS-1722 calls this
        // code "stale_token"; the engine returns "stale_partition". Re-run
        // partition for a fresh token (deterministic qHash → same token for
        // the same file set), then retry once.
        runEnginePartition();
        br.partitionToken = m_token;
        b = TestAuditEngine::brief(br);
    }
    return b;
}

QString TestAuditDialog::assembleCappedPrompt(
    const TestAuditEngine::BriefResult &b) const {
    QString fixed;
    fixed += QStringLiteral("# Test-audit chunk review — ") + b.chunkId
             + QStringLiteral("\n\n## Dimensions to audit (all active)\n\n");
    for (const QString &d : m_dimensionsActive)
        fixed += QStringLiteral("- ") + d + QChar('\n');
    fixed += QStringLiteral("\n## Framework context\n\n")
             + QString::fromUtf8(
                   QJsonDocument(b.frameworkContext).toJson(QJsonDocument::Compact))
             + QChar('\n');
    fixed += jsonArrayBlock(tr("Prior false positives (do not re-raise)"),
                            b.priorFalsePositives);

    const QString prePass =
        jsonArrayBlock(tr("Pre-pass grep hits (prioritisation only)"),
                       b.prePassFindings);
    // TestAuditEngine returns absolute chunk paths; BriefDispatch.inlineBodies
    // tolerates absolute-under-root paths directly and relativises the fence
    // header itself (ANTS-1731), so no prefix-stripping is needed here.
    const QString bulk =
        QStringLiteral("\n## Test files (verbatim)\n\n")
        + BriefDispatch::inlineBodies(projectCwd(), b.sourcePaths,
                                      kPerFileCapBytes);

    const qint64 fixedBytes = fixed.toUtf8().size();
    const qint64 bulkBytes  = bulk.toUtf8().size();
    if (fixedBytes + static_cast<qint64>(prePass.toUtf8().size()) + bulkBytes
        <= kPromptCapBytes)
        return fixed + prePass + bulk;

    // Over budget: pre-pass context is the lowest priority — drop it first.
    const QString dropMarker = QStringLiteral(
        "\n[brief truncated — pre-pass context omitted to fit the 200 KiB "
        "budget]\n");
    if (fixedBytes + static_cast<qint64>(dropMarker.toUtf8().size()) + bulkBytes
        <= kPromptCapBytes)
        return fixed + dropMarker + bulk;

    // Still over: the test bodies alone exceed the cap — clip them.
    const QString hardMarker = QStringLiteral(
        "\n[brief truncated — test bodies clipped to fit the 200 KiB "
        "budget]\n");
    QByteArray utf = (fixed + dropMarker + bulk).toUtf8();
    const int room =
        static_cast<int>(kPromptCapBytes) - hardMarker.toUtf8().size();
    if (room > 0 && utf.size() > room) utf.truncate(room);
    return QString::fromUtf8(utf) + hardMarker;
}

LlmRequest TestAuditDialog::composeBrief(const ReviewLane &laneRef) {
    LlmRequest req;
    if (config()) {
        req.endpoint = config()->aiEndpoint();
        req.apiKey   = config()->aiApiKey();
        req.model    = config()->aiModel();
    }
    req.maxTokens    = 2048;
    req.systemPrompt = systemPrompt(m_dimensionsActive);

    const auto b = briefFor(laneRef.id);
    if (!b.ok) {
        req.userPrompt = QStringLiteral("[test-audit: brief failed (%1)]")
                             .arg(b.code);
        return req;
    }
    req.userPrompt = assembleCappedPrompt(b);
    return req;
}

void TestAuditDialog::onAllReportsCollected(
    const QHash<QString, QString> &reportsById) {
    for (auto it = reportsById.constBegin(); it != reportsById.constEnd(); ++it)
        m_collectedReports.insert(it.key(), it.value());

    // Write each verbatim report under .audit_cache/test_audit_<token>/.
    const QString absDir = projectCwd() + QChar('/') + reportsRelDir();
    QDir().mkpath(absDir);
    for (auto it = reportsById.constBegin(); it != reportsById.constEnd(); ++it)
        writeReport(absDir + QChar('/') + it.key() + QStringLiteral(".md"),
                    it.value());

    TestAuditEngine::SynthRequest sreq;
    sreq.callerCwd      = projectCwd();
    sreq.partitionToken = m_token;
    sreq.reportsDir     = reportsRelDir();   // project-relative, inside root
    sreq.mode           = QStringLiteral("summary");
    m_lastSynth = TestAuditEngine::synthesize(sreq);
    renderResults();
    persistResumeState();

    // Single synthesis follow-up to the model (does NOT re-enter this hook).
    if (m_lastSynth.ok && config()
        && endpointDispatchable(config()->aiEndpoint())) {
        LlmJob job;
        job.id = QStringLiteral("synthesis");
        job.request.endpoint     = config()->aiEndpoint();
        job.request.apiKey       = config()->aiApiKey();
        job.request.model        = config()->aiModel();
        job.request.maxTokens    = 2048;
        job.request.systemPrompt = QStringLiteral(
            "You are a test-audit synthesiser. Write a concise cross-chunk "
            "summary grouped by dimension and severity from the per-chunk "
            "reports below.");
        job.request.userPrompt   = m_lastSynth.prompt;
        dispatchOne(job, [this](const LlmResult &r) {
            m_synthText = r.text;
            renderResults();
        });
    }
}

void TestAuditDialog::performFoldIn() {
    TestAuditEngine::FoldInRequest fr;
    fr.callerCwd  = projectCwd();
    fr.framework  = m_framework;
    fr.dimensions = m_dimensionsActive;
    int files = 0;
    for (const auto &c : m_chunks) files += c.paths.size();
    fr.filesScanned = files;

    if (m_foldInMode == FoldInMode::Narrative) {
        fr.narrativeMode = true;
        fr.narrativeMd   = m_narrative;
    } else {
        fr.actionable   = m_actionable;
        fr.rawFindings  = m_actionable.size();
    }
    // The engine owns ID allocation + insert (testauditengine.h:21) — do
    // NOT call the base allocateFoldInIds / insertFoldInBlock helpers here
    // or the IDs would be double-allocated.
    const auto res = TestAuditEngine::foldIn(fr);
    if (!res.ok && statusLabel())
        statusLabel()->setText(tr("Fold-in failed (%1): %2")
                                   .arg(res.code, res.error));
}

void TestAuditDialog::persistResumeState() {
    if (m_token.isEmpty()) return;
    QJsonObject o;
    o[QStringLiteral("partition_token")] = m_token;
    o[QStringLiteral("dimensions")] =
        QJsonArray::fromStringList(m_dimensionsActive);
    o[QStringLiteral("collected_chunk_ids")] =
        QJsonArray::fromStringList(m_collectedReports.keys());
    SessionMemoryEngine::execute(projectCwd(), SessionMemoryEngine::Op::Set,
                                 QStringLiteral("test_audit_resume"), o,
                                 m_sessionMemBaseDir);
}

bool TestAuditDialog::loadResume() {
    const auto r = SessionMemoryEngine::execute(
        projectCwd(), SessionMemoryEngine::Op::Get,
        QStringLiteral("test_audit_resume"), QJsonValue(), m_sessionMemBaseDir);
    if (!r.ok || !r.found) return false;
    const QJsonObject o = r.value.toObject();
    m_persistedToken = o.value(QStringLiteral("partition_token")).toString();
    m_persistedCollected.clear();
    const QJsonArray ids =
        o.value(QStringLiteral("collected_chunk_ids")).toArray();
    for (const QJsonValue &v : ids) m_persistedCollected.insert(v.toString());
    return true;
}

QStringList TestAuditDialog::unreviewedChunkIds() const {
    QStringList out;
    for (const TestAuditEngine::Chunk &c : m_chunks)
        if (!m_persistedCollected.contains(c.id)) out << c.id;
    return out;
}

void TestAuditDialog::rebuildPanel() {
    if (m_dimsEdit && m_dimsEdit->text().trimmed().isEmpty()
        && !m_dimensionsActive.isEmpty()) {
        // Show the resolved auto dimension set as a hint (non-destructive:
        // an empty field still means "auto" on the next partition).
        m_dimsEdit->setToolTip(tr("Active: %1")
                                   .arg(m_dimensionsActive.join(
                                       QStringLiteral(", "))));
    }
}

void TestAuditDialog::renderResults() {
    if (!m_resultsView) return;
    QString s;
    if (m_lastSynth.ok) {
        s += tr("Synthesis — %1 reports read.\n\n")
                 .arg(m_lastSynth.reportsRead);
        s += m_lastSynth.prompt;
    } else if (!m_lastSynth.code.isEmpty()) {
        s += tr("Synthesis failed (%1): %2")
                 .arg(m_lastSynth.code, m_lastSynth.error);
    }
    if (!m_synthText.isEmpty())
        s += QStringLiteral("\n\n=== Model summary ===\n\n") + m_synthText;
    m_resultsView->setPlainText(s);
}
