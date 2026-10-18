#include "ClipView.h"
#include "OverviewBar.h"
#include <QCursor>
#include "PianoRoll.h"
#include "SampleEditor.h"
#include "TakesEditor.h"
#include "EnvelopeEditor.h"
#include "InfoView.h"
#include "core/Model.h"
#include "core/Scales.h"
#include "core/Theme.h"
#include "widgets/DragValue.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QStackedWidget>
#include <QSignalBlocker>
#include <QSettings>
#include <QPainter>
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {

constexpr int kBoxWidth = 132;   // the clip boxes are narrow: small fields, short labels

const double kQuantBeats[] = {-1, 0, 16, 8, 4, 2, 1, 0.5, 0.25};
const char*  kQuantNames[] = {"Global", "None", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16"};
const char*  kFollowNames[] = {"No Action", "Stop", "Again", "Previous", "Next", "First", "Last", "Any", "Other"};
const double kGrains[] = {0, 1, 0.5, 0.25, 0.125};   // Beats mode slices: transients, then grids
const char*  kGrainNames[] = {"Transients", "1/4", "1/8", "1/16", "1/32"};

QLabel* dimLabel(const QString& t, QWidget* parent) { auto* l = new QLabel(t, parent); l->setObjectName("dimLabel"); return l; }

// A label that draws its text elided to its width (text() stays whole; the whole text is its tooltip).
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return QSize(24, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setPen(palette().color(foregroundRole()));
        p.setFont(font());
        p.drawText(contentsRect(), int(alignment()) | Qt::TextSingleLine, fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

QPushButton* toolButton(const QString& text, const QString& title, const QString& help, QWidget* parent, bool checkable = false) {
    auto* b = new QPushButton(text, parent);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCheckable(checkable);
    InfoView::attach(b, title, help);
    return b;
}

QString barsText(double beats) {   // bars. beats. sixteenths
    const int s = int(std::round(beats * 4));
    return QString("%1. %2. %3").arg(s / 16).arg((s / 4) % 4).arg(s % 4);
}
QString positionText(double beats) {   // a position: 1.1.1 is beat 0
    const int s = int(std::round(beats * 4));
    return QString("%1. %2. %3").arg(s / 16 + 1).arg((s / 4) % 4 + 1).arg(s % 4 + 1);
}

} // namespace

ClipView::ClipView(Project* project, Transport* transport, QWidget* parent)
    : QWidget(parent), m_project(project)
{
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_props = new QWidget(this);
    m_props->setObjectName("clipProps");
    auto* pl = new QHBoxLayout(m_props);   // the clip boxes, side by side
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(1);
    // little round switches at the bottom left: which boxes show (the Clip box always does)
    auto* toggles = new QWidget(m_props);
    toggles->setObjectName("clipToggles");
    toggles->setFixedWidth(18);
    auto* tl = new QVBoxLayout(toggles);
    tl->setContentsMargins(3, 2, 2, 4);
    tl->setSpacing(3);
    tl->addStretch(1);
    auto toggleButton = [&](const QString& text, const QString& key, const QString& title, const QString& help) {
        auto* b = new QToolButton(toggles);
        b->setObjectName("clipBoxToggle");
        b->setProperty("boxKey", key);
        b->setText(text);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setChecked(QSettings("Sessions", "Sessions").value("clipView/" + key, true).toBool());
        InfoView::attach(b, title, help);
        tl->addWidget(b);
        return b;
    };
    m_launchToggle = toggleButton("L", "launch", "Launch Box", "Shows or hides the Launch box (launch mode, quantization, follow actions).");
    m_notesToggle = toggleButton("♪", "notes", "Notes / Sample Box", "Shows or hides the Notes box (MIDI clips) or the Sample box (audio clips).");
    m_envToggle = toggleButton("E", "envelopes", "Envelopes Box", "Shows or hides the Envelopes box.");
    pl->addWidget(toggles);
    m_editors = new QStackedWidget(this);
    m_roll = new PianoRoll(project, transport, m_editors);
    m_sampleEditor = new SampleEditor(project, transport, m_editors);
    m_envEditor = new EnvelopeEditor(project, transport, m_editors);
    connect(m_roll, &PianoRoll::commandRequested, this, &ClipView::commandRequested);
    connect(m_sampleEditor, &SampleEditor::commandRequested, this, &ClipView::commandRequested);
    m_takesEditor = new TakesEditor(project, transport, m_editors);
    m_editors->addWidget(m_roll);
    m_editors->addWidget(m_sampleEditor);
    m_editors->addWidget(m_envEditor);
    m_editors->addWidget(m_takesEditor);

    // A box: a title strip over its controls.
    auto box = [&](const QString& title, QWidget* content, const QString& help) {
        auto* frame = new QWidget(m_props);
        frame->setObjectName("clipBox");
        frame->setFixedWidth(kBoxWidth);
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(0);
        auto* label = new QLabel(title, frame);
        label->setObjectName("clipBoxTitle");
        label->setFixedHeight(16);
        InfoView::attach(label, title, help);
        fl->addWidget(label);
        auto* inner = new QVBoxLayout;
        inner->setContentsMargins(4, 4, 4, 4);
        content->setParent(frame);
        inner->addWidget(content, 1);
        fl->addLayout(inner, 1);
        pl->addWidget(frame);
        return frame;
    };
    box("Clip", buildClipTab(), "The clip's name, length, loop and grid.");
    m_launchBox = box("Launch", buildLaunchTab(), "How the clip behaves when launched from the Session View: mode, legato, quantization, follow actions.");
    m_notesBox = box("Notes", buildNotesTab(), "Note tools and the clip's scale.");
    m_sampleBox = box("Sample", buildSampleTab(), "The clip's sample: warp mode, original tempo, gain, transpose, reverse.");
    m_envBox = box("Envelopes", buildEnvelopeTab(), "Clip envelopes: a parameter's line that plays with the clip and loops with it.");
    lay->addWidget(m_props);
    // the editor column: the clip in miniature on top (what's on screen outlined; drag it sideways to scroll,
    // up / down to zoom, click to centre there), the editor under it
    auto* column = new QWidget(this);
    auto* cl = new QVBoxLayout(column);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    m_strip = new ClipOverview(project, transport, column);
    m_strip->setFixedHeight(18);
    m_strip->setCursor(Qt::ArrowCursor);
    m_strip->setViewport([this](double& f, double& l) { return editorViewport(f, l); }, [this](double f, double l) { setEditorViewport(f, l); });
    connect(m_strip, &ClipOverview::clicked, this, [this] {   // centre the editor on the beat you clicked
        double f, l;
        if (!editorViewport(f, l)) return;
        const double at = m_strip->beatAt(m_strip->mapFromGlobal(QCursor::pos()).x()), span = l - f;
        setEditorViewport(at - span / 2, at + span / 2);
    });
    cl->addWidget(m_strip);
    cl->addWidget(m_editors, 1);
    lay->addWidget(column, 1);
    for (QToolButton* b : {m_launchToggle, m_notesToggle, m_envToggle})
        connect(b, &QToolButton::toggled, this, [this, b](bool on) {
            QSettings("Sessions", "Sessions").setValue("clipView/" + b->property("boxKey").toString(), on);
            showBoxes();
        });

    connect(m_roll, &PianoRoll::gridChanged, this, [this](double) { m_grid->setText(gridLabel()); });
    connect(m_roll, &PianoRoll::notesChanged, this, [this] { emit m_project->clipChanged(); });
    connect(project, &Project::clipChanged, this, [this] { if (m_clip && !m_name->hasFocus()) refresh(); });
    connect(project, &Project::restored, this, [this] { refresh(); m_roll->refreshRows(); });

    m_grid->setText(gridLabel());
    setClip(nullptr);
}

// ---- tabs -------------------------------------------------------------------

QWidget* ClipView::buildClipTab() {
    auto* w = new QWidget(m_props);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);

    m_name = new QLineEdit(w);
    m_name->setObjectName("clipName");
    InfoView::attach(m_name, "Clip Name", "Type to rename the clip.");
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (!m_clip) return;
        EditScope edit(m_project, "Rename Clip", clipMergeKey(m_clip->id, 1));   // typing merges into one step
        m_clip->name = t;
        emit m_project->clipChanged();
    });
    l->addWidget(m_name);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(4);
    form->setVerticalSpacing(3);
    auto fixed = [&](const QString& v, const QString& title, const QString& help) {
        auto* d = new DragValue(w); d->setEditable(false); d->setAccentText(false); d->setCursor(Qt::ArrowCursor);
        d->setMinimumChars(5); d->setFormatter([v](double) { return v; });
        InfoView::attach(d, title, help); return d;
    };
    form->addRow(dimLabel("Signature", w), fixed("4 / 4", "Clip Time Signature", "Fixed at 4/4 for now."));
    auto position = [&](const QString& title, const QString& help) {   // the start marker and the loop's position (0.23)
        auto* d = new DragValue(w);
        d->setRange(0, 1024); d->setStep(0.25); d->setDefault(0); d->setDragSensitivity(400); d->setMinimumChars(5);
        d->setFormatter(positionText);
        InfoView::attach(d, title, help);
        return d;
    };
    m_start = position("Clip Start", "Where the clip begins playing (bars. beats. sixteenths): before the loop it runs into it, inside it it starts part-way. The ▶ in the editor.");
    connect(m_start, &DragValue::valueEdited, this, [this](double v) { editClip("Move Clip Start", 3, [v](Clip& c) { c.start = v; }); m_roll->update(); });
    form->addRow(dimLabel("Start", w), m_start);
    m_position = position("Loop Position", "Where the loop brace begins. With Loop on the clip goes round the brace from here for the length; with Loop off it stops at the brace's end.");
    connect(m_position, &DragValue::valueEdited, this, [this](double v) { editClip("Move Loop", 4, [v](Clip& c) { c.loopStart = v; }); m_roll->update(); });
    form->addRow(dimLabel("Position", w), m_position);

    m_length = new DragValue(w);
    m_length->setRange(1, 256);
    m_length->setStep(1);
    m_length->setDefault(4);
    m_length->setDragSensitivity(400);
    m_length->setMinimumChars(5);
    m_length->setFormatter(barsText);
    InfoView::attach(m_length, "Loop Length", "Bars. Beats. Sixteenths. Drag up or down, or double-click to type beats.");
    connect(m_length, &DragValue::valueEdited, this, [this](double v) { editClip("Clip Length", 2, [v](Clip& c) { c.length = v; }); m_roll->update(); });
    form->addRow(dimLabel("Length", w), m_length);

    m_loop = toolButton("Loop", "Loop", "On: the clip repeats. Off: it plays once and stops.", w, true);
    connect(m_loop, &QPushButton::clicked, this, [this](bool on) { editClip(on ? "Loop On" : "Loop Off", 0, [on](Clip& c) { c.loop = on; }); });
    form->addRow(dimLabel("Loop", w), m_loop);

    m_grid = toolButton("", "Grid", "Click to cycle the note grid. Ctrl+1 makes it finer, Ctrl+2 coarser.", w);
    connect(m_grid, &QPushButton::clicked, this, [this] { double g = m_roll->gridSize() <= 1.0 / 32 ? 1.0 : m_roll->gridSize() / 2; m_roll->setGridSize(g); });
    form->addRow(dimLabel("Grid", w), m_grid);
    l->addLayout(form);

    auto* dupLoop = m_dupLoop = toolButton("Dupl. Loop", "Duplicate Loop", "Doubles the clip length and copies its notes into the new half.", w);
    auto* clear = m_clear = toolButton("Clear", "Clear Notes", "Deletes every note in the clip.", w);
    connect(dupLoop, &QPushButton::clicked, this, [this] {
        if (!m_clip) return;
        EditScope edit(m_project, "Duplicate Loop");
        double L = m_clip->length;
        QVector<Note> add;
        for (const Note& n : m_clip->notes) if (n.start < L) { Note c = n; c.start += L; add.push_back(c); }
        m_clip->notes += add;
        m_clip->length = std::min(256.0, L * 2);
        emit m_project->clipChanged();
        m_roll->update();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (!m_clip) return;
        EditScope edit(m_project, "Clear Notes");
        m_clip->notes.clear();
        emit m_project->clipChanged();
        m_roll->setClip(nullptr);
        m_roll->setClip(m_clip);
    });
    auto* row = new QHBoxLayout; row->setSpacing(4); row->addWidget(dupLoop); row->addWidget(clear);
    l->addLayout(row);
    m_save = toolButton("Save to Library", "Save Clip to User Library", "Saves the clip (its notes or its audio and warping, its launch settings and envelopes) into the browser's User Library, under Clips. "
                                                                        "Drag it from there onto any slot or lane, in this set or another.", w);
    m_save->setObjectName("saveClip");
    connect(m_save, &QPushButton::clicked, this, [this] { emit commandRequested("saveClip"); });
    l->addWidget(m_save);
    l->addStretch(1);
    return w;
}

