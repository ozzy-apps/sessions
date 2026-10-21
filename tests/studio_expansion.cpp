// Recorded content, NAM tone, mono-compatible stereo width and native waveform drawer.
#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "audio/Engine.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "core/Sample.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QPushButton>
#include <QToolButton>
#include <QTemporaryDir>
#include <QSettings>
#include <QSet>
#include <QTest>
#include <QUndoStack>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace sessions;
namespace ds=drumstudio;namespace sw=stereowidth;
namespace {int checks=0,failures=0;std::atomic<bool> watch{false};std::atomic<int> allocations{0};
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);std::fflush(stdout);}}
void settle(){QApplication::processEvents();QApplication::processEvents();}
std::vector<float> defaults(DeviceType type){std::vector<float> p(audio::kMaxParams);const auto* spec=specOf(type);for(size_t i=0;i<spec->params.size();++i)p[i]=spec->params[i].def;return p;}
struct Audio{std::vector<float> l,r;double power=0;};
Audio effect(DeviceType type,const std::vector<float>& params,double hz=997,int mode=0,int rate=48000,int block=128){
    auto proc=audio::createProcessor(type,rate);audio::Context ctx;ctx.sampleRate=rate;Audio out;out.l.resize(rate);out.r.resize(rate);
    for(int at=0;at<rate;at+=block){const int n=std::min(block,rate-at);float l[audio::kMaxBlock],r[audio::kMaxBlock];for(int i=0;i<n;++i){l[i]=.1f*std::sin(6.28318530718*hz*(at+i)/rate);r[i]=mode==1?-l[i]:mode==2?.07f*std::cos(6.28318530718*hz*(at+i)/rate):l[i];}
        watch=true;proc->process(l,r,n,params.data(),ctx);watch=false;for(int i=0;i<n;++i){out.l[at+i]=l[i];out.r[at+i]=r[i];if(at+i>rate/2)out.power+=l[i]*l[i]+r[i]*r[i];}}
    return out;
}
Audio drum(const std::vector<float>& p,const audio::drums::Content* content,int note=38){auto proc=audio::createProcessor(DeviceType::DrumStudio,48000);audio::Context ctx;ctx.drumContent=content;Audio out;out.l.resize(96000);out.r.resize(96000);
    for(int at=0;at<96000;at+=audio::kMaxBlock){const int n=std::min(audio::kMaxBlock,96000-at);float l[audio::kMaxBlock]{},r[audio::kMaxBlock]{};audio::EventList events;if(at==0)events.add({0,note,.85f,1000});watch=true;proc->render(events,l,r,n,p.data(),ctx);watch=false;for(int i=0;i<n;++i){out.l[at+i]=l[i];out.r[at+i]=r[i];out.power+=l[i]*l[i]+r[i]*r[i];}}return out;}
