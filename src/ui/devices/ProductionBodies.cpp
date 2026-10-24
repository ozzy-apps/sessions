#include "Bodies.h"
#include "audio/ProductionEffects.h"
#include "core/Model.h"
#include "core/ModulationCurve.h"
#include "ui/InfoView.h"
#include "widgets/DragValue.h"
#include <QGridLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QWheelEvent>

namespace sessions::devui {
namespace {
using T = DeviceType;
class SpectrumPlot : public Display {
  public:
    SpectrumPlot(DeviceBody* b, int w, int h) : Display(b, w, h) {
        b->animate(this);
    }

  protected:
    void spectra(QPainter& p, const QRectF& r, bool input) {
        QPainterPath path;
        for (int b = 0; b < 256; ++b) {
            double x = r.left() + r.width() * b / 255.,
                   y = r.bottom() - std::clamp((m_body->spectrum(b, !input) + 96) / 96., 0., 1.) * r.height();
            if (b == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        p.setPen(QPen(input ? col::label : col::cyan, 1));
        p.drawPath(path);
    }
    void draw(QPainter& p, const QRectF& r) override {
        drawFreqGrid(p, r, 48);
        spectra(p, r, false);
        p.setPen(col::gridText);
        p.drawText(r.adjusted(5, 3, -4, -2), Qt::AlignTop | Qt::AlignRight, "dBFS");
        for (int db : {0, -24, -48, -72})
            p.drawText(QPointF(r.left() + 3, r.bottom() - (db + 96) / 96. * r.height() + 10), QString::number(db));
    }
};
class ParametricPlot final : public SpectrumPlot {
    int selected = 0, drag = -1;
    std::function<void(int)> select;

  public:
    ParametricPlot(DeviceBody* b, int w, int h, std::function<void(int)> s)
        : SpectrumPlot(b, w, h), select(std::move(s)) {
        setCursor(Qt::SizeAllCursor);
        InfoView::attach(this, "Parametric EQ",
                         "Drag a band to change frequency and gain. Wheel changes Q. Select a band below for its "
                         "filter, channel and slope. Audition isolates a band's frequency range.");
    }
    void setSelected(int b) {
        selected = b;
        update();
    }

  protected:
    QPointF point(int b, const QRectF& r) const {
        return {freqToX(m_body->real(parametric::param(b, parametric::Frequency)), r),
                r.center().y() - m_body->real(parametric::param(b, parametric::Gain)) / 48 * r.height()};
    }
    void draw(QPainter& p, const QRectF& r) override {
        using namespace parametric;
        drawFreqGrid(p, r, 24);
        if (m_body->on(Analyze)) {
            spectra(p, r, true);
            spectra(p, r, false);
        }
        const auto params = m_body->positions();
        for (int c = 0; c < 2; ++c) {
            QPainterPath path;
            float response[256];
            audio::parametricResponseCurve(params.data(), response, 256, 48000, c);
            for (int x = 0; x < 256; ++x) {
                double db = response[x];
                QPointF pt(r.left() + x / 255. * r.width(), r.center().y() - db / 48 * r.height());
                if (x == 0)
                    path.moveTo(pt);
                else
                    path.lineTo(pt);
            }
            p.setPen(QPen(c ? col::cyan : col::curve, 1.5));
            p.drawPath(path);
        }
        for (int b = 0; b < 8; ++b)
            drawHandle(p, point(b, r), b == selected ? col::curve : col::cyan, QString::number(b + 1),
                       !m_body->on(param(b, Enabled)));
    }
    void mousePressEvent(QMouseEvent* e) override {
        double d = 10000;
        for (int b = 0; b < 8; ++b) {
            double v = QLineF(e->position(), point(b, inner())).length();
            if (v < d) {
                d = v;
                drag = b;
            }
        }
        if (d > 25)
            drag = -1;
        if (drag >= 0) {
            selected = drag;
            select(drag);
            mouseMoveEvent(e);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (drag < 0)
            return;
        using namespace parametric;
        const auto r = inner();
        m_body->editMany(
            {{param(drag, Frequency), m_body->posFor(param(drag, Frequency), xToFreq(e->position().x(), r))},
             {param(drag, Gain),
              m_body->posFor(param(drag, Gain), (r.center().y() - e->position().y()) / r.height() * 48)}},
            900 + drag);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        drag = -1;
    }
    void wheelEvent(QWheelEvent* e) override {
        int p = parametric::param(selected, parametric::Q);
        m_body->edit(p, m_body->pos(p) + e->angleDelta().y() / 6000.);
        e->accept();
    }
};
class ParametricBody final : public DeviceBody {
  public:
    ParametricBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        using namespace parametric;
        setObjectName("parametricEqBody");
        setFixedWidth(850);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 4, 8, 4);
        root->setSpacing(2);
        auto* pages = new QStackedWidget(this);
        pages->setFixedHeight(61);
        auto* plot = new ParametricPlot(this, 834, 99, [pages](int b) { pages->setCurrentIndex(b); });
        root->addWidget(plot);
        auto* tabs = hbox(2);
        for (int b = 0; b < 8; ++b) {
            auto* button = new QToolButton(this);
            button->setText(QString::number(b + 1));
            button->setFixedSize(32, 18);
            connect(button, &QToolButton::clicked, this, [plot, pages, b] {
                plot->setSelected(b);
                pages->setCurrentIndex(b);
            });
            tabs->addWidget(button);
        }
        tabs->addStretch();
        tabs->addWidget(toggle(Analyze, "Analyze", 68));
        tabs->addWidget(section("Audition"));
        tabs->addWidget(chooser(Audition, 86));
        root->addLayout(tabs);
        for (int b = 0; b < 8; ++b) {
            auto* page = new QWidget(pages);
            auto* row = hbox(4);
            page->setLayout(row);
            row->addWidget(toggle(param(b, Enabled), "On", 32));
            row->addWidget(chooser(param(b, Type), 102));
            for (auto field : {Frequency, Gain, Q})
                row->addWidget(knob(param(b, field), 20, field == Frequency ? "Freq" : field == Gain ? "Gain" : "Q"));
            row->addWidget(chooser(param(b, Channel), 74));
            row->addWidget(chooser(param(b, Slope), 70));
            row->addStretch();
            row->addWidget(knob(Output, 20));
            pages->addWidget(page);
        }
        root->addWidget(pages);
    }
};
class SpectrumBody final : public DeviceBody {
  public:
    SpectrumBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("spectrumBody");
        setFixedWidth(570);
        auto* row = rootRow(this);
        row->addWidget(new SpectrumPlot(this, 470, 178));
        auto* col = vbox();
        col->addWidget(knob(0, 24));
        col->addWidget(toggle(1, "Hold", 62));
        col->addStretch();
        row->addLayout(col);
    }
};
class DynamicsMeter : public Display {
    int band;

