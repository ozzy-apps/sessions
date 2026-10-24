#include "DeviceBody.h"
#include "Bodies.h"
#include "audio/Dsp.h"
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Scales.h"
#include "core/Transport.h"
#include "ui/InfoView.h"
#include "widgets/Knob.h"
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace sessions::devui {

Device* findDevice(Project* project, int track, int id) {
    Track* t = project->trackAt(track);
    if (!t) return nullptr;
    for (Device& d : t->devices) if (d.id == id) return &d;
    return nullptr;
}

namespace {
QHash<int, int>& drumPadSelection() { static QHash<int, int> s; return s; }   // device id -> pad
}

int  selectedDrumPad(int deviceId) { return drumPadSelection().value(deviceId, 0); }
void selectDrumPad(int deviceId, int pad) {
    pad = std::clamp(pad, 0, drumrack::kPads - 1);
    if (drumPadSelection().value(deviceId, 0) == pad && drumPadSelection().contains(deviceId)) return;
    drumPadSelection().insert(deviceId, pad);
    emit rackSignals()->chainSelected(deviceId, pad);   // the Device View shows that pad's devices
}

namespace {
QHash<int, int>& chainSelection() { static QHash<int, int> s; return s; }   // rack id -> chain
}

int  selectedChain(int rackId) { return chainSelection().value(rackId, 0); }
void selectChain(int rackId, int chain) {
    if (chainSelection().value(rackId, 0) == chain && chainSelection().contains(rackId)) return;
    chainSelection().insert(rackId, std::max(0, chain));
    emit rackSignals()->chainSelected(rackId, chain);
}
RackSignals* rackSignals() { static RackSignals s; return &s; }

namespace {
QHash<int, int>& zoneState() { static QHash<int, int> s; return s; }   // rack id -> 1 + mode while shown, -(1 + mode) while hidden
}
bool zonesShown(int rackId) { return zoneState().value(rackId, 0) > 0; }
void showZones(int rackId, bool on) { const int m = zoneMode(rackId) + 1; zoneState().insert(rackId, on ? m : -m); }
int  zoneMode(int rackId) { return std::abs(zoneState().value(rackId, 1)) - 1; }
void setZoneMode(int rackId, int which) {
    which = std::clamp(which, 0, int(ChainZones::Select));
    zoneState().insert(rackId, zonesShown(rackId) ? which + 1 : -(which + 1));
}

QString drumPadLabel(const Device& drum, int pad) {
    const int note = drumrack::kFirstNote + pad;
    QString s = QString("%1%2").arg(noteName(note)).arg(note / 12 - 2);
    const QString name = pad >= 0 && pad < drum.pads.size() ? drum.pads[pad].name : QString();
    return name.isEmpty() ? s : s + " " + name;
}

QList<const Device*> padDevices(const Project* project, int track, int drumId, int pad) {
    QList<const Device*> out;
    const Track* t = project ? project->trackAt(track) : nullptr;
    if (t) for (const Device& d : t->devices) if (d.rack == drumId && d.chain == pad) out << &d;
    return out;
}

const Device* padInstrument(const Project* project, int track, int drumId, int pad) {
    for (const Device* d : padDevices(project, track, drumId, pad)) if (d->kind == DeviceKind::Instrument) return d;
    return nullptr;
}

namespace {

// The knob cells' label and value styles, from the theme (panels are rebuilt when it changes).
QString kLabelStyle() { return QString("color:%1;font-size:9px;background:transparent;border:0;").arg(theme::devLabel.name()); }
QString kValueStyle() { return QString("color:%1;font-size:9px;background:transparent;border:0;").arg(theme::devValue.name()); }
QString kMacroValueStyle() { return QString("color:#7fd67f;font-size:9px;background:transparent;border:0;"); }   // turned by a Macros knob
QString kModValueStyle() { return QString("color:%1;font-size:9px;background:transparent;border:0;").arg(theme::devCyan.name()); }   // modulated by the device itself

QFont smallFont(const QWidget* w, double pt = 7.0, bool bold = false) {
    QFont f = w->font();
    f.setPointSizeF(pt);
    f.setBold(bold);
    return f;
}

// ---- Toggle: a small lit button
class ToggleButton : public QWidget {
public:
    ToggleButton(DeviceBody* body, int i, const QString& text, int width) : QWidget(body), m_body(body), m_i(i), m_text(text) {
        setFixedSize(width > 0 ? width : std::max(26, fontMetrics().horizontalAdvance(text) + 12), 16);
        setCursor(Qt::PointingHandCursor);
        setProperty("mapTarget", body->target(i));
        InfoView::attach(this, body->name(i), "Click to switch it on or off.");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool on = m_body->on(m_i);
        p.setPen(QPen(col::separator, 1));
        p.setBrush(on ? col::buttonOn : col::button);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        p.setPen(on ? col::buttonOnText : col::buttonText);
        p.setFont(smallFont(this, 7, true));
        p.drawText(rect(), Qt::AlignCenter, m_text);
    }
    void mousePressEvent(QMouseEvent*) override { m_body->edit(m_i, m_body->on(m_i) ? 0.0 : 1.0); }
private:
    DeviceBody* m_body; int m_i; QString m_text;
};

// ---- Choice strip: segmented buttons (text or icons)
class ChoiceStrip : public QWidget {
public:
    ChoiceStrip(DeviceBody* body, int i, std::vector<QString> labels, IconFn icon, int width, int height)
        : QWidget(body), m_body(body), m_i(i), m_labels(std::move(labels)), m_icon(std::move(icon)) {
        const ParamSpec& ps = specOf(body->ctx().type)->params[size_t(i)];
        m_n = int(ps.choices.size());
        if (m_labels.empty()) for (const char* c : ps.choices) m_labels.push_back(QString::fromUtf8(c));
        int w = width;
        if (w <= 0) {
            QFontMetrics fm(smallFont(this, 7, true));
            int seg = 18;
            for (const QString& l : m_labels) seg = std::max(seg, fm.horizontalAdvance(l) + 10);
            w = m_icon ? m_n * 22 : seg * m_n;
        }
        setFixedSize(w, height);
        setCursor(Qt::PointingHandCursor);
        setProperty("mapTarget", body->target(i));
        InfoView::attach(this, body->name(i), "Click a choice. The mouse wheel steps through them.");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int cur = m_body->pick(m_i);
        const QRectF all = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(col::separator, 1));
        p.setBrush(col::button);
        p.drawRoundedRect(all, 2, 2);
        p.setFont(smallFont(this, 7, true));
        for (int k = 0; k < m_n; ++k) {
            const QRectF seg(all.left() + all.width() * k / m_n, all.top(), all.width() / m_n, all.height());
            if (k == cur) {
                p.setPen(Qt::NoPen);
                p.setBrush(col::buttonOn);
                p.drawRoundedRect(seg.adjusted(0.5, 0.5, -0.5, -0.5), 1.5, 1.5);
            }
            if (k > 0 && k != cur && k - 1 != cur) {
                p.setPen(QPen(col::separator, 1));
                p.drawLine(QPointF(seg.left(), seg.top() + 2), QPointF(seg.left(), seg.bottom() - 2));
            }
            const QColor c = k == cur ? col::buttonOnText : col::buttonText;
            if (m_icon) m_icon(p, seg.adjusted(4, 3, -4, -3), k, c);
            else { p.setPen(c); p.drawText(seg, Qt::AlignCenter, k < int(m_labels.size()) ? m_labels[size_t(k)] : QString()); }
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int k = std::clamp(int(e->position().x() * m_n / std::max(1, width())), 0, m_n - 1);
        m_body->edit(m_i, choicePosition(k, m_n));
    }
    void wheelEvent(QWheelEvent* e) override {
        const int step = e->angleDelta().y() > 0 ? 1 : e->angleDelta().y() < 0 ? -1 : 0;
        if (step) m_body->edit(m_i, choicePosition(std::clamp(m_body->pick(m_i) + step, 0, m_n - 1), m_n));
        e->accept();
    }
private:
    DeviceBody* m_body; int m_i, m_n = 1;
    std::vector<QString> m_labels;
    IconFn m_icon;
};

// ---- Chooser: a dark field with the current choice; click for the list
class Chooser : public QWidget {
public:
    Chooser(DeviceBody* body, int i, int width) : QWidget(body), m_body(body), m_i(i) {
        const ParamSpec& ps = specOf(body->ctx().type)->params[size_t(i)];
        int w = width;
        if (w <= 0) {
            QFontMetrics fm(smallFont(this, 7.5));
            w = 30;
            for (const char* c : ps.choices) w = std::max(w, fm.horizontalAdvance(QString::fromUtf8(c)) + 22);
        }
        setFixedSize(w, 16);
        setCursor(Qt::PointingHandCursor);
        setProperty("mapTarget", body->target(i));
        InfoView::attach(this, body->name(i), "Click to choose. The mouse wheel steps through the choices.");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(col::displayEdge, 1));
        p.setBrush(theme::inset);
        p.drawRoundedRect(r, 2, 2);
        p.setFont(smallFont(this, 7.5));
        p.setPen(col::value);
        p.drawText(r.adjusted(5, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, m_body->text(m_i));
        QPainterPath tri;
        const QPointF c(r.right() - 7, r.center().y());
        tri.moveTo(c + QPointF(-3, -1.5)); tri.lineTo(c + QPointF(3, -1.5)); tri.lineTo(c + QPointF(0, 2.5)); tri.closeSubpath();
        p.fillPath(tri, col::label);
    }
    void mousePressEvent(QMouseEvent*) override {
        const ParamSpec& ps = specOf(m_body->ctx().type)->params[size_t(m_i)];
        const int n = int(ps.choices.size());
        QMenu menu(this);
        for (int k = 0; k < n; ++k) {
            QAction* a = menu.addAction(QString::fromUtf8(ps.choices[size_t(k)]));
            a->setCheckable(true);
            a->setChecked(k == m_body->pick(m_i));
            a->setData(k);
        }
        if (QAction* a = menu.exec(mapToGlobal(QPoint(0, height())))) m_body->edit(m_i, choicePosition(a->data().toInt(), n));
    }
    void wheelEvent(QWheelEvent* e) override {
        const int n = int(specOf(m_body->ctx().type)->params[size_t(m_i)].choices.size());
        const int step = e->angleDelta().y() > 0 ? -1 : e->angleDelta().y() < 0 ? 1 : 0;
        if (step) m_body->edit(m_i, choicePosition(std::clamp(m_body->pick(m_i) + step, 0, n - 1), n));
        e->accept();
    }
private:
    DeviceBody* m_body; int m_i;
};

// A body for devices without one of their own: every knob in columns of three.
class GenericBody : public DeviceBody {
public:
    GenericBody(const DeviceContext& ctx, QWidget* parent) : DeviceBody(ctx, parent) {
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(8, 6, 8, 6);
        grid->setSpacing(2);
        for (int i = 0; i < count(); ++i) grid->addWidget(knob(i), i % 3, i / 3);
        setFixedWidth(16 + ((count() + 2) / 3) * 50);
    }
};

} // namespace

// ============================================================================ icons

void waveIcon(QPainter& p, const QRectF& r, int shape, const QColor& c) {
    QPainterPath path;
    const double l = r.left(), t = r.top(), w = r.width(), h = r.height(), m = t + h / 2;
    switch (shape) {
    case 0: path.moveTo(l, r.bottom()); path.lineTo(l + w / 2, t); path.lineTo(l + w / 2, r.bottom()); path.lineTo(l + w, t); break;
    case 1: path.moveTo(l, r.bottom()); path.lineTo(l, t); path.lineTo(l + w / 2, t); path.lineTo(l + w / 2, r.bottom()); path.lineTo(l + w, r.bottom()); path.lineTo(l + w, t); break;
    case 2: path.moveTo(l, m); path.lineTo(l + w / 4, t); path.lineTo(l + w * 3 / 4, r.bottom()); path.lineTo(l + w, m); break;
    default:
        path.moveTo(l, m);
        for (int k = 1; k <= 24; ++k) path.lineTo(l + w * k / 24, m - h / 2 * std::sin(2 * M_PI * k / 24));
        break;
    }
    p.setPen(QPen(c, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void filterIcon(QPainter& p, const QRectF& r, int type, const QColor& c) {
    QPainterPath path;
    const double l = r.left(), t = r.top() + 1, w = r.width(), b = r.bottom();
    switch (type) {
    case 1: path.moveTo(l, b); path.quadTo(l + w * 0.4, t, l + w * 0.55, t); path.lineTo(l + w, t); break;
    case 2: path.moveTo(l, b); path.quadTo(l + w / 2, t - 3, l + w, b); break;
    case 3: path.moveTo(l, t); path.lineTo(l + w * 0.35, t); path.quadTo(l + w / 2, b + 2, l + w * 0.65, t); path.lineTo(l + w, t); break;
    default: path.moveTo(l, t); path.lineTo(l + w * 0.45, t); path.quadTo(l + w * 0.6, t, l + w, b); break;
    }
    p.setPen(QPen(c, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void lfoIcon(QPainter& p, const QRectF& r, int shape, const QColor& c) {
    QPainterPath path;
    const int n = 32;
    for (int k = 0; k <= n; ++k) {
        const double ph = double(k) / n * 0.999;
        double v = shape == 5 ? dsp::lfoShape(5, std::floor(ph * 4) + 7.0) : dsp::lfoShape(shape, ph);
        const QPointF pt(r.left() + r.width() * k / n, r.center().y() - v * r.height() / 2);
        if (k == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    p.setPen(QPen(c, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// ============================================================================ DeviceBody

DeviceBody::DeviceBody(const DeviceContext& ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setFixedHeight(kBodyHeight);
    setStyleSheet("background:transparent;");
    connect(ctx.project, &Project::deviceChanged, this, [this] {   // MIDI map, other panels, a macro, …
        if (m_ctx.transport) follow(m_ctx.transport->beat());
        refresh();
    });
}

const Device* DeviceBody::device() const { return findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId); }

int DeviceBody::count() const { return paramCount(m_ctx.type); }

double DeviceBody::pos(int i) const {
    if (i >= 0 && size_t(i) < m_auto.size() && m_auto[size_t(i)] >= 0) return m_auto[size_t(i)];
    const Device* d = device();
    if (d && i >= 0 && i < d->params.size()) return d->params[i].value;
    const DeviceSpec* s = specOf(m_ctx.type);
    return s && i >= 0 && size_t(i) < s->params.size() ? s->params[size_t(i)].def : 0.0;
}

double  DeviceBody::real(int i) const { return paramValue(m_ctx.type, i, pos(i)); }
int     DeviceBody::pick(int i) const { return choiceIndex(m_ctx.type, i, pos(i)); }
bool    DeviceBody::on(int i) const { return pos(i) >= 0.5; }
QString DeviceBody::text(int i) const { return paramText(m_ctx.type, i, pos(i)); }

QString DeviceBody::name(int i) const {
    const DeviceSpec* s = specOf(m_ctx.type);
    return s && i >= 0 && size_t(i) < s->params.size() ? QString::fromUtf8(s->params[size_t(i)].name) : QString();
}

std::vector<float> DeviceBody::positions() const {
    std::vector<float> v(audio::kMaxParams, 0.0f);
    for (int i = 0; i < std::min(count(), audio::kMaxParams); ++i) v[size_t(i)] = float(pos(i));
    return v;
}

double DeviceBody::posFor(int i, double x) const {
    const DeviceSpec* s = specOf(m_ctx.type);
    if (!s || i < 0 || size_t(i) >= s->params.size()) return 0;
    const ParamSpec& p = s->params[size_t(i)];
    double v = 0;
    switch (p.kind) {
    case ParamKind::Linear:
    case ParamKind::Int:     v = (x - p.lo) / (p.hi - p.lo); break;
    case ParamKind::Exp:     v = std::log(std::max(x, p.lo) / p.lo) / std::log(p.hi / p.lo); break;
    case ParamKind::FaderDb: v = 0.75 * std::pow(10.0, x / 40.0); break;
    case ParamKind::Toggle:  v = x >= 0.5 ? 1 : 0; break;
    case ParamKind::Choice:  v = choicePosition(int(x), int(p.choices.size())); break;
    }
    return std::clamp(v, 0.0, 1.0);
}

float DeviceBody::meter(int k) const {
    audio::Engine* e = m_ctx.transport ? m_ctx.transport->engine() : nullptr;
    return e ? e->deviceMeter(m_ctx.deviceId, k) : 0.0f;
}

float DeviceBody::scope(int k) const {
    audio::Engine* e = m_ctx.transport ? m_ctx.transport->engine() : nullptr;
    return e ? e->deviceScope(m_ctx.deviceId, k) : 0.0f;
}

QString DeviceBody::target(int i) const { return QString("device:%1:%2:%3").arg(m_ctx.trackId).arg(m_ctx.deviceId).arg(i); }
float DeviceBody::spectrum(int bin, bool output) const {
    audio::Engine* e = m_ctx.transport ? m_ctx.transport->engine() : nullptr;
    return e ? e->deviceSpectrum(m_ctx.deviceId, bin, output) : -120;
}

void DeviceBody::edit(int i, double v) {
    const Device* cur = device();
    if (!cur || i < 0 || i >= cur->params.size()) return;
    v = std::clamp(v, 0.0, 1.0);
    if (cur->params[i].value == v && (size_t(i) >= m_auto.size() || m_auto[size_t(i)] < 0)) return;
    {
        EditScope edit(m_ctx.project, "Change " + cur->params[i].name, deviceMergeKey(m_ctx.deviceId, i));
        // Look the device up again after the snapshot: Qt's containers share their data
        // until written, so a pointer from before it would write into the snapshot too.
        Device* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);
        d->params[i].value = v;
        m_ctx.project->touchTarget(m_ctx.track, target::device(m_ctx.deviceId, i));
    }
    if (size_t(i) < m_auto.size()) m_auto[size_t(i)] = -1;
    emit m_ctx.project->deviceChanged();
}

void DeviceBody::editMany(const std::vector<std::pair<int, double>>& changes, int gesture) {
    const Device* cur = device();
    if (!cur) return;
    {
        EditScope edit(m_ctx.project, "Change " + cur->name, deviceMergeKey(m_ctx.deviceId, 200 + (gesture & 31)));
        Device* d = findDevice(m_ctx.project, m_ctx.track, m_ctx.deviceId);   // after the snapshot (see edit())
        for (const auto& [i, v] : changes) {
            if (i < 0 || i >= d->params.size()) continue;
            d->params[i].value = std::clamp(v, 0.0, 1.0);
            m_ctx.project->touchTarget(m_ctx.track, target::device(m_ctx.deviceId, i));
            if (size_t(i) < m_auto.size()) m_auto[size_t(i)] = -1;
        }
    }
    emit m_ctx.project->deviceChanged();
}

QWidget* DeviceBody::knob(int i, int size, const QString& label, bool bipolar) {
    auto* cell = new QWidget(this);
    auto* l = new QVBoxLayout(cell);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto* name = new QLabel(label.isEmpty() ? this->name(i) : label, cell);
    name->setStyleSheet(kLabelStyle());
    name->setAlignment(Qt::AlignCenter);
    auto* k = new Knob(cell, size);
    k->setRange(0, 1);
    const DeviceSpec* s = specOf(m_ctx.type);
    k->setDefault(s && size_t(i) < s->params.size() ? s->params[size_t(i)].def : 0.5);
    k->setBipolar(bipolar);
    k->setValue(pos(i), false);
    k->setProperty("mapTarget", target(i));
    InfoView::attach(k, this->name(i), "Drag up or down, Shift for fine. Click it, then press Delete to reset. "
                                       "Green value: a Macros knob turns it (its own setting comes back when the mapping goes).");
    auto* val = new QLabel(text(i), cell);
    val->setStyleSheet(kValueStyle());
    val->setAlignment(Qt::AlignCenter);
    l->addWidget(name);
    l->addWidget(k, 0, Qt::AlignHCenter);
    l->addWidget(val);
    cell->setFixedWidth(size < 22 ? 36 : std::max(size + 20, 46));
    connect(k, &DragValue::valueEdited, this, [this, i](double v) { edit(i, v); });
    m_refreshers.push_back([this, i, k, val] {
        k->setValue(pos(i), false);
        val->setText(text(i));
        val->setStyleSheet(m_ctx.project->macroFor(m_ctx.track, m_ctx.deviceId, i) ? kMacroValueStyle() : modulated(i) ? kModValueStyle() : kValueStyle());
    });
    return cell;
}

QWidget* DeviceBody::toggle(int i, const QString& text, int width) {
    auto* t = new ToggleButton(this, i, text.isEmpty() ? name(i) : text, width);
    watch(t);
    return t;
}

QWidget* DeviceBody::choices(int i, std::vector<QString> labels, IconFn icon, int width, int height) {
    auto* c = new ChoiceStrip(this, i, std::move(labels), std::move(icon), width, height);
    watch(c);
    return c;
}

QWidget* DeviceBody::chooser(int i, int width) {
    auto* c = new Chooser(this, i, width);
    watch(c);
    return c;
}

QWidget* DeviceBody::field(int i, int width) {
    auto* v = new DragValue(this);
    v->setRange(0, 1);
    const DeviceSpec* s = specOf(m_ctx.type);
    v->setDefault(s && size_t(i) < s->params.size() ? s->params[size_t(i)].def : 0.5);
    v->setValue(pos(i), false);
    v->setEditable(false);
    v->setDragSensitivity(160);
    v->setFormatter([this, i](double x) { return paramText(m_ctx.type, i, x); });
    v->setFixedWidth(width);
    v->setProperty("mapTarget", target(i));
    InfoView::attach(v, name(i), "Drag up or down, Shift for fine. Click it, then press Delete to reset.");
    connect(v, &DragValue::valueEdited, this, [this, i](double x) { edit(i, x); });
    m_refreshers.push_back([this, i, v] { v->setValue(pos(i), false); });
    return v;
}

QLabel* DeviceBody::section(const QString& title) {
    auto* l = new QLabel(title, this);
    l->setStyleSheet("color:#8a8a8a;font-size:9px;font-weight:600;background:transparent;border:0;");
    return l;
}

// ---- macro knobs ----------------------------------------------------------------

QString DeviceBody::macroLabel(int k) const {
    const Device* d = device();
    const Track* t = m_ctx.project->trackAt(m_ctx.track);
    if (d && t)
        for (const MacroMap& m : d->maps) {
            if (m.macro != k) continue;
            if (m.deviceId == d->id && isRack(*d) && m.param >= racks::kBase && m.param < racks::ChainSelect) {   // a rack's own chain knob: "Pad Vol"
                const int c = (m.param - racks::kBase) / racks::kPerChain, kn = (m.param - racks::kBase) % racks::kPerChain;
                static const char* shortNames[] = {"Vol", "Pan", "On", "Solo"};
                static const char* midiNames[] = {"Transp", "Vel", "On", "Solo"};
                return d->chains.value(c, "Chain") + " " + (m_ctx.type == DeviceType::MidiEffectRack ? midiNames : shortNames)[kn];
            }
            if (m.deviceId == d->id && isRack(*d) && m.param == racks::ChainSelect) return "Chain Sel";
            for (const Device& o : t->devices) if (o.id == m.deviceId && m.param < o.params.size()) return o.params[m.param].name;
        }
    return name(k);
}

void DeviceBody::macroMenu(int k, const QPoint& at) {
    QMenu menu(this);
    QMenu* to = menu.addMenu("Map " + macroLabel(k) + " to");
    fillMacroTargets(to, k);
    const Device* d = device();
    const bool has = d && std::any_of(d->maps.begin(), d->maps.end(), [k](const MacroMap& m) { return m.macro == k; });
    QAction* clear = menu.addAction("Clear Mappings");
    clear->setEnabled(has);
    connect(clear, &QAction::triggered, this, [this, k] { clearMacro(k); });
    menu.exec(at);
}

// The Macros device: every other device on the track. A rack: the devices in its chains (by
// chain) and its own chain knobs (volume, pan, on).
void DeviceBody::fillMacroTargets(QMenu* menu, int k) {
    const Track* t = m_ctx.project->trackAt(m_ctx.track);
    const Device* self = device();
    if (!t || !self) return;
    const bool rack = isRack(*self);
    bool any = false;
    auto addKnobs = [&](QMenu* sub, const Device& o, int from, int to) {
        for (int p = from; p < to && p < o.params.size(); ++p) {
            int owner = 0;
            const MacroMap* m = m_ctx.project->macroFor(m_ctx.track, o.id, p, &owner);
            QString text = o.params[p].name;
            if (m) text += QString("   (macro %1)").arg(m->macro + 1);
            const int id = o.id;
            sub->addAction(text, this, [this, k, id, p] { addMacroMap(k, id, p); });
        }
    };
    if (rack) {
        const bool midi = m_ctx.type == DeviceType::MidiEffectRack;
        {
            const int id = self->id;
            menu->addAction("Chain Selector", this, [this, k, id] { addMacroMap(k, id, racks::ChainSelect); });
            menu->addSeparator();
        }
        for (int c = 0; c < self->chains.size(); ++c) {
            QMenu* chainMenu = menu->addMenu(self->chains[c]);
            QMenu* knobs = chainMenu->addMenu("Chain");
            for (int kn : {racks::Volume, racks::Pan, racks::On}) {
                const int p = racks::param(c, kn), id = self->id;
                const char* label = kn == racks::On ? "On" : kn == racks::Volume ? (midi ? "Transpose" : "Volume") : (midi ? "Velocity" : "Pan");
                knobs->addAction(QString(label), this, [this, k, id, p] { addMacroMap(k, id, p); });
            }
            for (const Device& o : t->devices)
                if (o.rack == self->id && o.chain == c && !o.params.isEmpty()) addKnobs(chainMenu->addMenu(o.name), o, 0, int(o.params.size()));
        }
        any = !self->chains.isEmpty();
    } else {
        for (const Device& o : t->devices) {
            if (o.id == m_ctx.deviceId || deviceType(o.name) == DeviceType::Macros || o.params.isEmpty()) continue;
            addKnobs(menu->addMenu(o.name), o, 0, int(o.params.size()));
            any = true;
        }
    }
    if (!any) menu->addAction(rack ? "Add a device to a chain first" : "Add a device to this track first")->setEnabled(false);
}

void DeviceBody::addMacroMap(int k, int deviceId, int param) {
    Project* project = m_ctx.project;
    EditScope edit(project, "Map Macro");
    Track* t = project->trackAt(m_ctx.track);
    if (!t) return;
    double current = 0.5;
    for (Device& o : t->devices) {
        o.maps.removeIf([&](const MacroMap& m) { return m.deviceId == deviceId && m.param == param; });   // one macro per knob
        if (o.id == deviceId && param < o.params.size()) current = o.params[param].value;
    }
    Device* d = findDevice(project, m_ctx.track, m_ctx.deviceId);
    if (!d) return;
    const bool first = std::none_of(d->maps.begin(), d->maps.end(), [k](const MacroMap& m) { return m.macro == k; });
    d->maps.push_back({k, deviceId, param, 0.0, 1.0});
    if (first && k < d->params.size()) d->params[k].value = current;   // the knob doesn't jump
    emit project->structureChanged();
    emit project->deviceChanged();
}

void DeviceBody::removeMacroMap(int row) {
    Project* project = m_ctx.project;
    EditScope edit(project, "Remove Macro Mapping");
    Device* d = findDevice(project, m_ctx.track, m_ctx.deviceId);
    if (!d || row < 0 || row >= d->maps.size()) return;
    d->maps.removeAt(row);
    emit project->structureChanged();
    emit project->deviceChanged();
}

void DeviceBody::clearMacro(int k) {
    Project* project = m_ctx.project;
    EditScope edit(project, "Clear Macro Mappings");
    Device* d = findDevice(project, m_ctx.track, m_ctx.deviceId);
    if (!d) return;
    d->maps.removeIf([k](const MacroMap& m) { return m.macro == k; });
    emit project->structureChanged();
    emit project->deviceChanged();
}

void DeviceBody::setMacroRange(int row, int which, double x) {
    Project* project = m_ctx.project;
    EditScope edit(project, which == 0 ? "Macro Min" : "Macro Max", deviceMergeKey(m_ctx.deviceId, 240 + (row % 8) * 2 + which));
    Device* d = findDevice(project, m_ctx.track, m_ctx.deviceId);
    if (!d || row < 0 || row >= d->maps.size()) return;
    (which == 0 ? d->maps[row].lo : d->maps[row].hi) = std::clamp(x, 0.0, 1.0);
    emit project->deviceChanged();
}

// One mapping: which knob, its Min and Max, and a delete button.
QHBoxLayout* DeviceBody::macroMapRow(QWidget* parent, const MacroMap& m, int row, int labelWidth) {
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(3);
    const Track* t = m_ctx.project->trackAt(m_ctx.track);
    QString what = "(deleted device)";
    bool alive = false;
    if (t) for (const Device& o : t->devices)
        if (o.id == m.deviceId && m.param < o.params.size()) {
            what = o.id == m_ctx.deviceId && isRack(o) && m.param >= racks::kBase && m.param < racks::ChainSelect
                 ? o.chains.value((m.param - racks::kBase) / racks::kPerChain) + " · " + o.params[m.param].name.section(' ', 2)
                 : o.name + " · " + o.params[m.param].name;
            alive = true;
        }
    auto* l = new QLabel(QString("%1 ▸ %2").arg(m.macro + 1).arg(what), parent);
    l->setFixedWidth(labelWidth);
    l->setStyleSheet(alive ? "color:#d8d8d8;font-size:9px;" : "color:#ff5a4a;font-size:9px;");
    l->setText(l->fontMetrics().elidedText(l->text(), Qt::ElideRight, labelWidth - 4));
    l->setToolTip(what);
    line->addWidget(l);
    for (int which = 0; which < 2; ++which) {
        auto* v = new DragValue(parent);
        v->setRange(0, 1);
        v->setDefault(which == 0 ? 0 : 1);
        v->setValue(which == 0 ? m.lo : m.hi, false);
        v->setMinimumChars(4);
        v->setDragSensitivity(200);
        v->setFormatter([](double x) { return QString("%1%").arg(int(std::lround(x * 100))); });
        InfoView::attach(v, which == 0 ? "Min" : "Max", which == 0 ? "Where the knob goes with the macro all the way down." : "Where the knob goes with the macro all the way up.");
        connect(v, &DragValue::valueEdited, this, [this, row, which](double x) { setMacroRange(row, which, x); });
        line->addWidget(v);
    }
    auto* del = new QToolButton(parent);
    del->setText("✕");
    del->setFocusPolicy(Qt::NoFocus);
    del->setStyleSheet("QToolButton{font-size:9px;padding:0 3px;}");
    InfoView::attach(del, "Remove Mapping", "The knob goes back to its own value.");
    connect(del, &QToolButton::clicked, this, [this, row] { removeMacroMap(row); });
    line->addWidget(del);
    line->addStretch(1);
    return line;
}

void DeviceBody::watch(QWidget* w) { m_watched.push_back(w); }
void DeviceBody::animate(Display* d) { m_animated.push_back(d); }

void DeviceBody::refresh() {
    for (auto& r : m_refreshers) r();
    for (QWidget* w : m_watched) w->update();
}

void DeviceBody::tick() { for (Display* d : m_animated) d->tick(); }

// Knobs show what plays: a Macros knob mapped to them, the playing clip's envelope, or the
// track's automation (Project::drivenValue, the engine's order).
void DeviceBody::follow(double beat) {
    const Track* t = m_ctx.project->trackAt(m_ctx.track);
    if (!t) return;
    const int n = count();
    m_auto.resize(size_t(n), -1);
    const bool playing = m_ctx.transport && m_ctx.transport->playing();
    bool maps = false;
    for (const Device& d : t->devices) maps = maps || !d.maps.isEmpty();
    const ClipPtr clip = playing ? m_ctx.project->playingClip(m_ctx.track, beat) : nullptr;
    const bool drivenAtAll = maps || !t->automation.isEmpty() || (clip && !clip->envelopes.isEmpty());
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        const double v = drivenAtAll ? m_ctx.project->drivenValue(m_ctx.track, target::device(m_ctx.deviceId, i), beat, playing) : -1;
        if (v != m_auto[size_t(i)]) { m_auto[size_t(i)] = v; changed = true; }
    }
    if (changed) refresh();
}

DeviceBody* createBody(const DeviceContext& ctx, QWidget* parent) {
    DeviceBody* b = nullptr;
    const DeviceSpec* s = specOf(ctx.type);
    if (!s) return new GenericBody(ctx, parent);
    if (isRackType(ctx.type)) b = createRackBody(ctx, parent);
    else if (isGuitarType(ctx.type)) b = createGuitarBody(ctx, parent);
    else switch (s->kind) {
    case DeviceKind::Instrument:  b = createInstrumentBody(ctx, parent); break;
    case DeviceKind::AudioEffect: b = createEffectBody(ctx, parent); break;
    case DeviceKind::MidiEffect:  b = createMidiBody(ctx, parent); break;
    }
    if (!b) b = new GenericBody(ctx, parent);
    b->refresh();
    return b;
}

// ============================================================================ displays

Display::Display(DeviceBody* body, int w, int h) : QWidget(body), m_body(body) {
    setFixedSize(w, h);
    body->watch(this);
}

void Display::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col::displayEdge, 1));
    p.setBrush(col::display);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
    p.setClipRect(inner());
    QFont f = font(); f.setPointSizeF(6.5); p.setFont(f);
    draw(p, inner());
}

double freqToX(double f, const QRectF& r) { return r.left() + r.width() * std::log(std::max(f, 1.0) / 20.0) / std::log(1000.0); }
double xToFreq(double x, const QRectF& r) { return 20.0 * std::pow(1000.0, std::clamp((x - r.left()) / r.width(), 0.0, 1.0)); }

void drawFreqGrid(QPainter& p, const QRectF& r, double dbRange) {
    static const double lines[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
    for (double f : lines) {
        const bool strong = f == 100 || f == 1000 || f == 10000;
        p.setPen(QPen(strong ? col::gridStrong : col::grid, 1));
        const double x = std::round(freqToX(f, r)) + 0.5;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    p.setPen(col::gridText);
    p.drawText(QPointF(freqToX(100, r) + 2, r.bottom() - 2), "100");
    p.drawText(QPointF(freqToX(1000, r) + 2, r.bottom() - 2), "1k");
    p.drawText(QPointF(freqToX(10000, r) + 2, r.bottom() - 2), "10k");
    if (dbRange > 0) {
        for (int k = -2; k <= 2; ++k) {
            const double y = std::round(r.center().y() - k * r.height() / 4) + 0.5;
            p.setPen(QPen(k == 0 ? col::gridStrong : col::grid, 1));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
    }
}

void drawHandle(QPainter& p, const QPointF& at, const QColor& c, const QString& label, bool hollow) {
    p.setPen(QPen(hollow ? c : QColor(0, 0, 0, 160), 1.2));
    p.setBrush(hollow ? QColor(0, 0, 0, 0) : c);
    p.drawEllipse(at, 4.5, 4.5);
    if (!label.isEmpty() && !hollow) {
        p.setPen(theme::ink);
        QFont f = p.font(); f.setPointSizeF(5.5); f.setBold(true); p.setFont(f);
        p.drawText(QRectF(at.x() - 5, at.y() - 5, 10, 10), Qt::AlignCenter, label);
        f.setBold(false); f.setPointSizeF(6.5); p.setFont(f);
    }
}

void drawCaption(QPainter& p, const QRectF& r, const QString& text, Qt::Alignment align) {
    p.setPen(col::gridText);
    p.drawText(r.adjusted(4, 2, -4, -2), int(align), text);
}

// ---- ADSR

AdsrDisplay::AdsrDisplay(DeviceBody* body, int w, int h, int a, int d, int s, int r, QColor color, const QString& caption)
    : Display(body, w, h), m_a(a), m_d(d), m_s(s), m_r(r), m_color(color), m_caption(caption) {
    setCursor(Qt::SizeAllCursor);
    InfoView::attach(this, caption.isEmpty() ? QString("Envelope") : caption + " Envelope",
                     "Drag the handles: the first sets the attack, the second the decay and the sustain level, the last the release.");
}

QPointF AdsrDisplay::point(int which, const QRectF& r) const {
    const double left = r.left() + 4, top = r.top() + 8, bottom = r.bottom() - 4, h = bottom - top;
    const double seg = (r.width() - 8) / 3.5;
    const double aW = seg * m_body->pos(m_a), dW = seg * m_body->pos(m_d), sW = seg * 0.5, rW = seg * m_body->pos(m_r);
    const double sus = m_s >= 0 ? m_body->pos(m_s) : 0.0;
    switch (which) {
    case 0:  return {left + aW, top};
    case 1:  return {left + aW + dW, bottom - sus * h};
    case 2:  return {left + aW + dW + sW, bottom - sus * h};
    default: return {left + aW + dW + sW + rW, bottom};
    }
}

void AdsrDisplay::draw(QPainter& p, const QRectF& r) {
    for (int k = 1; k < 4; ++k) {
        p.setPen(QPen(col::grid, 1));
        const double x = std::round(r.left() + r.width() * k / 4) + 0.5;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    const QPointF p0 = point(0, r), p1 = point(1, r), p2 = point(2, r), p3 = point(3, r);
    const double bottom = r.bottom() - 4;
    QPainterPath path;
    path.moveTo(r.left() + 4, bottom);
    path.lineTo(p0);
    path.quadTo(QPointF(p0.x() + (p1.x() - p0.x()) * 0.2, p1.y()), p1);
    path.lineTo(p2);
    path.quadTo(QPointF(p2.x() + (p3.x() - p2.x()) * 0.2, p3.y()), p3);
    QPainterPath fill = path;
    fill.lineTo(r.left() + 4, bottom);
    QColor f = m_color; f.setAlpha(42);
    p.fillPath(fill, f);
    p.setPen(QPen(m_color, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    drawHandle(p, p0, col::handle);
    drawHandle(p, p1, col::handle);
    drawHandle(p, p3, col::handle);
    drawLive(p, r);
    if (!m_caption.isEmpty()) drawCaption(p, r, m_caption, Qt::AlignTop | Qt::AlignRight);
}

void AdsrDisplay::mousePressEvent(QMouseEvent* e) {
    const QRectF r = inner();
    const QPointF at = e->position();
    double best = 1e9;
    m_drag = -1;
    for (int k : {0, 1, 3}) {
        const double d = QLineF(at, point(k, r)).length();
        if (d < best) { best = d; m_drag = k; }
    }
    mouseMoveEvent(e);
}

void AdsrDisplay::mouseMoveEvent(QMouseEvent* e) {
    if (m_drag < 0 || !(e->buttons() & Qt::LeftButton)) return;
    const QRectF r = inner();
    const double left = r.left() + 4, top = r.top() + 8, bottom = r.bottom() - 4, h = bottom - top;
    const double seg = (r.width() - 8) / 3.5;
    const QPointF at = e->position();
    const double aW = seg * m_body->pos(m_a), dW = seg * m_body->pos(m_d);
    std::vector<std::pair<int, double>> ch;
    if (m_drag == 0) ch.push_back({m_a, (at.x() - left) / seg});
    else if (m_drag == 1) {
        ch.push_back({m_d, (at.x() - left - aW) / seg});
        if (m_s >= 0) ch.push_back({m_s, (bottom - at.y()) / h});
    } else ch.push_back({m_r, (at.x() - left - aW - dW - seg * 0.5) / seg});
    m_body->editMany(ch, m_drag);
}

// ---- Filter

FilterDisplay::FilterDisplay(DeviceBody* body, int w, int h, int cutoff, int reso, int type, int fixedType, std::function<float()> live)
    : Display(body, w, h), m_cutoff(cutoff), m_reso(reso), m_type(type), m_fixed(fixedType), m_live(std::move(live)) {
    setCursor(Qt::SizeAllCursor);
    InfoView::attach(this, "Filter", "Drag the dot: left and right set the cutoff, up and down the resonance.");
}

namespace {
constexpr double kFilterTopDb = 24, kFilterBottomDb = -36;
double dbToY(double db, const QRectF& r) { return r.top() + (kFilterTopDb - std::clamp(db, kFilterBottomDb, kFilterTopDb)) / (kFilterTopDb - kFilterBottomDb) * r.height(); }
double yToDb(double y, const QRectF& r) { return kFilterTopDb - (y - r.top()) / r.height() * (kFilterTopDb - kFilterBottomDb); }
}

void FilterDisplay::draw(QPainter& p, const QRectF& r) {
    drawFreqGrid(p, r, 0);
    p.setPen(QPen(col::gridStrong, 1));
    const double zero = std::round(dbToY(0, r)) + 0.5;
    p.drawLine(QPointF(r.left(), zero), QPointF(r.right(), zero));
    const int type = m_type >= 0 ? m_body->pick(m_type) : m_fixed;
    const double reso = m_body->pos(m_reso);
    const float q = float(0.6 + reso * reso * 12.0);
    const double set = m_body->real(m_cutoff);
    const double live = m_live ? double(m_live()) : 0.0;
    const double fc = live > 1 ? live : set;
    QPainterPath path;
    for (int x = 0; x <= int(r.width()); x += 2) {
        const double f = xToFreq(r.left() + x, r);
        const double db = 20 * std::log10(std::max(1e-6f, dsp::Svf::magnitude(type, float(f), float(fc), q)));
        const QPointF pt(r.left() + x, dbToY(db, r));
        if (x == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    QPainterPath fill = path;
    fill.lineTo(r.right(), r.bottom()); fill.lineTo(r.left(), r.bottom()); fill.closeSubpath();
    p.fillPath(fill, col::curveFill);
    p.setPen(QPen(col::curve, 1.5));
    p.drawPath(path);
    const QPointF handle(freqToX(set, r), dbToY(20 * std::log10(q), r));
    drawHandle(p, handle, col::handle, {}, false);
    if (live > 1 && std::fabs(std::log2(live / set)) > 0.05) drawHandle(p, QPointF(freqToX(live, r), handle.y()), col::curve, {}, true);
}

void FilterDisplay::mousePressEvent(QMouseEvent* e) { m_drag = true; mouseMoveEvent(e); }

void FilterDisplay::mouseMoveEvent(QMouseEvent* e) {
    if (!m_drag || !(e->buttons() & Qt::LeftButton)) return;
    const QRectF r = inner();
    const double f = xToFreq(e->position().x(), r);
    const double q = std::pow(10.0, yToDb(e->position().y(), r) / 20.0);
    const double reso = std::sqrt(std::clamp((q - 0.6) / 12.0, 0.0, 1.0));
    m_body->editMany({{m_cutoff, m_body->posFor(m_cutoff, f)}, {m_reso, reso}}, 8);
}

} // namespace sessions::devui