double difference(const Audio& a,const Audio& b){double d=0;for(size_t i=0;i<a.l.size();++i)d+=std::abs(a.l[i]-b.l[i])+std::abs(a.r[i]-b.r[i]);return d;}
}
void* operator new(std::size_t n){if(watch.load())++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}void* operator new[](std::size_t n){return ::operator new(n);}void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(int argc,char** argv){QApplication app(argc,argv);QTemporaryDir prefs;QCoreApplication::setOrganizationName("SessionsExpansionTest");QCoreApplication::setApplicationName("Expansion");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,prefs.path());theme::apply(theme::Kind::Dark);
    const bool skipContent=qEnvironmentVariableIsSet("SESSIONS_SKIP_DRUM_CONTENT_TEST");
    // Four appended NAM controls stay flat for old saved devices.
    Project project;auto nam=project.makeDevice("NAM",DeviceKind::AudioEffect);auto json=setfile::deviceToJson(nam);auto values=json["params"].toArray();while(values.size()>8)values.removeLast();json["params"]=values;const auto old=setfile::deviceFromJson(json);bool migration=old.params.size()==namdev::kParams;for(int i=namdev::Bass;i<namdev::kParams;++i)migration&=old.params[i].value==.5;check(migration,"Old NAM captures acquire four neutral tone controls without renumbering");
    auto flat=defaults(DeviceType::Nam);const auto reference=effect(DeviceType::Nam,flat,750);for(auto pair:{std::pair<int,double>{namdev::Bass,60},{namdev::Mid,750},{namdev::Treble,8000},{namdev::Presence,14000}}){auto p=flat;p[pair.first]=1;const auto neutral=effect(DeviceType::Nam,flat,pair.second),boost=effect(DeviceType::Nam,p,pair.second);p[pair.first]=0;const auto cut=effect(DeviceType::Nam,p,pair.second);check(boost.power>neutral.power*3&&cut.power<neutral.power*.35,"Each NAM tone band produces useful audible boost and cut");}
    for(int rate:{8000,44100,96000}){auto p=flat;for(int i=namdev::Bass;i<namdev::kParams;++i)p[i]=1;const auto x=effect(DeviceType::Nam,p,997,0,rate);check(std::all_of(x.l.begin(),x.l.end(),[](float v){return std::isfinite(v)&&std::abs(v)<30;}),"NAM tone remains finite at extreme settings and supported rates");}
    auto p=defaults(DeviceType::StereoWidth);auto neutral=effect(DeviceType::StereoWidth,p,997,2);p[sw::Width]=0;auto mono=effect(DeviceType::StereoWidth,p,997,2);bool monoOutput=true;for(size_t i=0;i<mono.l.size();++i)monoOutput&=mono.l[i]==mono.r[i];check(monoOutput,"Width zero produces a true mono output");p[sw::Width]=1;p[sw::Spread]=1;auto widened=effect(DeviceType::StereoWidth,p,997,2);double midError=0,sideEnergy=0;for(size_t i=0;i<neutral.l.size();++i){midError=std::max(midError,std::abs(double(widened.l[i]+widened.r[i]-neutral.l[i]-neutral.r[i])));sideEnergy+=std::pow(widened.l[i]-widened.r[i],2);}check(midError<1e-6&&sideEnergy>1,"Widening preserves the centre and mono fold-down");
    auto monoWide=effect(DeviceType::StereoWidth,p,997);double spread=0;for(size_t i=0;i<monoWide.l.size();++i)spread+=std::abs(monoWide.l[i]-monoWide.r[i]);check(spread>10,"Mono Spread creates audible stereo from a mono source");p[sw::Mix]=0;auto dry=effect(DeviceType::StereoWidth,p,997,2);check(difference(neutral,dry)<.001,"Mix zero bypasses widening and delay without latency");
    p=defaults(DeviceType::StereoWidth);const auto bass=effect(DeviceType::StereoWidth,p,40,1);p[sw::MonoBass]=1;const auto protectedBass=effect(DeviceType::StereoWidth,p,40,1);check(protectedBass.power<bass.power*.01,"Mono Bass strongly removes low-frequency side energy");const auto high=effect(DeviceType::StereoWidth,p,5000,1);p[sw::MonoBass]=0;check(high.power>effect(DeviceType::StereoWidth,p,5000,1).power*.9,"Mono Bass preserves high-frequency stereo information");p[sw::Spread]=.8;p[sw::MonoBass]=1;check(difference(effect(DeviceType::StereoWidth,p,997,2,48000,64),effect(DeviceType::StereoWidth,p,997,2,48000,128))<1e-6,"Stereo widening is continuous across callback sizes");
    // Every downloaded bank is independently decoded and actually rendered, never just listed.
    if(!skipContent)for(int bank=0;bank<6;++bank){if(!audio::drums::contentInstalled(bank)&&!qEnvironmentVariableIsSet("SESSIONS_REQUIRE_DRUM_CONTENT")){std::printf("SKIP optional content: %s\n",audio::drums::contentName(bank));continue;}auto content=audio::drums::loadContentBank(bank);check(bool(content),"Approved drum library is installed and loadable");if(!content)continue;check(content->error.isEmpty(),qPrintable(content->error));check(content->recordings>50&&content->frames>44100,"Library contains decoded recordings, not Git LFS pointer files");
        auto knobs=defaults(DeviceType::DrumStudio);knobs[ds::ContentLibrary]=choicePosition(bank+1,7);auto prepared=audio::drums::prepareContent(knobs.data());const auto* hit=audio::drums::recordedHit(*content,38,105,0);check(hit&&!hit->close.mono.empty(),"Each library supplies a recorded snare at playable velocity");
        const auto audio=drum(knobs,prepared.get());check(audio.power>.001&&std::all_of(audio.l.begin(),audio.l.end(),[](float v){return std::isfinite(v)&&std::abs(v)<2;}),"Each added library produces bounded recorded drum audio");
        if(hit&&!hit->alternate.mono.empty()){knobs[ds::recordedMic(ds::Snare)]=1;check(difference(audio,drum(knobs,prepared.get()))>.01,"Alternate mic selection switches the recorded microphone audio");}
        const auto* next=audio::drums::recordedHit(*content,38,105,1);check(hit&&next&&hit!=next&&hit->close.mono!=next->close.mono,"Repeated snare hits have separately recorded round robins");
        if(bank!=2)for(int note:{36,42,48,49})check(audio::drums::recordedHit(*content,note,100,0)!=nullptr,"Complete kits have their own kick, hats, high tom and crash");
        std::printf("Content %s: %d recordings, %.1f MB decoded\n",qPrintable(content->name),content->recordings,content->frames*4/1e6);std::fflush(stdout);
    }
    check(allocations==0,"New tone, stereo and recorded-content paths allocate nothing in audio callbacks");
    project.scenes={"1"};project.addTrack("Studio",theme::accent);project.tracks[0].devices={project.makeDevice("Drum Rack",DeviceKind::Instrument)};auto& rack=project.tracks[0].devices[0];std::vector<float> wave(44100);for(size_t i=0;i<wave.size();++i)wave[i]=.6f*std::sin(i*.1f)*std::exp(-double(i)/5000.);rack.pads[0].sample=samples::fromFrames(wave,{},44100);rack.pads[0].name="Test kick";
    audio::Engine engine(&project);Transport transport(&project,&engine);DeviceView view(&project,&transport);view.resize(1480,620);view.show();settle();auto* toggle=view.findChild<QToolButton*>("drumWaveShelfToggle");check(toggle,"Drum Rack exposes its waveform shelf control");if(toggle){toggle->click();QTest::qWait(230);settle();auto* shelf=view.findChild<QWidget*>("drumWaveShelf");auto* waveView=view.findChild<QWidget*>("drumWaveShelfDisplay");check(shelf&&shelf->isVisible()&&shelf->width()>450&&waveView&&waveView->width()>=480,"Waveform shelf slides open inside the device chain");auto* body=view.panels().front()->body();auto* fit=view.findChild<QToolButton*>("drumWaveShelfFit");auto* zoom=view.findChild<QToolButton*>("drumWaveShelfZoomIn");check(waveView&&body->rect().contains(QRect(waveView->mapTo(body,QPoint()),waveView->size()))&&fit&&zoom&&body->rect().contains(QRect(zoom->mapTo(body,QPoint()),zoom->size())),"The full waveform and zoom controls stay inside the expanded panel");const int expanded=body->width();toggle->click();QTest::qWait(230);settle();check(!shelf->isVisible()&&view.findChild<devui::DeviceBody*>()->width()<expanded-400,"Closing the shelf restores compact Drum Rack width");toggle->click();QTest::qWait(230);settle();}
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):QString();if(!output.isEmpty()){QDir().mkpath(output);view.grab().save(output+"/drum-wave-shelf-dark.png");theme::apply(theme::Kind::Light);QTest::qWait(230);settle();view.grab().save(output+"/drum-wave-shelf-light.png");theme::apply(theme::Kind::Dark);}
    for(auto type:{DeviceType::Nam,DeviceType::StereoWidth}){project.tracks[0].devices={project.makeDevice(type==DeviceType::Nam?"NAM":"Stereo Width",DeviceKind::AudioEffect)};emit project.structureChanged();settle();auto* body=view.panels().front()->body();QSet<QString> targets;for(auto* w:body->findChildren<QWidget*>())targets.insert(w->property("mapTarget").toString());bool mapped=true;for(int i=0;i<body->count();++i)mapped&=targets.contains(body->target(i));check(mapped,"NAM and Stereo Width expose every control for MIDI mapping");check(body->minimumSizeHint().height()<=devui::kBodyHeight+4,"New effect controls fit the compact device pane");const int undo=project.undoStack()->index();body->edit(type==DeviceType::Nam?int(namdev::Presence):int(sw::Width),.8);check(project.undoStack()->index()==undo+1,"New effect controls use the shared undo stack");project.undoStack()->undo();settle();if(!output.isEmpty())view.grab().save(output+(type==DeviceType::Nam?"/nam-tone.png":"/stereo-width.png"));}
    for(int bank=0;bank<6;++bank){const auto preset=project.makeDevice(audio::drums::contentName(bank),DeviceKind::Instrument);check(deviceType(preset.name)==DeviceType::DrumStudio&&preset.params.size()==ds::kParams&&choiceIndex(DeviceType::DrumStudio,ds::ContentLibrary,preset.params[ds::ContentLibrary].value)==bank+1,"Each browser drum preset selects its native library");}
    if(!skipContent&&audio::drums::contentInstalled(0)){
        project.tracks[0].devices={project.makeDevice("Big Rusty Drums",DeviceKind::Instrument)};emit project.structureChanged();settle();const int id=project.tracks[0].devices[0].id;view.resize(1480,780);view.expandDevice(id);settle();
        check(view.expandedEditor()&&view.expandedEditor()->pick(ds::ContentLibrary)==1,"A downloaded kit opens in the actual embedded Drum Studio");transport.previewNote(0,38,.85);double power=0;for(int block=0;block<32;++block){float data[audio::kMaxBlock*2]{};watch=true;engine.process(data,audio::kMaxBlock);watch=false;for(float sample:data)power+=sample*sample;}
        check(power>.001&&engine.deviceScope(id,ds::Snare)>.01,"A browser drum preset plays through the real engine and studio audition path");
        if(!output.isEmpty())for(auto kind:{theme::Kind::Dark,theme::Kind::Light}){theme::apply(kind);settle();view.grab().save(output+(kind==theme::Kind::Dark?"/recorded-kit-dark.png":"/recorded-kit-light.png"));}
    }
    check(allocations==0,"The engine's recorded kit audition also allocates nothing in callbacks");
    std::printf("studio_expansion: %d checks, %d failures, %d callback allocations\n",checks,failures,allocations.load());return failures?1:0;
}