QWidget* ClipView::buildLaunchTab() {
    auto* w = new QWidget(m_props);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(4);
    form->setVerticalSpacing(3);

    m_mode = new QComboBox(w);
    m_mode->addItems({"Trigger", "Gate", "Toggle", "Repeat"});
    m_mode->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_mode, "Launch Mode", "Trigger: click starts it. Gate: plays while you hold the mouse. Toggle: click again to stop. Repeat: retriggers on the grid while held.");
    connect(m_mode, &QComboBox::activated, this, [this](int i) { editClip("Launch Mode", 0, [i](Clip& c) { c.launchMode = LaunchMode(i); }); });
    form->addRow(dimLabel("Mode", w), m_mode);

    m_legato = new QCheckBox("Legato", w);
    m_legato->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_legato, "Legato", "Starts from the playing clip's position instead of the beginning, so switching clips doesn't jump.");
    connect(m_legato, &QCheckBox::clicked, this, [this](bool on) { editClip("Legato", 0, [on](Clip& c) { c.legato = on; }); });
    form->addRow(dimLabel("", w), m_legato);

    m_quant = new QComboBox(w);
    for (const char* n : kQuantNames) m_quant->addItem(n);
    m_quant->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_quant, "Clip Quantization", "When this clip starts after you launch it. Global uses the control bar setting.");
    connect(m_quant, &QComboBox::activated, this, [this](int i) { editClip("Clip Quantization", 0, [i](Clip& c) { c.quantization = kQuantBeats[i]; }); });
    form->addRow(dimLabel("Quantize", w), m_quant);

    m_follow = new QCheckBox("Follow Action", w);
    m_follow->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_follow, "Follow Action", "After the follow time, the clip does action A or B (chance decides): stop, play again, play the next / previous / first / last / any / another clip in its track.");
    connect(m_follow, &QCheckBox::clicked, this, [this](bool on) { editClip("Follow Action", 0, [on](Clip& c) { c.followEnabled = on; }); });
    form->addRow(m_follow);

    m_followTime = new DragValue(w);
    m_followTime->setRange(0, 256);
    m_followTime->setStep(1);
    m_followTime->setDefault(0);
    m_followTime->setDragSensitivity(400);
    m_followTime->setMinimumChars(6);
    m_followTime->setFormatter([](double v) { return v < 0.5 ? QString("Loop") : barsText(v); });
    InfoView::attach(m_followTime, "Follow Action Time", "How long the clip plays before its follow action. Loop = one pass of the clip.");
    connect(m_followTime, &DragValue::valueEdited, this, [this](double v) { editClip("Follow Time", 3, [v](Clip& c) { c.followTime = v; }); });
    form->addRow(dimLabel("Time", w), m_followTime);

    auto actionCombo = [&](const QString& title) {
        auto* c = new QComboBox(w);
        for (const char* n : kFollowNames) c->addItem(n);
        c->setFocusPolicy(Qt::NoFocus);
        InfoView::attach(c, title, "What happens when the follow time is up.");
        return c;
    };
    m_actionA = actionCombo("Follow Action A");
    connect(m_actionA, &QComboBox::activated, this, [this](int i) { editClip("Follow Action A", 0, [i](Clip& c) { c.followA = FollowAction(i); }); });
    form->addRow(dimLabel("A", w), m_actionA);

    m_chance = new DragValue(w);
    m_chance->setRange(0, 1);
    m_chance->setDefault(1);
    m_chance->setMinimumChars(6);
    m_chance->setFormatter([](double v) { return QString("%1 % A").arg(int(std::round(v * 100))); });
    InfoView::attach(m_chance, "Chance", "How often action A happens rather than B.");
    connect(m_chance, &DragValue::valueEdited, this, [this](double v) { editClip("Follow Chance", 4, [v](Clip& c) { c.followChanceA = v; }); });
    form->addRow(dimLabel("Chance", w), m_chance);

    m_actionB = actionCombo("Follow Action B");
    connect(m_actionB, &QComboBox::activated, this, [this](int i) { editClip("Follow Action B", 0, [i](Clip& c) { c.followB = FollowAction(i); }); });
    form->addRow(dimLabel("B", w), m_actionB);
    return w;
}