  public:
    DynamicsMeter(DeviceBody* b, int bn) : Display(b, 182, 22), band(bn) {
        b->animate(this);
    }

  protected:
    void draw(QPainter& p, const QRectF& r) override {
        float gain = m_body->scope(band);
        p.fillRect(QRectF(r.left(), r.top(), r.width() * std::clamp((gain + 36) / 72., 0., 1.), r.height()),
                   col::cyanFill);
        p.setPen(col::value);
        p.drawText(r, Qt::AlignCenter, QString::number(gain, 'f', 1) + " dB");
    }
};
class MultibandBody final : public DeviceBody {
  public:
    MultibandBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        using namespace multiband;
        setObjectName("multibandBody");
        setFixedWidth(760);
        auto* root = rootRow(this, 8);
        for (int b = 0; b < 3; ++b) {
            auto* col = vbox(2);
            col->addWidget(section(b == 0 ? "Low" : b == 1 ? "Mid" : "High"));
            col->addWidget(new DynamicsMeter(this, b));
            auto* grid = new QGridLayout;
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setSpacing(1);
            const char* labels[] = {"Above", "Ratio", "Below", "Ratio", "Attack", "Release", "Gain"};
            for (int f = 0; f < 7; ++f)
                grid->addWidget(knob(param(b, Band(f)), 18, labels[f]), f / 3, f % 3);
            col->addLayout(grid);
            auto* buttons = hbox(3);
            buttons->addWidget(toggle(param(b, Enabled), "On", 50));
            buttons->addWidget(toggle(param(b, Solo), "Solo", 50));
            col->addLayout(buttons);
            col->addStretch();
            root->addLayout(col);
        }
        auto* col = vbox(2);
        col->addWidget(section("Crossover / Output"));
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(1);
        for (int i = 0; i < kBase; ++i)
            grid->addWidget(knob(i, 18, i == 0 ? "Low" : i == 1 ? "High" : i == 3 ? "Mix" : QString()), i / 2, i % 2);
        col->addLayout(grid);
        col->addStretch();
        root->addLayout(col);
    }
};
class ModPlot final : public Display {
    bool drawing = false;

