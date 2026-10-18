#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "audio/Engine.h"
#include "core/Grooves.h"
#include "core/Presets.h"
#include "core/SetFile.h"
#include "core/Sample.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/MainWindow.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTest>
#include <QTextStream>
#include <QToolButton>
#include <QUndoStack>
#include <QtEndian>
#include <cmath>
#include <cstdio>
#include <set>
using namespace sessions;
namespace ds = drumstudio;
static int checks = 0, failures = 0;
#define CHECK(c,m) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL: %s\n",qPrintable(QString(m))); } } while(0)
struct Render { std::vector<float> audio; double peak=0, rms=0; bool finite=true; };
static std::vector<float> defaults(int recipe=0) { std::vector<float> p; for(const auto& x:specOf(DeviceType::DrumStudio)->params)p.push_back(x.def);p[ds::Kit]=choicePosition(recipe,ds::kKits);return p; }
static Render render(const QVector<Note>& notes, std::vector<float> params, double tempo=120, double seconds=2, int rate=48000) {
    auto processor=audio::createProcessor(DeviceType::DrumStudio,rate);audio::Context ctx;ctx.sampleRate=rate;ctx.tempo=tempo;ctx.beatsPerSample=tempo/(rate*60.);ctx.playing=true;
    Render r;r.audio.resize(size_t(seconds*rate)*2);int next=0;double sum=0;
    for(int pos=0;pos<int(r.audio.size()/2);pos+=audio::kMaxBlock){
        const int n=std::min(audio::kMaxBlock,int(r.audio.size()/2)-pos);audio::EventList ev;float l[audio::kMaxBlock]{},rr[audio::kMaxBlock]{};
        while(next<notes.size()){const auto& note=notes[next];const int at=int(std::llround(note.start/tempo*60*rate));if(at>=pos+n)break;ev.add({std::max(0,at-pos),note.pitch,float(note.velocity),int(.1*rate)});++next;}
        ctx.beat=pos*ctx.beatsPerSample;processor->render(ev,l,rr,n,params.data(),ctx);
        for(int i=0;i<n;++i)for(int ch=0;ch<2;++ch){const float x=ch?rr[i]:l[i];r.audio[size_t(pos+i)*2+ch]=x;r.finite&=std::isfinite(x);r.peak=std::max(r.peak,double(std::abs(x)));sum+=x*x;}
    }r.rms=std::sqrt(sum/r.audio.size());return r;
}
static double energy(const Render& r,int first,int end){double sum=0;for(int i=first;i<end;++i)sum+=r.audio[size_t(i)*2]*r.audio[size_t(i)*2];return sum/(end-first);}
static bool midiValid(const QByteArray& bytes,int expected,int bars){
    if(bytes.size()<22 || bytes.first(4)!="MThd" || bytes.mid(14,4)!="MTrk" || qFromBigEndian<quint16>(bytes.constData()+12)!=960)return false;
    if(qFromBigEndian<quint32>(bytes.constData()+18)!=quint32(bytes.size()-22))return false;
    int at=22,tick=0,count=0;std::multiset<int> active;bool end=false,tempo=false,meter=false;
    auto vlq=[&](){int n=0;for(int k=0;k<4&&at<bytes.size();++k){const auto b=quint8(bytes[at++]);n=(n<<7)|(b&127);if(!(b&128))return n;}return -1;};
    while(at<bytes.size()){
        const int dt=vlq();if(dt<0 || at>=bytes.size())return false;tick+=dt;const quint8 status=quint8(bytes[at++]);
        if(status==0xff){if(at>=bytes.size())return false;int type=quint8(bytes[at++]);int size=vlq();if(size<0||at+size>bytes.size())return false;tempo|=type==0x51&&size==3;meter|=type==0x58&&size==4;
            if(type==0x2f){end=tick==bars*4*960&&size==0&&active.empty();}at+=size;
        }else if(status==0x99||status==0x89){if(at+2>bytes.size())return false;int note=quint8(bytes[at++]),vel=quint8(bytes[at++]);if(note>127||vel>127)return false;if(status==0x99){++count;active.insert(note);}else{auto it=active.find(note);if(it==active.end())return false;active.erase(it);}}
        else return false;
    }return end&&tempo&&meter&&count==expected;
}
int main(int argc,char**argv){
    QApplication app(argc,argv);theme::apply(theme::Kind::Dark);const QString exportDir=argc>1?QString::fromLocal8Bit(argv[1]):QString();
    CHECK(audio::drums::error().isEmpty(),audio::drums::error());CHECK(audio::drums::loadedLayers()==285,"All 285 bundled recorded layers load without external plugins");
    for(int b=0;b<3;++b)for(int note:{36,37,38,39,40,41,43,42,44,45,46,47,49,51,52,53,55,57,59}){
        const auto& layers=audio::drums::library()[b].notes[note];bool valid=true;
        for(const auto& layer:layers)valid&=layer.rate==44100&&layer.mono.size()>400&&std::all_of(layer.mono.begin(),layer.mono.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;});
        CHECK(valid,QString("Recorded source %1 / articulation %2 has five valid layers").arg(b).arg(note));
        CHECK(layers[0].mono!=layers[4].mono,"Velocity changes the recorded waveform, not only its amplitude");
    }
    double worst=0;
    for(int kit=0;kit<6;++kit)for(int note:{35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,55,57,59}){
        auto r=render({Note{note,0,.1,.85,1}},defaults(kit),120,1.1);CHECK(r.finite&&r.peak>.005&&r.peak<1,QString("Recipe %1 / GM %2 is audible and below full scale").arg(kit).arg(note));worst=std::max(worst,r.peak);
    }
    auto p=defaults();p[ds::Overheads]=p[ds::Room]=p[ds::Bleed]=0;p[ds::Width]=0;
    const auto loud=render({Note{38,0,.1,1,1}},p);const auto soft=render({Note{38,0,.1,.15,1}},p);CHECK(soft.rms<loud.rms*.3,"Ghost notes retain useful dynamic range");
    auto mute=p;mute[ds::mute(ds::Snare)]=1;CHECK(render({Note{38,0,.1,1,1}},mute).peak<1e-8,"Mute removes a drum from the close and ambience buses");
    auto solo=p;solo[ds::solo(ds::Kick)]=1;CHECK(render({Note{38,0,.1,1,1}},solo).peak<1e-8&&render({Note{36,0,.1,1,1}},solo).peak>.05,"Solo isolates its drum");
    auto shortP=p,longP=p;shortP[ds::param(ds::Kick,ds::Decay)]=0;longP[ds::param(ds::Kick,ds::Decay)]=1;
    CHECK(energy(render({Note{36,0,.1,1,1}},longP),20000,38000)>energy(render({Note{36,0,.1,1,1}},shortP),20000,38000)*4,"Decay changes the recorded tail without changing pitch");
    auto alone=render({Note{46,0,.1,1,1}},p);auto choke=render({Note{46,0,.1,1,1},Note{42,.2,.1,.6,1}},p);
    CHECK(energy(choke,15000,26000)<energy(alone,15000,26000)*.2,"A closed hat chokes the open recording with a short fade");
    for(int sr:{44100,96000}){auto r=render({Note{48,0,.1,.9,1}},p,120,1,sr);CHECK(r.finite&&r.peak>.02&&r.peak<1,"Source resampling and high tom tuning work at different audio rates");}
    CHECK(render({Note{60,0,.1,1,1}},p).peak==0,"Notes outside the GM map remain silent");
    QSet<QString> names;QStringList styles=grooves::styles();CHECK(styles.size()>=19,"At least 19 musical styles are available");
    Project clipPack; clipPack.scenes = {"1"};
    std::vector<float> reel;QString guide="# Drum Studio grooves\n\nGM MIDI channel 10, 960 PPQN, 4/4, tempo metadata, complete loop length.\nDrag grooves or use To Clip in Sessions; export preserves humanization.\nRecorded acoustic samples: Glen MacArthur / AVL Drumkits (see bundled credits).\n\n";
    if(!exportDir.isEmpty())QDir().mkpath(exportDir+"/MIDI");
    for(int i=0;i<int(grooves::all().size());++i){const auto& g=grooves::all()[size_t(i)];const QString key=QString("%1 - %2").arg(g.style,g.name);CHECK(!names.contains(key),"Groove names are unique within their style");names.insert(key);
        for(const auto& line:QString::fromLatin1(g.lanes).split('\n'))CHECK(line.mid(2).size()==g.bars*16,key+" has complete lanes");
        const auto notes=grooves::notes(g,1,1);bool valid=!notes.isEmpty();for(const auto& n:notes)valid&=ds::drumFor(n.pitch)>=0&&n.start>=0&&n.start<g.bars*4&&n.velocity>0&&n.velocity<=1;CHECK(valid,key+" produces valid GM notes");
        CHECK(midiValid(grooves::midi(g,1,1),notes.size(),g.bars),key+" exports complete, balanced MIDI with tempo and meter");
        const int recipe=QString(g.style)=="Metal"?3:QString(g.style)=="Jazz"?4:QString(g.style)=="Drum & Bass"?2:0;
        const auto r=render(grooves::notes(g),defaults(recipe),g.tempo,g.bars*240./g.tempo+.25);CHECK(r.finite&&r.rms>.001&&r.peak<1,key+" renders a usable groove without clipping");worst=std::max(worst,r.peak);
        if(!exportDir.isEmpty()){
            QString style=g.style;style.replace('&',"and");QDir().mkpath(exportDir+"/MIDI/"+style);QString error;
            CHECK(grooves::exportMidi(exportDir+"/MIDI/"+style+'/'+QString(g.name)+".mid",g,0,0,&error),error);
            QDir().mkpath(exportDir+"/Sessions Clips/"+style);
            const auto clip=grooves::makeClip(clipPack,g); const QString path=exportDir+"/Sessions Clips/"+style+'/'+QString(g.name)+".sessionsclip";
            CHECK(presets::saveClip(path,key,*clip,&error),error); Clip read;
            CHECK(presets::readClip(path,read,nullptr,&error)&&setfile::clipToJson(read)["notes"]==setfile::clipToJson(*clip)["notes"]&&read.length==clip->length&&read.name==key,"Native groove clips preserve every note and the loop length");
            guide+=QString("- %1 · %2\n").arg(key,grooves::describe(g));
            if(styles.indexOf(g.style)>=0){const QString marker="["+QString(g.style)+"]";if(!guide.contains(marker)){guide+="\n"+marker+QString(" audition starts at %1 s\n").arg(reel.size()/96000.,0,'f',2);reel.insert(reel.end(),r.audio.begin(),r.audio.end());reel.insert(reel.end(),12000*2,0);}}
        }
    }
    std::printf("%zu grooves, %lld styles, worst default peak %.3f\n",grooves::all().size(),styles.size(),worst);
    Project project;project.scenes={"1"};project.addTrack("Drums",theme::accent);project.tracks[0].devices={project.makeDevice("Drum Studio",DeviceKind::Instrument)};const int id=project.tracks[0].devices[0].id;
    CHECK(project.tracks[0].devices[0].params.size()==66,"Original 50 parameters are preserved and 16 mute/solo parameters appended");
    auto json=setfile::deviceToJson(project.tracks[0].devices[0]);auto old=json["params"].toArray();while(old.size()>50)old.removeLast();json["params"]=old;const auto restored=setfile::deviceFromJson(json);
    CHECK(restored.params.size()==66&&restored.params[ds::mute(ds::Snare)].value==0,"Old saved Drum Studios acquire unmuted channels");
    audio::Engine engine(&project);Transport transport(&project,&engine);DeviceView view(&project,&transport);view.resize(1240,600);view.show();QApplication::processEvents();
    view.findChild<QPushButton*>("studioExpand")->click();QApplication::processEvents();auto* editor=view.expandedEditor();QPointer<devui::DeviceBody> stable=editor;
    CHECK(view.editorExpanded()&&editor&&!editor->isWindow()&&editor->objectName()=="drumStudioEditor","Drum Studio expands inside the main device pane");
    editor->findChild<QPushButton*>("studioTab1")->click();QApplication::processEvents();auto* fader=editor->findChild<QSlider*>("studioFader1");const double before=project.tracks[0].devices[0].params[ds::param(ds::Snare,ds::Level)].value;
    const int undo=project.undoStack()->index();fader->setValue(500);CHECK(project.undoStack()->index()==undo+1,"Mixer fader edit is undoable");project.undoStack()->undo();QApplication::processEvents();CHECK(stable&&std::abs(project.tracks[0].devices[0].params[ds::param(ds::Snare,ds::Level)].value-before)<1e-8,"Undo preserves the expanded mixer and restores its level");
    view.rebuild();QApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);CHECK(view.expandedEditor()==stable,"Compact-panel rebuilds reuse the expanded editor");
    view.collapseEditor();CHECK(!view.editorExpanded(),"Collapse returns to the compact device chain");view.expandDevice(id);CHECK(view.expandedEditor()==stable&&view.editorExpanded(),"Reopening retains the editor and its page");
    editor->findChild<QPushButton*>("studioTab2")->click();auto* stylesBox=editor->findChild<QComboBox*>("studioStyles");stylesBox->setCurrentText("Techno");auto* list=editor->findChild<QListWidget*>("studioGrooveList");CHECK(list->count()==6,"Techno contains six playable grooves");
    auto* search=editor->findChild<QLineEdit*>("studioGrooveSearch");search->setText("warehouse");CHECK(list->count()==1,"Groove search combines style and text filtering");search->clear();auto* preview=editor->findChild<QPushButton*>("studioPreview");
    const int beforePreview=project.undoStack()->index();preview->click();QTest::qWait(12);CHECK(preview->isChecked()&&!transport.playing()&&project.undoStack()->index()==beforePreview&&!project.tracks[0].clipSlots[0],"Preview loops without starting transport or inserting clips");view.collapseEditor();QApplication::processEvents();CHECK(!preview->isChecked(),"Collapsing the editor stops groove preview");view.expandDevice(id);
    editor->findChild<QPushButton*>("studioToClip")->click();QApplication::processEvents();CHECK(project.tracks[0].clipSlots[0]&&!project.tracks[0].clipSlots[0]->notes.isEmpty(),"To Clip creates an editable MIDI clip on the instrument track");
    if(!exportDir.isEmpty()){
        QFile g(exportDir+"/guide.md");g.open(QIODevice::WriteOnly);g.write(guide.toUtf8());QString error;CHECK(samples::writeWav(exportDir+"/Drum Studio - Audition.wav",reel.data(),reel.size()/2,48000,samples::WavFormat::Int16,&error),error);
        editor->findChild<QPushButton*>("studioTab0")->click();QApplication::processEvents();editor->grab().save(exportDir+"/drum-studio-kit.png");editor->findChild<QPushButton*>("studioTab1")->click();QApplication::processEvents();editor->grab().save(exportDir+"/drum-studio-mixer.png");editor->findChild<QPushButton*>("studioTab2")->click();QApplication::processEvents();editor->grab().save(exportDir+"/drum-studio-grooves.png");theme::apply(theme::Kind::Light);QApplication::processEvents();editor->grab().save(exportDir+"/drum-studio-light.png");CHECK(list->styleSheet().contains(theme::devDisplay.name()) && list->styleSheet().contains(theme::devValue.name()), "The persistent groove list follows Light theme colors");theme::apply(theme::Kind::Dark);
    }
    project.addTrack("Other",theme::accent);project.selectedTrack=1;emit project.selectionChanged();QApplication::processEvents();CHECK(!view.editorExpanded()&&!stable->isVisible(),"Changing track hides the other track's expanded editor");project.selectedTrack=0;emit project.selectionChanged();QApplication::processEvents();CHECK(view.editorExpanded()&&view.expandedEditor()==stable,"Returning to the track restores its expanded editor");
    {EditScope edit(&project,"Delete Studio");project.tracks[0].devices.clear();}emit project.structureChanged();QApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);CHECK(!view.editorExpanded()&&!stable,"Deleting the instrument releases its expanded editor");
    // Exercise integration with the real splitter and the application's existing undo shortcut.
    MainWindow window;window.resize(1440,1000);window.show();auto* wp=window.project();wp->selectedTrack=0;wp->tracks[0].devices={wp->makeDevice("Osmium",DeviceKind::Instrument)};emit wp->structureChanged();for(auto* tab:window.findChildren<QToolButton*>())if(tab->text()=="Device")tab->click();QApplication::processEvents();
    const int compactHeight=window.deviceView()->height();window.deviceView()->expandDevice(wp->tracks[0].devices[0].id);QApplication::processEvents();
    CHECK(window.deviceView()->height()>compactHeight+100&&!window.deviceView()->expandedEditor()->isWindow(),"The embedded Osmium raises the main detail pane without opening a window");
    auto* synth = window.deviceView()->expandedEditor();
    const double oldDrive = wp->tracks[0].devices[0].params[monolith::FDrive].value;
    synth->edit(monolith::FDrive, .42); synth->setFocus(); window.activateWindow(); QTest::qWait(20);
    QTest::keyClick(synth, Qt::Key_Z, Qt::ControlModifier); QApplication::processEvents();
    CHECK(std::abs(wp->tracks[0].devices[0].params[monolith::FDrive].value - oldDrive) < 1e-6, "The application undo shortcut works from the embedded editor");
    if(!exportDir.isEmpty()){window.deviceView()->expandedEditor()->grab().save(exportDir+"/osmium-sound.png");window.grab().save(exportDir+"/sessions-osmium-expanded.png");}
    window.deviceView()->collapseEditor();QApplication::processEvents();CHECK(window.deviceView()->height()==compactHeight,"Collapse restores the compact rack height");
    if(!exportDir.isEmpty()){
        wp->tracks[0].devices={wp->makeDevice("Drum Studio",DeviceKind::Instrument)};emit wp->structureChanged();QApplication::processEvents();window.deviceView()->expandDevice(wp->tracks[0].devices[0].id);QApplication::processEvents();window.grab().save(exportDir+"/sessions-drums-expanded.png");
    }
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
