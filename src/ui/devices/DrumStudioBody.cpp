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
#include "core/Grooves.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include <QApplication>
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
        const float hits = m_body->meter(1);
        if (hits != m_hits) { m_hits = hits; m_lit = int(m_body->meter(0)); m_glow = 1.0; }
        else m_glow = std::max(0.0, m_glow - 0.1);
        if (m_glow > 0) update();
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
        QFont f = p.font(); f.setPointSizeF(6.5); p.setFont(f);
        for (int d = 0; d < ds::kDrums; ++d) {
            const QPointF c(r.left() + kSpots[d].x * r.width(), r.top() + kSpots[d].y * r.height());
            const double rad = kSpots[d].r * r.height();
            const bool cymbal = d == ds::HiHat || d == ds::Ride || d == ds::Crash;
            const bool lit = m_lit == d && m_glow > 0;
            QColor fill = cymbal ? theme::withAlpha(col::curve, 90) : theme::devCell;
            if (lit) fill = col::curve;
            p.setPen(QPen(d == sel ? col::handle : col::gridStrong, d == sel ? 1.6 : 1.0));
            p.setBrush(fill);
            p.drawEllipse(c, rad, rad);
            if (!cymbal) { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(Qt::NoBrush); p.drawEllipse(c, rad * 0.78, rad * 0.78); }
            else { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(col::display); p.drawEllipse(c, rad * 0.18, rad * 0.18); }
            if (d == ds::HiHat) { p.setPen(QPen(col::gridStrong, 0.8)); p.setBrush(Qt::NoBrush); p.drawEllipse(c + QPointF(2, 2), rad, rad); }
            p.setPen(d == sel ? col::value : col::gridText);
            p.drawText(QRectF(c.x() - 30, c.y() + rad - 1, 60, 11), Qt::AlignCenter, ds::drumName(d));
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
    float m_hits = 0;
    int m_lit = -1;
    double m_glow = 0;
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
        setStyleSheet(QString("QListWidget{background:%1;border:1px solid %2;font-size:10px;} QListWidget::item{padding:1px 4px;}").arg(col::display.name(), col::displayEdge.name()));
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

} // namespace

DeviceBody* createDrumStudioBody(const DeviceContext& ctx, QWidget* parent) { return new DrumStudioBody(ctx, parent); }

} // namespace sessions::devui