  public:
    ModPlot(DeviceBody* b) : Display(b, 260, 84) {
        b->animate(this);
        if (b->ctx().type == T::Shaper)
            setCursor(Qt::CrossCursor);
    }

  protected:
    void draw(QPainter& p, const QRectF& r) override {
        p.setPen(QPen(col::grid, 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        const Device* d = m_body->device();
        if (d && m_body->ctx().type == T::Shaper) {
            QPainterPath path;
            for (int i = 0; i <= 256; ++i) {
                double x = i / 256., y = d->curves.isEmpty() || d->curves[0].isEmpty()
                                             ? x
                                             : modcurve::value(d->curves[0].data(), d->curves[0].size(), x);
                QPointF at(r.left() + x * r.width(), r.bottom() - y * r.height());
                if (!i)
                    path.moveTo(at);
                else
                    path.lineTo(at);
            }
            p.setPen(QPen(col::curve, 1.5));
            p.drawPath(path);
        }
        double value = m_body->scope(0);
        p.fillRect(QRectF(r.left(), r.bottom() - value * r.height(), r.width(), 2), col::cyan);
        drawCaption(p, r, QString::number(value * 100, 'f', 0) + " %");
    }
    void mousePressEvent(QMouseEvent* e) override {
        drawing = m_body->ctx().type == T::Shaper;
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!drawing)
            return;
        auto* p = m_body->ctx().project;
        EditScope edit(p, "Draw Shaper", deviceMergeKey(m_body->ctx().deviceId, 1000));
        auto* d = findDevice(p, m_body->ctx().track, m_body->ctx().deviceId);
        if (!d)
            return;
        if (d->curves.isEmpty())
            d->curves.resize(1);
        auto& curve = d->curves[0];
        if (curve.size() != 33) {
            curve.clear();
            for (int i = 0; i <= 32; ++i)
                curve.push_back({i / 32., i / 32.});
        }
        const auto r = inner();
        int index = std::clamp(int(std::round((e->position().x() - r.left()) / r.width() * 32)), 0, 32);
        curve[index].y = std::clamp((r.bottom() - e->position().y()) / r.height(), 0., 1.);
        emit p->deviceChanged();
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        drawing = false;
    }
};
class ModBody final : public DeviceBody {
    QVBoxLayout* mappings = nullptr;
    QWidget* list = nullptr;
    void add(int id, int param) {
        EditScope edit(m_ctx.project, "Map Modulator");
        auto* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
        if (!d)
            return;
        for (const auto& m : d->maps)
            if (m.deviceId == id && m.param == param)
                return;
        if (auto* track = m_ctx.project->trackAt(m_ctx.track))
            for (auto& source : track->devices)
                source.maps.removeIf([id, param](const MacroMap& m) { return m.deviceId == id && m.param == param; });
        d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
        d->maps.push_back({0, id, param, 0, 1});
        emit m_ctx.project->structureChanged();
    }
    void menu() {
        QMenu menu(this);
        const Track* t = m_ctx.project->trackAt(m_ctx.track);
        if (!t)
            return;
        auto* mixer = menu.addMenu("Mixer");
        mixer->addAction("Volume", this, [this] { add(0, 0); });
        if (!m_ctx.project->isMaster(m_ctx.track)) {
            mixer->addAction("Pan", this, [this] { add(0, 1); });
            for (int i = 0; m_ctx.track < m_ctx.project->tracks.size() && i < m_ctx.project->returns.size(); ++i)
                mixer->addAction("Send " + m_ctx.project->returns[i].name, this, [this, i] { add(0, 2 + i); });
        }
        for (const Device& d : t->devices) {
            if (d.id == m_ctx.deviceId || modulator::supports(deviceType(d.name)))
                continue;
            auto* sub = menu.addMenu(d.name);
            for (int i = 0; i < d.params.size(); ++i)
                sub->addAction(d.params[i].name, this, [this, id = d.id, i] { add(id, i); });
        }
        menu.exec(QCursor::pos());
    }

