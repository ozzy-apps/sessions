#include "Grooves.h"
#include "ArrangementOps.h"
#include "DeviceSpecs.h"
#include <QMimeData>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace sessions::grooves {

namespace {

// One bar: 16 steps; two bars: 32. Written out plainly so they read like a drum chart.
const std::vector<Groove>& library() {
    static const std::vector<Groove> g = {
        // ---------------------------------------------------------------- Rock
        {"Rock", "Basic Eighths", 1, 0.5, false,
         "K|x.......x.......\nS|....x.......x...\nH|x.x.x.x.x.x.x.x.", 120},
        {"Rock", "Driving", 1, 0.5, false,
         "K|x...x.x.x...x...\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 128},
        {"Rock", "Four on the Floor", 1, 0.5, false,
         "K|x...x...x...x...\nS|....X.......X...\nH|x.x.x.x.x.x.x.O.", 132},
        {"Rock", "Half Time", 1, 0.5, false,
         "K|x.........x.....\nS|........X.......\nH|x.x.x.x.x.x.x.x.", 90},
        {"Rock", "Open Hat Chorus", 1, 0.5, false,
         "K|x.......x.x.....\nS|....X.......X...\nH|O.O.O.O.O.O.O.O.\nC|x...............", 132},
        {"Rock", "Ride Verse", 1, 0.5, false,
         "K|x.......x..x....\nS|....x.......x...\nR|x.x.x.x.x.x.x.x.", 116},
        {"Rock", "Tom Groove", 2, 0.5, false,
         "K|x.......x.......x.......x.......\nS|....X.......X.......X.......X...\n3|x.x...x.x.x...x.x.x...x.x.x...x.\n2|..........x...........x.........", 110},
        {"Rock", "Sixteenth Hats", 1, 0.5, false,
         "K|x......xx.......\nS|....X.......X...\nH|xoxoxoxoxoxoxoxo", 100},
        // ---------------------------------------------------------------- Pop
        {"Pop", "Straight", 1, 0.5, false,
         "K|x.....x...x.....\nS|....x.......x...\nH|x.x.x.x.x.x.x.x.", 118},
        {"Pop", "Clap Backbeat", 1, 0.5, false,
         "K|x.......x.x.....\nS|....x.......x...\nP|....x.......x...\nH|x.x.x.x.x.x.x.x.", 115},
        {"Pop", "Sixteenths", 1, 0.5, false,
         "K|x.....x.x.......\nS|....x.......x...\nH|xoxoxoxoxoxoxoxo", 108},
        {"Pop", "Half Time Ballad", 1, 0.5, false,
         "K|x.........x.....\nS|........x.......\nH|x.x.x.x.x.x.x.x.\nC|x...............", 76},
        {"Pop", "Side Stick Verse", 1, 0.5, false,
         "K|x.......x.x.....\nS|....r.......r...\nH|x.x.x.x.x.x.x.x.", 112},
        // ---------------------------------------------------------------- Funk
        {"Funk", "Syncopated", 1, 0.5, false,
         "K|x..x..x...x.x...\nS|....X..o..o.X..o\nH|x.x.x.x.x.x.x.x.", 104},
        {"Funk", "Ghost Notes", 1, 0.5, false,
         "K|x.....x..x......\nS|o.o.X.o.o.o.X.o.\nH|x.x.x.x.x.x.x.O.", 98},
        {"Funk", "Sixteenth Hats", 1, 0.5, false,
         "K|x..x....x..x....\nS|....X.......X...\nH|xoxoxoxoxoxoxoxo", 100},
        {"Funk", "Linear", 1, 0.5, false,
         "K|x...x...x.x.....\nS|..o.x.o...o.x.o.\nH|.x.x.x.x.x.x.x.x", 102},
        {"Funk", "Open Hat Push", 2, 0.5, false,
         "K|x.....x...x.x...x.....x...x.....\nS|....X.......X.......X.....X.X...\nH|x.x.x.x.x.x.x.O.x.x.x.x.x.x.x.O.", 106},
        // ---------------------------------------------------------------- Hip Hop
        {"Hip Hop", "Boom Bap", 1, 0.56, false,
         "K|x......x..x.....\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 92},
        {"Hip Hop", "Lo-Fi Swing", 1, 0.62, false,
         "K|x.....x...x.....\nS|....X.......X...\nH|x.x.x.xox.x.x.xo", 85},
        {"Hip Hop", "Trap Hats", 2, 0.5, false,
         "K|x.......x.x.....x.....x.........\nS|........X...............X.......\nH|xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nP|........x...............x.......", 140},
        {"Hip Hop", "Half Time", 1, 0.54, false,
         "K|x.........x.x...\nS|........X.......\nH|x.x.x.x.x.x.x.x.", 88},
        {"Hip Hop", "Bounce", 1, 0.58, false,
         "K|x..x.....x..x...\nS|....X.......X...\nH|x.xxx.x.x.xxx.x.", 96},
        // ---------------------------------------------------------------- House
        {"House", "Four to the Floor", 1, 0.5, false,
         "K|x...x...x...x...\nP|....x.......x...\nH|..O...O...O...O.", 124},
        {"House", "Offbeat Open Hat", 1, 0.54, false,
         "K|x...x...x...x...\nS|....x.......x...\nH|x.O.x.O.x.O.x.O.", 126},
        {"House", "Garage Shuffle", 1, 0.62, false,
         "K|x.....x.x.......\nS|....x.......x...\nH|x.x.x.x.x.x.x.x.\nP|....x.......x...", 130},
        {"House", "Deep", 2, 0.56, false,
         "K|x...x...x...x...x...x...x...x...\nS|....r.......r.......r.......r...\nH|..x...x...x...x...x...x...x..xx.", 122},
        {"House", "Breakbeat", 1, 0.5, false,
         "K|x.....x...x.....\nS|....X.......X..o\nH|x.x.x.x.x.x.x.x.", 128},
        // ---------------------------------------------------------------- Drum & Bass
        {"Drum & Bass", "Two Step", 1, 0.5, false,
         "K|x.........x.....\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 174},
        {"Drum & Bass", "Amen Feel", 2, 0.5, false,
         "K|x.........x.....x.........x.x...\nS|....X..o..o.X.......X..o..o.X.o.\nH|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.\nR|...........b...............b....", 170},
        {"Drum & Bass", "Jump Up", 1, 0.5, false,
         "K|x.......x.x.....\nS|....X.......X...\nH|xoxoxoxoxoxoxoxo", 176},
        {"Drum & Bass", "Liquid", 2, 0.5, false,
         "K|x.........x.....x.......x.x.....\nS|....X.......X.......X.......X.o.\nR|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.\nH|..........o...............o.....", 172},
        // ---------------------------------------------------------------- Metal
        {"Metal", "Double Kick", 1, 0.5, false,
         "K|xxxxxxxxxxxxxxxx\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 160},
        {"Metal", "Thrash", 1, 0.5, false,
         "K|x.x.x.x.x.x.x.x.\nS|..X...X...X...X.\nH|x.x.x.x.x.x.x.x.", 190},
        {"Metal", "Blast Beat", 1, 0.5, false,
         "K|xxxxxxxxxxxxxxxx\nS|.x.x.x.x.x.x.x.x\nR|x.x.x.x.x.x.x.x.", 200},
        {"Metal", "Half Time Breakdown", 2, 0.5, false,
         "K|x..x..x...xxx...x..x..x...x.x...\nS|........X...............X.......\nC|x...............................\nR|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.", 110},
        {"Metal", "Gallop", 1, 0.5, false,
         "K|x.xxx.xxx.xxx.xx\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 150},
        // ---------------------------------------------------------------- Jazz
        {"Jazz", "Swing Ride", 1, 0.67, true,
         "R|x...x.x.x...x.x.\nH|....p.......p...\nK|o...............\nS|.......o.......o", 150},
        {"Jazz", "Medium Swing", 2, 0.67, true,
         "R|x...x.x.x...x.x.x...x.x.x...x.x.\nH|....p.......p.......p.......p...\nK|o.......o...o...o.......o.......\nS|..o......o....o.....o.....o...o.", 130},
        {"Jazz", "Bossa Nova", 2, 0.5, false,
         "K|x..x..x.x..x..x.x..x..x.x..x..x.\nS|r..r..r...r..r..r..r..r...r..r..\nH|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.", 132},
        {"Jazz", "Brushes Ballad", 1, 0.62, true,
         "K|x.......x.......\nS|o.o.o.o.o.o.o.o.\nH|....p.......p...\nR|x...x...x...x...", 72},
        {"Jazz", "Up Tempo", 1, 0.6, true,
         "R|x...x.x.x...x.x.\nH|....p.......p...\nK|x.......x.......\nS|..o...X...o...o.", 220},
        // ---------------------------------------------------------------- Reggae
        {"Reggae", "One Drop", 1, 0.54, false,
         "K|........x.......\nS|........r.......\nH|x.x.x.x.x.x.x.x.", 75},
        {"Reggae", "Steppers", 1, 0.54, false,
         "K|x...x...x...x...\nS|....r.......r...\nH|x.x.x.x.x.x.x.O.", 80},
        {"Reggae", "Rockers", 1, 0.54, false,
         "K|x...x...x...x...\nS|........X.......\nH|x.xxx.xxx.xxx.xx", 78},
        {"Reggae", "Ska", 1, 0.5, false,
         "K|x.......x.......\nS|....x.......x...\nH|..x...x...x...x.", 160},
        // ---------------------------------------------------------------- Latin
        {"Latin", "Samba", 2, 0.5, false,
         "K|x..x..x.x..x..x.x..x..x.x..x..x.\nS|.r.r..r..r.r..r..r.r..r..r.r..r.\nH|xoxoxoxoxoxoxoxoxoxoxoxoxoxoxoxo", 100},
        {"Latin", "Songo", 2, 0.5, false,
         "K|x.......x..x....x.......x..x....\nS|..r..o..r..o..o...r..o..r..o..o.\nR|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.\n2|.............x...............x..", 108},
        {"Latin", "Cascara", 2, 0.5, false,
         "R|x.x.xx.x.x.xx.x.x.x.xx.x.x.xx.x.\nK|...x......x........x......x.....\nS|..r.....r.....r...r.....r.....r.", 112},
        {"Latin", "Cumbia", 1, 0.5, false,
         "K|x...x...x...x...\nS|..r...r...r...r.\nH|xoxoxoxoxoxoxoxo", 98},
        // ---------------------------------------------------------------- Blues
        {"Blues", "Shuffle", 1, 0.67, true,
         "K|x.......x.......\nS|....X.......X...\nH|x.x.x.x.x.x.x.x.", 120},
        {"Blues", "Slow Blues", 1, 0.67, true,
         "K|x.........x.....\nS|....x.......x...\nR|x.x.x.x.x.x.x.x.", 66},
        {"Blues", "Texas Shuffle", 1, 0.67, true,
         "K|x.x.x.x.x.x.x.x.\nS|o.x.X.x.o.x.X.x.\nH|x.x.x.x.x.x.x.x.", 126},
        // ---------------------------------------------------------------- Punk
        {"Punk", "Fast Eighths", 1, 0.5, false,
         "K|x.x.x.x.x.x.x.x.\nS|..X...X...X...X.\nH|x.x.x.x.x.x.x.x.", 180},
        {"Punk", "D-Beat", 1, 0.5, false,
         "K|x..x..x.x..x..x.\nS|..X...X...X...X.\nH|O.O.O.O.O.O.O.O.", 185},
        {"Punk", "Skate", 1, 0.5, false,
         "K|x...x.x.x...x.x.\nS|....X.......X...\nH|O.O.O.O.O.O.O.O.\nC|x...............", 170},
        // ---------------------------------------------------------------- R&B
        {"R&B", "Slow Jam", 1, 0.58, false,
         "K|x.....x...x.....\nS|....X.......X...\nH|x.xxx.x.x.xxx.x.\nP|....x.......x...", 70},
        {"R&B", "Neo Soul", 2, 0.6, false,
         "K|x.......x.x.....x.....x.x.......\nS|....X.......X.o.....X.......X...\nH|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.\nR|...........................b....", 82},
        {"R&B", "Two Step", 1, 0.56, false,
         "K|x..x.....x.x....\nS|....X.......X...\nH|x.x.x.x.xox.x.xo", 94},
        // ------------------------------------------------------------ Techno / breaks
        {"Techno", "Warehouse", 2, .5, false,
         "K|X...x...x...x...X...x...x...x...\nP|....x.......x.......x.......x...\nH|..O...O...O...O...O...O...O...O.", 134},
        {"Techno", "Rolling Sixteenths", 1, .5, false,
         "K|X...x...x...x...\nS|....x.......x...\nH|xoxoxoxoxoxoxoxO", 138},
        {"Techno", "Dub Techno", 2, .55, false,
         "K|X...x...x...x...X...x...x...x...\nS|....r.......r.......r.......r...\nH|..x...x...x...O...x...x...x...x.", 122},
        {"Techno", "Broken", 2, .5, false,
         "K|x.....x...x..x..x.....x.x..x....\nS|....X.......X.......X.......X...\nH|x.xox.xox.xox.xox.xox.xox.xox.xO", 140},
        {"Techno", "Industrial", 1, .5, false,
         "K|X...X...X...X...\nS|....X.......X...\nH|..O...O...O...O.\n3|...o...o...o...o", 145},
        {"Techno", "Hardgroove", 2, .53, false,
         "K|X...x...x...x...X...x...x...x...\nS|....x.......x.......x.......x...\nH|xoxOxoOoxoxOxoOoxoxOxoOoxoxOxoOo\n2|...o......o........o......o.....", 142},
        {"Drum & Bass", "Rolling Ghosts", 2, .5, false,
         "K|X.........x.....X.........x..x..\nS|....X..o.o..X.o.....X..o.o..X..o\nH|xoxoxoxoxoxoxoxOxoxoxoxoxoxoxoxO", 174},
        {"Drum & Bass", "Neuro Half Time", 2, .5, false,
         "K|X.....x........xX.....x...x.....\nS|........X...............X....o..\nH|x.o.x.o.x.o.x.oOx.o.x.o.x.o.x.o.", 172},
        {"Drum & Bass", "Jungle Chops", 2, .5, false,
         "K|X......x..x.....X.........x.x...\nS|....X.o...o.X..o....X..o..o.X.oo\nH|x.xox.x.x.xox.x.x.xox.x.x.xox.O.\nC|x...............................", 168},
        {"Drum & Bass", "Dancefloor", 2, .5, false,
         "K|X.........x.....X.......x.x.....\nS|....X.......X.......X.......X...\nH|xoxoxoxoxoxoxoxOxoxoxoxoxoxoxoxO\nR|..b.......b.......b.......b.....", 174},
        {"Breakbeat", "Syncopated Break", 2, .53, false,
         "K|X.....x...x.....X..x....x.x.....\nS|....X..o....X.o.....X.......X..o\nH|x.xox.xOx.xox.xOx.xox.xOx.xox.O.", 132},
        {"Breakbeat", "Big Beat", 1, .5, false,
         "K|X.....x.x.....x.\nS|....X.......X...\nH|x.x.x.O.x.x.x.O.\nC|x...............", 128},
        {"Breakbeat", "Electro", 1, .5, false,
         "K|X......x..x.....\nS|....X.......X...\nH|xox.xox.xox.xoxO", 130},
        {"Breakbeat", "Garage Two Step", 2, .62, false,
         "K|X.....x.........X..x......x.....\nS|....X.......X.......X.......X...\nH|x.o.x.O.x.o.x.O.x.o.x.O.x.o.x.O.", 132},
        {"Metal", "Double Kick Variation", 2, .5, false,
         "K|XoxoxoxoxoxoxoxoXoxoxoxoXoxxxxxx\nS|....X.......X.......X.......X...\nR|x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.\nC|x...............x...............", 170},
        {"Metal", "China Breakdown", 1, .5, false,
         "K|X..xx...X.xx..x.\nS|........X.......\nC|c...c...c...c...", 120},
        {"Metal", "Skank Beat", 1, .5, false,
         "K|x...x...x...x...\nS|..X...X...X...X.\nR|x.x.x.x.x.x.x.x.", 200},
        {"Reggae", "Dub One Drop", 2, .58, false,
         "K|........x...............x.......\nS|........r...............r.....o.\nH|x.xox.x.x.xox.O.x.xox.x.x.xox.O.", 72},
        {"Reggae", "Rockers Variation", 2, .54, false,
         "K|X.......x.....x.X.......x.......\nS|........r...............r.......\nH|x.xox.xOx.xox.xOx.xox.xOx.xox.xO", 82},
        {"Disco", "Four on the Floor", 1, .5, false,
         "K|X...x...x...x...\nS|....X.......X...\nH|x.O.x.O.x.O.x.O.", 118},
        {"Disco", "Funky Sixteenths", 2, .53, false,
         "K|X...x...x...x...X...x...x...x...\nS|....X..o....X.......X..o....X..o\nH|xoxOxoxOxoxOxoxOxoxOxoxOxoxOxoxO", 116},
        {"Afrobeat", "Interlocking", 2, .5, false,
         "K|X.....x...x..x..X.....x...x..x..\nS|....r......or.......r......or...\nH|x.xox.x.x.xox.x.x.xox.x.x.xox.x.", 108},
        {"Afrobeat", "Ride Bell", 2, .5, false,
         "K|X..x..x...x.....X..x..x...x.....\nS|....r.......r.......r.......r...\nR|b..b..b.b..b..b.b..b..b.b..b..b.", 112},
        {"Dubstep", "Half Time", 1, .5, false,
         "K|X.........x.....\nS|........X.......\nH|x.x.x.x.x.x.x.O.", 140},
        {"Dubstep", "Shuffled", 2, .6, false,
         "K|X.....x......x..X.........x.....\nS|........X...............X....o..\nH|xox.x.O.xox.x.O.xox.x.O.xox.x.O.", 140},
        {"Rock", "Verse to Fill", 2, .5, false,
         "K|X.......x.x.....X.......x.......\nS|....X.......X.......X...xx......\nH|x.x.x.x.x.x.x.x.x.x.x.x.........\n1|..........................xx....\n2|............................xx..\n3|..............................xx", 122},
        {"Fills", "DnB Turnaround", 1, .5, false,
         "K|X.........x.....\nS|....X..o....XoXX\nH|x.x.x.x.x.x.....", 174},
        {"Fills", "Metal Descent", 1, .5, false,
         "K|x.x.x.x.x.x.x.xx\nS|XXXX............\n1|....XXXX........\n2|........XXXX....\n3|............XXXX", 170},
        // ---------------------------------------------------------------- Fills
        {"Fills", "Snare Roll", 1, 0.5, false,
         "K|x.......x.......\nS|....x...xxxxXXXX\nH|x.x.x.x.........\nC|................", 120},
        {"Fills", "Around the Kit", 1, 0.5, false,
         "K|x.......x.......\nS|....x...xx......\n1|..........xx....\n2|............xx..\n3|..............xx", 120},
        {"Fills", "Tom Build", 2, 0.5, false,
         "K|x.......x.......x.......x...x...\nS|....x.......x.......x...x.x.x.x.\n1|........................x.x.....\n3|............................x.x.\nC|................................", 120},
        {"Fills", "Flam Fill", 1, 0.5, false,
         "K|x.......x...x...\nS|....x...f.f.f.ff\nH|x.x.x.x.........", 120},
        {"Fills", "Crash Ending", 1, 0.5, false,
         "K|x...x...x...X...\nS|....x.......X...\nC|............x...\nH|x.x.x.x.x.x.....", 120},
    };
    return g;
}

int noteOf(char lane) {
    switch (lane) {
    case 'K': return 36; case 'S': return 38; case 'H': return 42; case 'R': return 51; case 'C': return 49;
    case '1': return 48; case '2': return 45; case '3': return 41; case 'P': return 39;
    default: return -1;
    }
}

uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
double unit(uint32_t h) { return (h & 0xffff) / 65535.0 * 2 - 1; }   // -1 .. 1

} // namespace

const std::vector<Groove>& all() { return library(); }

QStringList styles() {
    QStringList out;
    for (const Groove& g : library()) if (!out.contains(g.style)) out << g.style;
    return out;
}

QVector<int> inStyle(const QString& style) {
    QVector<int> out;
    const auto& lib = library();
    for (int i = 0; i < int(lib.size()); ++i) if (style == lib[size_t(i)].style) out << i;
    return out;
}

int find(const QString& style, const QString& name) {
    const auto& lib = library();
    for (int i = 0; i < int(lib.size()); ++i) if (style == lib[size_t(i)].style && name == lib[size_t(i)].name) return i;
    return -1;
}

QVector<Note> notes(const Groove& g, double humanVel, double humanTime, unsigned seed) {
    QVector<Note> out;
    const int steps = g.bars * 16;
    const double length = g.bars * 4.0;
    uint32_t h = hash32(seed * 2654435761u + 7);
    for (const QString& line : QString::fromLatin1(g.lanes).split('\n', Qt::SkipEmptyParts)) {
        if (line.size() < 3 || line[1] != '|') continue;
        const char lane = line[0].toLatin1();
        const int base = noteOf(lane);
        if (base < 0) continue;
        const QString pattern = line.mid(2);
        for (int i = 0; i < steps && i < pattern.size(); ++i) {
            const char c = pattern[i].toLatin1();
            if (c == '.') continue;
            int pitch = base;
            double vel = 0.8;
            bool flam = false;
            switch (c) {
            case 'x': vel = 0.8; break;
            case 'X': vel = 1.0; break;
            case 'o': vel = 0.42; break;
            case 'f': vel = 0.92; flam = true; break;
            case 'O': vel = 0.85; if (lane == 'H') pitch = 46; break;
            case 'p': vel = 0.6; if (lane == 'H') pitch = 44; break;
            case 'r': vel = 0.72; if (lane == 'S') pitch = 37; break;
            case 'R': vel = 1.0; if (lane == 'S') pitch = 40; break;
            case 'b': vel = 0.85; if (lane == 'R') pitch = 53; break;
            case 'c': vel = 0.9; if (lane == 'C') pitch = 52; break;
            case 's': vel = 0.8; if (lane == 'C') pitch = 55; break;
            default: continue;
            }
            double start = i * 0.25;
            const bool off = g.swing8 ? (i % 4 == 2) : (i % 2 == 1);   // the off-eighths, or the off-sixteenths
            if (off) start += (g.swing - 0.5) * 2 * (g.swing8 ? 0.5 : 0.25);
            if (humanVel > 0) { h = hash32(h); vel = std::clamp(vel + unit(h) * 0.15 * humanVel, 0.05, 1.0); }
            if (humanTime > 0) { h = hash32(h); start = std::max(0.0, start + unit(h) * (1.0 / 64) * humanTime); }
            if (start >= length) continue;
            if (flam) out.push_back({pitch, std::max(0.0, start - 1.0 / 32), 0.125, vel * 0.5, 1.0});
            out.push_back({pitch, start, 0.25, vel, 1.0});
        }
    }
    std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
    return out;
}

QByteArray midi(const Groove& g, double humanVel, double humanTime) {
    QByteArray track;
    auto vlq = [](quint32 value) { QByteArray b(1, char(value & 127)); while ((value >>= 7)) b.prepend(char((value & 127) | 128)); return b; };
    const auto title = QString("%1 - %2").arg(g.style, g.name).toUtf8();
    track += char(0); track += QByteArray::fromHex("ff03"); track += vlq(title.size()); track += title;
    const quint32 tempo = quint32(std::llround(60000000. / g.tempo));
    track += QByteArray::fromHex("00ff5103"); track += char(tempo >> 16); track += char(tempo >> 8); track += char(tempo);
    track += QByteArray::fromHex("00ff580404021808");
    struct Event { int tick, note, velocity; bool on; }; std::vector<Event> events;
    constexpr int ppqn = 960;
    for (const auto& n : notes(g, humanVel, humanTime)) {
        const int tick = int(std::llround(n.start * ppqn));
        events.push_back({tick, n.pitch, std::clamp(int(std::lround(n.velocity * 127)), 1, 127), true});
        events.push_back({std::min(g.bars * 4 * ppqn, tick + 96), n.pitch, 0, false});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick || (a.tick == b.tick && a.on < b.on); });
    int last = 0;
    for (const auto& e : events) { track += vlq(e.tick - last); track += char(e.on ? 0x99 : 0x89); track += char(e.note); track += char(e.velocity); last = e.tick; }
    track += vlq(std::max(0, g.bars * 4 * ppqn - last)); track += QByteArray::fromHex("ff2f00");
    QByteArray out = QByteArray::fromHex("4d546864000000060000000103c0"); out += "MTrk";
    const quint32 size = qToBigEndian(quint32(track.size())); out.append(reinterpret_cast<const char*>(&size), 4); out += track;
    return out;
}
bool exportMidi(const QString& path, const Groove& g, double humanVel, double humanTime, QString* error) {
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { if (error) *error = file.errorString(); return false; }
    const auto bytes = midi(g, humanVel, humanTime);
    if (file.write(bytes) != bytes.size() || !file.commit()) { if (error) *error = file.errorString(); return false; }
    return true;
}

QString describe(const Groove& g) {
    QString s = QString("%1 %2").arg(g.bars).arg(g.bars == 1 ? "bar" : "bars");
    if (g.swing > 0.51) s += g.swing8 ? " · shuffle" : " · swing";
    s += QString(" · %1 BPM").arg(int(g.tempo));
    return s;
}

QMimeData* mimeFor(int index, double humanVel, double humanTime) {
    auto* mime = new QMimeData;
    mime->setData(kMime, QString("%1|%2|%3").arg(index).arg(humanVel).arg(humanTime).toUtf8());
    if (index >= 0 && index < int(library().size())) mime->setText(QString("%1 %2").arg(library()[size_t(index)].style, library()[size_t(index)].name));
    return mime;
}

int indexIn(const QMimeData* mime, double* humanVel, double* humanTime) {
    if (!mime || !mime->hasFormat(kMime)) return -1;
    const QStringList parts = QString::fromUtf8(mime->data(kMime)).split('|');
    bool ok = false;
    const int i = parts.value(0).toInt(&ok);
    if (!ok || i < 0 || i >= int(library().size())) return -1;
    if (humanVel) *humanVel = parts.value(1).toDouble();
    if (humanTime) *humanTime = parts.value(2).toDouble();
    return i;
}

ClipPtr makeClip(Project& p, const Groove& g, double humanVel, double humanTime) {
    ClipPtr c = p.newClip(QString("%1 %2").arg(g.style, g.name), g.bars * 4.0);
    c->notes = notes(g, humanVel, humanTime, unsigned(c->id));
    return c;
}

namespace {
// A MIDI track for the clip: the one given when it is one, else a new one with a Drum Studio.
int drumTrackFor(Project& p, int track) {
    const Track* t = p.trackAt(track);
    if (t && !p.isReturn(track) && t->type == TrackType::Midi) return track;
    const int at = t && !p.isReturn(track) ? track + 1 : int(p.tracks.size());
    const int i = p.insertTrack(at, TrackType::Midi);
    p.tracks[i].name = "Drums";
    p.tracks[i].devices = {p.makeDevice("Drum Studio", DeviceKind::Instrument)};
    return i;
}
}

int toSession(Project& p, int track, int scene, const ClipPtr& clip) {
    if (!clip) return -1;
    const int t = drumTrackFor(p, track);
    scene = std::clamp(scene, 0, int(p.scenes.size()));
    while (scene >= p.scenes.size()) p.insertScene(int(p.scenes.size()));
    p.tracks[t].clipSlots[scene] = clip;
    emit p.structureChanged();
    return t;
}

int toArrangement(Project& p, int track, double beat, const ClipPtr& clip) {
    if (!clip) return -1;
    const int t = drumTrackFor(p, track);
    const double at = std::max(0.0, beat);
    arrangement::clearRange(p, p.tracks[t], at, at + clip->length);
    p.tracks[t].arrangement.push_back({at, clip->length, 0, clip});
    auto& arr = p.tracks[t].arrangement;
    std::sort(arr.begin(), arr.end(), [](const ArrangementClip& a, const ArrangementClip& b) { return a.start < b.start; });
    emit p.structureChanged();
    return t;
}

} // namespace sessions::grooves
