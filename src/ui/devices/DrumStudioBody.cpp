// The Drum Studio's panel: three pages on buttons at its left, like the Monolith's.
//  - Kit: the kit seen from above (click a drum to select it and hear it; it lights when hit),
//    the kit chooser, the selected drum's Level / Pan / Tune / Decay / Tone, Wires, Beater, Volume.
//  - Mics: the mic setup (close mics, the overhead pair, the room pair) with the room's level,
//    and the Overheads / Room / Room Size / Bleed / Width / Velocity knobs.
//  - Grooves: the library by style; drag a groove onto a Session slot or an Arrangement lane,
//    double-click it or press To Clip for a clip on this track; Humanize adds life to it.
// Parameter indices follow core/DeviceSpecs.h (drumstudio::).

#include "Bodies.h"
#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "core/Grooves.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHideEvent>
#include <QLineEdit>
#include <QSlider>
#include <QTimer>
#include <QDrag>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <cmath>

namespace sessions::devui {

namespace {

namespace ds = drumstudio;

QHash<int, int>& studioPages() { static QHash<int, int> p; return p; }   // device id -> the page shown
QHash<int, int>& studioDrums() { static QHash<int, int> d; return d; }   // device id -> the drum selected

// Where each drum is drawn, seen from above (0 .. 1 of the display), and how big.
struct DrumSpot { double x, y, r; };
const DrumSpot kSpots[ds::kDrums] = {
    {0.50, 0.70, 0.17},   // kick
    {0.33, 0.56, 0.11},   // snare
    {0.13, 0.50, 0.10},   // hi-hat
    {0.40, 0.30, 0.095},  // tom 1
    {0.60, 0.28, 0.105},  // tom 2
    {0.80, 0.56, 0.13},   // floor tom
    {0.86, 0.24, 0.13},   // ride
    {0.18, 0.20, 0.12},   // crash
};

// The kit from above. Click a drum to select it (its knobs show) and hear it; a hit lights it.
class KitView : public Display {
public:
    KitView(DeviceBody* b, int w, int h, std::function<void(int)> selected) : Display(b, w, h), m_selected(std::move(selected)) {
        setCursor(Qt::PointingHandCursor);
        InfoView::attach(this, "Kit", "The kit from the drummer's seat: click a drum to select it (its knobs show beside the kit) and hear it. A drum lights when it's hit.");
    }
    void tick() override {
        for (int d = 0; d < ds::kDrums; ++d) m_glow[d] = std::max(double(m_body->scope(d)), m_glow[d] - .08);
        update();
    }
    int drumAt(const QPointF& pt) const {
        const QRectF r = inner();
        int best = -1; double bestD = 1e9;
        for (int d = 0; d < ds::kDrums; ++d) {
            const QPointF c(r.left() + kSpots[d].x * r.width(), r.top() + kSpots[d].y * r.height());
            const double rad = kSpots[d].r * r.height(), dist = std::hypot(pt.x() - c.x(), pt.y() - c.y());
            if (dist <= rad + 3 && dist < bestD) { best = d; bestD = dist; }
        }
        return best;
    }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setRenderHint(QPainter::Antialiasing, true);
        const int sel = studioDrums().value(m_body->ctx().deviceId, ds::Snare);
        QFont f = p.font(); f.setPixelSize(height() > 220 ? 12 : 9); p.setFont(f);
        for (int d = 0; d < ds::kDrums; ++d) {
            const QPointF c(r.left() + kSpots[d].x * r.width(), r.top() + kSpots[d].y * r.height());
            const double rad = kSpots[d].r * r.height();
            const bool cymbal = d == ds::HiHat || d == ds::Ride || d == ds::Crash;
            const bool lit = m_glow[d] > .04;
            QColor fill = cymbal ? theme::withAlpha(col::curve, 90) : theme::devCell;
            if (lit) fill = col::curve;
            p.setPen(QPen(d == sel ? col::handle : col::gridStrong, d == sel ? 1.6 : 1.0));
            p.setBrush(fill);
            p.drawEllipse(c, rad, rad);
            if (!cymbal) { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(Qt::NoBrush); p.drawEllipse(c, rad * 0.78, rad * 0.78); }
            else { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(col::display); p.drawEllipse(c, rad * 0.18, rad * 0.18); }
            if (d == ds::HiHat) { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(Qt::NoBrush); p.drawEllipse(c + QPointF(2, 2), rad, rad); }
            p.setPen(d == sel ? col::value : col::label);
            p.drawText(QRectF(c.x() - 45, c.y() + rad - 1, 90, 16), Qt::AlignCenter, ds::drumName(d));
        }
        drawCaption(p, r, m_body->text(ds::Kit), Qt::AlignTop | Qt::AlignRight);
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int d = drumAt(e->position());
        if (d < 0) return;
        studioDrums().insert(m_body->ctx().deviceId, d);
        m_selected(d);
        if (Transport* t = m_body->ctx().transport) t->previewNote(m_body->ctx().track, ds::noteFor(d), 0.9);
        update();
    }
private:
    std::function<void(int)> m_selected;
    double m_glow[ds::kDrums]{};
};

// The mics around the kit: the close mics as dots on the drums, the overhead pair above (spread
// by Width), the room pair far off (further with Room Size), each with its level; the room's
// level now as a bar.
class MicView : public Display {
public:
    MicView(DeviceBody* b, int w, int h) : Display(b, w, h) {
        InfoView::attach(this, "Mics", "How the kit is miked: the close mics on every drum, the overhead pair above the kit (Width spreads them), "
                                       "and the room pair further away (Room Size moves them back). The bar on the right is the room's level.");
    }
    void tick() override { m_room = std::max(m_body->meter(2), m_room - 1.5f); update(); }
protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF kit = r.adjusted(10, 14, -34, -8);
        // the kit, faint
        for (int d = 0; d < ds::kDrums; ++d) {
            const QPointF c(kit.left() + kSpots[d].x * kit.width(), kit.top() + 0.35 * kit.height() + kSpots[d].y * kit.height() * 0.6);
            const double rad = kSpots[d].r * kit.height() * 0.55;
            p.setPen(QPen(col::gridStrong, 0.8));
            p.setBrush(theme::devCell);
            p.drawEllipse(c, rad, rad);
            p.setBrush(col::handle);
            p.setPen(Qt::NoPen);
            p.drawEllipse(c + QPointF(rad * 0.5, -rad * 0.5), 1.8, 1.8);   // the close mic
        }
        const double width = m_body->pos(ds::Width), size = m_body->pos(ds::RoomSize);
        const double ohDb = m_body->real(ds::Overheads), roomDb = m_body->real(ds::Room);
        auto mic = [&](const QPointF& at, const QString& label, double db, const QColor& c) {
            const double a = std::clamp((db + 60) / 66.0, 0.0, 1.0);
            p.setPen(QPen(c, 1));
            p.setBrush(theme::withAlpha(c, int(60 + 180 * a)));
            p.drawEllipse(at, 4, 4);
            p.setPen(col::gridText);
            p.drawText(QRectF(at.x() - 24, at.y() + 5, 48, 10), Qt::AlignCenter, label);
        };
        QFont f = p.font(); f.setPointSizeF(6.5); p.setFont(f);
        const double spread = 0.12 + 0.3 * width;
        mic(QPointF(kit.center().x() - spread * kit.width(), kit.top() + 2), "OH L", ohDb, col::cyan);
        mic(QPointF(kit.center().x() + spread * kit.width(), kit.top() + 2), "OH R", ohDb, col::cyan);
        const double far = 0.44 + 0.06 * size;
        mic(QPointF(kit.center().x() - far * kit.width(), kit.bottom() - 4), "Room L", roomDb, col::curve);
        mic(QPointF(kit.center().x() + far * kit.width(), kit.bottom() - 4), "Room R", roomDb, col::curve);
        // the room's level
        const QRectF bar(r.right() - 20, r.top() + 10, 8, r.height() - 20);
        p.setPen(QPen(col::grid, 1)); p.setBrush(Qt::NoBrush); p.drawRect(bar);
        const double lvl = std::clamp((m_room + 60) / 60.0, 0.0, 1.0);
        p.fillRect(QRectF(bar.left() + 1, bar.bottom() - lvl * (bar.height() - 2), bar.width() - 2, lvl * (bar.height() - 2)), col::curve);
        p.setPen(col::gridText);
        p.drawText(QRectF(bar.left() - 12, bar.bottom() + 1, 32, 10), Qt::AlignCenter, "Room");
    }
private:
    float m_room = -120;
};