QWidget* ClipView::buildNotesTab() {
    auto* w = new QWidget(m_props);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setSpacing(3);
    struct Tool { const char* text; const char* title; const char* help; int row, col; std::function<void()> run; };
    auto transpose = [this](int d) {
        editNotes(d > 0 ? "Transpose Up" : "Transpose Down", [d](QVector<Note>& n, const QList<int>& which) {
            for (int i : which) n[i].pitch = std::clamp(n[i].pitch + d, 0, 127);
        });
    };
    auto stretch = [this](double f) {
        if (!m_clip) return;
        const bool all = m_roll->selectedNotes().isEmpty();
        EditScope edit(m_project, f > 1 ? "x2" : ":2");   // notes and loop length in one step
        editNotes(f > 1 ? "x2" : ":2", [f](QVector<Note>& n, const QList<int>& which) {
            double a0 = 1e9;
            for (int i : which) a0 = std::min(a0, n[i].start);
            for (int i : which) { n[i].start = a0 + (n[i].start - a0) * f; n[i].length *= f; }
        });
        if (all && m_clip) editClip("Clip Length", 0, [f](Clip& c) { c.length = std::clamp(c.length * f, 1.0, 256.0); });
    };
    const QList<Tool> tools = {
        {"−12", "Octave Down", "Moves the notes down an octave.", 0, 0, [transpose] { transpose(-12); }},
        {"−1", "Transpose Down", "Moves the notes down a semitone.", 0, 1, [transpose] { transpose(-1); }},
        {"+1", "Transpose Up", "Moves the notes up a semitone.", 0, 2, [transpose] { transpose(1); }},
        {"+12", "Octave Up", "Moves the notes up an octave.", 0, 3, [transpose] { transpose(12); }},
        {":2", "Half Time", "Halves note positions and lengths (and the loop, for the whole clip).", 1, 0, [stretch] { stretch(0.5); }},
        {"x2", "Double Time", "Doubles note positions and lengths (and the loop, for the whole clip).", 1, 1, [stretch] { stretch(2.0); }},
        {"Rev", "Reverse", "Plays the notes backwards in time.", 1, 2, [this] {
            const bool all = m_roll->selectedNotes().isEmpty();
            const double L = m_clip ? m_clip->length : 0;
            editNotes("Reverse", [all, L](QVector<Note>& n, const QList<int>& which) {
                double a0 = all ? 0 : 1e9, a1 = all ? L : 0;
                if (!all) for (int i : which) { a0 = std::min(a0, n[i].start); a1 = std::max(a1, n[i].start + n[i].length); }
                for (int i : which) n[i].start = std::max(0.0, a0 + a1 - (n[i].start + n[i].length));
            });
        }},
        {"Inv", "Invert", "Flips the notes upside down within their pitch range.", 1, 3, [this] {
            editNotes("Invert", [](QVector<Note>& n, const QList<int>& which) {
                int lo = 127, hi = 0;
                for (int i : which) { lo = std::min(lo, n[i].pitch); hi = std::max(hi, n[i].pitch); }
                for (int i : which) n[i].pitch = lo + hi - n[i].pitch;
            });
        }},
    };
    for (const Tool& t : tools) {
        auto* b = toolButton(t.text, t.title, t.help, w);
        b->setMinimumWidth(26);
        connect(b, &QPushButton::clicked, this, t.run);
        grid->addWidget(b, t.row, t.col);
    }
    auto* legato = toolButton("Legato", "Legato", "Stretches each note up to the next one.", w);
    connect(legato, &QPushButton::clicked, this, [this] {
        const bool all = m_roll->selectedNotes().isEmpty();
        const double L = m_clip ? m_clip->length : 0;
        editNotes("Legato", [all, L](QVector<Note>& n, const QList<int>& which) {
            QList<int> order = which;
            std::sort(order.begin(), order.end(), [&n](int a, int b) { return n[a].start < n[b].start; });
            for (int k = 0; k < order.size(); ++k) {
                double next = -1;
                for (int j = k + 1; j < order.size(); ++j) if (n[order[j]].start > n[order[k]].start + 1e-9) { next = n[order[j]].start; break; }
                if (next > 0) n[order[k]].length = next - n[order[k]].start;
                else if (all && L > n[order[k]].start) n[order[k]].length = L - n[order[k]].start;
            }
        });
    });
    grid->addWidget(legato, 2, 0, 1, 4);
    l->addLayout(grid);

    // scale
    auto* scaleRow = new QHBoxLayout;
    scaleRow->setSpacing(3);
    m_scaleOn = toolButton("Scale", "Scale", "Lights up the rows in the clip's scale in the note editor.", w, true);
    m_root = new QComboBox(w);
    for (int i = 0; i < 12; ++i) m_root->addItem(noteName(i));
    m_root->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_root, "Root", "The scale's root note.");
    scaleRow->addWidget(m_scaleOn);
    scaleRow->addWidget(m_root);
    l->addLayout(scaleRow);
    m_scale = new QComboBox(w);
    for (const ScaleDef& s : kScales) m_scale->addItem(s.name);
    m_scale->setFocusPolicy(Qt::NoFocus);
    InfoView::attach(m_scale, "Scale", "Which notes belong to the clip's scale.");
    l->addWidget(m_scale);
    m_fold = toolButton("Fold", "Fold", "Shows only the rows in the scale, or only rows that hold notes when Scale is off.", w, true);
    l->addWidget(m_fold);
    l->addStretch(1);

    connect(m_scaleOn, &QPushButton::clicked, this, [this](bool on) { editClip(on ? "Scale On" : "Scale Off", 0, [on](Clip& c) { c.scaleOn = on; }); m_roll->refreshRows(); });
    connect(m_root, &QComboBox::activated, this, [this](int i) { editClip("Scale Root", 0, [i](Clip& c) { c.scaleRoot = i; c.scaleOn = true; }); m_roll->refreshRows(); });
    connect(m_scale, &QComboBox::activated, this, [this](int i) { editClip("Scale", 0, [i](Clip& c) { c.scale = i; c.scaleOn = true; }); m_roll->refreshRows(); });
    connect(m_fold, &QPushButton::clicked, this, [this](bool on) { m_roll->setFold(on); });
    return w;
}

