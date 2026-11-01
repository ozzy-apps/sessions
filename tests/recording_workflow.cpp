#include "audio/TrackRender.h"
#include "core/MidiCapture.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/MainWindow.h"
#include "ui/ArrangementView.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QtEndian>
#include <cmath>
#include <cstdio>
using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
bool near(double a, double b, double e = 1e-5) { return std::abs(a-b) < e; }
void base(Project& p, int count = 1) {
    p.tracks.clear(); p.returns.clear(); p.scenes = {"Scene"};
    for (int i=0;i<count;++i) { p.addTrack(QString("Track %1").arg(i+1), QColor("#4488cc")); p.tracks[i].type=TrackType::Audio; p.tracks[i].monitor=Monitor::Off; }
    p.tempo = 120;
}
ClipPtr constant(Project& p, int track, double level, double start = 0, double length = 2) {
    auto clip=p.newClip("Audio",length); clip->samplePath="generated.wav";
    clip->sample=samples::fromFrames(std::vector<float>(96000,float(level)),{},48000);
    clip->loop=false; clip->warp=false; clip->warpMarkers={{0,0},{4,2}};
    p.tracks[track].arrangement.push_back({start,length,0,clip}); return clip;
}
double rms(const SamplePtr& s, double start=0.1, double end=0.4) {
    if (!s) return -1; double sum=0; int count=0;
    for (int i=int(start*s->rate); i<std::min<int>(s->frames(),int(end*s->rate));++i) {sum+=s->left[i]*s->left[i];++count;}
    return count ? std::sqrt(sum/count) : 0;
}
void parameter(Device& d, const QString& name, double real) {
    const auto type=deviceType(d.name); const auto* spec=specOf(type);
    for(int i=0;i<int(spec->params.size());++i) if(QString(spec->params[i].name)==name) {
        const auto& p=spec->params[i]; d.params[i].value=p.kind==ParamKind::Choice?choicePosition(int(real),int(p.choices.size())):p.kind==ParamKind::Exp?std::log(real/p.lo)/std::log(p.hi/p.lo):(real-p.lo)/(p.hi-p.lo); return;
    }
}
}
int main(int argc,char** argv) {
    qputenv("SESSIONS_AUDIO","none"); qputenv("SESSIONS_NO_UPDATES","1");
    QApplication app(argc,argv); QTemporaryDir temp;
    QCoreApplication::setOrganizationName("Sessions-Recording-Tests"); QCoreApplication::setApplicationName("Parity");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
    const auto path=[&](const QString& name){return QDir(temp.path()).filePath(name);};
    {
        MidiCapture h;
        h.noteOn(7,60,.8,0,0,9,true); h.noteOff(7,60,.5,1);
        h.noteOn(7,64,.6,.5,1,10,true); h.noteOff(7,64,1,3); // tempo changed between notes
        h.noteOn(8,40,1,.5,1,10,true);
        auto p=h.phrase(7,1.2,3.4);
        check(p.notes.size()==2 && near(p.timelineStart,8) && near(p.notes[0].start,1),"Capture preserves running bar alignment and isolates track ids");
        check(near(p.notes[1].start,2) && near(p.notes[1].length,2),"Monotonic beat stamps preserve tempo changes");
        h.consume(7); check(h.phrase(7,1.2,3.4).notes.isEmpty() && h.phrase(8,1.2,3.4).notes.size()==1,"Capture consumes only its own track");
        h.clear(); h.noteOn(7,60,.9,0,0,0,false); h.noteOff(7,60,.5,1);
        h.noteOn(7,67,.5,4,8,0,false); h.noteOff(7,67,4.5,9);
        check(h.phrase(7,5,10).notes.size()==1 && h.phrase(7,5,10).notes[0].pitch==67,"Silence separates the latest phrase");
        h.clear(); h.noteOn(7,60,.8,0,0,0,false); h.noteOn(7,64,.5,3,6,0,false);
        check(h.phrase(7,4,8).notes.size()==2,"Held notes bridge a phrase gap and capture through now");
        h.newSegment(); h.noteOn(7,72,.4,5,10,40,true); h.noteOff(7,72,5.2,10.4);
        check(h.phrase(7,6,12).notes.size()==1,"A transport seek/segment separates unrelated phrases");
        h.clear(); h.noteOn(7,60,.8,0,0,15.5,true); h.noteOff(7,60,.5,1);
        h.noteOn(7,62,.8,.5,1,0.5,true); h.noteOff(7,62,1,2);
        p=h.phrase(7,1,2); check(p.notes.size()==2 && near(p.notes[1].start,4.5),"Loop wrap preserves performance order without negative notes");
        h.clear(); for(int i=0;i<MidiCapture::kMaxNotes+500;++i) h.noteOn(7,i%128,.5,i*.001,i*.002,0,false);
        check(h.size()==MidiCapture::kMaxNotes,"Retrospective note history is bounded");
        check(h.phrase(7,1000,2000).notes.isEmpty(),"Expired performances are not captured");
        h.clear(); h.noteOn(7,60,.8,0,0,0,false);
        p=h.phrase(7,100,300); check(p.length<=128 && p.notes[0].length<=128,"Long held notes respect the maximum clip length");
    }
    {
        Project p; base(p); p.tracks[0].type=TrackType::Midi;
        audio::Engine engine(&p); Transport transport(&p,&engine); QString error;
        const int id=p.tracks[0].id;
        auto occupied=p.newClip("Keep",4); p.tracks[0].clipSlots[0]=occupied;
        transport.noteOn(0,60,.75); QTest::qWait(25); transport.noteOff(0,60);
        const auto before=p.captureEditState();
        auto clip=transport.captureMidi(0,false,0,&error);
        check(clip && clip->notes.size()==1 && near(clip->notes[0].velocity,.75),"Transport captures stopped playing without Record");
        check(p.scenes.size()==2 && p.tracks[0].clipSlots[0]==occupied && p.tracks[0].clipSlots[1]==clip,"Session capture creates a free slot without overwriting");
        p.undoStack()->undo(); check(p.captureEditState()==before,"Capture scene and clip are one undo step");
        p.undoStack()->redo(); check(p.tracks[0].clipSlots[1] && p.tracks[0].clipSlots[1]->notes.size()==1,"Capture redo restores notes");
        check(!transport.captureMidi(0,true,-1,&error),"Repeated capture does not duplicate consumed notes");
        transport.setStartMarker(12); transport.noteOn(0,67,.5); QTest::qWait(20); transport.noteOff(0,67);
        clip=transport.captureMidi(0,true,-1,&error);
        check(clip && near(p.tracks[0].arrangement.back().start,12),"Stopped Arrangement capture uses the insert marker");
        transport.noteOn(0,72,.5); QTest::qWait(20); transport.noteOff(0,72);
        p.insertTrack(1,TrackType::Midi); p.moveTrack(0,1);
        clip=transport.captureMidi(p.indexOfTrack(id),true,-1,&error);
        check(clip && clip->notes[0].pitch==72,"Capture history follows a track moved to another index");
        transport.reset(); check(!transport.captureMidi(0,true,-1,&error),"New/open reset drops MIDI history from the previous set");
        transport.previewNote(0,60); check(!transport.captureMidi(0,true,-1,&error),"Piano-roll audition is excluded from Capture MIDI");
    }
    audio::RenderOptions o; o.lengthBeats=1; o.tailSeconds=0; o.format=samples::WavFormat::Float32;
    {
        Project p; base(p,2); constant(p,0,.2); constant(p,1,.7);
        p.tracks[0].pan=.5; p.mainVolume=.1;
        const auto before=p.captureEditState(); QString error;
        o.sourceTrackId=p.tracks[0].id;
        check(audio::renderSnapshot(p,path("track.wav"),o,&error),"Selected track render succeeds");
        auto s=samples::load(path("track.wav"));
        check(near(rms(s),.1,.0001) && s && near(s->right[10000],.2,.0001),"Track export includes pan/fader and excludes other tracks/Master fader");
        check(p.captureEditState()==before,"Export does not mutate the live project");
        p.tracks[0].solo=false; p.tracks[1].solo=true;
        check(audio::renderSnapshot(p,path("solo.wav"),o,&error) && near(rms(samples::load(path("solo.wav"))),.1,.0001),"Individual exports ignore other tracks' solo state");
        QFile keep(path("keep.wav")); keep.open(QIODevice::WriteOnly); keep.write("keep"); keep.close();
        check(!audio::renderSnapshot(p,path("keep.wav"),o,&error,[](double){return false;}),"Single render cancellation is reported");
        keep.open(QIODevice::ReadOnly); check(keep.readAll()=="keep","Cancelled render preserves an existing destination"); keep.close();
        o.sourceTrackId=999999; check(!audio::renderSnapshot(p,path("missing.wav"),o,&error) && !QFile::exists(path("missing.wav")),"Missing source never publishes an empty file");
        o.sourceTrackId=p.tracks[0].id;
        check(audio::renderSnapshot(p,path("snapshot.wav"),o,&error,[&](double){p.tracks[0].active=false;return true;}) && near(rms(samples::load(path("snapshot.wav"))),.1,.0001),"Edits during export cannot change its value snapshot");
    }
    {
        Project p; base(p); constant(p,0,.2,0,1); constant(p,0,.9,1,1);
        o.sourceTrackId=p.tracks[0].id; o.lengthBeats=1; o.tailSeconds=.5;
        QString error; check(audio::renderSnapshot(p,path("tail.wav"),o,&error),"Range with tail renders");
        auto s=samples::load(path("tail.wav"));
        check(s && s->frames()==48000 && rms(s,.6,.9)<1e-8,"Tail has exact length and excludes clips following the range");
        o.tailSeconds=0; o.startBeat=.5; o.preRoll=true;
        check(audio::renderSnapshot(p,path("preroll.wav"),o,&error),"Nonzero range renders with pre-roll");
        check(near(rms(samples::load(path("preroll.wav")),0,.1),.2,.00001),"Pre-roll warms mixer gains before the first exported frame");
        o.startBeat=0; o.lengthBeats=0; check(!audio::renderSnapshot(p,path("zero.wav"),o,&error),"Zero-length renders are rejected"); o.lengthBeats=1;
    }
    {
        Project p; base(p); auto c=constant(p,0,0,0,.25);
        std::vector<float> impulse(48000,0); impulse[1200]=.4;
        c->sample=samples::fromFrames(std::move(impulse),{},48000);
        auto delay=p.makeDevice("Delay",DeviceKind::AudioEffect);
        parameter(delay,"Time",3); parameter(delay,"Dry/Wet",1); parameter(delay,"Feedback",.5); parameter(delay,"Ping Pong",0);
        p.tracks[0].devices.push_back(delay);
        o.sourceTrackId=p.tracks[0].id; o.startBeat=0; o.lengthBeats=.25; o.tailSeconds=.6; o.preRoll=true; QString error;
        check(audio::renderSnapshot(p,path("ring-tail.wav"),o,&error),"Delay tail renders after new input is stopped");
        auto s=samples::load(path("ring-tail.wav")); float peak=0;
        if(s) for(int i=20000;i<std::min<int>(s->frames(),32000);++i) peak=std::max(peak,std::abs(s->left[i]));
        check(peak>.3,"Stopping at the range end preserves the processor's actual effect tail");
        o.startBeat=.5; o.lengthBeats=1; o.tailSeconds=0;
        check(audio::renderSnapshot(p,path("history-tail.wav"),o,&error),"Selected range pre-roll includes earlier effect history");
        s=samples::load(path("history-tail.wav")); peak=0;
        if(s) for(float v:s->left) peak=std::max(peak,std::abs(v));
        check(peak>.3,"A delayed sound originating before the selection survives into its render");
        p.tracks[0].type=TrackType::Midi; p.tracks[0].devices={p.makeDevice("Analog Synth",DeviceKind::Instrument)};
        auto midi=p.newClip("Keys",4); midi->notes={{60,0,.25,.8,1}};
        p.tracks[0].arrangement={{0,1,0,midi}};
        o.startBeat=0; o.lengthBeats=1; o.tailSeconds=.2;
        check(audio::renderSnapshot(p,path("midi-print.wav"),o,&error) && rms(samples::load(path("midi-print.wav")),.02,.1)>.001,"MIDI track printing runs its instrument into the offline file");
        o.tailSeconds=0;
    }
    {
        // Every sidechain source must still run when printing only the target track.
        Project p; base(p,2); constant(p,0,.2); constant(p,1,.7);
        auto gate=p.makeDevice("Gate",DeviceKind::AudioEffect); gate.sidechainTrack=p.tracks[1].id;
        parameter(gate,"Threshold",-12); parameter(gate,"Attack",.1);
        p.tracks[0].devices.push_back(gate); o.sourceTrackId=p.tracks[0].id; QString error;
        check(audio::renderSnapshot(p,path("keyed.wav"),o,&error),"Sidechained track renders");
        check(rms(samples::load(path("keyed.wav")))>.15,"External sidechain remains audible to the detector during an isolated export");
        p.tracks[1].active=false;
        for(auto& d:p.tracks[0].devices) parameter(d,"Key Tap",2);
        check(audio::renderSnapshot(p,path("key-muted.wav"),o,&error) && rms(samples::load(path("key-muted.wav")))<.001,"Post-mixer sidechain respects a muted key source");
    }
    {
        Project p; base(p,3); p.tracks[0].type=TrackType::Group; p.tracks[0].name="Bus";
        p.tracks[1].name="Same:/Name"; p.tracks[2].name="Same:/Name";
        p.tracks[1].groupId=p.tracks[0].id; constant(p,1,.2); constant(p,2,.3);
        const int ret=p.insertReturn(); p.trackAt(ret)->devices.clear();
        p.tracks[1].sends[0]=.5;
        o.startBeat=0; o.lengthBeats=1; o.tailSeconds=.1; o.preRoll=false; QString error;
        o.sourceTrackId=p.tracks[0].id;
        check(audio::renderSnapshot(p,path("group.wav"),o,&error) && near(rms(samples::load(path("group.wav"))),.2,.0002),"Group export contains its members without the unrelated track");
        o.sourceTrackId=p.trackAt(ret)->id;
        check(audio::renderSnapshot(p,path("return.wav"),o,&error) && near(rms(samples::load(path("return.wav"))),.1,.0002),"Return export contains sends from the complete arrangement");
        check(audio::exportStems(p,path("stems"),o,true,&error),"Whole aligned stem batch publishes successfully");
        QFile manifest(QDir(path("stems")).filePath("stems.json")); manifest.open(QIODevice::ReadOnly);
        const auto json=QJsonDocument::fromJson(manifest.readAll()).object(); const auto files=json["files"].toArray();
        check(files.size()==4 && json["sampleRate"].toInt()==48000 && near(json["startBeat"].toDouble(),0),"Manifest records all tracks/groups/returns and shared timing");
        QSet<QString> names; bool aligned=true;
        for(const auto& f:files) { const QString name=f.toObject()["file"].toString(); names.insert(name); auto s=samples::load(QDir(path("stems")).filePath(name)); aligned &= s && s->frames()==28800 && !name.contains('/') && !name.contains(':'); }
        check(aligned && names.size()==4,"Every stem has identical sample length and a unique safe filename");
        check(!audio::exportStems(p,path("stems"),o,true,&error),"Stem batches never overwrite an existing folder");
        check(!audio::exportStems(p,path("cancelled-stems"),o,true,&error,[](double f){return f<.4;}) && !QFile::exists(path("cancelled-stems")),"Cancellation discards the whole unpublished stem batch");
        check(audio::exportStems(p,path("leaf-stems"),o,false,&error) && QDir(path("leaf-stems")).entryList({"*.wav"},QDir::Files).size()==3,"Default stems omit duplicate group outputs but retain returns");
        const auto before=p.captureEditState(); const int source=p.tracks[0].id;
        const int inserted=audio::bounceTrack(p,source,path("bounce.wav"),o,true,&error);
        check(inserted==2 && p.tracks[inserted].groupId==0 && p.tracks[inserted].type==TrackType::Audio,"A group bounce is inserted after its children as a sibling");
        check(inserted>=0 && !p.tracks[0].active && p.tracks[inserted].active && p.tracks[0].arrangement.isEmpty(),"Bounce mutes source only after success and keeps original group/devices");
        check(inserted>=0 && p.tracks[inserted].devices.isEmpty() && p.tracks[inserted].sends.value(0)==0 && !p.tracks[inserted].arrangement[0].clip->warp,"Bounce does not apply the printed processing twice");
        p.undoStack()->undo(); check(p.captureEditState()==before,"Source mute and bounced track are one undo operation");
        p.undoStack()->redo(); check(p.tracks.size()==4,"Bounce redo restores the printed track");
        check(setfile::save(path("bounce.sessions"),p,{},&error),"Bounced track saves in a set");
        Project loaded; setfile::Extras extras;
        check(setfile::load(path("bounce.sessions"),loaded,extras,&error) && loaded.tracks[2].arrangement[0].clip->sample,"Bounced audio reloads with the set");
        const auto stable=p.captureEditState();
        check(audio::bounceTrack(p,source,path("cancel-bounce.wav"),o,true,&error,[](double){return false;})<0 && p.captureEditState()==stable && !QFile::exists(path("cancel-bounce.wav")),"Cancelled bounce changes no tracks and leaves no file");
    }
    {
        const float audio[]={2.5f,-2.5f}; QString error;
        check(samples::writeWav(path("headroom.wav"),audio,1,48000,samples::WavFormat::Float32,&error),"Float WAV writes headroom");
        QFile file(path("headroom.wav")); file.open(QIODevice::ReadOnly); const auto bytes=file.readAll();
        check(near(qFromLittleEndian<float>(bytes.constData()+44),2.5) && near(qFromLittleEndian<float>(bytes.constData()+48),-2.5),"Float export preserves peaks beyond 0 dBFS");
    }
    {
        Project p; base(p); constant(p,0,.2); o.startBeat=0; o.lengthBeats=1; o.tailSeconds=0;
        o.sourceTrackId=p.tracks[0].id; o.normalize=true; QString error;
        check(audio::renderSnapshot(p,path("normalized.wav"),o,&error),"Streaming normalization succeeds");
        auto s=samples::load(path("normalized.wav")); float peak=0;
        if(s) for(float v:s->left) peak=std::max(peak,std::abs(v));
        check(near(peak,.98855,.00001),"Normalization reaches -0.1 dBFS without buffering the whole render");
        check(!audio::renderSnapshot(p,path("normal-cancel.wav"),o,&error,[](double f){return f<.95;}) && !QFile::exists(path("normal-cancel.wav")),"Cancellation during the normalization write leaves no published file");
        o.normalize=false;
        const auto before=p.captureEditState();
        check(audio::bounceTrack(p,p.tracks[0].id,path("changed-bounce.wav"),o,true,&error,[&](double){p.tracks[0].pan=.25;return true;})<0
              && !QFile::exists(path("changed-bounce.wav")) && p.tracks.size()==1,"Bounce detects a set edited during rendering before committing");
        p.restoreEditState(before);
        check(audio::bounceTrack(p,p.tracks[0].id,path("audition-bounce.wav"),o,false,&error)>=0
              && p.tracks[0].active && !p.tracks[1].active,"Default bounce preserves source playback and starts the copy muted");
    }
    {
        MainWindow window; window.project()->loadEmpty(); window.project()->selectedTrack=0;
        window.arrangementView()->selectTime(4,8,0,0);
        for(auto kind:{theme::Kind::Dark,theme::Kind::Light}) {
            theme::apply(kind);
            std::unique_ptr<QDialog> bounce(window.bounceDialog()), output(window.exportDialog());
            check(bounce && output,"Recording dialogs exist in both themes");
            check(bounce->findChild<QComboBox*>("bounceRange")->currentText()=="Time selection","Bounce defaults to the selected Arrangement range");
            auto* outputs=output->findChild<QComboBox*>("exportSource"); outputs->setCurrentIndex(1);
            check(output->findChild<QComboBox*>("exportRange")->currentText()=="Time selection" && outputs->currentData().toInt()==-1000,"Export exposes aligned stems and selected time");
            if(argc>1) {
                QDir().mkpath(argv[1]); const QString label=kind==theme::Kind::Dark?"dark":"light";
                bounce->show(); app.processEvents(); bounce->grab().save(QDir(argv[1]).filePath("bounce-"+label+".png")); bounce->hide();
                output->show(); app.processEvents(); output->grab().save(QDir(argv[1]).filePath("export-stems-"+label+".png"));
            }
        }
    }
    std::printf("recording_workflow: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