// The grooves of one style: drag one out (grooves::kMime), or double-click for a clip on this track.
class GrooveList : public QListWidget {
public:
    GrooveList(QWidget* parent, std::function<QPair<double, double>()> humanize) : QListWidget(parent), m_humanize(std::move(humanize)) {
        setFocusPolicy(Qt::NoFocus);
        applyStyle();
        connect(theme::notifier(), &theme::Notifier::changed, this, [this] { applyStyle(); });
        InfoView::attach(this, "Grooves", "Drum patterns in this style: drag one onto a Session slot or an Arrangement lane (a MIDI track takes it; anywhere else gets a new track with a Drum Studio), "
                                          "or double-click it (or press To Clip) for a clip in this track's next free slot. Humanize gives the hits a little life.");
    }
protected:
    void mousePressEvent(QMouseEvent* e) override { m_press = e->pos(); QListWidget::mousePressEvent(e); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || (e->pos() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        QListWidgetItem* it = currentItem();
        if (!it) return;
        const auto h = m_humanize();
        auto* drag = new QDrag(this);
        drag->setMimeData(grooves::mimeFor(it->data(Qt::UserRole).toInt(), h.first, h.second));
        drag->exec(Qt::CopyAction);
    }
private:
    void applyStyle() {
        setStyleSheet(QString("QListWidget{background:%1;color:%2;border:1px solid %3;font-size:10px;} QListWidget::item{padding:3px 4px;} QListWidget::item:selected{background:%4;color:%5;}")
            .arg(col::display.name(), col::value.name(), col::displayEdge.name(), theme::accent.name(), col::buttonOnText.name()));
    }
    std::function<QPair<double, double>()> m_humanize;
    QPoint m_press;
};

class DrumStudioBody : public DeviceBody {
public:
    DrumStudioBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        auto* root = rootRow(this);
        auto* pageCol = vbox(2);
        m_pages = new QStackedWidget(this);
        const char* names[3] = {"Kit", "Mics", "Grooves"};
        const char* helps[3] = {"The kit: pick one of six, click a drum to select it and set its Level, Pan, Tune, Decay and Tone.",
                                "The mics: the overheads, the room and its size, bleed between the mics, the stereo width, and the velocity curve.",
                                "The groove library: drum patterns in every style, to drag into your clips."};
        for (int k = 0; k < 3; ++k) {
            auto* b = new QPushButton(names[k], this);
            b->setObjectName("studioPage");
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedSize(52, 18);
            b->setStyleSheet(QString("QPushButton#studioPage{background:%1;color:%2;border:0;border-radius:2px;font-size:9px;}"
                                     "QPushButton#studioPage:checked{background:%3;color:%4;}").arg(col::button.name(), col::buttonText.name(), col::buttonOn.name(), col::buttonOnText.name()));
            InfoView::attach(b, QString("%1 page").arg(names[k]), helps[k]);
            connect(b, &QPushButton::clicked, this, [this, k] { showPage(k); });
            m_pageButtons.push_back(b);
            pageCol->addWidget(b);
        }
        auto* expand = new QPushButton("Expand", this); expand->setObjectName("studioExpand"); expand->setFixedSize(52, 22);
        connect(expand, &QPushButton::clicked, this, [this] { emit editorRequested(); }); pageCol->addWidget(expand);
        pageCol->addStretch(1);
        root->addLayout(pageCol);
        root->addWidget(vline(this));
        root->addWidget(m_pages, 1);
        m_pages->addWidget(buildKit());
        m_pages->addWidget(buildMics());
        m_pages->addWidget(buildGrooves());
        showPage(studioPages().value(c.deviceId, 0));
        showDrum(studioDrums().value(c.deviceId, ds::Snare));
        setFixedWidth(8 + 52 + 4 + 1 + 4 + m_pages->sizeHint().width() + 8);
        refresh();
    }
    DeviceBody* createEditor(QWidget* parent) override { return createDrumStudioEditor(ctx(), parent); }
    int page() const { return m_pages->currentIndex(); }
    void showPage(int k) {
        studioPages().insert(ctx().deviceId, k);
        m_pages->setCurrentIndex(k);
        for (int i = 0; i < int(m_pageButtons.size()); ++i) m_pageButtons[size_t(i)]->setChecked(i == k);
    }
    void showDrum(int d) {
        studioDrums().insert(ctx().deviceId, d);
        m_drumPages->setCurrentIndex(d);
        m_drumTitle->setText(ds::drumName(d));
        update();
    }
    // The groove chosen in the list as a clip in this track's next free slot (one undo step).
    void grooveToClip() {
        QListWidgetItem* it = m_grooves->currentItem();
        if (!it) return;
        const int index = it->data(Qt::UserRole).toInt();
        const DeviceContext& c = ctx();
        const grooves::Groove& g = grooves::all()[size_t(index)];
        int scene = -1, track = -1;
        {
            EditScope edit(c.project, "Add Groove");
            const Track* t = c.project->trackAt(c.track);
            scene = 0;
            if (t) while (scene < t->clipSlots.size() && t->clipSlots[scene]) ++scene;
            const ClipPtr clip = grooves::makeClip(*c.project, g, m_humanVel->value(), m_humanTime->value());
            track = grooves::toSession(*c.project, c.track, scene, clip);
        }
        const Track* t = c.project->trackAt(track);
        InfoView::notify(QString("%1 %2 added").arg(g.style, g.name), QString("In slot %1 of %2.").arg(scene + 1).arg(t ? t->name : QString()));
    }
private:
    QWidget* buildKit() {
        auto* page = new QWidget(m_pages);
        auto* row = hbox(6);
        row->setContentsMargins(0, 0, 0, 0);
        auto* kitView = new KitView(this, 300, 176, [this](int d) { showDrum(d); });
        animate(kitView);
        row->addWidget(kitView, 0, Qt::AlignTop);
        auto* side = vbox(3);
        side->addWidget(section("Kit"));
        side->addWidget(chooser(ds::Kit, 120));
        m_drumTitle = section("Snare");
        side->addWidget(m_drumTitle);
        m_drumPages = new QStackedWidget(page);
        for (int d = 0; d < ds::kDrums; ++d) {
            auto* dp = new QWidget(m_drumPages);
            auto* knobs = hbox(1);
            knobs->setContentsMargins(0, 0, 0, 0);
            knobs->addWidget(knob(ds::param(d, ds::Level), 24, "Level"));
            knobs->addWidget(knob(ds::param(d, ds::Pan), 24, "Pan", true));
            knobs->addWidget(knob(ds::param(d, ds::Tune), 24, "Tune", true));
            knobs->addWidget(knob(ds::param(d, ds::Decay), 24, "Decay"));
            knobs->addWidget(knob(ds::param(d, ds::Tone), 24, "Tone"));
            knobs->addWidget(toggle(ds::mute(d), "M", 20)); knobs->addWidget(toggle(ds::solo(d), "S", 20));
            dp->setLayout(knobs);
            m_drumPages->addWidget(dp);
        }
        side->addWidget(m_drumPages);
        auto* glob = hbox(1);
        glob->addWidget(knob(ds::Wires, 22, "Wires"));
        glob->addWidget(knob(ds::Beater, 22, "Beater"));
        glob->addWidget(knob(ds::Volume, 26));
        glob->addStretch(1);
        side->addLayout(glob);
        side->addStretch(1);
        row->addLayout(side);
        page->setLayout(row);
        return page;
    }
    QWidget* buildMics() {
        auto* page = new QWidget(m_pages);
        auto* row = hbox(6);
        row->setContentsMargins(0, 0, 0, 0);
        auto* view = new MicView(this, 300, 176);
        animate(view);
        row->addWidget(view, 0, Qt::AlignTop);
        auto* side = vbox(3);
        side->addWidget(section("Mics"));
        auto* r1 = hbox(1);
        r1->addWidget(knob(ds::Overheads, 26, "Overheads"));
        r1->addWidget(knob(ds::Room, 26, "Room"));
        r1->addWidget(knob(ds::RoomSize, 26, "Size"));
        side->addLayout(r1);
        auto* r2 = hbox(1);
        r2->addWidget(knob(ds::Bleed, 24, "Bleed"));
        r2->addWidget(knob(ds::Width, 24, "Width"));
        r2->addWidget(knob(ds::Velocity, 24, "Velocity"));
        side->addLayout(r2);
        side->addStretch(1);
        row->addLayout(side);
        page->setLayout(row);
        return page;
    }
    QWidget* buildGrooves() {
        auto* page = new QWidget(m_pages);
        auto* row = hbox(6);
        row->setContentsMargins(0, 0, 0, 0);
        m_styles = new QListWidget(page);
        m_styles->setFixedWidth(96);
        m_styles->setFocusPolicy(Qt::NoFocus);
        m_styles->setStyleSheet(QString("QListWidget{background:%1;border:1px solid %2;font-size:10px;} QListWidget::item{padding:1px 4px;}").arg(col::display.name(), col::displayEdge.name()));
        InfoView::attach(m_styles, "Styles", "Which style's grooves to list.");
        for (const QString& s : grooves::styles()) m_styles->addItem(s);
        row->addWidget(m_styles);
        m_grooves = new GrooveList(page, [this] { return qMakePair(m_humanVel->value(), m_humanTime->value()); });
        m_grooves->setFixedWidth(170);
        row->addWidget(m_grooves);
        auto* side = vbox(3);
        side->addWidget(section("Groove"));
        m_grooveInfo = new QLabel(page);
        m_grooveInfo->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name()));
        m_grooveInfo->setWordWrap(true);
        m_grooveInfo->setFixedWidth(120);
        side->addWidget(m_grooveInfo);
        auto human = [&](const QString& title, const QString& help) {
            auto* d = new DragValue(page);
            d->setRange(0, 1); d->setDefault(0); d->setValue(0, false); d->setMinimumChars(5);
            d->setFormatter([](double v) { return QString("%1 %").arg(int(std::lround(v * 100))); });
            InfoView::attach(d, title, help);
            return d;
        };
        m_humanVel = human("Humanize Velocity", "How much the hits vary in strength, like a player's would.");
        m_humanTime = human("Humanize Timing", "How much the hits drift off the grid, like a player's would.");
        auto* hv = hbox(4); hv->addWidget(new QLabel("Vel", page)); hv->addWidget(m_humanVel); hv->addWidget(new QLabel("Time", page)); hv->addWidget(m_humanTime); hv->addStretch(1);
        for (QLabel* l : page->findChildren<QLabel*>()) if (l != m_grooveInfo && l != m_drumTitle) l->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;border:0;").arg(col::label.name()));
        side->addLayout(hv);
        auto* toClip = new QToolButton(page);
        toClip->setObjectName("grooveToClip");
        toClip->setText("To Clip");
        toClip->setFocusPolicy(Qt::NoFocus);
        toClip->setStyleSheet("QToolButton{font-size:9px;padding:2px 10px;}");
        InfoView::attach(toClip, "To Clip", "Puts the groove in this track's next free Session slot as a MIDI clip. One undo step.");
        connect(toClip, &QToolButton::clicked, this, [this] { grooveToClip(); });
        side->addWidget(toClip, 0, Qt::AlignLeft);
        side->addStretch(1);
        row->addLayout(side);
        page->setLayout(row);
        connect(m_styles, &QListWidget::currentRowChanged, this, [this](int) { fillGrooves(); });
        connect(m_grooves, &QListWidget::currentRowChanged, this, [this](int) {
            QListWidgetItem* it = m_grooves->currentItem();
            m_grooveInfo->setText(it ? grooves::describe(grooves::all()[size_t(it->data(Qt::UserRole).toInt())]) : QString());
        });
        connect(m_grooves, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { grooveToClip(); });
        m_styles->setCurrentRow(0);
        return page;
    }
    void fillGrooves() {
        m_grooves->clear();
        QListWidgetItem* style = m_styles->currentItem();
        if (!style) return;
        for (int i : grooves::inStyle(style->text())) {
            auto* it = new QListWidgetItem(grooves::all()[size_t(i)].name, m_grooves);
            it->setData(Qt::UserRole, i);
            it->setToolTip(grooves::describe(grooves::all()[size_t(i)]) + "\nDrag onto a slot or a lane, or double-click for a clip.");
        }
        if (m_grooves->count()) m_grooves->setCurrentRow(0);
    }

    QStackedWidget* m_pages;
    std::vector<QPushButton*> m_pageButtons;
    QStackedWidget* m_drumPages = nullptr;
    QLabel* m_drumTitle = nullptr;
    QListWidget* m_styles = nullptr;
    GrooveList* m_grooves = nullptr;
    QLabel* m_grooveInfo = nullptr;
    DragValue* m_humanVel = nullptr;
    DragValue* m_humanTime = nullptr;
};

