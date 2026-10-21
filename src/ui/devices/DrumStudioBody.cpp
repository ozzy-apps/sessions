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
#include "audio/DrumMicModel.h"
#include "audio/Engine.h"
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
#include <QKeyEvent>
#include <QMenu>
#include <QGridLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QIcon>
#include <complex>
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

// Native, resolution-independent kit stage. The same geometry drives drawing and hit testing.
class KitView : public Display {
public:
    KitView(DeviceBody* b, int w, int h, std::function<void(int)> selected) : Display(b,w,h), m_selected(std::move(selected)) {
        setCursor(Qt::PointingHandCursor); setFocusPolicy(Qt::StrongFocus);
        setAccessibleName("Drum kit stage");
        InfoView::attach(this,"Kit stage","Click a piece to select and audition it. Move pieces enables drag positioning; horizontal position changes stereo placement. Delete removes the selected piece. Empty slots can be added again. In Microphones, drag a mic to change its distance and angle.");
    }
    void setMoveMode(bool on) { m_move=on; setCursor(on?Qt::OpenHandCursor:Qt::PointingHandCursor); update(); }
    void setAudition(std::function<void(int)> fn) { m_audition=std::move(fn); }
    void setMicMode(std::function<void(int)> fn) { m_mics=true; m_micSelected=std::move(fn); }
    void selectMic(int d) { m_mic=d; update(); }
    void tick() override { for(int d=0;d<ds::kDrums;++d)m_glow[d]=std::max(double(m_body->scope(d)),m_glow[d]-.07); update(); }
    QRectF stage() const {
        if(height()<=230)return inner().adjusted(8,8,-8,-20);
        const QRectF r=inner().adjusted(20,24,-20,-30);
        const double scale=std::min(r.width(),r.height()*1.3);
        return QRectF(r.center().x()-scale*.5,r.center().y()-scale/2.6,scale,scale/1.3);
    }
    QPointF center(int d) const { const auto r=stage(); return {r.left()+m_body->pos(ds::piece(d,ds::StageX))*r.width(),r.top()+m_body->pos(ds::piece(d,ds::StageY))*r.height()}; }
    double radius(int d) const { return ds::stageRadius[d]*stage().height(); }
    QPointF micPoint(int d, bool right=false) const {
        const auto r=stage();
        if(d==ds::kDrums)return {r.center().x()+(right?1:-1)*(.16+.2*m_body->pos(ds::Width))*r.width(),r.top()+(.025+.06*m_body->pos(ds::OhHeight))*r.height()};
        if(d==ds::kDrums+1)return {r.left()+(right?.94:.06)*r.width(),r.top()+(.94-.08*m_body->pos(ds::RoomDistance))*r.height()};
        const double theta=(-135+m_body->real(ds::piece(d,ds::MicAngle)))*3.14159265359/180;
        const double distance=radius(d)+r.height()*(.025+.1*m_body->pos(ds::piece(d,ds::MicDistance)));
        return center(d)+QPointF(std::cos(theta)*distance,std::sin(theta)*distance);
    }
    int drumAt(const QPointF& pt) const {
        int best=-1; double score=1e9;
        for(int d=0;d<ds::kDrums;++d){const auto c=center(d);const double dist=std::hypot(pt.x()-c.x(),pt.y()-c.y());if(dist<radius(d)+5&&dist/radius(d)<score){best=d;score=dist/radius(d);}}
        return best;
    }
protected:
    void draw(QPainter& p,const QRectF& r) override {
        p.setRenderHint(QPainter::Antialiasing); const bool large=height()>230; const auto room=stage();
        if(large){
            QLinearGradient ground(r.topLeft(),r.bottomRight());ground.setColorAt(0,col::display);ground.setColorAt(1,theme::devCell);
            p.fillRect(r,ground);
            const QRectF rug=room.adjusted(-10,-5,10,12);
            p.setPen(QPen(col::gridStrong,1));p.setBrush(theme::withAlpha(col::grid,55));p.drawRoundedRect(rug,18,18);
            p.setPen(QPen(theme::withAlpha(col::gridStrong,65),.7));
            for(double x=rug.left()+12;x<rug.right();x+=14)p.drawLine(QPointF(x,rug.top()+8),QPointF(x,rug.bottom()-8));
            p.setPen(QPen(theme::withAlpha(col::gridStrong,100),1,Qt::DashLine));p.setBrush(Qt::NoBrush);
            p.drawEllipse(room.center()+QPointF(0,room.height()*.05),room.width()*.39,room.height()*.4);
        }
        const int selected=studioDrums().value(m_body->ctx().deviceId,ds::Snare);
        QFont font=p.font();font.setPixelSize(large?11:9);p.setFont(font);
        // Stands and the kick pedal sit underneath the kit.
        if(large)for(int d=0;d<ds::kDrums;++d)if(m_body->on(ds::piece(d,ds::Present))){
            const QPointF c=center(d);const double rad=radius(d);
            p.setPen(QPen(col::gridStrong,2));
            if(d!=ds::Kick)for(int i=0;i<3;++i){double a=(i*120+90)*3.14159265359/180;p.drawLine(c,c+QPointF(std::cos(a)*rad*1.22,std::sin(a)*rad*1.22));}
            else {p.setBrush(col::grid);p.drawRoundedRect(QRectF(c.x()-rad*.14,c.y()+rad*.8,rad*.28,rad*.55),3,3);}
        }
        for(int d : {ds::Kick,ds::Tom1,ds::Tom2,ds::Tom3,ds::Snare,ds::HiHat,ds::Ride,ds::Crash}){
            const QPointF c=center(d);const double rad=radius(d);const bool present=m_body->on(ds::piece(d,ds::Present));
            if(!present){
                p.setBrush(Qt::NoBrush);p.setPen(QPen(col::gridStrong,1,Qt::DashLine));p.drawEllipse(c,rad,rad);
                p.setPen(col::label);p.drawText(QRectF(c.x()-rad,c.y()-10,rad*2,20),Qt::AlignCenter,"+");
                if(large)p.drawText(QRectF(c.x()-60,c.y()+rad+4,120,16),Qt::AlignCenter,ds::drumName(d));continue;
            }
            const bool cymbal=d==ds::HiHat||d==ds::Ride||d==ds::Crash;
            if(large){p.setPen(Qt::NoPen);p.setBrush(theme::withAlpha(Qt::black,65));p.drawEllipse(c+QPointF(3,6),rad+2,rad+2);}
            if(d==selected){p.setBrush(theme::withAlpha(theme::accent,20));p.setPen(QPen(theme::accent,1.8));p.drawEllipse(c,rad+5,rad+5);}
            if(cymbal){
                const QColor metal=QColor(179,145,78);QRadialGradient finish(c-QPointF(rad*.3,rad*.4),rad*1.6);
                finish.setColorAt(0,metal.lighter(165));finish.setColorAt(.45,metal);finish.setColorAt(.7,metal.darker(135));finish.setColorAt(1,metal.lighter(125));
                p.setBrush(finish);p.setPen(QPen(metal.lighter(130),1));p.drawEllipse(c,rad,rad);
                p.setBrush(Qt::NoBrush);p.setPen(QPen(QColor(60,44,21,65),.6));
                for(double rr=rad*.25;rr<rad;rr+=large?3.2:4.5)p.drawEllipse(c,rr,rr);
                p.setBrush(metal.lighter(120));p.setPen(QPen(metal.darker(145),1));p.drawEllipse(c,rad*.2,rad*.2);
                p.setBrush(col::display);p.setPen(Qt::NoPen);p.drawEllipse(c,large?2.5:1.5,large?2.5:1.5);
                if(d==ds::HiHat){p.setBrush(Qt::NoBrush);p.setPen(QPen(metal.darker(150),1));p.drawEllipse(c+QPointF(0,3),rad,rad);}
            }else{
                const int source=m_body->pick(ds::piece(d,ds::Source));const int bank=source?source-1:audio::drums::bankFor(m_body->pick(ds::Kit));
                const QColor shell=bank==0?QColor(61,75,83):bank==1?QColor(110,66,49):QColor(151,121,76);
                p.setBrush(shell);p.setPen(QPen(col::gridStrong,2));p.drawEllipse(c,rad,rad);
                const bool light=col::display.lightness()>128;
                QRadialGradient head(c-QPointF(rad*.22,rad*.28),rad*1.5);head.setColorAt(0,light?QColor(244,243,235):QColor(125,132,135));head.setColorAt(.42,light?QColor(220,221,217):QColor(74,79,82));head.setColorAt(1,light?QColor(179,185,183):QColor(33,36,39));
                p.setBrush(head);p.setPen(QPen(col::label,1));p.drawEllipse(c,rad*.88,rad*.88);
                p.setBrush(Qt::NoBrush);p.setPen(QPen(theme::withAlpha(col::value,65),.7));p.drawEllipse(c,rad*.79,rad*.79);
                if(large){
                    for(int i=0;i<(d==ds::Kick?10:8);++i){double a=i*6.28318530718/(d==ds::Kick?10:8);QPointF at=c+QPointF(std::cos(a)*rad*.95,std::sin(a)*rad*.95);p.setPen(QPen(col::label,2.5));p.drawLine(at,at+(c-at)*.09);}
                    p.setPen(QPen(theme::withAlpha(col::value,35),.8));p.drawEllipse(c,rad*.31,rad*.31);
                    if(d==ds::Snare)for(int i=-2;i<=2;++i)p.drawLine(c+QPointF(-rad*.5,i*2),c+QPointF(rad*.5,i*2));
                }
            }
            if(m_glow[d]>.04){p.setBrush(theme::withAlpha(theme::accent,int(m_glow[d]*100)));p.setPen(Qt::NoPen);p.drawEllipse(c,rad*.85,rad*.85);}
            const QRectF label(c.x()-59,c.y()+rad+7,118,large?19:13);
            p.setPen(d==selected?col::value:col::label);p.drawText(label,Qt::AlignCenter,ds::drumName(d));
            if(m_body->on(ds::mute(d))){p.setPen(theme::accent);p.drawText(QRectF(c.x()-10,c.y()-9,20,18),Qt::AlignCenter,"M");}
        }
        if(m_mics){
            auto mic=[&](int d,bool right){const QPointF at=micPoint(d,right);const bool sel=d==m_mic;const QColor color=sel?theme::accent:col::cyan;
                if(d<ds::kDrums){p.setPen(QPen(theme::withAlpha(color,140),1,Qt::DashLine));p.drawLine(center(d),at);}
                else {p.setPen(QPen(theme::withAlpha(color,70),1,Qt::DashLine));p.drawLine(at,room.center());}
                p.setBrush(col::display);p.setPen(QPen(color,sel?2:1));p.drawEllipse(at,sel?10:8,sel?10:8);
                p.drawRoundedRect(QRectF(at.x()-2.5,at.y()-5,5,8),2,2);p.drawLine(at+QPointF(-4,0),at+QPointF(-4,4));p.drawLine(at+QPointF(-4,4),at+QPointF(4,4));p.drawLine(at+QPointF(4,4),at+QPointF(4,0));
                if(d>=ds::kDrums){p.setPen(color);p.drawText(QRectF(at.x()-40,at.y()+(d==ds::kDrums?-29:13),80,16),Qt::AlignCenter,QString(d==ds::kDrums?"OH %1":"Room %1").arg(right?"R":"L"));}
            };
            for(int d=0;d<ds::kDrums;++d)if(m_body->on(ds::piece(d,ds::Present)))mic(d,false);
            for(int d=ds::kDrums;d<ds::kDrums+2;++d){mic(d,false);mic(d,true);}
        }
        p.setPen(col::label);p.drawText(r.adjusted(12,8,-12,-8),Qt::AlignTop|Qt::AlignLeft,m_mics?"MICROPHONE POSITION":m_move?"MOVE PIECES":"KIT");
        if(large){p.setPen(col::gridText);p.drawText(r.adjusted(12,8,-12,-8),Qt::AlignBottom|Qt::AlignHCenter,m_mics?"Drag mic · distance / angle":"Drummer perspective");}
    }
    void mousePressEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton)return;
        setFocus();m_press=e->position();m_drag=-1;m_dragged=false;
        if(m_mics){
            double best=18;int hit=-1;
            for(int d=0;d<ds::kDrums+2;++d){if(d<ds::kDrums&&!m_body->on(ds::piece(d,ds::Present)))continue;
                for(bool right:{false,true}){if(d<ds::kDrums&&right)continue;const auto pt=micPoint(d,right);const double distance=std::hypot(pt.x()-m_press.x(),pt.y()-m_press.y());if(distance<best){best=distance;hit=d;}}}
            if(hit<0)hit=drumAt(m_press);
            if(hit>=0){m_drag=hit;m_mic=hit;if(hit<ds::kDrums){studioDrums().insert(m_body->ctx().deviceId,hit);m_selected(hit);}if(m_micSelected)m_micSelected(hit);update();}return;
        }
        m_drag=drumAt(m_press);if(m_drag<0)return;studioDrums().insert(m_body->ctx().deviceId,m_drag);m_selected(m_drag);update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if(!(e->buttons()&Qt::LeftButton)||m_drag<0||(!m_mics&&!m_move))return;
        if(!m_dragged&&(e->position()-m_press).manhattanLength()<QApplication::startDragDistance())return;
        m_dragged=true;const auto r=stage();
        if(m_mics){
            if(m_drag<ds::kDrums){const auto delta=e->position()-center(m_drag);const double len=std::hypot(delta.x(),delta.y());const double angle=std::atan2(delta.y(),delta.x())*180/3.14159265359+135;
                m_body->editMany({{ds::piece(m_drag,ds::MicDistance),(len-radius(m_drag)-r.height()*.025)/(r.height()*.1)},{ds::piece(m_drag,ds::MicAngle),angle/90}},16+m_drag);
            }else if(m_drag==ds::kDrums)m_body->editMany({{ds::Width,(std::abs(e->position().x()-r.center().x())/r.width()-.16)/.2},{ds::OhHeight,(e->position().y()-r.top()-r.height()*.025)/(r.height()*.06)}},24);
            else m_body->editMany({{ds::RoomDistance,(r.top()+r.height()*.94-e->position().y())/(r.height()*.08)}},25);
        }else{
            const int d=m_drag;const double margin=ds::stageRadius[d]+.04;
            m_body->editMany({{ds::piece(d,ds::StageX),std::clamp((e->position().x()-r.left())/r.width(),margin,1-margin)},{ds::piece(d,ds::StageY),std::clamp((e->position().y()-r.top())/r.height(),margin,1-margin)}},d);
        }update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton)return;
        if(m_drag>=0&&!m_dragged&&!m_mics){if(!m_body->on(ds::piece(m_drag,ds::Present)))m_body->edit(ds::piece(m_drag,ds::Present),1);else if(!m_move)audition(m_drag);}
        m_drag=-1;
    }
    void keyPressEvent(QKeyEvent* e) override {
        const int d=studioDrums().value(m_body->ctx().deviceId,ds::Snare);
        if(e->key()==Qt::Key_Delete){m_body->edit(ds::piece(d,ds::Present),0);e->accept();}
        else if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Space){if(m_body->on(ds::piece(d,ds::Present)))audition(d);e->accept();}
        else Display::keyPressEvent(e);
    }