QWidget* ClipView::buildSampleTab() {
    auto* w = new QWidget(m_props);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    m_file = new QLabel(w);
    m_file->setObjectName("dimLabel");
    m_file->setWordWrap(false);
    InfoView::attach(m_file, "Sample", "The audio file this clip plays.");
    l->addWidget(m_file);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(4);
    form->setVerticalSpacing(3);
    m_warpOn = toolButton("Warp", "Warp", "On: the clip follows the set's tempo (through its warp markers). "
                                          "Off: it plays at the file's own speed, whatever the tempo, like a recording should.", w, true);
    m_warpOn->setObjectName("warpOn");
    connect(m_warpOn, &QPushButton::clicked, this, [this](bool on) { editClip(on ? "Warp On" : "Warp Off", 0, [on](Clip& c) { c.warp = on; }); });
    m_warp = new QComboBox(w);
    for (WarpMode m : {WarpMode::Beats, WarpMode::Tones, WarpMode::Texture, WarpMode::Repitch, WarpMode::Complex, WarpMode::ComplexPro}) m_warp->addItem(warpModeName(m));
    m_warp->setFocusPolicy(Qt::NoFocus);
    m_warp->setObjectName("warpMode");
    InfoView::attach(m_warp, "Warp Mode", "How the audio is stretched to the tempo. Beats: slices (drums). Tones: grains lined up with each other (bass, vocals). "
                                          "Texture: grains that wander (pads, noise). Re-Pitch: like a record, the pitch follows the tempo. "
                                          "Complex: a phase vocoder, every partial keeps its pitch (full mixes). Complex Pro: the vocoder with clean chords, "
                                          "sharp hits, and Formants to keep a voice a voice when transposed.");
    connect(m_warp, &QComboBox::activated, this, [this](int i) { editClip("Warp Mode", 0, [i](Clip& c) { c.warpMode = WarpMode(i); }); });
    auto* warpRow = new QHBoxLayout; warpRow->setSpacing(3);
    warpRow->addWidget(m_warpOn); warpRow->addWidget(m_warp, 1);
    form->addRow(dimLabel("Warp", w), warpRow);
    m_slices = new QComboBox(w);
    for (const char* n : kGrainNames) m_slices->addItem(n);
    m_slices->setFocusPolicy(Qt::NoFocus);
    m_slices->setObjectName("warpSlices");
    InfoView::attach(m_slices, "Slices", "Where Beats mode cuts the audio: at its transients (the hits it finds, best for most loops), "
                                         "or on a fixed grid.");
    connect(m_slices, &QComboBox::activated, this, [this](int i) { editClip("Warp Slices", 0, [i](Clip& c) { c.grain = kGrains[i]; }); });
    // the row shows what the mode offers: Beats its slices, Tones / Texture the grain size (and Flux), Complex Pro the formants
    auto* extra = new QHBoxLayout; extra->setSpacing(3);
    extra->addWidget(m_slices, 1);
    m_grainSize = new DragValue(w);
    m_grainSize->setRange(12, 256);
    m_grainSize->setStep(1);
    m_grainSize->setDefault(50);
    m_grainSize->setDragSensitivity(240);
    m_grainSize->setMinimumChars(6);
    m_grainSize->setObjectName("warpGrain");
    m_grainSize->setFormatter([](double v) { return QString("%1 ms").arg(int(std::lround(v))); });
    InfoView::attach(m_grainSize, "Grain Size", "How long each grain is. Shorter grains follow fast changes, longer ones keep a sustained tone smoother.");
    connect(m_grainSize, &DragValue::valueEdited, this, [this](double v) { editClip("Grain Size", 4, [v](Clip& c) { c.grainMs = std::clamp(v, 12.0, 256.0); }); });
    extra->addWidget(m_grainSize, 1);
    m_flux = new DragValue(w);
    m_flux->setRange(0, 100);
    m_flux->setStep(1);
    m_flux->setDefault(25);
    m_flux->setDragSensitivity(200);
    m_flux->setMinimumChars(5);
    m_flux->setObjectName("warpFlux");
    m_flux->setFormatter([](double v) { return QString("%1 %").arg(int(std::lround(v))); });
    InfoView::attach(m_flux, "Flux", "How far Texture's grains wander from where they should be: more is looser and cloudier.");
    connect(m_flux, &DragValue::valueEdited, this, [this](double v) { editClip("Flux", 5, [v](Clip& c) { c.flux = std::clamp(v / 100, 0.0, 1.0); }); });
    extra->addWidget(m_flux, 1);
    m_formants = new DragValue(w);
    m_formants->setRange(0, 100);
    m_formants->setStep(1);
    m_formants->setDefault(0);
    m_formants->setDragSensitivity(200);
    m_formants->setMinimumChars(5);
    m_formants->setObjectName("warpFormants");
    m_formants->setFormatter([](double v) { return QString("%1 %").arg(int(std::lround(v))); });
    InfoView::attach(m_formants, "Formants", "Complex Pro: how much the formants stay where they are when the clip is transposed, so a voice keeps its character.");
    connect(m_formants, &DragValue::valueEdited, this, [this](double v) { editClip("Formants", 6, [v](Clip& c) { c.formants = std::clamp(v / 100, 0.0, 1.0); }); });
    extra->addWidget(m_formants, 1);
    m_warpExtraLabel = dimLabel("Slices", w);
    form->addRow(m_warpExtraLabel, extra);

    m_bpm = new DragValue(w);
    m_bpm->setRange(20, 999);
    m_bpm->setDefault(120);
    m_bpm->setDragSensitivity(300);
    m_bpm->setMinimumChars(6);
    m_bpm->setFormatter([](double v) { return QString::number(v, 'f', 2); });
    InfoView::attach(m_bpm, "Original Tempo", "The tempo the audio was played at. Set it right and the clip follows the set's tempo. "
                                              "Changing it moves the warp markers (and the clip's length) with it.");
    connect(m_bpm, &DragValue::valueEdited, this, [this](double v) { setSampleBpm(v); });
    auto* half = toolButton(":2", "Half Tempo", "Halves the original tempo: the clip plays twice as long.", w);
    auto* twice = toolButton("x2", "Double Tempo", "Doubles the original tempo: the clip plays in half the time.", w);
    connect(half, &QPushButton::clicked, this, [this] { if (m_clip) setSampleBpm(m_clip->sampleBpm() / 2); });
    connect(twice, &QPushButton::clicked, this, [this] { if (m_clip) setSampleBpm(m_clip->sampleBpm() * 2); });
    auto* bpmRow = new QHBoxLayout; bpmRow->setSpacing(3);
    bpmRow->addWidget(m_bpm, 1); bpmRow->addWidget(half); bpmRow->addWidget(twice);
    form->addRow(dimLabel("BPM", w), bpmRow);
    // the tempo found in the audio and how the clip sits against the set's tempo; Sync fits it again
    m_tempoInfo = new ElidedLabel(w);
    m_tempoInfo->setObjectName("dimLabel");
    m_tempoInfo->setWordWrap(false);
    InfoView::attach(m_tempoInfo, "Found Tempo", "The tempo Sessions heard in the audio, and the tempo the clip plays at: the set's tempo halved or doubled "
                                                 "until it's nearest the audio's own, so the clip is in sync whatever the set's tempo.");
    auto* sync = toolButton("Sync", "Sync to Tempo", "Fits the clip to the set's tempo again from its audio: finds the tempo if it isn't known yet, "
                                                   "then sets the warp markers and the loop. Use it on a clip whose markers you've moved, or one from an older set.", w);
    sync->setObjectName("syncTempo");
    connect(sync, &QPushButton::clicked, this, [this] { if (m_clip && m_project->syncClipToTempo(m_clip)) m_sampleEditor->setClip(m_clip); });
    auto* tempoRow = new QHBoxLayout; tempoRow->setSpacing(3);
    tempoRow->addWidget(m_tempoInfo, 1); tempoRow->addWidget(sync, 0, Qt::AlignTop);
    form->addRow(dimLabel("Tempo", w), tempoRow);

    m_gain = new DragValue(w);
    m_gain->setRange(-70, 24);
    m_gain->setDefault(0);
    m_gain->setDragSensitivity(300);
    m_gain->setMinimumChars(7);
    m_gain->setFormatter([](double v) { return v <= -69.9 ? QString("-inf dB") : QString("%1%2 dB").arg(v > 0.05 ? "+" : "").arg(v, 0, 'f', 1); });
    InfoView::attach(m_gain, "Clip Gain", "How loud the clip plays, before the track's devices and fader.");
    connect(m_gain, &DragValue::valueEdited, this, [this](double v) { editClip("Clip Gain", 6, [v](Clip& c) { c.gain = v <= -69.9 ? -70 : v; }); });
    form->addRow(dimLabel("Gain", w), m_gain);

    m_transpose = new DragValue(w);
    m_transpose->setRange(-48, 48);
    m_transpose->setStep(1);
    m_transpose->setDefault(0);
    m_transpose->setDragSensitivity(200);
    m_transpose->setMinimumChars(6);
    m_transpose->setFormatter([](double v) { const int st = int(std::lround(v)); return st == 0 ? QString("0 st") : QString("%1%2 st").arg(st > 0 ? "+" : "").arg(st); });
    InfoView::attach(m_transpose, "Transpose", "Shifts the pitch in semitones (Beats mode; in Re-Pitch the tempo sets the pitch).");
    connect(m_transpose, &DragValue::valueEdited, this, [this](double v) { editClip("Transpose", 7, [v](Clip& c) { c.transpose = int(std::lround(v)); }); });
    form->addRow(dimLabel("Transp.", w), m_transpose);
    l->addLayout(form);

    m_reverse = toolButton("Reverse", "Reverse", "Plays the sample backwards. The warp markers flip with it.", w, true);
    connect(m_reverse, &QPushButton::clicked, this, [this](bool on) {
        if (!m_clip) return;
        const double dur = m_clip->sample ? m_clip->sample->seconds() : m_clip->warpMarkers.value(m_clip->warpMarkers.size() - 1).seconds;
        editClip(on ? "Reverse" : "Reverse Off", 0, [dur](Clip& c) {
            if (c.warpMarkers.size() >= 2) {   // mirror the markers: the clip keeps its beats, the audio runs the other way
                const double b0 = c.warpMarkers.first().beat, b1 = c.warpMarkers.last().beat;
                QVector<WarpMarker> flipped;
                for (int i = int(c.warpMarkers.size()) - 1; i >= 0; --i)
                    flipped.push_back({b0 + b1 - c.warpMarkers[i].beat, dur - c.warpMarkers[i].seconds});
                c.warpMarkers = flipped;
            }
            c.reverse = !c.reverse;
        });
    });
    m_takesShow = toolButton("Takes", "Takes", "Loop recordings keep every pass as a take. Shows the Takes view: drag across a take to use that stretch of it in the clip (the comp); "
                                               "the joins crossfade. Click again for the sample editor.", w, true);
    m_takesShow->setObjectName("takesShow");
    connect(m_takesShow, &QPushButton::clicked, this, [this](bool on) { showTakes(on); });
    auto* bottom = new QHBoxLayout; bottom->setSpacing(4); bottom->addWidget(m_reverse); bottom->addWidget(m_takesShow);
    l->addLayout(bottom);
    l->addStretch(1);
    return w;
}

