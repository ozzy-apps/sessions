#include "audio/Devices.h"
#include "audio/Dsp.h"
#include "audio/Engine.h"
#include "core/DeviceSpecs.h"
#include "core/Grooves.h"
#include "core/Presets.h"
#include "core/SetFile.h"
#include "core/Sample.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
#include <functional>
using namespace sessions;
namespace db = drumbuss;
constexpr auto type = DeviceType::DrumBuss;
static int failures=0, checks=0;
#define CHECK(c,m) do { ++checks; if(!(c)){++failures;std::printf("FAIL: %s\n",qPrintable(QString(m)));} }while(0)
using Audio=std::vector<float>;
static std::vector<float> defaults(){std::vector<float> p;for(const auto& s:specOf(type)->params)p.push_back(s.def);return p;}
static std::vector<float> neutral(){auto p=defaults();p[db::Drive]=p[db::Crunch]=p[db::Boom]=p[db::Comp]=0;p[db::Damp]=p[db::Decay]=1;p[db::Transients]=.5;p[db::Mix]=1;return p;}
static double position(int param,double real){const auto& s=specOf(type)->params[size_t(param)];return s.kind==ParamKind::Exp?std::log(real/s.lo)/std::log(s.hi/s.lo):(real-s.lo)/(s.hi-s.lo);}
static Audio tone(double hz,double level=.4,double seconds=1,int rate=48000){Audio a(size_t(seconds*rate)*2);for(size_t i=0;i<a.size()/2;++i)a[i*2]=a[i*2+1]=float(level*std::sin(2*M_PI*hz*i/rate));return a;}
static Audio hit(double hz,double seconds=1,int rate=48000){auto a=tone(hz,.7,seconds,rate);for(size_t i=0;i<a.size()/2;++i){const float env=std::exp(-double(i)/(rate*.14));a[i*2]*=env;a[i*2+1]*=env;}return a;}
static Audio process(const Audio& input,std::vector<float> p,int rate=48000,std::function<void(int,std::vector<float>&)> change={}){
    auto processor=audio::createProcessor(type,rate);Audio out(input.size());audio::Context ctx;ctx.sampleRate=rate;
    for(int at=0;at<int(input.size()/2);at+=audio::kMaxBlock){int n=std::min(audio::kMaxBlock,int(input.size()/2)-at);float l[audio::kMaxBlock],r[audio::kMaxBlock];for(int i=0;i<n;++i){l[i]=input[size_t(at+i)*2];r[i]=input[size_t(at+i)*2+1];}if(change)change(at,p);processor->process(l,r,n,p.data(),ctx);for(int i=0;i<n;++i){out[size_t(at+i)*2]=l[i];out[size_t(at+i)*2+1]=r[i];}}
    return out;
}
static double rms(const Audio& a,double begin=.2,double end=.8,int rate=48000){int first=int(begin*rate)*2,last=std::min(int(end*rate)*2,int(a.size()));double sum=0;for(int i=first;i<last;++i)sum+=a[size_t(i)]*a[size_t(i)];return last>first?std::sqrt(sum/(last-first)):0;}
static double peak(const Audio& a){double p=0;for(float x:a)p=std::max(p,double(std::abs(x)));return p;}
static double difference(const Audio&a,const Audio&b){double x=0;for(size_t i=0;i<a.size();++i)x=std::max(x,double(std::abs(a[i]-b[i])));return x;}
static double component(const Audio&a,double hz,int rate=48000){double re=0,im=0;int first=rate/4,end=std::min(int(a.size()/2),rate);for(int i=first;i<end;++i){double phase=2*M_PI*hz*i/rate;re+=a[size_t(i)*2]*std::cos(phase);im+=a[size_t(i)*2]*std::sin(phase);}return std::hypot(re,im)*2/(end-first);}
static Audio kitLoop(int groove,int recipe=0){const auto& g=grooves::all()[size_t(groove)];const auto notes=grooves::notes(g);std::vector<float> p;for(const auto& s:specOf(DeviceType::DrumStudio)->params)p.push_back(s.def);p[drumstudio::Kit]=choicePosition(recipe,6);auto proc=audio::createProcessor(DeviceType::DrumStudio,48000);Audio out(size_t((g.bars*240/g.tempo+.4)*48000)*2);audio::Context ctx;ctx.sampleRate=48000;int next=0;
    for(int at=0;at<int(out.size()/2);at+=128){int n=std::min(128,int(out.size()/2)-at);float l[128]{},r[128]{};audio::EventList ev;while(next<notes.size()){const auto& note=notes[next];int t=int(note.start*60/g.tempo*48000);if(t>=at+n)break;ev.add({std::max(0,t-at),note.pitch,float(note.velocity),4800});++next;}proc->render(ev,l,r,n,p.data(),ctx);for(int i=0;i<n;++i){out[size_t(at+i)*2]=l[i];out[size_t(at+i)*2+1]=r[i];}}return out;
}
int main(int argc,char**argv){
    QApplication app(argc,argv);theme::apply(theme::Kind::Dark);const QString dir=argc>1?QString::fromLocal8Bit(argv[1]):QString();
    CHECK(deviceType("Drum Buss")==type&&specOf(type)->kind==DeviceKind::AudioEffect&&paramCount(type)==14,"Drum Buss is a registered native audio effect with 14 parameters");
    CHECK(paramCount(DeviceType::Osmium)==182&&paramCount(DeviceType::DrumStudio)==66,"Existing device parameter layouts stay unchanged");
    auto zero=neutral();const auto sine=tone(997);const auto reference=process(sine,zero);auto dry=defaults();dry[db::Mix]=0;dry[db::Boom]=dry[db::Crunch]=dry[db::Comp]=1;
    CHECK(difference(reference,process(sine,dry))<1e-6,"Dry/Wet zero returns the aligned original input independently of processing controls");
    auto wet=defaults();wet[db::Drive]=.75;wet[db::Crunch]=.4;const auto fully=process(sine,wet);wet[db::Mix]=.5;auto half=process(sine,wet);double error=0;for(size_t i=0;i<half.size();++i)error=std::max(error,std::abs(double(half[i]-(fully[i]+reference[i])*.5f)));CHECK(error<2e-6,"The parallel blend is aligned and equals the dry/processed average");
    auto output=zero;output[db::Output]=position(db::Output,-6);auto lower=process(sine,output);CHECK(std::abs(rms(lower)/rms(reference)-dsp::dbToGain(-6))<.0001,"Output applies accurate dB gain");
    auto trim=zero;trim[db::Trim]=position(db::Trim,6);CHECK(std::abs(rms(process(sine,trim))/rms(reference)-dsp::dbToGain(6))<.0001,"Trim applies gain before the processing chain");trim[db::Mix]=0;CHECK(difference(process(sine,trim),reference)<1e-6,"Input Trim does not alter the dry branch");
    std::vector<Audio> modes;
    for(int mode=0;mode<3;++mode){auto p=zero;p[db::Drive]=.85;p[db::Type]=choicePosition(mode,3);modes.push_back(process(sine,p));CHECK(std::all_of(modes.back().begin(),modes.back().end(),[](float f){return std::isfinite(f);})&&peak(modes.back())<1.1,"Each drive character produces finite bounded audio");}
    CHECK(difference(modes[0],modes[1])>.01&&difference(modes[1],modes[2])>.01,"Soft, Medium and Hard have distinct nonlinear responses");
    CHECK(component(modes[0],2991)>.003,"Soft drive generates harmonics");
    auto highSine=tone(9000,.7);auto hard=zero;hard[db::Drive]=1;hard[db::Type]=1;auto alias=process(highSine,hard);auto naive=highSine;for(auto& x:naive)x=std::clamp(x*dsp::dbToGain(24),-.85f,.85f)*std::pow(dsp::dbToGain(24),-.25f);
    CHECK(component(alias,21000)<component(naive,21000)*.4,"Four-times oversampling reduces high-frequency clipping aliases");
    auto crunch=zero;crunch[db::Crunch]=.8;auto crunchOut=process(sine,crunch);CHECK(component(crunchOut,2991)>.005&&difference(crunchOut,reference)>.02,"Crunch adds nonlinear mid-high character");
    auto damp=zero;damp[db::Damp]=0;CHECK(rms(process(tone(10000),damp))<rms(process(tone(10000),zero))*.05,"Damp removes harsh high frequencies");
    auto compressed=zero;compressed[db::Comp]=1;double uncompressedRatio=rms(process(tone(997,.8),zero))/rms(process(tone(997,.03),zero));double compressedRatio=rms(process(tone(997,.8),compressed))/rms(process(tone(997,.03),compressed));CHECK(compressedRatio<uncompressedRatio*.6,"The fixed compressor reduces loud/soft dynamic range");
    const auto burst=hit(2000);auto plus=zero,minus=zero;plus[db::Transients]=1;minus[db::Transients]=0;const auto baseHit=process(burst,zero),positive=process(burst,plus),negative=process(burst,minus);
    CHECK(rms(positive,.07,.19)>rms(baseHit,.07,.19)*1.1,"Positive Transients adds high-band sustain");CHECK(rms(negative,.07,.19)<rms(baseHit,.07,.19)*.65,"Negative Transients reduces high-band sustain");CHECK(peak(negative)/rms(negative,.07,.19)>peak(baseHit)/rms(baseHit,.07,.19)*1.4,"Negative Transients retains attack relative to the tightened tail");
    const auto sub=tone(30);CHECK(difference(process(sub,minus),process(sub,zero))<.04,"Transient shaping largely preserves frequencies below 100 Hz");
    auto decay=zero;decay[db::Decay]=0;CHECK(rms(process(hit(50),decay),.35,.6)<rms(process(hit(50),zero),.35,.6)*.8,"Decay shortens incoming bass even with Boom off");
    CHECK(std::abs(db::tunedFrequency(55,true)-55)<1e-8&&std::abs(db::tunedFrequency(57,true)-58.270470189)<1e-6,"Force To Note uses equal-tempered tuning");
    Audio impulse(48000*3*2);impulse[200]=impulse[201]=1;auto boom=zero;boom[db::Boom]=boom[db::BassSolo]=1;boom[db::Frequency]=position(db::Frequency,57);boom[db::ForceNote]=1;boom[db::Decay]=.9;
    const auto ring=process(impulse,boom);CHECK(peak(ring)>.001&&rms(ring,.15,.35)>.00005,"Boom generates a decaying low-frequency response from a short hit");
    int crossings=0;int firstCross=-1,lastCross=0;for(int i=10000;i<30000;++i)if(ring[size_t(i-1)*2]<=0&&ring[size_t(i)*2]>0){++crossings;if(firstCross<0)firstCross=i;lastCross=i;}const double measured=(crossings-1)*48000./std::max(1,lastCross-firstCross);CHECK(crossings>10&&std::abs(measured-db::tunedFrequency(57,true))<.3,"The rendered Boom tail rings at the forced note frequency");
    auto shortBoom=boom;shortBoom[db::Decay]=0;CHECK(rms(ring,.2,.6)>rms(process(impulse,shortBoom),.2,.6)*10,"Boom Decay controls resonator tail length");
    CHECK(process(Audio(96000,0),boom)==Audio(96000,0),"Boom does not self-oscillate or generate sound from silence");
    bool centered=true;for(size_t i=0;i<ring.size()/2;++i)centered&=ring[i*2]==ring[i*2+1];CHECK(centered,"Generated bass is mono and centered");
    auto oneSide=sine;for(size_t i=0;i<oneSide.size()/2;++i)oneSide[i*2+1]=0;auto stereo=process(oneSide,zero);bool silent=true;for(size_t i=0;i<stereo.size()/2;++i)silent&=stereo[i*2+1]==0;CHECK(silent,"With Boom off, processing does not leak between stereo channels");
    for(int rate:{32000,44100,48000,96000})for(int mode=0;mode<3;++mode){auto p=defaults();p[db::Drive]=p[db::Crunch]=p[db::Boom]=p[db::Comp]=1;p[db::Type]=choicePosition(mode,3);auto out=process(hit(60,.6,rate),p,rate);CHECK(peak(out)<4&&std::all_of(out.begin(),out.end(),[](float f){return std::isfinite(f);}),"Extreme controls remain bounded at multiple audio rates");}
    dsp::Noise rng;for(int k=0;k<32;++k){auto p=defaults();for(auto& x:p)x=rng.uniform();auto out=process(tone(77,.8,.08),p);CHECK(peak(out)<40&&std::all_of(out.begin(),out.end(),[](float f){return std::isfinite(f);}),"Random parameter combinations remain finite");}
    auto automated=process(tone(220,.2),defaults(),48000,[](int at,std::vector<float>& p){if(at>=12000){p[db::Type]=1;p[db::Drive]=1;p[db::Crunch]=.8;p[db::ForceNote]=1;p[db::Frequency]=.9;p[db::Boom]=.6;}});double step=0;for(int i=11900;i<15000;++i)step=std::max(step,std::abs(double(automated[size_t(i)*2]-automated[size_t(i-1)*2])));CHECK(step<.25,"Automated mode and gain changes glide instead of producing a switching click");
    auto processor=audio::createProcessor(type,48000);auto pp=defaults();pp[db::Boom]=1;float l[128],r[128];audio::Context ctx;for(int b=0;b<50;++b){std::fill_n(l,128,.6f);std::fill_n(r,128,.6f);processor->process(l,r,128,pp.data(),ctx);}processor->panic();std::fill_n(l,128,0);std::fill_n(r,128,0);processor->process(l,r,128,pp.data(),ctx);CHECK(std::all_of(l,l+128,[](float x){return x==0;})&&std::all_of(r,r+128,[](float x){return x==0;}),"Panic clears the oversamplers, filters, dynamics and resonator tails");
    Project project;project.scenes={"1"};project.addTrack("Drums",theme::accent);project.tracks[0].devices={project.makeDevice("Drum Studio",DeviceKind::Instrument),project.makeDevice("Drum Buss",DeviceKind::AudioEffect)};const int id=project.tracks[0].devices[1].id;
    auto saved=project.tracks[0].devices[1];saved.params[db::ForceNote].value=1;saved.params[db::Boom].value=.65;const auto restored=setfile::deviceFromJson(setfile::deviceToJson(saved));CHECK(restored.name=="Drum Buss"&&restored.params.size()==14&&restored.params[db::ForceNote].value==1&&restored.params[db::Boom].value==.65,"Drum Buss parameters survive set serialization");
    QTemporaryDir temp;QString errorText;Project::DeviceBlocks block{{saved}};CHECK(presets::save(temp.filePath("Drum Buss.sessionspreset"),"Punch",block,&errorText),errorText);Project::DeviceBlocks read;CHECK(presets::read(temp.filePath("Drum Buss.sessionspreset"),read,nullptr,&errorText)&&read.size()==1&&read[0][0].params[db::Boom].value==.65,"Drum Buss saves and loads through the preset library");
    audio::Engine engine(&project);Transport transport(&project,&engine);DeviceView view(&project,&transport);view.resize(1250,240);view.show();QApplication::processEvents();auto* body=view.findChild<devui::DeviceBody*>("drumBussBody");CHECK(body&&body->height()==192&&body->minimumSizeHint().height()<=196,"The compact panel fits the standard device height");
    QSet<QString> targets;for(auto* w:body->findChildren<QWidget*>())targets.insert(w->property("mapTarget").toString());for(int i=0;i<14;++i)CHECK(targets.contains(body->target(i)),"Every Drum Buss parameter has a MIDI-mappable control");
    auto* hardButton=body->findChild<QToolButton*>("drumBussType2");hardButton->click();CHECK(choiceIndex(type,db::Type,project.tracks[0].devices[1].params[db::Type].value)==2,"The Hard button selects its drive character");project.undoStack()->undo();QApplication::processEvents();for(auto* panel:view.panels())if(panel->deviceId()==id)body=panel->body();CHECK(body->pick(db::Type)==0,"Undo restores the drive character and panel state");
    auto* field=body->findChild<DragValue*>(QString("drumBussValue%1").arg(db::Trim));QTest::mouseDClick(field,Qt::LeftButton);QApplication::processEvents();auto* input=field->findChild<QLineEdit*>();CHECK(input,"Trim opens a numeric editor");if(input){input->selectAll();QTest::keyClicks(input,"6");QTest::keyClick(input,Qt::Key_Return);}CHECK(std::abs(body->real(db::Trim)-6)<1e-6,"Typing 6 into Trim means +6 dB");
    body->edit(db::Frequency,position(db::Frequency,57));body->edit(db::ForceNote,1);CHECK(body->findChild<QLabel*>("drumBussNote")->text().contains("A#"),"The effective Boom note is visible in the panel");
    if(!dir.isEmpty()){QDir().mkpath(dir);body->edit(db::Trim,.5);body->edit(db::Boom,.45);QApplication::processEvents();for(auto* panel:view.panels())if(panel->deviceId()==id)panel->grab().save(dir+"/drum-buss-dark.png");view.grab().save(dir+"/drum-buss-chain.png");theme::apply(theme::Kind::Light);QApplication::processEvents();for(auto* panel:view.panels())if(panel->deviceId()==id)panel->grab().save(dir+"/drum-buss-light.png");theme::apply(theme::Kind::Dark);
        Audio reel;const QStringList styles{"Rock","Drum & Bass","Techno","Metal","Reggae"};QString guide="# Drum Buss audition\n\nEach groove plays dry, then through Drum Buss. Shared gain; no normalization.\n";
        for(const auto& style:styles){const int g=grooves::inStyle(style).first();const auto audio=kitLoop(g,style=="Metal"?3:0);auto p=defaults();p[db::Comp]=1;p[db::Drive]=.35;p[db::Crunch]=.2;p[db::Boom]=.25;p[db::Decay]=.6;p[db::Transients]=.6;p[db::Output]=position(db::Output,-3);auto processed=process(audio,p);CHECK(peak(processed)<1,"The drum audition remains below full scale");guide+=QString("- %1: dry %2 s, Drum Buss %3 s\n").arg(style).arg(reel.size()/96000.,0,'f',2).arg((reel.size()+audio.size()+12000)/96000.,0,'f',2);reel.insert(reel.end(),audio.begin(),audio.end());reel.insert(reel.end(),12000,0);reel.insert(reel.end(),processed.begin(),processed.end());reel.insert(reel.end(),24000,0);}
        CHECK(samples::writeWav(dir+"/Drum Buss - Before and After.wav",reel.data(),reel.size()/2,48000,samples::WavFormat::Int16,&errorText),errorText);QFile g(dir+"/guide.md");g.open(QIODevice::WriteOnly);g.write(guide.toUtf8());
    }
    std::printf("Boom frequency %.3f Hz, parallel error %.8f, mode-change step %.4f\n",measured,error,step);std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