private:
    void audition(int d) { if(m_audition)m_audition(d);else if(m_body->ctx().transport)m_body->ctx().transport->previewNote(m_body->ctx().track,ds::noteFor(d),.8); }
    std::function<void(int)> m_selected,m_micSelected,m_audition;
    bool m_move=false,m_mics=false,m_dragged=false;int m_drag=-1,m_mic=ds::Snare;QPointF m_press;double m_glow[ds::kDrums]{};
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
            const QPointF c(kit.left() + ds::stageX[d] * kit.width(), kit.top() + 0.35 * kit.height() + ds::stageY[d] * kit.height() * 0.6);
            const double rad = ds::stageRadius[d] * kit.height() * 0.55;
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
        side->addWidget(chooser(ds::ContentLibrary, 120));side->addWidget(chooser(ds::Kit, 120));
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
// Cached min/max envelopes of the actual selected recording and velocity layer.
class DrumWave : public Display {
public:
    DrumWave(DeviceBody* b,std::function<int()> note,std::function<int()> drum,std::function<double()> velocity)
        :Display(b,360,164),m_note(std::move(note)),m_drum(std::move(drum)),m_velocity(std::move(velocity)) {
        setObjectName("studioWaveform");setMinimumSize(280,112);setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
        InfoView::attach(this,"Recorded source","The original recording at the audition velocity. The velocity layer follows the selected library; RR 1 previews the first repeated-hit variation. Tune changes playback rate; Decay is shown as a damping envelope when below ×1.");
    }
    void draw(QPainter& p,const QRectF& r) override {
        const int drum=m_drum(),gm=m_note(),source=m_body->pick(ds::piece(drum,ds::Source));
        const int bank=source?source-1:audio::drums::bankFor(m_body->pick(ds::Kit));
        const int v=std::clamp(int(std::lround(m_velocity()*127)),1,127);const int layer=v<=26?0:v<=52?1:v<=77?2:v<=102?3:4;
        int layers=5,shownLayer=layer+1;const auto* sample=&audio::drums::library()[bank].notes[audio::drums::sampleNote(gm)][layer];
        const auto positions=m_body->positions();const int content=audio::drums::contentIndex(positions.data(),drum);
        if(content>=0){if(content!=m_contentIndex||!m_contentBank){m_contentBank=audio::drums::loadContentBank(content);m_contentIndex=content;}
            if(m_contentBank)if(const auto* hit=audio::drums::recordedHit(*m_contentBank,gm,v,0,&layers)){sample=m_body->on(ds::recordedMic(drum))&&!hit->alternate.mono.empty()?&hit->alternate:&hit->close;shownLayer=1;for(const auto& group:m_contentBank->notes[size_t(gm)]){if(v<=group.high)break;++shownLayer;}}}
        else m_contentBank.reset();const auto& s=*sample;
        const auto plot=r.adjusted(10,28,-10,-26);const int width=std::max(1,int(plot.width()));
        if(m_sample!=&s||m_width!=width){
            m_sample=&s;m_width=width;m_peaks.assign(size_t(width),{});
            for(int x=0;x<width;++x){const size_t a=size_t(x)*s.mono.size()/width,b=size_t(x+1)*s.mono.size()/width;float lo=0,hi=0;
                for(size_t i=a;i<b;++i){lo=std::min(lo,s.mono[i]);hi=std::max(hi,s.mono[i]);}m_peaks[size_t(x)]={lo,hi};}
        }
        p.setRenderHint(QPainter::Antialiasing);p.setPen(col::grid);p.drawLine(plot.topLeft()+QPointF(0,plot.height()*.5),plot.topRight()+QPointF(0,plot.height()*.5));
        const double seconds=s.mono.size()/s.rate; p.setFont(QFont(p.font().family(),8));
        for(int i=0;i<5;++i){double x=plot.left()+plot.width()*i/4.;p.setPen(col::grid);p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));p.setPen(col::gridText);p.drawText(QRectF(x-18,plot.bottom()+6,40,15),Qt::AlignCenter,QString::number(seconds*i/4.,'f',2));}
        p.setPen(QPen(col::curve,1));for(int x=0;x<width;++x){auto pair=m_peaks[size_t(x)];p.drawLine(QPointF(plot.left()+x,plot.center().y()-pair.second*plot.height()*.46),QPointF(plot.left()+x,plot.center().y()-pair.first*plot.height()*.46));}
        double decay=m_body->real(ds::param(drum,ds::Decay));const int recipe=m_body->pick(ds::Kit);decay*=recipe==2?.65:recipe==3?.8:1;
        if(decay<.999){const double tau=(drum>=ds::Ride?1.5:.35)*decay/(1-decay);QPainterPath path;for(int x=0;x<=width;++x){double y=plot.center().y()-std::exp(-(seconds*x/width)/tau)*plot.height()*.46;if(!x)path.moveTo(plot.left()+x,y);else path.lineTo(plot.left()+x,y);}p.setPen(QPen(col::cyan,1,Qt::DashLine));p.drawPath(path);}
        p.setPen(col::label);p.drawText(r.adjusted(10,7,-10,-7),Qt::AlignTop|Qt::AlignLeft,"RECORDED SOURCE");
        p.setPen(col::curve);p.drawText(r.adjusted(10,7,-10,-7),Qt::AlignTop|Qt::AlignRight,QString("%1 / %2 · RR 1 · %3 s").arg(shownLayer).arg(layers).arg(seconds,0,'f',2));
    }
