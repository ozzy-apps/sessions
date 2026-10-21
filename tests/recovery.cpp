// Recovery persistence, real forced termination, immutable worker snapshots and native UI.
#include "core/Recovery.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "audio/Engine.h"
#include "ui/MainWindow.h"
#include "ui/Preferences.h"
#include "ui/RecoveryDialog.h"
#include <QApplication>
#include <QComboBox>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks=0, failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",text);}}
QByteArray bytes(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
void put(const QString& path,const QByteArray& data){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);file.open(QIODevice::WriteOnly);file.write(data);}
QStringList snapshots(const QString& dir){QStringList out;QDirIterator it(dir,{"*.sessions"},QDir::Files,QDirIterator::Subdirectories);while(it.hasNext())out<<it.next();out.sort();return out;}
bool idle(RecoveryManager* manager){QElapsedTimer clock;clock.start();while(manager->busy()&&clock.elapsed()<20000)QTest::qWait(5);QTest::qWait(10);return !manager->busy();}
void populate(Project& p,const QString& base){
    p.loadEmpty();auto clip=p.newClip("Pattern",4);clip->notes={{60,0,.5,.8,1}};p.tracks[0].clipSlots[0]=clip;
    QDir().mkpath(base+"/Samples");std::vector<float> stereo(16000);for(size_t i=0;i<stereo.size();++i)stereo[i]=.2f*std::sin(i*.1);
    const auto path=base+"/Samples/source.wav";samples::writeWav(path,stereo.data(),8000,48000,samples::WavFormat::Float32);
    auto audio=p.newClip("Audio",4);audio->samplePath=path;audio->sample=samples::load(path);audio->warp=false;p.tracks[1].clipSlots[0]=audio;
    emit p.structureChanged();
}
int child(QApplication& app,const QString& base,const QString& mode){
    const bool recording=mode.startsWith("recording");
    MainWindow window;window.enableRecovery(base+"/Recovery");populate(*window.project(),base+"/Project");
    window.saveSet(base+"/Project/Song.sessions");
    if(recording){
        auto* p=window.project();auto* engine=window.engine();auto* transport=window.transport();engine->shutdown();
        p->tracks[1].type=TrackType::Audio;p->tracks[0].armed=p->tracks[1].armed=true;p->recordFolder=base+"/Recorded";emit p->structureChanged();
        QTest::qWait(20);transport->setArrangementRecord(true);transport->play();
        float input[audio::kMaxBlock*2],output[audio::kMaxBlock*2];
        for(int block=0;block<160;++block){for(int i=0;i<audio::kMaxBlock*2;++i)input[i]=.2f*std::sin((block*audio::kMaxBlock*2+i)*.07);engine->pushInput(input,audio::kMaxBlock);engine->process(output,audio::kMaxBlock);if(block==10)transport->noteOn(0,64,.7);if(block==30)transport->noteOff(0,64);if(block==40)transport->noteOn(0,60,.8);QTest::qWait(3);}
    }else{window.project()->renameTrack(0,"Unsaved track");window.project()->tracks[0].clipSlots[0]->notes[0].pitch=72;emit window.project()->clipChanged();}
    window.saveRecoveryNow();if(!idle(window.recoveryManager()))return 2;
    if(recording&&!window.transport()->audioRecording())return 3;
    if(mode=="recording-save"){
        const auto saved=base+"/Project/Live.sessions";if(!window.saveSet(saved))return 4;window.newSet();idle(window.recoveryManager());
        EditState state;setfile::Extras extras;if(!setfile::read(saved,state,extras))return 5;
        bool audio=false;for(const auto& clip:state.clips)audio|=clip.isAudio()&&clip.sample&&clip.samplePath.startsWith(base+"/Recorded");return audio?0:6;
    }
    std::printf("READY\n");std::fflush(stdout);return app.exec(); // parent kills us; no destructors or close event
}
bool crashWriter(const QString& base,bool recording){
    QProcess process;process.setProgram(QCoreApplication::applicationFilePath());process.setArguments({"--crash-writer",base,recording?"recording":"edits"});process.start();if(!process.waitForStarted(10000))return false;
    QByteArray output;QElapsedTimer timer;timer.start();while(!output.contains("READY\n")&&timer.elapsed()<30000&&process.state()!=QProcess::NotRunning){process.waitForReadyRead(100);output+=process.readAllStandardOutput();QApplication::processEvents();}
    const bool ready=output.contains("READY\n");
    RecoveryManager observer(base+"/Recovery");check(observer.candidates().isEmpty(),"A live process's snapshots are never offered for recovery");
    process.kill();process.waitForFinished(10000);if(!ready)std::fprintf(stderr,"Child output: %s\n",process.readAllStandardError().constData());return ready;
}
}
int main(int argc,char** argv){
    QTemporaryDir temporary;
    if(argc<2||QString::fromLocal8Bit(argv[1])!="--crash-writer"){qputenv("XDG_CONFIG_HOME",(temporary.path()+"/Config").toUtf8());qputenv("XDG_DATA_HOME",(temporary.path()+"/Data").toUtf8());}
    qputenv("SESSIONS_AUDIO","none");QApplication app(argc,argv);Q_INIT_RESOURCE(resources);app.setOrganizationName("SessionsRecoveryTest");app.setApplicationName("SessionsRecoveryTest");theme::installAppStyle();theme::apply(theme::Kind::Dark);
    if(argc>3&&QString::fromLocal8Bit(argv[1])=="--crash-writer")return child(app,QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]));
    const QString captures=argc>1?QString::fromLocal8Bit(argv[1]):QString();if(!captures.isEmpty())QDir().mkpath(captures);
    check(RecoveryManager::intervalSeconds()==120&&RecoveryManager::backupCount()==10,"Recovery defaults to two minutes and ten manual-save backups");
    QSettings("Sessions","Sessions").setValue("recovery/intervalSeconds",-5);QSettings("Sessions","Sessions").setValue("recovery/backupCount",999);
    check(RecoveryManager::intervalSeconds()==120&&RecoveryManager::backupCount()==10,"Malformed recovery settings fall back to bounded defaults");
    QSettings("Sessions","Sessions").setValue("recovery/intervalSeconds",120);QSettings("Sessions","Sessions").setValue("recovery/backupCount",5);
    Project project;populate(project,temporary.path()+"/Project");setfile::Extras extras;const auto original=temporary.path()+"/Project/Song.sessions";setfile::save(original,project,extras);const auto originalBytes=bytes(original);
    const auto root=temporary.path()+"/Store";QString latest;
    {
        RecoveryManager manager(root);QSignalSpy written(&manager,&RecoveryManager::snapshotWritten);
        for(int i=0;i<7;++i){auto state=project.captureEditState();state.tempo=120+i;manager.checkpoint(state,extras,"Song",original);check(idle(&manager),"Snapshot writes complete asynchronously");}
        check(snapshots(root).size()==3,"Rotation retains exactly three successfully committed recovery snapshots");
        check(manager.candidates().isEmpty(),"An active window cannot recover its own live snapshot");
        auto frozen=project.captureEditState();frozen.tempo=151;manager.checkpoint(frozen,extras,"Song",original);frozen.tempo=1;frozen.clips[frozen.clips.firstKey()].notes[0].pitch=2;check(idle(&manager),"The worker finishes a detached value snapshot");
        latest=written.last()[0].toString();EditState read;setfile::Extras loaded;check(setfile::read(latest,read,loaded)&&read.tempo==151&&read.clips.first().notes.first().pitch==60,"UI edits cannot mutate a snapshot while the worker serializes it");
        const int count=written.size();auto unchanged=project.captureEditState();unchanged.tempo=151;manager.checkpoint(unchanged,extras,"Song",original);idle(&manager);check(written.size()==count,"Unchanged dirty projects do not rotate duplicate snapshots");
        for(int i=0;i<30;++i){auto state=project.captureEditState();state.tempo=160+i;manager.checkpoint(std::move(state),extras,"Song",original);}idle(&manager);latest=written.last()[0].toString();check(setfile::read(latest,read,loaded)&&read.tempo==189,"Rapid snapshot requests coalesce to the newest version");
        check(bytes(original)==originalBytes,"Autosave never writes or cleans the manual set file");
    }
    RecoveryManager reader(root);auto entries=reader.candidates();check(entries.size()==3,"Snapshots become recoverable when their owning process/window ends without a clean close");
    put(latest,"truncated");entries=reader.candidates();bool corrupt=false;QString valid;for(const auto& e:entries){corrupt|=!e.error.isEmpty();if(e.error.isEmpty()&&valid.isEmpty())valid=e.file;}check(corrupt&&!valid.isEmpty(),"A corrupt newest snapshot leaves earlier usable recovery versions visible");
    EditState recovered=project.captureEditState();const auto before=recovered;setfile::Extras loaded;RecoveryEntry entry;QString error;
    check(!reader.restore(latest,recovered,loaded,entry,&error)&&recovered==before&&!error.isEmpty(),"Corrupt recovery leaves the current project state untouched");
    {
        auto payload=QJsonDocument::fromJson(bytes(valid)).object();payload.insert("tempo",42);put(valid,QJsonDocument(payload).toJson());
        check(!reader.restore(valid,recovered,loaded,entry,&error)&&error.contains("checksum")&&recovered==before,"A parseable snapshot with modified project data fails its checksum without changing the project");
        for(const auto& candidate:reader.candidates())if(candidate.error.isEmpty()){valid=candidate.file;break;}
        const auto huge=QDir(QFileInfo(valid).absolutePath()).filePath("oversized.sessions");QFile file(huge);file.open(QIODevice::WriteOnly);file.resize(256LL*1024*1024+1);file.close();
        check(!reader.restore(huge,recovered,loaded,entry,&error)&&error.contains("256 MB")&&recovered==before,"Oversized recovery files are rejected before parsing or media loading");QFile::remove(huge);
        const auto link=QDir(QFileInfo(valid).absolutePath()).filePath("linked.sessions");
        if(QFile::link(original,link)){check(!reader.discard(link,&error)&&bytes(original)==originalBytes,"Recovery cleanup rejects symlinks to files outside its store");QFile::remove(link);}
    }
    check(!reader.discard(original,&error)&&bytes(original)==originalBytes,"Recovery deletion cannot reach ordinary project files");
    if(!captures.isEmpty())for(auto kind:{theme::Kind::Dark,theme::Kind::Light}){theme::apply(kind);RecoveryDialog dialog(&reader);dialog.show();QTest::qWait(30);check(dialog.findChild<QTreeWidget*>("recoveryList")->topLevelItemCount()==3,"Recovery picker shows valid and damaged snapshots");dialog.grab().save(captures+(kind==theme::Kind::Dark?"/recover-dark.png":"/recover-light.png"));dialog.reject();}
    check(reader.restore(valid,recovered,loaded,entry,&error),"An earlier valid snapshot can be claimed and restored");reader.clear();idle(&reader);
    {
        RecoveryManager backup(root+"Backups");QSignalSpy written(&backup,&RecoveryManager::backupWritten);
        for(int i=0;i<9;++i){auto state=project.captureEditState();state.tempo=120+i;backup.backup(std::move(state),extras,original);idle(&backup);}
        const auto folder=RecoveryManager::backupFolder(original);check(snapshots(folder).size()==5,"Manual-save backups obey their configured retention count");
        check(written.size()==9&&setfile::read(written.last()[0].toString(),recovered,loaded)&&recovered.tempo==128,"The newest rolling backup is a normal, loadable Sessions set");
        check(bytes(original)==originalBytes,"Background backup rotation never changes the original set");
        const QString moved=temporary.path()+"/Moved";check(QDir().rename(temporary.path()+"/Project",moved),"Project folder moves with its backups and source recordings");
        const auto movedBackup=QDir(RecoveryManager::backupFolder(moved+"/Song.sessions")).filePath(QFileInfo(written.last()[0].toString()).fileName());
        check(setfile::read(movedBackup,recovered,loaded)&&recovered.clips.last().sample&&recovered.clips.last().samplePath.startsWith(moved),"Backup sample references remain portable after moving the whole project");
    }
    {
        const QString blocked=temporary.path()+"/Blocked";put(blocked,"file");RecoveryManager manager(blocked+"/Recovery");QSignalSpy failed(&manager,&RecoveryManager::writeFailed);manager.checkpoint(project.captureEditState(),extras,"Untitled",{});check(failed.size()==1&&!manager.busy(),"Unwritable recovery storage reports failure without blocking or marking a save successful");
        const QString backupSet=temporary.path()+"/BlockedBackup/Song.sessions";put(temporary.path()+"/BlockedBackup/Backups","file");manager.backup(project.captureEditState(),extras,backupSet);idle(&manager);check(failed.size()==2,"A backup-folder failure is reported independently of a successful manual save");
    }
    const QString crash=temporary.path()+"/Crash";check(crashWriter(crash,false),"A real process writes a recovery snapshot before being forcibly killed");
    {
        MainWindow window;window.enableRecovery(crash+"/Recovery");const auto entries=window.recoveryManager()->candidates();check(!entries.isEmpty(),"Stale process locks are released after forced termination");
        const auto manual=bytes(crash+"/Project/Song.sessions");const int count=snapshots(crash+"/Recovery").size();
        RecoveryDialog later(window.recoveryManager());later.findChild<QPushButton*>("recoveryLater")->click();check(snapshots(crash+"/Recovery").size()==count,"Later keeps recovery snapshots for a future startup");
        check(!entries.isEmpty()&&window.recoverSet(entries.first().file,&error),"MainWindow opens an actual crashed session through the recovery path");idle(window.recoveryManager());
        check(window.setPath().isEmpty()&&window.setModified()&&window.setName().contains("Recovered")&&window.project()->tracks[0].name=="Unsaved track","Recovery opens an unsaved copy with the user's latest edits");
        check(window.project()->tracks[0].clipSlots[0]->notes[0].pitch==72&&window.project()->tracks[1].clipSlots[0]->sample,"Recovered MIDI edits and external recording references are intact");
        check(bytes(crash+"/Project/Song.sessions")==manual,"Recovering a copy never replaces the last manual save");
        const int undo=window.project()->undoStack()->index();window.saveRecoveryNow();idle(window.recoveryManager());check(window.setModified()&&window.project()->undoStack()->index()==undo,"Autosave preserves the dirty flag and undo history");
        QTimer::singleShot(0,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->done(QMessageBox::Cancel);});check(!window.close(),"Cancelling Close leaves the recovered session open");
        check(!snapshots(crash+"/Recovery").isEmpty(),"Cancelling Close retains recovery snapshots");
        const auto copy=crash+"/Project/Recovered.sessions";check(window.saveSet(copy,&error),"A recovered session can be saved under its own filename");idle(window.recoveryManager());check(!window.setModified()&&bytes(crash+"/Project/Song.sessions")==manual,"Saving the recovered copy clears its dirty state without overwriting the original");
        window.project()->renameTrack(0,"Another edit");window.saveRecoveryNow();window.newSet();idle(window.recoveryManager());check(snapshots(crash+"/Recovery").isEmpty(),"New Set retires pending old writes so a discarded document cannot reappear");
        window.project()->renameTrack(0,"Keep until discard");window.saveRecoveryNow();idle(window.recoveryManager());QTimer::singleShot(0,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->done(QMessageBox::Discard);});check(window.close(),"Explicit Discard closes the set");idle(window.recoveryManager());check(snapshots(crash+"/Recovery").isEmpty(),"Confirmed clean close removes only that document's recovery snapshots");
    }
    const QString recordCrash=temporary.path()+"/RecordingCrash";check(crashWriter(recordCrash,true),"A recording process can checkpoint without ending its live takes");
    {
        RecoveryManager manager(recordCrash+"/Recovery");const auto entries=manager.candidates();check(!entries.isEmpty()&&manager.restore(entries.first().file,recovered,loaded,entry,&error),"A forced recording shutdown leaves a readable recovery set");
        bool held=false,released=false,audio=false,nonSilent=false;
        for(const auto& track:recovered.tracks)for(const auto& pl:track.arrangement){const auto clip=recovered.clips.value(pl.clip);for(const auto& note:clip.notes){held|=note.pitch==60&&note.length>0;released|=note.pitch==64&&note.length>0;}audio|=clip.isAudio()&&clip.sample&&clip.sample->frames()>0;if(clip.isAudio()&&clip.sample)for(float value:clip.sample->left)nonSilent|=std::abs(value)>.01;}
        check(held&&released,"Recovery retains completed notes and the portion of a still-held MIDI note");check(audio,"A WAV interrupted before close has a readable checkpoint header and timeline placement");
        check(nonSilent,"The recovered live WAV contains the captured input signal");
        manager.clear();idle(&manager);
    }
    {
        QProcess process;process.start(QCoreApplication::applicationFilePath(),{"--crash-writer",temporary.path()+"/SavedRecording","recording-save"});
        check(process.waitForStarted(10000)&&process.waitForFinished(30000)&&process.exitCode()==0,"Saving during recording then opening New Set preserves the referenced live WAV");
        if(process.exitCode()!=0)std::fprintf(stderr,"Saved recording: %s\n",process.readAllStandardError().constData());
    }
    {
        MainWindow window;window.enableRecovery(temporary.path()+"/Timer");window.newSet();window.project()->renameTrack(0,"Timer edit");const int undo=window.project()->undoStack()->index();QMetaObject::invokeMethod(window.findChild<QTimer*>("recoveryTimer"),"timeout",Qt::DirectConnection);idle(window.recoveryManager());check(!snapshots(temporary.path()+"/Timer").isEmpty()&&window.project()->undoStack()->index()==undo,"The real autosave timer captures a dirty untitled set without adding an undo step");
        const auto savePath=temporary.path()+"/Responsive/Song.sessions";QDir().mkpath(QFileInfo(savePath).absolutePath());bool editRan=false;
        QTimer::singleShot(0,[&]{editRan=true;window.project()->renameTrack(0,"Edited during save");window.newSet();});
        check(window.saveSet(savePath,&error)&&editRan,"A manual save services UI events while the worker serializes and writes");
        check(setfile::read(savePath,recovered,loaded)&&recovered.tracks[0].name=="Timer edit"&&window.project()->tracks[0].name=="Edited during save"&&window.setModified(),"Edits during saving stay dirty and cannot replace the document mid-save");
        idle(window.recoveryManager());check(!snapshots(temporary.path()+"/Timer").isEmpty(),"A newer unsaved edit retains recovery even when the older manual save succeeds");
        check(!window.saveSet(temporary.path()+"/Missing/Folder/Song.sessions",&error)&&window.setModified(),"A failed background manual save preserves dirty state and existing recovery");
        PreferencesDialog prefs(window.engine(),window.midiInput(),[]{},&window);auto* pages=prefs.findChild<QListWidget*>();if(pages)pages->setCurrentRow(4);auto* interval=prefs.findChild<QComboBox*>("recoveryInterval");auto* backups=prefs.findChild<QComboBox*>("recoveryBackups");check(interval&&backups&&interval->currentData().toInt()==120&&backups->currentData().toInt()==5,"Startup preferences expose the actual recovery interval and backup retention");
        if(interval){interval->setCurrentIndex(interval->findData(0));check(RecoveryManager::intervalSeconds()==0,"Recovery can be disabled without deleting retained snapshots");}
        if(!captures.isEmpty())for(auto kind:{theme::Kind::Dark,theme::Kind::Light}){theme::apply(kind);prefs.show();QTest::qWait(30);prefs.grab().save(captures+(kind==theme::Kind::Dark?"/preferences-dark.png":"/preferences-light.png"));}prefs.close();window.newSet();
    }
    std::printf("recovery: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