void ClipView::setSampleBpm(double bpm) {
    if (!m_clip || !m_clip->isAudio() || m_clip->warpMarkers.size() < 2 || bpm <= 0) return;
    const double k = std::clamp(bpm, 5.0, 2000.0) / m_clip->sampleBpm();
    if (std::abs(k - 1) < 1e-9) return;
    editClip("Original Tempo", 5, [k](Clip& c) {
        const double b0 = c.warpMarkers.first().beat;
        for (WarpMarker& m : c.warpMarkers) m.beat = b0 + (m.beat - b0) * k;
        c.length = std::clamp(c.length * k, 0.25, 1024.0);
    });
    m_sampleEditor->setClip(m_clip);   // refit
}

QWidget* ClipView::buildEnvelopeTab() {
    auto* w = new QWidget(m_props);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    m_envDevice = new QComboBox(w);
    m_envDevice->setFocusPolicy(Qt::NoFocus);
    m_envDevice->setObjectName("envDevice");
    InfoView::attach(m_envDevice, "Envelope Device", "Mixer (volume, pan, sends) or one of the track's devices.");
    m_envParam = new QComboBox(w);
    m_envParam->setFocusPolicy(Qt::NoFocus);
    m_envParam->setObjectName("envParam");
    InfoView::attach(m_envParam, "Envelope Parameter", "The parameter to draw. ● marks the ones this clip already has an envelope for.");
    l->addWidget(m_envDevice);
    l->addWidget(m_envParam);
    m_envShow = toolButton("Show", "Show Envelope", "Swaps the note or sample editor for this envelope. Click again to go back.", w, true);
    m_envClear = toolButton("Clear", "Clear Envelope", "Deletes this parameter's envelope from the clip.", w);
    auto* row = new QHBoxLayout; row->setSpacing(4); row->addWidget(m_envShow); row->addWidget(m_envClear);
    l->addLayout(row);
    m_envList = new QLabel(w);
    m_envList->setObjectName("dimLabel");
    m_envList->setWordWrap(true);
    l->addWidget(m_envList);
    l->addStretch(1);

    connect(m_envDevice, &QComboBox::activated, this, [this] {
        refreshEnvelopeChoosers();   // that device's parameters
        if (m_envParam->count()) m_envEditor->setTarget(m_envParam->itemData(0).toString());
        refreshEnvelopeChoosers();
        showEnvelope(true);
    });
    connect(m_envParam, &QComboBox::activated, this, [this](int i) {
        m_envEditor->setTarget(m_envParam->itemData(i).toString());
        showEnvelope(true);
    });
    connect(m_envShow, &QPushButton::clicked, this, [this](bool on) { showEnvelope(on); });
    connect(m_envClear, &QPushButton::clicked, this, [this] {
        if (!m_clip || !m_clip->envelope(m_envEditor->target())) return;
        const QString tgt = m_envEditor->target();
        editClip("Clear Clip Envelope", 0, [tgt](Clip& c) { c.envelopes.removeIf([&tgt](const Envelope& e) { return e.target == tgt; }); });
    });
    return w;
}