// Expanded editors live in DeviceView, share the same parameters and undo stack,
// and retain their page/selection while the compact chain is rebuilt.
class DrumWave : public Display {
public:
    DrumWave(DeviceBody* b, std::function<int()> note) : Display(b, 360, 90), m_note(std::move(note)) {
        setMinimumWidth(240); setMaximumWidth(QWIDGETSIZE_MAX);
    }
    void draw(QPainter& p, const QRectF& r) override {
        const int gm = m_note();
        const auto& s = audio::drums::library()[audio::drums::bankFor(m_body->pick(ds::Kit))].notes[audio::drums::sampleNote(gm)][3];
        if (s.mono.empty()) return;
        p.setPen(QPen(col::curve, 1));
        for (int x = 0; x < int(r.width()); ++x) {
            const size_t a = size_t(x * s.mono.size() / r.width()), b = std::min(s.mono.size(), size_t((x + 1) * s.mono.size() / r.width()) + 1);
            float lo = 0, hi = 0; for (size_t i = a; i < b; i += 2) { lo = std::min(lo, s.mono[i]); hi = std::max(hi, s.mono[i]); }
            p.drawLine(QPointF(r.left() + x, r.center().y() - hi * r.height() * .43), QPointF(r.left() + x, r.center().y() - lo * r.height() * .43));
        }
        drawCaption(p, r, QString("%1 s · 5 layers").arg(s.mono.size() / s.rate, 0, 'f', 2));
    }
private: std::function<int()> m_note;
};
class GrooveGrid : public Display {
public:
    GrooveGrid(DeviceBody* b) : Display(b, 650, 260) { setMinimumSize(440, 220); setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX); }
    int index = 0; double playhead = -1;
    double humanVel = 0, humanTime = 0;
    void draw(QPainter& p, const QRectF& r) override {
        const auto& g = grooves::all()[size_t(index)];
        const QRectF grid = r.adjusted(66, 23, -8, -12); const double row = grid.height() / ds::kDrums;
        p.setFont(QFont(p.font().family(), 8));
        for (int d = 0; d < ds::kDrums; ++d) {
            p.setPen(col::label); p.drawText(QRectF(r.left() + 5, grid.top() + d * row, 58, row), Qt::AlignVCenter, ds::drumName(d));
            p.setPen(col::grid); p.drawLine(QPointF(grid.left(), grid.top() + d * row), QPointF(grid.right(), grid.top() + d * row));
        }
        for (int s = 0; s <= g.bars * 16; ++s) {
            const double x = grid.left() + s * grid.width() / (g.bars * 16);
            p.setPen(s % 4 ? col::grid : col::gridStrong); p.drawLine(QPointF(x, grid.top()), QPointF(x, grid.bottom()));
            if (s % 4 == 0 && s < g.bars * 16) { p.setPen(col::label); p.drawText(QRectF(x + 3, r.top() + 3, 40, 16), QString("%1.%2").arg(s / 16 + 1).arg(s / 4 % 4 + 1)); }
        }
        for (const auto& n : grooves::notes(g, humanVel, humanTime)) {
            const int d = ds::drumFor(n.pitch); if (d < 0) continue;
            const double x = grid.left() + n.start / (g.bars * 4) * grid.width();
            const double h = (row - 6) * n.velocity;
            p.fillRect(QRectF(x + 1, grid.top() + (d + 1) * row - 3 - h, std::max(3., grid.width() / (g.bars * 16) - 3), h), theme::withAlpha(col::curve, int(90 + 165 * n.velocity)));
        }
        if (playhead >= 0) { p.setPen(QPen(col::handle, 1.5)); const double x = grid.left() + playhead / (g.bars * 4) * grid.width(); p.drawLine(QPointF(x, grid.top()), QPointF(x, grid.bottom())); }
    }
};
class DrumLevel : public Display {
public:
    DrumLevel(DeviceBody* b, int drum) : Display(b, 12, 140), m_drum(drum) {
        setMinimumHeight(80); setMaximumHeight(QWIDGETSIZE_MAX); b->animate(this);
    }
    void draw(QPainter& p, const QRectF& r) override {
        const double gain = std::max(.0001f, m_body->scope(8 + m_drum));
        const double level = std::clamp((20 * std::log10(gain) + 48) / 48, 0., 1.);
        p.fillRect(QRectF(r.left(), r.bottom() - r.height() * level, r.width(), r.height() * level), gain >= 1 ? theme::devMissing : col::curve);
    }
private: int m_drum;
};
class DrumStudioEditor : public DeviceBody {
public:
    DrumStudioEditor(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("drumStudioEditor"); setMinimumSize(980, 460); setMaximumHeight(QWIDGETSIZE_MAX);
        auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 6, 8, 6); root->setSpacing(6);
        auto* bar = hbox(8); bar->addWidget(section("DRUM STUDIO")); bar->addWidget(chooser(ds::Kit, 150));
        auto* source = new QLabel(this); bar->addWidget(source); bar->addStretch();
        m_refreshers.push_back([this, source] { source->setText(QString("%1 · 5 velocity layers").arg(audio::drums::sourceName(pick(ds::Kit)))); });
        m_pages = new QStackedWidget(this);
        for (int i = 0; i < 3; ++i) {
            auto* b = new QPushButton(QStringList{"Kit", "Mixer", "Grooves"}[i], this); b->setObjectName(QString("studioTab%1").arg(i)); b->setCheckable(true); m_tabs.push_back(b);
            connect(b, &QPushButton::clicked, this, [this, i] { setPage(i); }); bar->addWidget(b);
        }
        root->addLayout(bar); root->addWidget(m_pages, 1);
        m_pages->addWidget(kitPage()); m_pages->addWidget(mixerPage()); m_pages->addWidget(groovePage());
        setPage(0); selectDrum(ds::Snare); applyStyle(); refresh();
        connect(theme::notifier(), &theme::Notifier::changed, this, [this] { applyStyle(); });
        connect(c.project, &Project::restored, this, [this] { refresh(); });
        m_preview.setInterval(4); m_preview.setTimerType(Qt::PreciseTimer);
        connect(&m_preview, &QTimer::timeout, this, [this] { previewTick(); });
    }
    void refresh() override {
        m_ctx.track = ctx().project->indexOfTrack(ctx().trackId); DeviceBody::refresh();
        if (m_info && m_index >= 0) { const auto& g = grooves::all()[size_t(m_index)]; m_info->setText(QString("%1 / %2 · %3 · Set %4 BPM").arg(g.style, g.name, grooves::describe(g)).arg(ctx().project->tempo, 0, 'f', 0)); }
    }
