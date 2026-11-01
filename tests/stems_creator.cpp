#include "audio/Engine.h"
#include "audio/Resample.h"
#include "core/DeviceSpecs.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "stems/ModelAssets.h"
#include "stems/StemImport.h"
#include "stems/StemsCreator.h"
#include "stems/StemsDsp.h"
#include "stems/WaveIO.h"
#include "ui/devices/DeviceBody.h"
#include "ui/DeviceView.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char* message) { ++checks; if (!value) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); } }
bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) QTest::qWait(10);
    return ready();
}
QString argument(const QStringList& args, const QString& key) { const int i=args.indexOf(key); return i>=0?args.value(i+1):QString{}; }
void writeResult(const QString& root, const std::vector<float>& audio, const QString& behavior = {}) {
    QJsonArray stems;
    const QStringList names{"drums","bass","piano","other","vocals"};
    for(int s=0;s<names.size();++s) {
        if(behavior=="partial" && s==4) break;
        stems::FloatWave writer(QDir(root).filePath(names[s]+".wav"));
        for(size_t i=0;i<audio.size()/2;++i) writer.add(audio[2*i]*.2f,audio[2*i+1]*.2f);
        writer.finish(audio.size()/2);
        stems.push_back(QJsonObject{{"name",names[s]},{"file",names[s]+".wav"}});
    }
    QFile manifest(QDir(root).filePath("result.json")); manifest.open(QIODevice::WriteOnly);
    manifest.write(QJsonDocument(QJsonObject{{"schema",1},{"model","spleeter-5stems-onnx-v1"},
        {"rate",44100},{"frames",double(audio.size()/2)},{"stems",stems}}).toJson());
}
int fakeWorker(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    const auto mode=argument(args,"--behavior");
    if(mode=="hang") { std::puts("{\"schema\":1,\"phase\":\"separating\",\"progress\":0.2,\"detail\":\"test\"}"); std::fflush(stdout); QThread::sleep(20); return 0; }
    if(mode=="fail") { std::puts("{\"schema\":1,\"phase\":\"error\",\"detail\":\"Test backend failure\"}"); return 4; }
    if(mode=="noisy") { QByteArray excessive(2*1024*1024,'x'); std::fwrite(excessive.constData(),1,excessive.size(),stdout); std::fflush(stdout); QThread::sleep(2); return 3; }
    try { writeResult(argument(args,"--output"),stems::readInputWave(argument(args,"--input")),mode); }
    catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 3; }
    return 0;
}
int host(Project& project, const QString& path, SamplePtr sample) {
    project.tracks.clear(); project.returns.clear(); project.scenes={"1","2"};
    project.addTrack("Stems source",QColor("#6688aa")); project.tracks[0].type=TrackType::Audio;
    auto d=project.makeDevice("Stems Creator",DeviceKind::AudioEffect); d.samplePath=path.isEmpty()?QString{}:QFileInfo(path).absoluteFilePath(); d.sample=sample;
    const int id=d.id; project.tracks[0].devices.push_back(std::move(d)); return id;
}
StemsCreator::Backend fixtureBackend(const QString& mode) {
    StemsCreator::Backend backend; backend.program=QCoreApplication::applicationFilePath();
    backend.arguments={"--fake-worker","--behavior",mode}; backend.verifyModels=false; return backend;
}
}
int main(int argc,char** argv) {
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--fake-worker") return fakeWorker(argc,argv);
    qputenv("SESSIONS_AUDIO","none"); qputenv("SESSIONS_NO_UPDATES","1");
    QApplication app(argc,argv); Q_INIT_RESOURCE(resources);
    QTemporaryDir temporary; const auto path=[&](const QString& name){return QDir(temporary.path()).filePath(name);};
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    QSettings("Sessions","Sessions").setValue("stems/outputFolder",path("Generated ä"));
    const QString captures=argument(app.arguments(),"--captures"), neural=argument(app.arguments(),"--neural");
    {
        std::vector<stems::Complex> data(stems::kFft);
        for(int i=0;i<stems::kFft;++i) data[i]=float(.2*std::sin(i*.71)+.1*std::cos(i*1.23));
        const auto original=data; stems::fft(data); stems::fft(data,true); double error=0;
        for(int i=0;i<stems::kFft;++i) error=std::max(error,double(std::abs(data[i]-original[i])));
        check(error<2e-6,"Forward/inverse FFT roundtrip preserves the waveform");
        const auto window=stems::hannWindow();
        check(window.size()==4096 && window[0]==0 && std::abs(window[2048]-1)<1e-7 && window.back()>0,"The model receives a periodic 4096-point Hann window");
        auto masks=stems::ratioMasks({1,2,3,4,5}); float total=0;
        for(float value:masks) total+=value;
        check(std::abs(total-1)<1e-6 && masks[4]>masks[3] && masks[3]>masks[2],"Five power-ratio masks share one joint normalization");
        auto strong=stems::ratioMasks({-10000,10000,0,1,-1});
        check(strong[1]==1 && strong[0]==0,"Mask softmax remains finite for extreme logits");
        bool rejected=false; try{stems::ratioMasks({NAN,0,1,2,3});}catch(...){rejected=true;}
        check(rejected,"Non-finite model logits are rejected");
        stems::FloatWave wave(path("float.wav")); wave.add(2.5f,-2.5f); wave.finish(1);
        const auto back=stems::readInputWave(path("float.wav"));
        check(back.size()==2 && back[0]==2.5f && back[1]==-2.5f,"Worker float WAV I/O retains unclipped headroom and exact sample counts");
    }
    std::vector<float> data(8820);
    for(size_t i=0;i<data.size()/2;++i){data[i*2]=float(.3*std::sin(i*.1));data[i*2+1]=float(.2*std::cos(i*.12));}
    const QString sourceFile=path("Mix with spaces & ä.wav");
    {stems::FloatWave wave(sourceFile);for(size_t i=0;i<data.size()/2;++i)wave.add(data[i*2],data[i*2+1]);wave.finish(data.size()/2);}
    QString why; const auto source=samples::load(sourceFile,&why);
    check(source && source->frames()==4410,"Test source is real stereo audio");
    check(stems::modelAssets().size()==5,"Pinned model catalog contains five source graphs");
    check(!stems::verifyModelFiles(temporary.path(),&why) && !why.isEmpty(),"Missing or invalid models report an actionable error");
    {
        const QString folder=path("fixture");QDir().mkpath(folder);writeResult(folder,data);
        const auto result=stems::readResult(folder,4410,&why);
        check(result.valid() && result.files.size()==5,"Complete aligned worker result validates");
        check(!stems::readResult(folder,4411,&why).valid(),"Worker duration mismatch is rejected");
        QFile manifest(QDir(folder).filePath("result.json"));manifest.open(QIODevice::ReadOnly);auto json=QJsonDocument::fromJson(manifest.readAll()).object();manifest.close();
        auto stems=json["stems"].toArray();auto invalid=stems[0].toObject();invalid["file"]="../escape.wav";stems[0]=invalid;json["stems"]=stems;
        manifest.open(QIODevice::WriteOnly|QIODevice::Truncate);manifest.write(QJsonDocument(json).toJson());manifest.close();
        check(!stems::readResult(folder,4410,&why).valid(),"Output traversal cannot enter the project");
        writeResult(folder,data);QFile::remove(QDir(folder).filePath("piano.wav"));
        check(!stems::readResult(folder,4410,&why).valid(),"Partial stems never validate as a complete separation");
    }
    {
        Project p;const int id=host(p,sourceFile,source);audio::Engine engine(&p);Transport transport(&p,&engine);
        transport.setStartMarker(8);StemsCreator service(&p,&transport,fixtureBackend("success"));QSignalSpy done(&service,&StemsCreator::finished);
        const auto before=p.captureEditState(); service.start(id);
        check(service.busy(),"Separation starts asynchronously without entering the live engine");
        check(waitFor([&]{return !done.isEmpty();}) && done[0][0].toBool(),"Worker results complete the grouped import");
        check(p.tracks.size()==7 && p.tracks[1].isGroup() && p.tracks[1].name.contains("Stems"),"Five audio tracks are created under one named group");
        bool aligned=true;const int group=p.tracks[1].id;
        for(int i=2;i<p.tracks.size();++i){const auto& t=p.tracks[i];aligned&=t.groupId==group && t.type==TrackType::Audio && t.arrangement.size()==1 && t.monitor==Monitor::Off;
            if(!t.arrangement.isEmpty()){const auto& a=t.arrangement[0];aligned&=a.start==8 && a.clip->sample && a.clip->sample->frames()==4410 && !a.clip->warp && !a.clip->loop && QFileInfo::exists(a.clip->samplePath);}}
        check(aligned,"All stems retain exact timing and native speed at the insert marker");
        check(p.tracks[0].devices[0].id==id && p.tracks[0].devices[0].samplePath==sourceFile,"The source device and audio remain intact");
        p.undoStack()->undo();check(p.captureEditState()==before,"The entire group is one undo step");
        p.undoStack()->redo();check(p.tracks.size()==7 && QFileInfo::exists(p.tracks[2].arrangement[0].clip->samplePath),"Redo restores all five tracks with their files");
        check(setfile::save(path("stems.sessions"),p,{},&why),"Grouped stems save in a set");
        Project loaded;setfile::Extras extras;
        check(setfile::load(path("stems.sessions"),loaded,extras,&why) && loaded.tracks.size()==7 && loaded.tracks[2].arrangement[0].clip->sample,"Grouped stem files reload from the saved set");
    }
    for(const auto& mode:QStringList{"fail","partial","noisy"}) {
        Project p;const int id=host(p,sourceFile,source);audio::Engine engine(&p);Transport transport(&p,&engine);
        StemsCreator service(&p,&transport,fixtureBackend(mode));QSignalSpy done(&service,&StemsCreator::finished);const auto before=p.captureEditState();service.start(id);
        check(waitFor([&]{return !done.isEmpty();}) && !done[0][0].toBool() && p.captureEditState()==before,"Failed, partial or excessive worker output cannot partially modify the set");
    }
    for(int mode=0;mode<3;++mode){
        Project p;const int id=host(p,sourceFile,source);audio::Engine engine(&p);Transport transport(&p,&engine);
        if(mode==2)p.insertTrack(p.tracks.size(),TrackType::Audio);
        StemsCreator service(&p,&transport,fixtureBackend("hang"));QSignalSpy done(&service,&StemsCreator::finished);service.start(id);
        check(waitFor([&]{return service.status(id).contains("Separating");}),"A running worker exposes progress without blocking the UI");
        if(mode==0)service.cancel();else if(mode==1)p.loadEmpty();else p.deleteTrack(0);
        check(waitFor([&]{return !done.isEmpty();}) && !done[0][0].toBool() && !service.busy(),"Cancel, New/Open and host deletion terminate the old job");
        bool noStems=true;for(const auto& t:p.tracks)noStems&=!t.isGroup();
        check(noStems,"A cancelled or reset job never imports late tracks");
    }
    {
        auto p=std::make_unique<Project>();const int id=host(*p,sourceFile,source);
        auto* engine=new audio::Engine(p.get(),p.get());auto* transport=new Transport(p.get(),engine,p.get());
        auto* service=new StemsCreator(p.get(),transport,fixtureBackend("hang"));
        QPointer<StemsCreator> guard(service);service->start(id);
        check(waitFor([&]{return service->status(id).contains("Separating");}),"Owner-close fixture reaches native worker execution");
        p.reset();
        check(guard.isNull(),"Closing the owning project reaps the worker without callbacks into destroyed model data");
    }
    {
        auto p=std::make_unique<Project>();const int id=host(*p,sourceFile,source);
        auto* engine=new audio::Engine(p.get(),p.get());auto* transport=new Transport(p.get(),engine,p.get());
        auto* service=new StemsCreator(p.get(),transport,fixtureBackend("success"));
        QPointer<StemsCreator> guard(service);service->start(id);p.reset();
        const bool drained=QThreadPool::globalInstance()->waitForDone(5000);app.processEvents();
        check(drained && guard.isNull(),"Closing during preparation discards the queued callback on the UI thread");
    }
    {
        Project crowded;host(crowded,sourceFile,source);
        for(int i=1;i<59;++i)crowded.addTrack(QString::number(i),QColor("#6688aa"));
        check(!stems::roomForStems(crowded,&why),"A full engine cannot import silent excess tracks");
        crowded.tracks.removeLast();check(stems::roomForStems(crowded,&why),"Exactly enough remaining track slots are accepted");
    }
    {
        Project p;const int id=host(p,{},{});audio::Engine engine(&p);Transport transport(&p,&engine);
        StemsCreator service(&p,&transport,fixtureBackend("success"));service.loadSource(id,sourceFile);
        check(waitFor([&]{return !service.loadingSource(id);}) && service.device(id)->sample && service.device(id)->samplePath==sourceFile,"The Load File action decodes and persists the selected source asynchronously");
        const auto before=p.captureEditState();service.loadSource(id,path("missing.wav"));
        check(p.captureEditState()==before,"A failed Load File leaves the previous source intact");
        service.loadSource(id,sourceFile);waitFor([&]{return !service.loadingSource(id);});
        if(!captures.isEmpty()){
            QDir().mkpath(captures);
            for(const auto kind:{theme::Kind::Dark,theme::Kind::Light}){
                theme::apply(kind);
                DevicePanel panel(&p,&transport,0,*service.device(id),nullptr,{});
                panel.show();app.processEvents();auto* body=panel.body();
                check(body->findChild<QPushButton*>("stemsCreate") && body->findChild<QPushButton*>("stemsLoadFile"),"Stems Creator exposes its functional controls in both themes");
                panel.grab().save(QDir(captures).filePath(kind==theme::Kind::Dark?"stems-dark.png":"stems-light.png"));
            }
        }
    }
    if(!neural.isEmpty()) {
        const auto mix=samples::load(neural,&why);check(bool(mix),"Real neural test input loads");
        if(mix){
            Project p;const int id=host(p,neural,mix);audio::Engine engine(&p);Transport transport(&p,&engine);
            StemsCreator service(&p,&transport,{});QSignalSpy done(&service,&StemsCreator::finished);service.start(id);
            const bool completed=waitFor([&]{return !done.isEmpty();},180000) && done[0][0].toBool();
            if(!completed && !done.isEmpty())std::fprintf(stderr,"Neural error: %s\n",qPrintable(done[0][2].toString()));
            check(completed,"Actual pinned ONNX models create a complete grouped neural separation");
            if(completed){
                double squared=0,energy=0,difference=0;const auto n=p.tracks[2].arrangement[0].clip->sample->frames();
                const double step=mix->rate/44100.;
                for(qint64 i=0;i<n;++i){double sum=0;for(int t=2;t<7;++t)sum+=p.tracks[t].arrangement[0].clip->sample->left[size_t(i)];
                    const double reference=audio::resample::read(mix->left,i*step,step);squared+=(sum-reference)*(sum-reference);energy+=reference*reference;
                    difference+=std::abs(p.tracks[3].arrangement[0].clip->sample->left[size_t(i)]-p.tracks[4].arrangement[0].clip->sample->left[size_t(i)]);}
                const double relative=std::sqrt(squared/std::max(energy,1e-20));
                std::printf("Actual neural reconstruction relative RMS error: %.9g; frames: %lld\n",relative,static_cast<long long>(n));
                check(relative<2e-6,"All actual neural stems reconstruct the resampled input, including the file edges and upper band");
                check(difference/n>1e-6,"Actual neural outputs differ; they are not copies of the input");
                if(!captures.isEmpty()) {
                    const auto set=path("Separated.sessions");check(setfile::save(set,p,{},&why),"Separated project saves for app capture");
                    theme::apply(theme::Kind::Dark);MainWindow window;
                    check(window.openSet(set,&why),"Separated project opens in the full app");
                    check(window.project()->tracks[0].devices[0].sample != nullptr,"The source device reloads its audio in the full app");
                    window.resize(1440,900);window.show();
                    window.project()->selectedTrack=0;emit window.project()->selectionChanged();
                    if(window.sessionViewShown()) QTest::keyClick(&window,Qt::Key_Tab);
                    if(window.clipViewShown()) QTest::keyClick(&window,Qt::Key_Tab,Qt::ShiftModifier);
                    window.arrangementView()->zoomToFit();app.processEvents();
                    window.grab().save(QDir(captures).filePath("stems-arrangement.png"));
                }
            }
        }
    }
    std::printf("Stems Creator: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