// Fills the two choosers for the clip's track and the editor's target.
void ClipView::refreshEnvelopeChoosers() {
    QSignalBlocker b1(m_envDevice), b2(m_envParam);
    const int owner = m_project->trackOfClip(m_clip);
    const Track* t = m_project->trackAt(owner);
    const QStringList targets = t ? m_project->automatableTargets(owner) : QStringList();
    QString current = m_envEditor->target();
    if (!targets.contains(current)) { current = target::volume(); m_envEditor->setTarget(current); }
    auto deviceOf = [](const QString& tgt) { return tgt.startsWith("device:") ? tgt.section(':', 1, 1).toInt() : 0; };
    // the device chooser: Mixer, then each device with parameters; picking one keeps its item selected
    const int pickedDevice = m_envDevice->currentIndex() >= 0 && m_envDevice->currentData().isValid() && sender() == m_envDevice
        ? m_envDevice->currentData().toInt() : deviceOf(current);
    m_envDevice->clear();
    m_envDevice->addItem("Mixer", 0);
    if (t) for (const Device& d : t->devices) if (!d.params.isEmpty()) m_envDevice->addItem(d.name, d.id);
    m_envDevice->setCurrentIndex(std::max(0, m_envDevice->findData(pickedDevice)));
    const int dev = m_envDevice->currentData().toInt();
    m_envParam->clear();
    for (const QString& tgt : targets) {
        if (deviceOf(tgt) != dev) continue;
        QString label = m_project->targetName(owner, tgt);
        if (tgt.startsWith("device:")) label = label.section(" · ", 1);
        if (m_clip && m_clip->envelope(tgt)) label += "  ●";
        m_envParam->addItem(label, tgt);
    }
    m_envParam->setCurrentIndex(std::max(0, m_envParam->findData(current)));
    QStringList names;
    if (m_clip) for (const Envelope& e : m_clip->envelopes) if (!e.points.isEmpty()) names << m_project->targetName(owner, e.target);
    m_envList->setText(names.isEmpty() ? "No envelopes" : names.join(", "));
    m_envClear->setEnabled(m_clip && m_clip->envelope(current));
}