protected:
    void hideEvent(QHideEvent* e) override { stopPreview(); DeviceBody::hideEvent(e); }
private:
    void setPage(int i) { if (i != 2) stopPreview(); m_pages->setCurrentIndex(i); for (int k = 0; k < 3; ++k) m_tabs[k]->setChecked(k == i); }
    int track() const { return ctx().project->indexOfTrack(ctx().trackId); }
    QPushButton* action(const QString& text, const QString& id, std::function<void()> fn) {
        auto* b = new QPushButton(text, this); b->setObjectName(id); connect(b, &QPushButton::clicked, this, std::move(fn)); return b;
    }
    void audition(int note) { if (ctx().transport) ctx().transport->previewNote(track(), note, m_velocity ? m_velocity->value() : .8); }
    void selectDrum(int d) {
        m_drum = d; studioDrums().insert(ctx().deviceId, d); m_drumTitle->setText(ds::drumName(d)); m_drumControls->setCurrentIndex(d);
        m_articulations->clear();
        const std::vector<std::vector<std::pair<const char*, int>>> arts{
            {{"Kick",36}}, {{"Center",38},{"Rimshot",40},{"Side stick",37},{"Clap",39}},
            {{"Closed",42},{"Pedal",44},{"Open",46}}, {{"High tom",48}}, {{"Center",45},{"Edge",47}},
            {{"Center",41},{"Edge",43}}, {{"Bow",51},{"Bell",53},{"Shank",59}},
            {{"Crash 1",49},{"Crash 2",57},{"Splash",55},{"China",52}}};
        for (const auto& a : arts[d]) m_articulations->addItem(a.first, a.second);
        m_note = ds::noteFor(d); update();
    }
    QWidget* kitPage() {
        auto* page = new QWidget(this); auto* row = new QHBoxLayout(page); row->setContentsMargins(0,0,0,0); row->setSpacing(12);
        auto* kit = new KitView(this, 500, 330, [this](int d) { selectDrum(d); }); kit->setMinimumSize(380,260); kit->setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX); animate(kit); row->addWidget(kit, 5);
        auto* side = vbox(8); m_drumTitle = section("Snare"); side->addWidget(m_drumTitle);
        auto* artRow = hbox(5); m_articulations = new QComboBox(this); m_articulations->setObjectName("studioArticulation"); artRow->addWidget(m_articulations, 1);
        connect(m_articulations, &QComboBox::activated, this, [this](int i) { m_note = m_articulations->itemData(i).toInt(); audition(m_note); update(); });
        artRow->addWidget(action("Audition", "studioAudition", [this] { audition(m_note); })); side->addLayout(artRow);
        auto* wave = new DrumWave(this, [this] { return m_note; }); watch(wave); side->addWidget(wave);
        m_drumControls = new QStackedWidget(this);
        for (int d = 0; d < ds::kDrums; ++d) {
            auto* w = new QWidget(this); auto* knobs = new QHBoxLayout(w); knobs->setContentsMargins(0,0,0,0);
            for (int k = 0; k < ds::kPerDrum; ++k) knobs->addWidget(knob(ds::param(d,k), 34, QStringList{"Level","Pan","Tune","Decay","Tone"}[k], k == ds::Pan || k == ds::Tune));
            m_drumControls->addWidget(w);
        }
        side->addWidget(m_drumControls);
        auto* lower = hbox(5); lower->addWidget(knob(ds::Wires, 28, "Wires")); lower->addWidget(knob(ds::Beater, 28, "Beater")); lower->addWidget(knob(ds::Velocity,28,"Response"));
        m_velocity = new DragValue(this); m_velocity->setRange(.05,1); m_velocity->setValue(.8,false); m_velocity->setFixedSize(72,24);
        m_velocity->setFormatter([](double v) { return QString("Vel %1").arg(int(std::lround(v * 127))); }); lower->addWidget(m_velocity); side->addLayout(lower); side->addStretch();
        row->addLayout(side, 4); return page;
    }
    QWidget* mixerPage() {
        auto* page = new QWidget(this); auto* row = new QHBoxLayout(page); row->setContentsMargins(0,0,0,0); row->setSpacing(4);
        for (int d = 0; d < ds::kDrums; ++d) {
            auto* strip = new QFrame(page); strip->setObjectName("studioStrip"); auto* col = new QVBoxLayout(strip); col->setContentsMargins(7,7,7,7); col->setSpacing(5);
            col->addWidget(action(ds::drumName(d), QString("studioHit%1").arg(d), [this,d] { audition(ds::noteFor(d)); }));
            col->addWidget(knob(ds::param(d,ds::Pan),24,"Pan",true),0,Qt::AlignHCenter);
            auto* switches = hbox(3); switches->addWidget(toggle(ds::mute(d),"M",28)); switches->addWidget(toggle(ds::solo(d),"S",28)); col->addLayout(switches);
            auto* fader = new QSlider(Qt::Vertical,strip); fader->setObjectName(QString("studioFader%1").arg(d)); fader->setRange(0,1000); fader->setProperty("mapTarget",target(ds::param(d,ds::Level))); auto* levelRow = hbox(10); levelRow->addStretch(); levelRow->addWidget(new DrumLevel(this, d), 0); levelRow->addWidget(fader, 0); levelRow->addStretch(); col->addLayout(levelRow, 1);
            connect(fader,&QSlider::valueChanged,this,[this,d](int v){ if (!m_refreshing) edit(ds::param(d,ds::Level),v / 1000.); });
            auto* value = new QLabel(strip); value->setAlignment(Qt::AlignCenter); col->addWidget(value);
            m_refreshers.push_back([this,d,fader,value] { m_refreshing=true; fader->setValue(int(pos(ds::param(d,ds::Level))*1000)); m_refreshing=false; value->setText(text(ds::param(d,ds::Level))); });
            row->addWidget(strip,1);
        }
        auto* bus = vbox(5); bus->addWidget(section("AMBIENCE")); auto* upper = hbox(2); upper->addWidget(knob(ds::Overheads,28,"Overheads")); upper->addWidget(knob(ds::Room,28,"Room")); bus->addLayout(upper);
        auto* mid = hbox(2); mid->addWidget(knob(ds::RoomSize,28,"Size")); mid->addWidget(knob(ds::Bleed,28,"Bleed")); bus->addLayout(mid);
        auto* bottom = hbox(2); bottom->addWidget(knob(ds::Width,28,"Width")); bottom->addWidget(knob(ds::Volume,30,"Output")); bus->addLayout(bottom); bus->addStretch(); row->addLayout(bus); return page;
    }
    QWidget* groovePage() {
        auto* page = new QWidget(this); auto* row = new QHBoxLayout(page); row->setContentsMargins(0,0,0,0); row->setSpacing(8);
        auto* library = vbox(5); m_style = new QComboBox(page); m_style->setObjectName("studioStyles"); m_style->addItem("All styles"); m_style->addItems(grooves::styles()); library->addWidget(m_style);
        m_search = new QLineEdit(page); m_search->setObjectName("studioGrooveSearch"); m_search->setPlaceholderText("Search grooves"); library->addWidget(m_search);
        m_list = new GrooveList(page,[this] { return QPair<double,double>{m_humanVel->value(),m_humanTime->value()}; }); m_list->setObjectName("studioGrooveList"); m_list->setMinimumWidth(200); library->addWidget(m_list,1);
        row->addLayout(library,1); auto* detail = vbox(6); m_info = new QLabel(page); detail->addWidget(m_info);
        m_grid = new GrooveGrid(this); detail->addWidget(m_grid,1);
        auto* tools = hbox(6); m_play = action("Preview", "studioPreview",[this] { if (m_play->isChecked()) startPreview(); else stopPreview(); }); m_play->setCheckable(true); tools->addWidget(m_play);
        tools->addWidget(action("To Clip", "studioToClip",[this] { toClip(); }));
        tools->addWidget(action("Export MIDI…", "studioExportMidi",[this] {
            if (m_index < 0) return; const auto& g=grooves::all()[size_t(m_index)];
            QString path=QFileDialog::getSaveFileName(this,"Export Groove",QString("%1 - %2.mid").arg(g.style,g.name),"MIDI (*.mid)");
            if (path.isEmpty()) return; if (!path.endsWith(".mid",Qt::CaseInsensitive)) path+=".mid"; QString error;
            if (!grooves::exportMidi(path,g,m_humanVel->value(),m_humanTime->value(),&error)) InfoView::notify("MIDI export failed",error);
        })); tools->addStretch();
        m_humanVel=new DragValue(page); m_humanTime=new DragValue(page);
        for (auto* v:{m_humanVel,m_humanTime}) { v->setRange(0,1); v->setValue(0,false); v->setFixedSize(64,24); v->setFormatter([](double x){return QString("%1 %").arg(int(x*100));}); }
        tools->addWidget(new QLabel("Velocity",page)); tools->addWidget(m_humanVel); tools->addWidget(new QLabel("Timing",page)); tools->addWidget(m_humanTime); detail->addLayout(tools); row->addLayout(detail,4);
        connect(m_style,&QComboBox::currentIndexChanged,this,[this] { filterGrooves(); }); connect(m_search,&QLineEdit::textChanged,this,[this] { filterGrooves(); });
        connect(m_list,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* it){ if (!it) return; m_index=it->data(Qt::UserRole).toInt(); m_grid->index=m_index; const auto& g=grooves::all()[size_t(m_index)]; m_info->setText(QString("%1 / %2 · %3 · Set %4 BPM").arg(g.style,g.name,grooves::describe(g)).arg(ctx().project->tempo,0,'f',0)); m_grid->update(); if(m_play->isChecked()) startPreview(); });
        connect(m_list,&QListWidget::itemDoubleClicked,this,[this]{toClip();});
        for (auto* v:{m_humanVel,m_humanTime}) connect(v,&DragValue::valueEdited,this,[this]{m_grid->humanVel=m_humanVel->value();m_grid->humanTime=m_humanTime->value();m_grid->update();if(m_play->isChecked())startPreview();});
        filterGrooves(); return page;
    }
    void filterGrooves() {
        stopPreview(); m_list->clear();
        for (int i=0;i<int(grooves::all().size());++i) { const auto& g=grooves::all()[size_t(i)];
            if (m_style->currentIndex()>0 && m_style->currentText()!=g.style) continue;
            if (!QString("%1 %2").arg(g.style,g.name).contains(m_search->text(),Qt::CaseInsensitive)) continue;
            auto* it=new QListWidgetItem(QString("%1 · %2").arg(g.style,g.name),m_list);it->setData(Qt::UserRole,i);
        }
        const bool any=m_list->count()>0; m_index=-1; m_grid->setVisible(any); m_play->setEnabled(any);
        if(any)m_list->setCurrentRow(0);else m_info->setText("No matching grooves");
    }
    void toClip() {
        if(m_index<0)return;stopPreview();const auto& g=grooves::all()[size_t(m_index)];
        EditScope edit(ctx().project,"Add Groove"); const int t=track(); const auto* tr=ctx().project->trackAt(t); int scene=0;
        if(tr)while(scene<tr->clipSlots.size()&&tr->clipSlots[scene])++scene;
        grooves::toSession(*ctx().project,t,scene,grooves::makeClip(*ctx().project,g,m_humanVel->value(),m_humanTime->value()));
    }
    void startPreview(){
        if(m_index<0 || !ctx().transport)return;
        m_notes=grooves::notes(grooves::all()[size_t(m_index)],m_humanVel->value(),m_humanTime->value());m_next=0;m_cycle=0;m_clock.restart();m_preview.start();m_play->setChecked(true);previewTick();
    }
    void stopPreview(){m_preview.stop();if(m_play)m_play->setChecked(false);if(m_grid){m_grid->playhead=-1;m_grid->update();}}
    void previewTick(){
        if(!isVisible() || track()!=ctx().project->selectedTrack){stopPreview();return;}
        const auto& g=grooves::all()[size_t(m_index)]; const double length=g.bars*4.; const double beat=m_clock.elapsed()*.001*ctx().project->tempo/60.;
        const int cycle=int(beat/length);if(cycle!=m_cycle){m_cycle=cycle;m_next=0;}
        const double phase=std::fmod(beat,length);
        while(m_next<m_notes.size()&&m_notes[m_next].start<=phase){const auto& n=m_notes[m_next++];ctx().transport->previewNote(track(),n.pitch,n.velocity);}
        m_grid->playhead=phase;m_grid->update();
    }
    void applyStyle(){ setStyleSheet(QString(
        "QWidget#drumStudioEditor{background:%1;color:%2;} QLabel{color:%2;background:transparent;border:0;font-size:11px;}"
        "QFrame#studioStrip{background:%3;border:1px solid %4;border-radius:2px;}"
        "QPushButton{background:%5;color:%2;border:1px solid %4;border-radius:2px;padding:4px 8px;font-size:11px;}"
        "QPushButton:checked{background:%6;color:%7;} QComboBox,QLineEdit{background:%3;color:%2;border:1px solid %4;border-radius:2px;min-height:24px;padding:0 5px;}"
        "QListWidget{background:%3;color:%2;border:1px solid %4;font-size:11px;} QListWidget::item{padding:5px;} QListWidget::item:selected{background:%6;color:%7;}"
        "QSlider::groove:vertical{background:%3;width:5px;}QSlider::handle:vertical{background:%2;height:8px;margin:0 -7px;}"
        ).arg(theme::panel.name(),col::value.name(),col::display.name(),col::separator.name(),col::button.name(),theme::accent.name(),col::buttonOnText.name())); }
    QStackedWidget* m_pages=nullptr; QStackedWidget* m_drumControls=nullptr; QLabel* m_drumTitle=nullptr;
    QComboBox* m_articulations=nullptr; QComboBox* m_style=nullptr; QLineEdit* m_search=nullptr;
    GrooveList* m_list=nullptr; GrooveGrid* m_grid=nullptr; QLabel* m_info=nullptr; QPushButton* m_play=nullptr;
    DragValue* m_velocity=nullptr;DragValue* m_humanVel=nullptr;DragValue* m_humanTime=nullptr;
    std::vector<QPushButton*> m_tabs; int m_drum=ds::Snare,m_note=38,m_index=0,m_next=0,m_cycle=0;bool m_refreshing=false;
    QTimer m_preview;QElapsedTimer m_clock;QVector<Note> m_notes;
};

} // namespace
DeviceBody* createDrumStudioEditor(const DeviceContext& ctx, QWidget* parent) { return new DrumStudioEditor(ctx, parent); }
DeviceBody* createDrumStudioBody(const DeviceContext& ctx, QWidget* parent) { return new DrumStudioBody(ctx, parent); }
} // namespace sessions::devui