private:
    std::shared_ptr<const audio::drums::ContentBank> m_contentBank;int m_contentIndex=-2;
    std::function<int()> m_note,m_drum;std::function<double()> m_velocity;const audio::drums::Layer* m_sample=nullptr;int m_width=0;std::vector<std::pair<float,float>>m_peaks;
};

class MicResponse : public Display {
public:
    MicResponse(DeviceBody* b,std::function<int()> selected):Display(b,360,176),m_selected(std::move(selected)){
        setObjectName("studioMicResponse");setMinimumSize(280,136);setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
        InfoView::attach(this,"Mic response","Static frequency response of the selected generic tonal model, including close-mic angle and distance gain. Original recording acoustics and the room reverb are not measured here. These are modeled microphones, not alternate recorded mic channels.");
    }
    void draw(QPainter& p,const QRectF& r)override{
        const int d=m_selected();const bool close=d<ds::kDrums,oh=d==ds::kDrums;
        const int model=m_body->pick(close?ds::piece(d,ds::MicModel):oh?ds::OhModel:ds::RoomModel);
        const float angle=close?m_body->real(ds::piece(d,ds::MicAngle)):0,cm=close?m_body->real(ds::piece(d,ds::MicDistance)):10;
        const double rate=m_body->ctx().transport&&m_body->ctx().transport->engine()?m_body->ctx().transport->engine()->sampleRate():48000;
        const double extra=close?0:10*std::log10((oh?1.5:3)/m_body->real(oh?ds::OhHeight:ds::RoomDistance));
        const auto grid=r.adjusted(35,30,-16,-25);p.setFont(QFont(p.font().family(),8));
        auto y=[&](double db){return grid.top()+(6-db)/30*grid.height();};
        for(int db:{-24,-12,0,6}){p.setPen(db==0?col::gridStrong:col::grid);p.drawLine(QPointF(grid.left(),y(db)),QPointF(grid.right(),y(db)));p.setPen(col::gridText);p.drawText(QRectF(r.left()+1,y(db)-8,29,16),Qt::AlignRight|Qt::AlignVCenter,QString::number(db));}
        for(int hz:{30,100,1000,10000}){const double x=grid.left()+std::log(double(hz)/20)/std::log(1000.)*grid.width();p.setPen(col::grid);p.drawLine(QPointF(x,grid.top()),QPointF(x,grid.bottom()));p.setPen(col::gridText);p.drawText(QRectF(x-16,grid.bottom()+4,35,16),Qt::AlignCenter,hz>=1000?QString("%1k").arg(hz/1000):QString::number(hz));}
        QPainterPath path;for(int i=0;i<=int(grid.width());++i){double hz=20*std::pow(1000.,i/grid.width());double db=audio::drums::micResponse(hz,model,angle,cm,rate)+extra;QPointF point(grid.left()+i,y(std::clamp(db,-24.,6.)));if(i==0)path.moveTo(point);else path.lineTo(point);}
        p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(col::cyan,1.8));p.drawPath(path);
        path.lineTo(grid.bottomRight());path.lineTo(grid.bottomLeft());path.closeSubpath();p.fillPath(path,theme::withAlpha(col::cyan,18));
        p.setPen(col::label);p.drawText(r.adjusted(10,7,-10,-7),Qt::AlignTop|Qt::AlignLeft,"MODELED RESPONSE · dB");
        p.setPen(col::cyan);p.drawText(r.adjusted(10,7,-10,-7),Qt::AlignTop|Qt::AlignRight,QStringList{"Recording","Dynamic","Condenser","Ribbon"}[model]);
    }