void ClipView::showEnvelope(bool on) {
    on = on && m_clip;
    m_envShow->setChecked(on);
    if (on) m_takesShow->setChecked(false);
    m_editors->setCurrentIndex(on ? 2 : m_takesShow->isChecked() ? 3 : audio() ? 1 : 0);
}

void ClipView::showTakes(bool on) {
    on = on && m_clip && !m_clip->takes.isEmpty();
    m_takesShow->setChecked(on);
    if (on) m_envShow->setChecked(false);
    m_editors->setCurrentIndex(m_envShow->isChecked() ? 2 : on ? 3 : audio() ? 1 : 0);
}

bool ClipView::audio() const { return m_clip && m_clip->isAudio(); }

bool ClipView::editorViewport(double& first, double& last) const {
    if (!m_clip) return false;
    if (m_editors->currentWidget() == m_roll) return m_roll->viewport(first, last);
    if (m_editors->currentWidget() == m_sampleEditor) return m_sampleEditor->viewport(first, last);
    return false;
}

void ClipView::setEditorViewport(double first, double last) {
    if (m_editors->currentWidget() == m_roll) m_roll->setViewport(first, last);
    else if (m_editors->currentWidget() == m_sampleEditor) m_sampleEditor->setViewport(first, last);
    if (m_strip) m_strip->update();
}

// ---- edits ------------------------------------------------------------------

void ClipView::editNotes(const QString& label, const std::function<void(QVector<Note>&, const QList<int>&)>& fn) {
    if (!m_clip || m_clip->notes.isEmpty()) return;
    QList<int> which = m_roll->selectedNotes();
    if (which.isEmpty()) for (int i = 0; i < m_clip->notes.size(); ++i) which.push_back(i);
    EditScope edit(m_project, label);
    fn(m_clip->notes, which);
    emit m_project->clipChanged();
    m_roll->refreshRows();
}

void ClipView::editClip(const QString& label, int mergeWhat, const std::function<void(Clip&)>& fn) {
    if (!m_clip) return;
    EditScope edit(m_project, label, mergeWhat ? clipMergeKey(m_clip->id, mergeWhat) : 0);
    fn(*m_clip);
    emit m_project->clipChanged();
}

QString ClipView::gridLabel() const {
    double g = m_roll->gridSize();
    int denom = int(std::round(4.0 / g));
    return QString("1/%1").arg(denom);
}

void ClipView::setClip(ClipPtr clip) {
    m_clip = clip;
    if (m_strip) m_strip->update();   // the strip shows the new clip at once
    const bool isAudio = audio();
    m_roll->setClip(isAudio ? nullptr : clip);
    m_sampleEditor->setClip(isAudio ? clip : nullptr);
    m_takesEditor->setClip(isAudio ? clip : nullptr);
    m_envEditor->setClip(clip);
    const bool takes = m_takesShow->isChecked() && isAudio && !clip->takes.isEmpty();
    m_takesShow->setChecked(takes);
    m_editors->setCurrentIndex(m_envShow->isChecked() && clip ? 2 : takes ? 3 : isAudio ? 1 : 0);
    showBoxes();
    refresh();
}

// Which boxes show: the Clip box always, the others by their switches; Notes or Sample by the clip's kind.
void ClipView::showBoxes() {
    const bool isAudio = audio();
    // hide one box before showing the other: the boxes' row never counts both
    QWidget* shown = isAudio ? m_sampleBox : m_notesBox;
    QWidget* hidden = isAudio ? m_notesBox : m_sampleBox;
    hidden->hide();
    shown->setVisible(m_notesToggle->isChecked());
    m_launchBox->setVisible(m_launchToggle->isChecked());
    m_envBox->setVisible(m_envToggle->isChecked());
}

