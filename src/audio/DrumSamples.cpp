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
#include <cstring>

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
    const auto installed=QCoreApplication::applicationDirPath()+"/../share/sessions/drum-content";
    return QFile::exists(installed+"/library.json")?installed:QString(SESSIONS_DRUM_CONTENT_DIR);
}
QJsonArray contentEntries(){QFile f(contentRoot()+"/library.json");if(!f.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(f.readAll()).object()["banks"].toArray();}
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
std::shared_ptr<const ContentBank> loadContentBank(int index){
    static std::weak_ptr<const ContentBank> cache[6];if(index<0||index>=6)return {};
    if(auto existing=cache[index].lock())return existing;
    const auto banks=contentEntries();if(index>=banks.size())return {};
    auto bank=std::make_shared<ContentBank>();const auto meta=banks[index].toObject();bank->name=meta["name"].toString();
    const auto notes=meta["notes"].toObject();
    for(auto it=notes.begin();it!=notes.end();++it){const int note=it.key().toInt();if(note<0||note>=128)continue;
        for(const auto& v:it.value().toArray()){const auto group=v.toObject();VelocityGroup layer;layer.low=group["low"].toInt(1);layer.high=group["high"].toInt(127);
            for(const auto& value:group["hits"].toArray()){const auto hit=value.toObject();RecordedHit recording;
                recording.close=readPair(hit["close"],bank->error);recording.alternate=readPair(hit["alternate"],bank->error);recording.overhead=readPair(hit["overhead"],bank->error);recording.room=readPair(hit["room"],bank->error);recording.gain=float(hit["gain"].toDouble(1));
                for(const auto* source:{&recording.close,&recording.alternate,&recording.overhead,&recording.room}){bank->frames+=source->mono.size()+source->right.size();if(!source->mono.empty())++bank->recordings;}
                if(!recording.close.mono.empty())layer.hits.push_back(std::move(recording));}
            if(!layer.hits.empty())bank->notes[size_t(note)].push_back(std::move(layer));}}
    cache[index]=bank;return bank;
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

}