private:std::function<int()>m_selected;
};

// Small functional navigation icons; generated natively so theme changes stay sharp.
QIcon studioIcon(int kind,const QColor& color){
    QPixmap pix(20,20);pix.fill(Qt::transparent);QPainter p(&pix);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(color,1.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    if(kind==0){p.drawEllipse(QRectF(2,6,13,8));p.drawLine(2,10,2,15);p.drawArc(QRectF(2,11,13,8),180*16,180*16);p.drawLine(6,2,17,7);}
    else if(kind==1){for(int i=0;i<3;++i){int x=4+i*6;p.drawLine(x,2,x,18);p.fillRect(QRectF(x-2,5+i*3,4,4),color);}}
    else if(kind==2){for(int y=0;y<3;++y)for(int x=0;x<4;++x){QRectF r(2+x*4.5,4+y*5,2.5,2.5);if((x+y)%3==0)p.fillRect(r,color);else p.drawRect(r);}}
    else {p.drawRoundedRect(QRectF(7,2,6,10),3,3);p.drawArc(QRectF(4,6,12,10),180*16,180*16);p.drawLine(10,16,10,19);p.drawLine(7,19,13,19);}
    return QIcon(pix);
}

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
        setObjectName("drumStudioEditor");setMinimumSize(880,500);setMaximumHeight(QWIDGETSIZE_MAX);
        auto* root=new QVBoxLayout(this);root->setContentsMargins(10,8,10,8);root->setSpacing(8);
        auto* bar=hbox(10);auto* title=new QLabel("DRUM STUDIO",this);title->setObjectName("studioHeading");bar->addWidget(title);
        auto* library=chooser(ds::ContentLibrary,160);library->setObjectName("studioContentLibrary");bar->addWidget(library);bar->addWidget(chooser(ds::Kit,112));auto* source=new QLabel(this);source->setObjectName("studioSourceInfo");source->setMinimumWidth(0);bar->addWidget(source,1);bar->addWidget(new QLabel("Output",this));bar->addWidget(field(ds::Volume,66));
        m_refreshers.push_back([this,source]{int pieces=0;for(int d=0;d<ds::kDrums;++d)pieces+=on(ds::piece(d,ds::Present));source->setText(QString("%1 pieces%2").arg(pieces).arg(pick(ds::ContentLibrary)>0&&!audio::drums::contentInstalled(pick(ds::ContentLibrary)-1)?" · Content missing":""));});
        root->addLayout(bar);auto* nav=hbox(4);m_pages=new QStackedWidget(this);
        for(int i=0;i<4;++i){auto* b=new QPushButton(QStringList{"Kit","Mixer","Grooves","Microphones"}[i],this);b->setObjectName(QString("studioTab%1").arg(i));b->setCheckable(true);b->setMinimumHeight(30);b->setIconSize(QSize(20,20));m_tabs.push_back(b);connect(b,&QPushButton::clicked,this,[this,i]{setPage(i);});nav->addWidget(b);}
        nav->addStretch();root->addLayout(nav);root->addWidget(m_pages,1);
        m_pages->addWidget(kitPage());m_pages->addWidget(mixerPage());m_pages->addWidget(groovePage());m_pages->addWidget(micPage());
        setPage(0);selectDrum(studioDrums().value(c.deviceId,ds::Snare));applyStyle();refresh();
        connect(theme::notifier(),&theme::Notifier::changed,this,[this]{applyStyle();});
        connect(c.project,&Project::restored,this,[this]{refresh();});
        m_preview.setInterval(4);m_preview.setTimerType(Qt::PreciseTimer);connect(&m_preview,&QTimer::timeout,this,[this]{previewTick();});
    }
    void refresh() override {
        m_ctx.track = ctx().project->indexOfTrack(ctx().trackId); DeviceBody::refresh();
        if (m_info && m_index >= 0) { const auto& g = grooves::all()[size_t(m_index)]; m_info->setText(QString("%1 / %2 · %3 · Set %4 BPM").arg(g.style, g.name, grooves::describe(g)).arg(ctx().project->tempo, 0, 'f', 0)); }
    }