void ClipView::refresh() {
    const bool has = m_clip != nullptr;
    m_props->setEnabled(has);
    QSignalBlocker b1(m_mode), b2(m_quant), b3(m_actionA), b4(m_actionB), b5(m_root), b6(m_scale);
    if (!has) { m_name->setText(""); m_length->setValue(4, false); m_start->setValue(0, false); m_position->setValue(0, false); m_envShow->setChecked(false); refreshEnvelopeChoosers(); return; }
    refreshEnvelopeChoosers();
    if (m_name->text() != m_clip->name) m_name->setText(m_clip->name);
    const bool isAudio = m_clip->isAudio();
    m_length->setRange(isAudio ? 0.25 : 1, isAudio ? 1024 : 256);
    m_length->setStep(isAudio ? 0.25 : 1);
    m_length->setValue(m_clip->length, false);
    m_start->setValue(m_clip->start, false);
    m_position->setValue(m_clip->loopStart, false);
    m_dupLoop->setEnabled(!isAudio);
    m_clear->setEnabled(!isAudio);
    m_grid->setEnabled(!isAudio);
    if (isAudio) {
        QSignalBlocker b7(m_warp);
        const QFileInfo fi(m_clip->samplePath);
        const QString takes = m_clip->takes.isEmpty() ? QString() : QString("%1 takes · ").arg(m_clip->takes.size());
        m_file->setText(m_file->fontMetrics().elidedText(takes + (m_clip->sample ? "" : "Missing: ") + fi.fileName(), Qt::ElideMiddle, kBoxWidth - 10));
        m_takesShow->setEnabled(!m_clip->takes.isEmpty());
        m_takesShow->setText(m_clip->takes.isEmpty() ? "Takes" : QString("Takes (%1)").arg(m_clip->takes.size()));
        m_reverse->setEnabled(m_clip->takes.isEmpty());   // takes play forwards
        m_file->setToolTip(QDir::toNativeSeparators(m_clip->samplePath));
        m_file->setStyleSheet(m_clip->sample ? QString() : "color:" + theme::record.name() + ";");
        m_warp->setCurrentIndex(int(m_clip->warpMode));
        m_warpOn->setChecked(m_clip->warp);
        m_warp->setEnabled(m_clip->warp);
        m_bpm->setEnabled(m_clip->warp);
        int g = 3;
        for (int i = 0; i < 5; ++i) if (std::abs(kGrains[i] - m_clip->grain) < 1e-9) g = i;
        { QSignalBlocker b8(m_slices); m_slices->setCurrentIndex(g); }
        const WarpMode wm = m_clip->warpMode;
        m_slices->setVisible(wm == WarpMode::Beats || wm == WarpMode::Repitch);
        m_slices->setEnabled(m_clip->warp && wm == WarpMode::Beats);
        m_grainSize->setVisible(wm == WarpMode::Tones || wm == WarpMode::Texture);
        m_flux->setVisible(wm == WarpMode::Texture);
        m_formants->setVisible(wm == WarpMode::Complex || wm == WarpMode::ComplexPro);
        m_formants->setEnabled(m_clip->warp && wm == WarpMode::ComplexPro);
        m_grainSize->setEnabled(m_clip->warp);
        m_flux->setEnabled(m_clip->warp);
        m_grainSize->setValue(m_clip->grainMs, false);
        m_flux->setValue(m_clip->flux * 100, false);
        m_formants->setValue(m_clip->formants * 100, false);
        m_warpExtraLabel->setText(wm == WarpMode::Texture ? "Grain Flux" : wm == WarpMode::Tones ? "Grain" : wm == WarpMode::Complex || wm == WarpMode::ComplexPro ? "Formants" : "Slices");
        m_bpm->setValue(m_clip->sampleBpm(), false);
        if (m_clip->detectedBpm > 0) {
            const double own = m_clip->sampleBpm();
            const int k = int(std::lround(std::log2(own / m_clip->detectedBpm)));
            const double plays = m_clip->detectedBpm * m_project->tempo / own;   // what you hear
            const QString share = k == 0 ? QString("1× the set") : k > 0 ? QString("1/%1 the set").arg(1 << k) : QString("%1× the set").arg(1 << -k);
            const QString full = m_clip->warp ? QString("found %1 · plays at %2 BPM (%3)").arg(m_clip->detectedBpm, 0, 'f', 1).arg(plays, 0, 'f', 1).arg(share)
                                              : QString("found %1 BPM · Warp off, plays as recorded").arg(m_clip->detectedBpm, 0, 'f', 1);
            m_tempoInfo->setText(full);
            m_tempoInfo->setToolTip(full);
        } else {
            m_tempoInfo->setText(m_clip->sample ? "no steady beat found" : "");
            m_tempoInfo->setToolTip(QString());
        }
        m_gain->setValue(m_clip->gain, false);
        m_transpose->setValue(m_clip->transpose, false);
        m_transpose->setEnabled(m_clip->warp && m_clip->warpMode != WarpMode::Repitch);
        m_reverse->setChecked(m_clip->reverse);
    }
    m_loop->setChecked(m_clip->loop);
    m_loop->setText(m_clip->loop ? "Loop On" : "Loop Off");
    m_mode->setCurrentIndex(int(m_clip->launchMode));
    m_legato->setChecked(m_clip->legato);
    int q = 0;
    for (int i = 0; i < 9; ++i) if (std::abs(kQuantBeats[i] - m_clip->quantization) < 1e-9) q = i;
    m_quant->setCurrentIndex(q);
    m_follow->setChecked(m_clip->followEnabled);
    m_followTime->setValue(m_clip->followTime, false);
    m_actionA->setCurrentIndex(int(m_clip->followA));
    m_actionB->setCurrentIndex(int(m_clip->followB));
    m_chance->setValue(m_clip->followChanceA, false);
    for (QWidget* f : {static_cast<QWidget*>(m_followTime), static_cast<QWidget*>(m_actionA), static_cast<QWidget*>(m_actionB), static_cast<QWidget*>(m_chance)})
        f->setEnabled(m_clip->followEnabled);
    m_scaleOn->setChecked(m_clip->scaleOn);
    m_root->setCurrentIndex(m_clip->scaleRoot);
    m_scale->setCurrentIndex(m_clip->scale);
    m_fold->setChecked(m_roll->fold());
    int owner = m_project->trackOfClip(m_clip);
    if (owner >= 0) {
        QColor c = m_project->clipColor(m_clip);
        m_name->setStyleSheet(QString("QLineEdit#clipName{background:%1;color:%2;}").arg(c.name(), theme::ink.name()));
    }
}

} // namespace sessions
