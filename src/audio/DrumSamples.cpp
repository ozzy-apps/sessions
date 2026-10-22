#include "DrumSamples.h"
#include "core/DeviceSpecs.h"
#include <QFileInfo>
#include <QLibrary>
#include <cmath>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMutex>
#include <QSettings>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>
#include <atomic>
#include <cstring>
#include <functional>

namespace sessions::audio::drums {
namespace {
struct Library {
    std::array<Bank, 3> banks;
    QString error;
    int loaded = 0;
    Library() {
        QString root = QCoreApplication::applicationDirPath() + "/../share/sessions/drums";
        if (!QFile::exists(root + "/library.json")) root = SESSIONS_DRUM_SOURCE_DIR;
        QFile manifest(root + "/library.json");
        if (!manifest.open(QIODevice::ReadOnly)) { error = "Drum recordings are missing"; return; }
        const auto entries = QJsonDocument::fromJson(manifest.readAll()).object()["banks"].toArray();
        for (int b = 0; b < entries.size() && b < 3; ++b) {
            const auto notes = entries[b].toObject()["notes"].toObject();
            for (auto it = notes.begin(); it != notes.end(); ++it) {
                const int note = it.key().toInt(); if (note < 0 || note > 127) continue;
                const auto files = it.value().toArray();
                for (int v = 0; v < files.size() && v < 5; ++v) {
                    QFile f(root + '/' + files[v].toString());
                    if (!f.open(QIODevice::ReadOnly)) { error = "Missing drum recording: " + f.fileName(); continue; }
                    const auto bytes = f.readAll();
                    if (bytes.size() < 44 || bytes.first(4) != "RIFF" || bytes.mid(8, 4) != "WAVE") { error = "Invalid drum WAV"; continue; }
                    int channels = 0, bits = 0, format = 0, rate = 0, data = 0, size = 0;
                    for (qint64 p = 12; p + 8 <= bytes.size();) {
                        const quint32 n = qFromLittleEndian<quint32>(bytes.constData() + p + 4);
                        if (p + 8 + n > bytes.size()) break;
                        if (bytes.mid(p, 4) == "fmt " && n >= 16) {
                            format = qFromLittleEndian<quint16>(bytes.constData() + p + 8);
                            channels = qFromLittleEndian<quint16>(bytes.constData() + p + 10);
                            rate = int(qFromLittleEndian<quint32>(bytes.constData() + p + 12));
                            bits = qFromLittleEndian<quint16>(bytes.constData() + p + 22);
                        } else if (bytes.mid(p, 4) == "data") { data = int(p + 8); size = int(n); }
                        p += 8 + n + (n & 1);
                    }
                    if (format != 1 || bits != 16 || channels != 1 || rate < 8000 || !data) { error = "Unsupported bundled drum WAV"; continue; }
                    auto& layer = banks[b].notes[note][v]; layer.rate = rate; layer.mono.resize(size / 2);
                    for (int i = 0; i < size / 2; ++i) layer.mono[i] = qFromLittleEndian<qint16>(bytes.constData() + data + i * 2) / 32768.f;
                    ++loaded;
                }
            }
        }
    }
};
const Library& data() { static const Library d; return d; }
}
const std::array<Bank, 3>& library() { return data().banks; }
int bankFor(int recipe) { constexpr int bank[6] = {0, 1, 0, 1, 2, 1}; return bank[recipe >= 0 && recipe < 6 ? recipe : 0]; }
const char* sourceName(int recipe) { constexpr const char* names[] = {"Black Pearl", "Red Zeppelin", "Blonde Bop"}; return names[bankFor(recipe)]; }
int sampleNote(int gm) { return gm == 35 ? 36 : (gm == 48 || gm == 50) ? 45 : gm; }
float sampleTune(int gm) { return (gm == 48 || gm == 50) ? 5.f : 0.f; }
QString error() { return data().error; }
int loadedLayers() { return data().loaded; }
namespace {
QString contentRoot(){
    const QString chosen=qEnvironmentVariable("SESSIONS_DRUM_CONTENT_ROOT");   // another copy of the libraries (and the tests' generated one)
    if(!chosen.isEmpty())return chosen;
    const auto installed=QCoreApplication::applicationDirPath()+"/../share/sessions/drum-content";
    return QFile::exists(installed+"/library.json")?installed:QString(SESSIONS_DRUM_CONTENT_DIR);
}
// The manifest is read once per content folder (it's large), then shared by every thread.
QJsonArray contentEntries(){
    static QMutex mutex;static QString root;static QJsonArray banks;
    const QString now=contentRoot();QMutexLocker lock(&mutex);
    if(now==root)return banks;
    QFile f(now+"/library.json");banks=f.open(QIODevice::ReadOnly)?QJsonDocument::fromJson(f.readAll()).object()["banks"].toArray():QJsonArray();root=now;return banks;
}
struct SfInfo {int64_t frames;int samplerate,channels,format,sections,seekable;};
Layer readLayer(const QString& relative,QString& error){
    Layer layer;if(relative.isEmpty())return layer;
    const QDir root(contentRoot());const QString absolute=QDir::cleanPath(QFileInfo(root.filePath(relative)).absoluteFilePath());
    if(!absolute.startsWith(root.absolutePath()+'/')){error="Invalid content path";return layer;}
    static QLibrary lib("libsndfile.so.1");
    using Open=void*(*)(const char*,int,SfInfo*);using Read=int64_t(*)(void*,float*,int64_t);using Close=int(*)(void*);
    static auto open=reinterpret_cast<Open>(lib.resolve("sf_open"));static auto read=reinterpret_cast<Read>(lib.resolve("sf_readf_float"));static auto close=reinterpret_cast<Close>(lib.resolve("sf_close"));
    if(!open||!read||!close){error="Drum content needs libsndfile1";return layer;}
    SfInfo info{};void* file=open(QFile::encodeName(absolute).constData(),0x10,&info);
    if(!file){error="Missing or invalid drum recording: "+relative;return layer;}
    if(info.frames<2||info.frames>192000*60||info.channels<1||info.channels>2||info.samplerate<8000||info.samplerate>192000){close(file);error="Unsupported drum recording: "+relative;return layer;}
    layer.rate=info.samplerate;layer.mono.resize(size_t(info.frames));if(info.channels==2)layer.right.resize(size_t(info.frames));
    float block[4096*2];int64_t at=0;
    while(at<info.frames){const auto got=read(file,block,std::min<int64_t>(4096,info.frames-at));if(got<=0)break;
        for(int64_t i=0;i<got;++i){const float left=block[i*info.channels],right=info.channels==2?block[i*info.channels+1]:left;
            layer.mono[size_t(at+i)]=std::isfinite(left)?left:0;if(info.channels==2)layer.right[size_t(at+i)]=std::isfinite(right)?right:0;}at+=got;}
    close(file);layer.mono.resize(size_t(at));if(info.channels==2)layer.right.resize(size_t(at));if(at<2)error="Empty drum recording: "+relative;return layer;
}
Layer readPair(const QJsonValue& value,QString& error){
    if(value.isString())return readLayer(value.toString(),error);
    const auto pair=value.toArray();if(pair.isEmpty())return {};
    Layer out=readLayer(pair[0].toString(),error);if(pair.size()>1){auto right=readLayer(pair[1].toString(),error);if(right.rate!=out.rate){error="Mismatched recorded mic rates";return {};}
        out.right=std::move(right.mono);const size_t size=std::max(out.mono.size(),out.right.size());out.mono.resize(size);out.right.resize(size);}return out;
}
}
const char* contentName(int index){constexpr const char* names[]={"Big Rusty Drums","Swirly Drums","Frankensnare","DRSKit","Naked Drums","MuldjordKit"};return index>=0&&index<6?names[index]:"Kit recipe";}
int contentIndex(const float* p,int drum){
    const int override=choiceIndex(DeviceType::DrumStudio,drumstudio::contentSource(drum),p[drumstudio::contentSource(drum)]);
    return override>=2?override-2:override==1?-1:choiceIndex(DeviceType::DrumStudio,drumstudio::ContentLibrary,p[drumstudio::ContentLibrary])-1;
}
bool contentInstalled(int index){
    const auto banks=contentEntries();if(index<0||index>=banks.size())return false;
    const auto notes=banks[index].toObject()["notes"].toObject();
    for(const auto& groups:notes)for(const auto& group:groups.toArray())for(const auto& hit:group.toObject()["hits"].toArray()){
        const QJsonValue source=hit.toObject()["close"];const auto pair=source.toArray();const auto path=source.isString()?source.toString():pair.isEmpty()?QString():pair.first().toString();
        if(!path.isEmpty())return QFile::exists(QDir(contentRoot()).filePath(path));
    }
    return false;
}
namespace {
// Decodes one bank. `cancel` is checked between recordings; `progress` gets 0..1.
std::shared_ptr<ContentBank> decodeBank(int index,const std::atomic<bool>* cancel,const std::function<void(double)>& progress,int delayMs){
    const auto banks=contentEntries();if(index<0||index>=banks.size())return {};
    auto bank=std::make_shared<ContentBank>();const auto meta=banks[index].toObject();bank->name=meta["name"].toString();
    const auto notes=meta["notes"].toObject();
    int total=0,read=0;
    for(auto it=notes.begin();it!=notes.end();++it)for(const auto& v:it.value().toArray())total+=v.toObject()["hits"].toArray().size();
    for(auto it=notes.begin();it!=notes.end();++it){const int note=it.key().toInt();if(note<0||note>=128)continue;
        for(const auto& v:it.value().toArray()){const auto group=v.toObject();VelocityGroup layer;layer.low=group["low"].toInt(1);layer.high=group["high"].toInt(127);
            for(const auto& value:group["hits"].toArray()){
                if(cancel&&cancel->load(std::memory_order_relaxed))return {};
                if(delayMs>0)QThread::msleep(ulong(delayMs));
                const auto hit=value.toObject();RecordedHit recording;
                recording.close=readPair(hit["close"],bank->error);recording.alternate=readPair(hit["alternate"],bank->error);recording.overhead=readPair(hit["overhead"],bank->error);recording.room=readPair(hit["room"],bank->error);recording.gain=float(hit["gain"].toDouble(1));
                for(const auto* source:{&recording.close,&recording.alternate,&recording.overhead,&recording.room}){bank->frames+=source->mono.size()+source->right.size();if(!source->mono.empty())++bank->recordings;}
                if(!recording.close.mono.empty())layer.hits.push_back(std::move(recording));
                if(progress)progress(total?double(++read)/total:1.0);}
            if(!layer.hits.empty())bank->notes[size_t(note)].push_back(std::move(layer));}}
    return bank;
}

// What the loader knows about each bank. Guarded by registryMutex(): the worker writes results,
// a blocking load waits on registryChanged().
using State=ContentLoader::State;
struct Entry {
    std::shared_ptr<const ContentBank> strong;   // the loader's hold: wanted, or kept within the budget
    std::weak_ptr<const ContentBank> weak;       // anyone's (a render state, a blocking load)
    State  state=State::Idle;
    double progress=0;
    QString error;
    std::shared_ptr<std::atomic<bool>> cancel;   // the running load's
    bool   elsewhere=false;                       // a blocking load decodes it on another thread
    int    installed=-1;                          // contentInstalled, once checked
};
QMutex& registryMutex(){static QMutex m;return m;}
QWaitCondition& registryChanged(){static QWaitCondition c;return c;}
std::array<Entry,kContentBanks>& entries(){static std::array<Entry,kContentBanks> e;return e;}
std::shared_ptr<const ContentBank> alive(const Entry& e){return e.strong?e.strong:e.weak.lock();}
qint64 bankBytes(const ContentBank& b){return qint64(b.frames)*qint64(sizeof(float));}
QThreadPool* worker(){
    static QThreadPool* pool=[]{auto* p=new QThreadPool;p->setMaxThreadCount(1);p->setExpiryTimeout(-1);
        if(QCoreApplication::instance())QObject::connect(QCoreApplication::instance(),&QCoreApplication::aboutToQuit,[p]{   // leave promptly
            {QMutexLocker lock(&registryMutex());for(auto& e:entries())if(e.cancel)e.cancel->store(true);}
            p->waitForDone(3000);});
        return p;}();
    return pool;
}
}

std::shared_ptr<const ContentBank> loadContentBank(int index){
    if(index<0||index>=kContentBanks)return {};
    QMutexLocker lock(&registryMutex());auto& e=entries()[size_t(index)];
    for(;;){
        if(auto bank=alive(e))return bank;
        if(e.state!=State::Loading&&!e.elsewhere)break;
        registryChanged().wait(&registryMutex());   // the worker (or another caller) is decoding it: share that
    }
    e.elsewhere=true;lock.unlock();
    auto bank=decodeBank(index,nullptr,{},0);
    lock.relock();e.elsewhere=false;e.weak=bank;
    if(bank&&bank->recordings>0&&e.state!=State::Ready){e.state=State::Ready;e.progress=1;e.error=bank->error;}
    registryChanged().wakeAll();
    return bank;
}
std::shared_ptr<const Content> prepareContent(const float* p,bool all){
    auto result=std::make_shared<Content>();bool used[6]{};
    for(int d=0;d<drumstudio::kDrums;++d){const int index=contentIndex(p,d);if(index>=0)used[index]=true;}
    for(int i=0;i<6;++i)if(used[i]||all)result->banks[i]=loadContentBank(i);return result;
}
const RecordedHit* recordedHit(const ContentBank& bank,int note,int velocity,unsigned roundRobin,int* layers){
    if(note<0||note>=128)return nullptr;const auto& groups=bank.notes[size_t(note)];if(layers)*layers=int(groups.size());
    if(groups.empty())return nullptr;const VelocityGroup* selected=&groups.back();
    for(const auto& group:groups)if(velocity<=group.high){selected=&group;break;}
    return selected->hits.empty()?nullptr:&selected->hits[roundRobin%selected->hits.size()];
}

std::array<bool,kContentBanks> reachableBanks(const float* p,const std::vector<std::tuple<int,float,float>>& ranges){
    constexpr int kChoices=16;
    auto choices=[&](int param){std::array<bool,kChoices> set{};
        auto mark=[&](float lo,float hi){int a=choiceIndex(DeviceType::DrumStudio,param,std::min(lo,hi)),b=choiceIndex(DeviceType::DrumStudio,param,std::max(lo,hi));
            if(a>b)std::swap(a,b);for(int c=std::max(0,a);c<=std::min(kChoices-1,b);++c)set[size_t(c)]=true;};
        mark(p[param],p[param]);for(const auto& [q,lo,hi]:ranges)if(q==param)mark(lo,hi);return set;};
    std::array<bool,kContentBanks> out{};
    const auto library=choices(drumstudio::ContentLibrary);
    for(int d=0;d<drumstudio::kDrums;++d){const auto source=choices(drumstudio::contentSource(d));
        for(int c=2;c<kChoices&&c-2<kContentBanks;++c)if(source[size_t(c)])out[size_t(c-2)]=true;   // its own library
        if(source[0])for(int c=1;c<kChoices&&c-1<kContentBanks;++c)if(library[size_t(c)])out[size_t(c-1)]=true;}   // following the kit's
    return out;
}

// ---- the background loader ------------------------------------------------------------------

ContentLoader* ContentLoader::instance(){static ContentLoader* loader=new ContentLoader;return loader;}
ContentLoader::ContentLoader(){m_budget=savedBudget();}

qint64 ContentLoader::savedBudget(){
    const int gb=QSettings("Sessions","Sessions").value("drums/memoryBudgetGB",4).toInt();
    return gb<=0?0:qint64(gb)<<30;
}

ContentLoader::Status ContentLoader::status(int bank) const{
    Status out;if(bank<0||bank>=kContentBanks){out.state=State::Missing;return out;}
    QMutexLocker lock(&registryMutex());auto& e=entries()[size_t(bank)];
    if(e.installed<0){lock.unlock();const bool installed=contentInstalled(bank);lock.relock();e.installed=installed;}
    const auto held=alive(e);
    out.state=!e.installed&&!held?State::Missing:held?State::Ready:e.state==State::Ready?State::Idle:e.state;
    out.progress=out.state==State::Ready?1:e.progress;out.error=e.error;out.bytes=held?bankBytes(*held):0;
    return out;
}

std::shared_ptr<const ContentBank> ContentLoader::ready(int bank) const{
    if(bank<0||bank>=kContentBanks)return {};
    QMutexLocker lock(&registryMutex());return alive(entries()[size_t(bank)]);
}

bool ContentLoader::wanted(int bank) const{return bank>=0&&bank<kContentBanks&&m_wanted[size_t(bank)];}

void ContentLoader::want(const std::array<bool,kContentBanks>& banks){
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    bool changed=false;
    {
        QMutexLocker lock(&registryMutex());
        for(int i=0;i<kContentBanks;++i){auto& e=entries()[size_t(i)];
            if(banks[size_t(i)]){m_lastWanted[size_t(i)]=now;
                if(auto held=alive(e)){if(!e.strong)e.strong=held;}
                else if(e.state==State::Idle||e.state==State::Ready){
                    if(e.installed<0){lock.unlock();const bool installed=contentInstalled(i);lock.relock();e.installed=installed;}
                    if(e.installed){e.state=State::Queued;e.progress=0;e.error.clear();changed=true;}
                }
            }else{
                if(e.state==State::Queued){e.state=State::Idle;changed=true;}
                if(e.state==State::Loading&&e.cancel)e.cancel->store(true);   // nobody wants it now
                if(e.state==State::Failed){e.state=State::Idle;e.error.clear();}   // asked for again later: try again
            }}
    }
    m_wanted=banks;
    trim();start();
    if(changed)for(int i=0;i<kContentBanks;++i)emit progressed(i);
}

void ContentLoader::setBudget(qint64 bytes){m_budget=std::max<qint64>(0,bytes);trim();}

qint64 ContentLoader::memoryUsed() const{
    QMutexLocker lock(&registryMutex());qint64 total=0;
    for(const auto& e:entries())if(auto held=alive(e))total+=bankBytes(*held);
    return total;
}

void ContentLoader::trim(){
    QMutexLocker lock(&registryMutex());
    qint64 used=0;
    for(int i=0;i<kContentBanks;++i)if(m_wanted[size_t(i)])if(auto held=alive(entries()[size_t(i)]))used+=bankBytes(*held);
    std::vector<int> kept;
    for(int i=0;i<kContentBanks;++i)if(!m_wanted[size_t(i)]&&entries()[size_t(i)].strong)kept.push_back(i);
    std::sort(kept.begin(),kept.end(),[this](int a,int b){return m_lastWanted[size_t(a)]>m_lastWanted[size_t(b)];});   // the most recent first
    for(int i:kept){auto& e=entries()[size_t(i)];const qint64 bytes=bankBytes(*e.strong);
        if(m_budget==0||used+bytes<=m_budget)used+=bytes;else e.strong.reset();}
}

void ContentLoader::start(){
    if(m_running>=0)return;
    int next=-1;
    {
        QMutexLocker lock(&registryMutex());
        for(int i=0;i<kContentBanks&&next<0;++i){const auto& e=entries()[size_t(i)];if(e.state==State::Queued&&m_wanted[size_t(i)]&&!e.elsewhere)next=i;}
        if(next<0)return;
        auto& e=entries()[size_t(next)];e.state=State::Loading;e.progress=0;e.cancel=std::make_shared<std::atomic<bool>>(false);
    }
    m_running=next;
    std::shared_ptr<std::atomic<bool>> cancel;{QMutexLocker lock(&registryMutex());cancel=entries()[size_t(next)].cancel;}
    const int bank=next,delay=m_testDelay;
    emit progressed(bank);
    worker()->start([this,bank,cancel,delay]{
        double reported=0;
        auto decoded=decodeBank(bank,cancel.get(),[&](double f){
            if(f-reported<.01&&f<1)return;
            reported=f;{QMutexLocker lock(&registryMutex());entries()[size_t(bank)].progress=f;}
            QMetaObject::invokeMethod(this,[this,bank]{emit progressed(bank);},Qt::QueuedConnection);
        },delay);
        const bool cancelled=cancel->load();
        {   // the result, right here: a blocking load on the window's thread may be waiting for it
            QMutexLocker lock(&registryMutex());auto& e=entries()[size_t(bank)];e.cancel.reset();
            if(cancelled){e.state=State::Idle;e.progress=0;}
            else if(!decoded||decoded->recordings==0){e.state=State::Failed;e.error=decoded&&!decoded->error.isEmpty()?decoded->error:QString("No recordings could be read.");}
            else{e.state=State::Ready;e.progress=1;e.error=decoded->error;e.strong=decoded;e.weak=decoded;}
            registryChanged().wakeAll();
        }
        QMetaObject::invokeMethod(this,[this,bank,cancelled]{done(bank,{},cancelled);},Qt::QueuedConnection);
    });
}

void ContentLoader::done(int bank,std::shared_ptr<const ContentBank>,bool cancelled){
    m_running=-1;
    if(cancelled&&m_wanted[size_t(bank)]){QMutexLocker lock(&registryMutex());auto& e=entries()[size_t(bank)];if(e.state==State::Idle)e.state=State::Queued;}   // wanted again meanwhile
    trim();
    emit progressed(bank);
    emit finished(bank);
    start();
}

bool ContentLoader::waitIdle(int ms){
    QElapsedTimer t;t.start();
    for(;;){
        QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        bool queued=false;{QMutexLocker lock(&registryMutex());for(int i=0;i<kContentBanks;++i)queued|=entries()[size_t(i)].state==State::Queued&&m_wanted[size_t(i)];}
        if(m_running<0&&!queued)return true;
        if(t.elapsed()>ms)return false;
        QThread::msleep(5);
    }
}

}
