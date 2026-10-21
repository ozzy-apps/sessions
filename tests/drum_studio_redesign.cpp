// Recorded audio, persistence, direct manipulation and embedded layout regression.
#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "audio/DrumMicModel.h"
#include "audio/Engine.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QPointer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSet>
#include <algorithm>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <atomic>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace sessions;
namespace ds=drumstudio;
namespace {
int checks=0,failures=0;std::atomic<bool> watchAlloc{false};std::atomic<int> allocations{0};
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}}
void settle(){QApplication::processEvents();QApplication::processEvents();}
std::vector<float> defaults(){std::vector<float> p;for(const auto& param:specOf(DeviceType::DrumStudio)->params)p.push_back(param.def);return p;}
std::vector<float> render(const std::vector<float>& p,int note=38,int rate=48000,int block=audio::kMaxBlock){
    auto processor=audio::createProcessor(DeviceType::DrumStudio,rate);audio::Context context;context.sampleRate=rate;
    std::vector<float> out(rate*3); // mono sum; enough time for the full snare recording
    for(int at=0;at<int(out.size());at+=block){const int n=std::min(block,int(out.size())-at);float l[audio::kMaxBlock]{},r[audio::kMaxBlock]{};audio::EventList ev;if(at==0)ev.add({0,note,.8f,rate/10});
        watchAlloc=true;processor->render(ev,l,r,n,p.data(),context);watchAlloc=false;
        for(int i=0;i<n;++i)out[size_t(at+i)]=l[i]+r[i];
    }return out;
}
double energy(const std::vector<float>& x){double e=0;for(float s:x)e+=s*s;return e;}
double difference(const std::vector<float>& a,const std::vector<float>& b){double e=0;for(size_t i=0;i<a.size();++i)e+=std::abs(a[i]-b[i]);return e;}
double bin(const std::vector<float>& x,double hz){std::complex<double> sum{};const auto step=std::polar(1.,-hz*6.28318530718/48000),origin=std::complex<double>(1,0);auto phase=origin;for(float s:x){sum+=double(s)*phase;phase*=step;}return std::abs(sum);}
QRectF stageRect(QWidget* stage){const auto r=QRectF(stage->rect()).adjusted(21.5,25.5,-21.5,-31.5);const double s=std::min(r.width(),r.height()*1.3);return {r.center().x()-s*.5,r.center().y()-s/2.6,s,s/1.3};}
void drag(QWidget* w,QPoint start,QPoint end){QTest::mousePress(w,Qt::LeftButton,Qt::NoModifier,start);QMouseEvent move(QEvent::MouseMove,end,w->mapToGlobal(end),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(w,&move);QTest::mouseRelease(w,Qt::LeftButton,Qt::NoModifier,end);settle();}
}
void* operator new(std::size_t n){if(watchAlloc.load(std::memory_order_relaxed))++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(int argc,char** argv){
    QApplication app(argc,argv);QTemporaryDir prefs;QCoreApplication::setOrganizationName("SessionsDrumRedesignTest");QCoreApplication::setApplicationName("DrumRedesign");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,prefs.path());theme::apply(theme::Kind::Dark);
    auto p=defaults();check(p.size()==ds::kParams&&ds::kParams<=audio::kMaxParams,"Appended kit/mic controls fit the engine's parameter capacity");
    Project project;project.scenes={"1"};project.addTrack("Drum Studio",theme::accent);project.tracks[0].devices={project.makeDevice("Drum Studio",DeviceKind::Instrument)};
    auto& initial=project.tracks[0].devices[0];auto json=setfile::deviceToJson(initial);auto params=json["params"].toArray();
    while(params.size()>ds::kLegacyParams)params.removeLast();json["params"]=params;const auto legacy=setfile::deviceFromJson(json);
    bool migration=legacy.params.size()==ds::kParams;for(int i=0;i<ds::kLegacyParams;++i)migration&=legacy.params[i].value==initial.params[i].value;for(int i=ds::kLegacyParams;i<ds::kParams;++i)migration&=std::abs(legacy.params[i].value-p[size_t(i)])<1e-6;
    check(migration,"A saved 66-control kit restores all legacy values and neutral new defaults");
    p[ds::Overheads]=p[ds::Room]=p[ds::Bleed]=0;
    const auto original=render(p);check(energy(original)>.1,"The default recorded snare plays through the neutral microphone");
    for(int d=0;d<ds::kDrums;++d){auto removed=p;removed[ds::piece(d,ds::Present)]=0;check(energy(render(removed,ds::noteFor(d)))==0,"Removing a kit piece silences its MIDI group");}
    auto hiddenSolo=p;hiddenSolo[ds::piece(ds::Kick,ds::Present)]=0;hiddenSolo[ds::solo(ds::Kick)]=1;check(render(hiddenSolo)==original,"A removed soloed piece does not silence the remaining kit");
    auto swapped=p;swapped[ds::piece(ds::Snare,ds::Source)]=choicePosition(1,4);check(render(swapped)==original,"Explicit Black Pearl matches the original recipe recording");
    swapped[ds::piece(ds::Snare,ds::Source)]=choicePosition(2,4);check(difference(render(swapped),original)>10,"Swapping a piece changes its actual recording");check(render(swapped,36)==render(p,36),"Swapping a snare leaves the kick recording unchanged");
    for(int model=1;model<4;++model){auto colored=p;colored[ds::piece(ds::Snare,ds::MicModel)]=choicePosition(model,4);const auto actual=render(colored);check(difference(actual,original)>1,"Each microphone model audibly changes the close recording");for(double hz:{200.,1000.,6000.}){const double db=20*std::log10(bin(actual,hz)/bin(original,hz));check(std::abs(db-audio::drums::micResponse(hz,model,0,10))<.08,"Displayed mic response matches the rendered audio transfer");}}
    auto distant=p;distant[ds::piece(ds::Snare,ds::MicDistance)]=1;check(energy(render(distant))<energy(original)*.25,"Increasing close-mic distance attenuates and delays the recording");
    auto angled=p;angled[ds::piece(ds::Snare,ds::MicAngle)]=1;check(energy(render(angled))<energy(original)*.5,"Off-axis placement audibly softens the close recording");
    check(difference(render(distant,38,48000,64),render(distant,38,48000,127))<.0001,"Microphone processing remains continuous across callback sizes");
    for(int rate:{8000,44100,96000})for(int model=0;model<4;++model){auto stress=defaults();stress[ds::piece(ds::Snare,ds::MicModel)]=choicePosition(model,4);stress[ds::piece(ds::Snare,ds::MicDistance)]=1;stress[ds::piece(ds::Snare,ds::MicAngle)]=1;stress[ds::OhModel]=stress[ds::RoomModel]=choicePosition(model,4);stress[ds::OhHeight]=stress[ds::RoomDistance]=1;const auto audio=render(stress,38,rate);check(std::all_of(audio.begin(),audio.end(),[](float s){return std::isfinite(s)&&std::abs(s)<4;}),"Mic models and maximum distances remain bounded across audio rates");}
    check(allocations==0,"All new microphone and piece paths allocate zero memory in audio rendering");
    audio::Engine engine(&project);Transport transport(&project,&engine);DeviceView view(&project,&transport);view.resize(1480,780);view.show();settle();view.findChild<QPushButton*>("studioExpand")->click();settle();auto* editor=view.expandedEditor();QPointer<devui::DeviceBody> stable=editor;
    check(editor&&!editor->isWindow()&&editor->objectName()=="drumStudioEditor","The redesigned studio uses the existing embedded editor host");if(!editor)return 1;
    auto current=[&]()->Device&{return project.tracks[0].devices[0];};
    QSet<QString> targets;for(auto* w:editor->findChildren<QWidget*>())targets.insert(w->property("mapTarget").toString());bool mapped=true;for(int i=0;i<ds::kParams;++i)mapped&=targets.contains(editor->target(i));check(mapped,"Every legacy and appended parameter has a real mappable control in the editor");
    auto* stage=editor->findChild<QWidget*>("studioKitStage");auto* waveform=editor->findChild<QWidget*>("studioWaveform");check(stage&&stage->width()>600&&stage->height()>380&&waveform->height()>=112,"Kit stage and source waveform use large readable drawing areas");
    auto* velocity=editor->findChild<DragValue*>("studioVelocity");velocity->setValue(.15,false);
    const auto auditionRect=stageRect(stage);const QPoint auditionPoint=(auditionRect.topLeft()+QPointF(ds::stageX[ds::Snare]*auditionRect.width(),ds::stageY[ds::Snare]*auditionRect.height())).toPoint();
    QTest::mouseClick(stage,Qt::LeftButton,Qt::NoModifier,auditionPoint);float audioBlock[audio::kMaxBlock*2]{};engine.process(audioBlock,audio::kMaxBlock);
    check(engine.deviceScope(current().id,ds::Snare)>.001&&engine.deviceScope(current().id,ds::Snare)<.15,"Clicking the kit stage uses the inspector's audition velocity");velocity->setValue(.8,false);
    auto* remove=editor->findChild<QPushButton*>("studioPresent");const int undo=project.undoStack()->index();remove->click();settle();check(current().params[ds::piece(ds::Snare,ds::Present)].value==0&&project.undoStack()->index()==undo+1,"Remove changes the saved presence control in one undo step");project.undoStack()->undo();settle();check(stable==view.expandedEditor()&&current().params[ds::piece(ds::Snare,ds::Present)].value==1,"Undo restores a removed piece without destroying the editor");
    editor->findChild<QPushButton*>("studioSelect7")->click();remove->click();settle();auto* add=editor->findChild<QAction*>("studioAdd7");add->trigger();settle();check(current().params[ds::piece(ds::Crash,ds::Present)].value==1,"The Add menu restores a removed cymbal");
    editor->findChild<QPushButton*>("studioSelect1")->click();auto* move=editor->findChild<QPushButton*>("studioMovePieces");move->click();const auto rectangle=stageRect(stage);const QPoint center=(rectangle.topLeft()+QPointF(ds::stageX[ds::Snare]*rectangle.width(),ds::stageY[ds::Snare]*rectangle.height())).toPoint();const int beforeDrag=project.undoStack()->index();drag(stage,center,center+QPoint(42,18));check(current().params[ds::piece(ds::Snare,ds::StageX)].value>ds::stageX[ds::Snare]+.03&&project.undoStack()->index()==beforeDrag+1,"Dragging a kit piece edits both saved coordinates in one undo step");project.undoStack()->undo();settle();check(std::abs(current().params[ds::piece(ds::Snare,ds::StageX)].value-ds::stageX[ds::Snare])<1e-6,"Undo restores the kit layout");
    editor->findChild<QPushButton*>("studioTab3")->click();settle();auto* selection=editor->findChild<QComboBox*>("studioMicSelection");auto* micStage=editor->findChild<QWidget*>("studioMicStage");selection->setCurrentIndex(ds::Snare);settle();const auto room=stageRect(micStage);const QPointF drum=room.topLeft()+QPointF(ds::stageX[ds::Snare]*room.width(),ds::stageY[ds::Snare]*room.height());const double micRadius=ds::stageRadius[ds::Snare]*room.height()+room.height()*.035;const QPoint mic=(drum+QPointF(-micRadius/std::sqrt(2.),-micRadius/std::sqrt(2.))).toPoint();const int beforeMic=project.undoStack()->index();drag(micStage,mic,mic+QPoint(-26,-30));check(current().params[ds::piece(ds::Snare,ds::MicDistance)].value>.3&&project.undoStack()->index()==beforeMic+1,"Dragging a close microphone changes distance and angle in one undo step");
    auto* modelControl=editor->findChild<QWidget*>("studioMicModel1");
    const int modelUndo=project.undoStack()->index();
    QWheelEvent wheel(QPointF(20,8),modelControl->mapToGlobal(QPoint(20,8)),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(modelControl,&wheel);settle();
    check(choiceIndex(DeviceType::DrumStudio,ds::piece(ds::Snare,ds::MicModel),current().params[ds::piece(ds::Snare,ds::MicModel)].value)==1&&project.undoStack()->index()==modelUndo+1,"The mic selector changes the actual saved model and supports undo");
    auto saved=setfile::deviceFromJson(setfile::deviceToJson(current()));check(saved.params[ds::piece(ds::Snare,ds::MicDistance)].value==current().params[ds::piece(ds::Snare,ds::MicDistance)].value,"Microphone placement survives project serialization");project.undoStack()->undo();project.undoStack()->undo();settle();
    editor->edit(ds::piece(ds::Snare,ds::MicModel),choicePosition(3,4));
    editor->edit(ds::piece(ds::Snare,ds::MicDistance),.35);
    editor->edit(ds::piece(ds::Snare,ds::MicAngle),.3);
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):QString();if(!output.isEmpty())QDir().mkpath(output);
    for(auto themeKind:{theme::Kind::Dark,theme::Kind::Light})for(auto size:{QSize(1480,780),QSize(920,590)}){
        theme::apply(themeKind);view.resize(size);settle();const QString themeName=themeKind==theme::Kind::Dark?"dark":"light";
        for(int page=0;page<4;++page){editor->findChild<QPushButton*>(QString("studioTab%1").arg(page))->click();settle();bool overflow=false;for(auto* scroll:view.findChildren<QScrollArea*>())if(scroll->isVisible())overflow|=scroll->horizontalScrollBar()->maximum()>0;check(!overflow,"Expanded pages fit the narrow and wide editor without horizontal scrolling");if(!output.isEmpty())editor->grab().save(output+QString("/studio-%1-%2-%3.png").arg(page).arg(size.width()).arg(themeName));}
    }
    view.collapseEditor();view.expandDevice(current().id);settle();check(view.expandedEditor()==stable,"Collapsing and reopening preserves the redesigned editor");
    std::printf("drum_studio_redesign: %d checks, %d failures, %d callback allocations\n",checks,failures,allocations.load());return failures?1:0;
}