protected:
    void hideEvent(QHideEvent* e) override { stopPreview(); DeviceBody::hideEvent(e); }
private:
    void setPage(int i) { if (i != 2) stopPreview(); m_pages->setCurrentIndex(i); for (int k = 0; k < 4; ++k) m_tabs[k]->setChecked(k == i); }
    int track() const { return ctx().project->indexOfTrack(ctx().trackId); }
    QPushButton* action(const QString& text, const QString& id, std::function<void()> fn) {
        auto* b = new QPushButton(text, this); b->setObjectName(id); connect(b, &QPushButton::clicked, this, std::move(fn)); return b;
    }
    void audition(int note) { const int d=ds::drumFor(note); if(d>=0 && !on(ds::piece(d,ds::Present)))return; if (ctx().transport) ctx().transport->previewNote(track(), note, m_velocity ? m_velocity->value() : .8); }
    void selectDrum(int d) {
        m_drum = d; studioDrums().insert(ctx().deviceId, d); m_drumTitle->setText(ds::drumName(d)); m_drumControls->setCurrentIndex(d);
        m_articulations->clear();
        const std::vector<std::vector<std::pair<const char*, int>>> arts{
            {{"Kick",36}}, {{"Center",38},{"Rimshot",40},{"Side stick",37},{"Clap",39}},
            {{"Closed",42},{"Pedal",44},{"Open",46}}, {{"High tom",48}}, {{"Center",45},{"Edge",47}},
            {{"Center",41},{"Edge",43}}, {{"Bow",51},{"Bell",53},{"Shank",59}},
            {{"Crash 1",49},{"Crash 2",57},{"Splash",55},{"China",52}}};
        for (const auto& a : arts[d]) m_articulations->addItem(a.first, a.second);
        m_note = ds::noteFor(d); refresh(); update();
    }
    QWidget* kitPage() {
        auto* page=new QWidget(this);auto* row=new QHBoxLayout(page);row->setContentsMargins(0,0,0,0);row->setSpacing(10);
        auto* stageCol=vbox(6);auto* tools=hbox(5);
        auto* move=action("Move pieces","studioMovePieces",[this]{m_kit->setMoveMode(m_moveButton->isChecked());});move->setCheckable(true);m_moveButton=move;tools->addWidget(move);
        auto* add=action("Add piece","studioAddPiece",[]{});auto* menu=new QMenu(add);add->setMenu(menu);
        for(int d=0;d<ds::kDrums;++d){auto* a=menu->addAction(ds::drumName(d));a->setObjectName(QString("studioAdd%1").arg(d));connect(a,&QAction::triggered,this,[this,d]{edit(ds::piece(d,ds::Present),1);selectDrum(d);});connect(menu,&QMenu::aboutToShow,a,[this,d,a]{a->setEnabled(!on(ds::piece(d,ds::Present)));});}
        tools->addWidget(add);tools->addStretch();tools->addWidget(action("Reset layout","studioResetLayout",[this]{std::vector<std::pair<int,double>>changes;for(int d=0;d<ds::kDrums;++d){changes.push_back({ds::piece(d,ds::StageX),ds::stageX[d]});changes.push_back({ds::piece(d,ds::StageY),ds::stageY[d]});}editMany(changes,12);}));stageCol->addLayout(tools);
        m_kit=new KitView(this,580,380,[this](int d){selectDrum(d);});m_kit->setAudition([this](int d){audition(ds::noteFor(d));});m_kit->setObjectName("studioKitStage");m_kit->setMinimumSize(360,260);m_kit->setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);animate(m_kit);stageCol->addWidget(m_kit,1);
        auto* pads=hbox(3);
        for(int d=0;d<ds::kDrums;++d){auto* hit=action(ds::drumName(d),QString("studioSelect%1").arg(d),[this,d]{selectDrum(d);if(on(ds::piece(d,ds::Present)))audition(ds::noteFor(d));else edit(ds::piece(d,ds::Present),1);});hit->setCheckable(true);hit->setMinimumWidth(0);pads->addWidget(hit,1);m_refreshers.push_back([this,d,hit]{hit->setChecked(d==m_drum);hit->setText(QString(on(ds::piece(d,ds::Present))?"%1":"+ %1").arg(ds::drumName(d)));});}stageCol->addLayout(pads);row->addLayout(stageCol,6);
        auto* sideFrame=new QFrame(page);sideFrame->setObjectName("studioCard");sideFrame->setMinimumWidth(330);sideFrame->setMaximumWidth(420);auto* side=new QVBoxLayout(sideFrame);side->setContentsMargins(12,10,12,10);side->setSpacing(8);
        auto* heading=hbox(6);m_drumTitle=new QLabel("Snare",this);m_drumTitle->setObjectName("studioPieceTitle");heading->addWidget(m_drumTitle);heading->addStretch();m_present=action("Remove","studioPresent",[this]{edit(ds::piece(m_drum,ds::Present),on(ds::piece(m_drum,ds::Present))?0:1);});heading->addWidget(m_present);side->addLayout(heading);
        auto* artRow=hbox(5);m_articulations=new QComboBox(this);m_articulations->setObjectName("studioArticulation");artRow->addWidget(m_articulations,1);
        connect(m_articulations,&QComboBox::activated,this,[this](int i){m_note=m_articulations->itemData(i).toInt();audition(m_note);update();});
        artRow->addWidget(action("Audition","studioAudition",[this]{audition(m_note);}));side->addLayout(artRow);
        m_wave=new DrumWave(this,[this]{return m_note;},[this]{return m_drum;},[this]{return m_velocity?m_velocity->value():.8;});watch(m_wave);side->addWidget(m_wave,1);
        m_drumControls=new QStackedWidget(this);
        for(int d=0;d<ds::kDrums;++d){
            auto* w=new QWidget(this);auto* controls=new QVBoxLayout(w);controls->setContentsMargins(0,0,0,0);controls->setSpacing(7);
            auto* contentRow=hbox(6);contentRow->addWidget(new QLabel("Library",w));auto* content=chooser(ds::contentSource(d),232);content->setObjectName(QString("studioContentSource%1").arg(d));contentRow->addWidget(content,1);controls->addLayout(contentRow);
            auto* sourceRow=hbox(6);sourceRow->addWidget(new QLabel("Original",w));auto* source=chooser(ds::piece(d,ds::Source),232);source->setObjectName(QString("studioPieceSource%1").arg(d));sourceRow->addWidget(source,1);controls->addLayout(sourceRow);
            auto* knobs=hbox(3);for(int k=0;k<ds::kPerDrum;++k)knobs->addWidget(knob(ds::param(d,k),42,QStringList{"Level","Pan","Tune","Decay","Tone"}[k],k==ds::Pan||k==ds::Tune),1);controls->addLayout(knobs);
            auto* switches=hbox(6);switches->addWidget(toggle(ds::piece(d,ds::Present),"In kit",46));switches->addWidget(toggle(ds::mute(d),"Mute",44));switches->addWidget(toggle(ds::solo(d),"Solo",44));switches->addStretch();switches->addWidget(new QLabel("X",w));switches->addWidget(field(ds::piece(d,ds::StageX),42));switches->addWidget(new QLabel("Y",w));switches->addWidget(field(ds::piece(d,ds::StageY),42));controls->addLayout(switches);m_drumControls->addWidget(w);
        }side->addWidget(m_drumControls);
        auto* lower=hbox(5);lower->addWidget(knob(ds::Wires,32,"Wires"));lower->addWidget(knob(ds::Beater,32,"Beater"));lower->addWidget(knob(ds::Velocity,32,"Response"));lower->addStretch();
        auto* velCol=vbox(3);velCol->addWidget(new QLabel("Audition velocity",this));m_velocity=new DragValue(this);m_velocity->setObjectName("studioVelocity");m_velocity->setRange(.05,1);m_velocity->setValue(.8,false);m_velocity->setFixedSize(92,28);m_velocity->setFormatter([](double v){return QString::number(int(std::lround(v*127)));});connect(m_velocity,&DragValue::valueEdited,this,[this]{m_wave->update();});velCol->addWidget(m_velocity);lower->addLayout(velCol);side->addLayout(lower);row->addWidget(sideFrame,4);
        m_refreshers.push_back([this]{if(m_present){const bool present=on(ds::piece(m_drum,ds::Present));m_present->setText(present?"Remove":"Add to kit");m_present->setProperty("mapTarget",target(ds::piece(m_drum,ds::Present)));}});
        return page;
    }
    void selectMic(int d){
        m_mic=std::clamp(d,0,ds::kDrums+1);if(m_micChoices){QSignalBlocker block(m_micChoices);m_micChoices->setCurrentIndex(m_mic);}if(m_micControls)m_micControls->setCurrentIndex(m_mic);if(m_micStage)m_micStage->selectMic(m_mic);if(m_response)m_response->update();
    }
    QWidget* micPage(){
        auto* page=new QWidget(this);auto* row=new QHBoxLayout(page);row->setContentsMargins(0,0,0,0);row->setSpacing(10);
        auto* stageCol=vbox(7);auto* tools=hbox(8);tools->addWidget(section("MICROPHONES"));tools->addStretch();auto* modeled=new QLabel("Recorded channels + mic models",this);modeled->setObjectName("studioModelNotice");tools->addWidget(modeled);stageCol->addLayout(tools);
        m_micStage=new KitView(this,580,380,[this](int d){selectDrum(d);});m_micStage->setObjectName("studioMicStage");m_micStage->setMicMode([this](int d){selectMic(d);});m_micStage->setMinimumSize(360,280);m_micStage->setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);animate(m_micStage);stageCol->addWidget(m_micStage,1);
        auto* buses=hbox(10);for(auto pair:{std::pair<int,QString>{ds::Overheads,"Overheads"},{ds::Room,"Room"},{ds::RoomSize,"Size"},{ds::Bleed,"Bleed"},{ds::Width,"Width"}})buses->addWidget(knob(pair.first,34,pair.second),1);stageCol->addLayout(buses);row->addLayout(stageCol,6);
        auto* card=new QFrame(page);card->setObjectName("studioCard");card->setMinimumWidth(330);card->setMaximumWidth(420);auto* side=new QVBoxLayout(card);side->setContentsMargins(12,10,12,10);side->setSpacing(10);
        m_micChoices=new QComboBox(card);m_micChoices->setObjectName("studioMicSelection");for(int d=0;d<ds::kDrums;++d)m_micChoices->addItem(QString("%1 · Close mic").arg(ds::drumName(d)));m_micChoices->addItem("Overhead pair");m_micChoices->addItem("Room pair");side->addWidget(m_micChoices);connect(m_micChoices,&QComboBox::currentIndexChanged,this,[this](int d){selectMic(d);});
        m_response=new MicResponse(this,[this]{return m_mic;});watch(m_response);side->addWidget(m_response,1);m_micControls=new QStackedWidget(this);
        for(int d=0;d<ds::kDrums+2;++d){
            auto* w=new QWidget(card);auto* controls=new QVBoxLayout(w);controls->setContentsMargins(0,0,0,0);controls->setSpacing(10);const bool close=d<ds::kDrums,oh=d==ds::kDrums;
            auto* modelRow=hbox(6);modelRow->addWidget(new QLabel("Model",w));auto* model=chooser(close?ds::piece(d,ds::MicModel):oh?ds::OhModel:ds::RoomModel,220);model->setObjectName(QString("studioMicModel%1").arg(d));modelRow->addWidget(model,1);controls->addLayout(modelRow);
            if(close){auto* recorded=hbox(6);recorded->addWidget(new QLabel("Recorded source",w));auto* variant=chooser(ds::recordedMic(d),152);variant->setObjectName(QString("studioRecordedMic%1").arg(d));InfoView::attach(variant,"Recorded source","Primary or alternate recorded microphone when supplied by this library. If no alternate recording exists, Primary is used. Tonal Model is applied after the chosen recording.");recorded->addWidget(variant,1);controls->addLayout(recorded);}
            auto* knobs=hbox(8);if(close){knobs->addWidget(knob(ds::piece(d,ds::MicDistance),52,"Distance · cm"),1);knobs->addWidget(knob(ds::piece(d,ds::MicAngle),52,"Off axis"),1);knobs->addWidget(knob(ds::param(d,ds::Level),42,"Level"),1);}
            else{knobs->addWidget(knob(oh?ds::OhHeight:ds::RoomDistance,52,oh?"Height · m":"Distance · m"),1);knobs->addWidget(knob(oh?ds::Overheads:ds::Room,52,"Level"),1);knobs->addWidget(knob(oh?ds::Width:ds::RoomSize,42,oh?"Width":"Size"),1);}controls->addLayout(knobs);m_micControls->addWidget(w);
        }side->addWidget(m_micControls);auto* micTools=hbox(6);micTools->addWidget(action("Audition","studioMicAudition",[this]{audition(ds::noteFor(m_mic<ds::kDrums?m_mic:m_drum));}));micTools->addWidget(action("Reset mic","studioResetMic",[this]{std::vector<std::pair<int,double>>changes;const auto* spec=specOf(ctx().type);if(m_mic<ds::kDrums){for(int k:{ds::MicModel,ds::MicDistance,ds::MicAngle}){int i=ds::piece(m_mic,k);changes.push_back({i,spec->params[size_t(i)].def});}}else{for(int i: m_mic==ds::kDrums?std::vector<int>{ds::OhModel,ds::OhHeight}:std::vector<int>{ds::RoomModel,ds::RoomDistance})changes.push_back({i,spec->params[size_t(i)].def});}editMany(changes,28);}));micTools->addStretch();side->addLayout(micTools);row->addWidget(card,4);selectMic(ds::Snare);return page;
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
    void applyStyle(){
        setStyleSheet(QString(
        "QWidget#drumStudioEditor{background:%1;color:%2;} QLabel{color:%2;background:transparent;border:0;font-size:11px;}"
        "QLabel#studioHeading{font-size:13px;font-weight:600;letter-spacing:1px;} QLabel#studioPieceTitle{font-size:17px;font-weight:600;}"
        "QLabel#studioSourceInfo,QLabel#studioModelNotice{color:%8;font-size:11px;}"
        "QFrame#studioCard,QFrame#studioStrip{background:%9;border:1px solid %4;border-radius:8px;}"
        "QPushButton{background:%5;color:%2;border:1px solid %4;border-radius:6px;padding:5px 8px;font-size:11px;}"
        "QPushButton:hover{border-color:%8;}QPushButton:checked{background:%6;color:%7;border-color:%6;} QPushButton:disabled{color:%8;}"
        "QComboBox,QLineEdit{background:%3;color:%2;border:1px solid %4;border-radius:5px;min-height:26px;padding:0 5px;}"
        "QListWidget{background:%3;color:%2;border:1px solid %4;border-radius:6px;font-size:11px;} QListWidget::item{padding:6px;} QListWidget::item:selected{background:%6;color:%7;}"
        "QSlider::groove:vertical{background:%3;width:5px;border-radius:2px;}QSlider::handle:vertical{background:%2;height:9px;margin:0 -7px;border-radius:3px;}"
        ).arg(theme::panel.name(),col::value.name(),col::display.name(),col::separator.name(),col::button.name(),theme::accent.name(),col::buttonOnText.name(),col::label.name(),theme::devCell.name()));
        for(int i=0;i<int(m_tabs.size());++i)m_tabs[size_t(i)]->setIcon(studioIcon(i,col::value));
    }
    KitView *m_kit=nullptr,*m_micStage=nullptr; DrumWave* m_wave=nullptr; MicResponse* m_response=nullptr;
    QPushButton *m_present=nullptr,*m_moveButton=nullptr;QComboBox* m_micChoices=nullptr;QStackedWidget* m_micControls=nullptr;int m_mic=ds::Snare;
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