  public:
    ModBody(const DeviceContext& c, QWidget* parent) : DeviceBody(c, parent) {
        setObjectName("modulatorBody");
        const bool isFollower = c.type == T::EnvelopeFollower;
        setFixedWidth(isFollower ? 810 : 670);
        auto* root = rootRow(this);
        auto* col = vbox();
        col->addWidget(new ModPlot(this));
        auto* row = hbox(2);
        if (!isFollower)
            row->addWidget(chooser(modulator::Rate, 90));
        if (!isFollower)
            row->addWidget(chooser(modulator::Shape, 90));
        auto* map = new QPushButton("Map", this);
        map->setFixedHeight(22);
        connect(map, &QPushButton::clicked, this, [this] { menu(); });
        row->addWidget(map);
        col->addLayout(row);
        auto* knobs = hbox(0);
        for (int i : {modulator::Hz, modulator::Depth, modulator::Offset, modulator::Phase})
            if (!isFollower || i == modulator::Depth || i == modulator::Offset)
                knobs->addWidget(knob(modulator::param(c.type, modulator::Param(i)), 20));
        col->addLayout(knobs);
        root->addLayout(col);
        {
            auto* follower = vbox();
            for (int i : {modulator::Rise, modulator::Fall, modulator::Gain})
                follower->addWidget(knob(modulator::param(c.type, modulator::Param(i)), 18));
            follower->addStretch();
            root->addLayout(follower);
        }
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setFixedSize(300, 176);
        list = new QWidget(scroll);
        mappings = new QVBoxLayout(list);
        mappings->setContentsMargins(2, 2, 2, 2);
        mappings->setSpacing(2);
        scroll->setWidget(list);
        root->addWidget(scroll);
        if (isFollower)
            root->addLayout(createSidechainSection(this, c.type));
        const Device* d = device();
        const Track* t = m_ctx.project->trackAt(m_ctx.track);
        if (d && t)
            for (int i = 0; i < d->maps.size(); ++i) {
                const auto& m = d->maps[i];
                QString label = m.deviceId     ? "Missing"
                                : m.param == 0 ? "Volume"
                                : m.param == 1 ? "Pan"
                                               : "Send " + QString::number(m.param - 1);
                for (const auto& target : t->devices)
                    if (target.id == m.deviceId)
                        label = target.name + " / " + target.params.value(m.param).name;
                auto* row = hbox(2);
                auto* text = new QLabel(label, list);
                text->setFixedWidth(126);
                text->setToolTip(label);
                row->addWidget(text);
                for (int bound = 0; bound < 2; ++bound) {
                    auto* v = new DragValue(list);
                    v->setRange(0, 1);
                    v->setValue(bound ? m.hi : m.lo, false);
                    v->setFixedWidth(48);
                    v->setFormatter([](double x) { return QString::number(x * 100, 'f', 0) + "%"; });
                    connect(v, &DragValue::valueEdited, this,
                            [this, i, bound](double x) { setMacroRange(i, bound, x); });
                    row->addWidget(v);
                }
                auto* remove = new QToolButton(list);
                remove->setText("×");
                remove->setFixedSize(20, 20);
                connect(remove, &QToolButton::clicked, this, [this, i] { removeMacroMap(i); });
                row->addWidget(remove);
                mappings->addLayout(row);
            }
        mappings->addStretch();
    }
};
} // namespace
DeviceBody* createProductionBody(const DeviceContext& c, QWidget* p) {
    switch (c.type) {
    case T::ParametricEQ:
        return new ParametricBody(c, p);
    case T::Spectrum:
        return new SpectrumBody(c, p);
    case T::MultibandDynamics:
        return new MultibandBody(c, p);
    case T::Lfo:
    case T::EnvelopeFollower:
    case T::Shaper:
        return new ModBody(c, p);
    default:
        return createStudioBody(c, p);
    }
}
} // namespace sessions::devui
