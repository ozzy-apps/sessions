// Sessions — a clip-based music sequencer. See handoff.md.
// `sessions [set.sessions]` opens a set; --help lists the dev flags.
#include "ui/MainWindow.h"
#include "ui/AppIcon.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "audio/Engine.h"
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QFont>
#include <QTimer>
#include <QSettings>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// `sessions --render out.wav [seconds]` plays the demo arrangement offline into a WAV and exits.
static int renderDemo(int argc, char** argv, int at) {
    QCoreApplication app(argc, argv);
    const QString path = at + 1 < argc ? QString::fromLocal8Bit(argv[at + 1]) : QString("sessions-demo.wav");
    const double seconds = at + 2 < argc ? std::atof(argv[at + 2]) : 22.0;
    sessions::Project project;
    project.loadDemo();
    sessions::audio::Engine engine(&project);
    if (!engine.renderToWav(path, seconds)) {
        std::fprintf(stderr, "Couldn't write %s\n", qPrintable(path));
        return 1;
    }
    std::printf("Wrote %s (%.1f s)\n", qPrintable(path), seconds);
    return 0;
}

// One Sessions at a time: a second start (say, a double-clicked set) hands its file to the
// window that's already open and exits. The protocol is one line, "open\t<path>" or "show",
// answered with "ok".
static QString instanceName() { return "sessions-" + QString::fromLocal8Bit(qgetenv("USER")); }

static bool handToRunningInstance(const QString& path) {
    QLocalSocket socket;
    socket.connectToServer(instanceName());
    if (!socket.waitForConnected(500)) return false;
    socket.write(path.isEmpty() ? QByteArray("show\n") : "open\t" + QFileInfo(path).absoluteFilePath().toUtf8() + "\n");
    socket.waitForBytesWritten(1000);
    return socket.waitForReadyRead(2000) && socket.readLine().trimmed() == "ok";
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--render")) return renderDemo(argc, argv, i);

    // UI zoom (View > Zoom, Preferences > Look) has to be in place before QApplication exists. A
    // QT_SCALE_FACTOR you set yourself wins; one Sessions set (marked, so a restarted Sessions
    // inheriting it still applies the new zoom: before 0.26 a zoom change showed only after a second start).
    const double zoom = QSettings("Sessions", "Sessions").value("ui/zoom", 1.0).toDouble();
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR") || qEnvironmentVariableIsSet("SESSIONS_SCALE_FROM_SETTINGS")) {
        if (zoom > 0 && zoom != 1.0) { qputenv("QT_SCALE_FACTOR", QByteArray::number(zoom)); qputenv("SESSIONS_SCALE_FROM_SETTINGS", "1"); }
        else { qunsetenv("QT_SCALE_FACTOR"); qunsetenv("SESSIONS_SCALE_FROM_SETTINGS"); }
    }

    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);   // the .qrc lives in the static sessions_core library
    app.setApplicationName("Sessions");
    app.setOrganizationName("Sessions");
    app.setApplicationVersion(SESSIONS_VERSION);
    app.setDesktopFileName("sessions");
    sessions::appicon::applyWindowIcon();   // Preferences > Look > App icon
    QFont f = app.font();
    f.setPointSizeF(8.5);
    app.setFont(f);
    sessions::theme::installAppStyle();                      // dialog buttons without icons, flat message-box glyphs
    sessions::theme::apply(sessions::theme::savedKind());   // View > Theme; also applies the stylesheet

    QCommandLineParser cli;
    cli.setApplicationDescription("A clip-based music sequencer.\n\n"
                                  "sessions --render OUT.wav [SECONDS] renders the demo arrangement offline.");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument("set", "A .sessions set to open.", "[set]");
    const QCommandLineOption sizeOpt("size", "Window size, e.g. 2560x1440 (for screenshots).", "WxH");
    const QCommandLineOption shotsOpt("screenshots", "Capture each view into DIR, then quit.", "DIR");
    // View > Zoom restarts with these: a snapshot of the set, and what it was.
    QCommandLineOption reopenOpt("reopen", "Snapshot of the set to reopen after a restart (deleted once read).", "file");
    QCommandLineOption reopenPathOpt("reopen-path", "Where the reopened set is saved.", "path");
    QCommandLineOption reopenNameOpt("reopen-name", "The reopened set's name.", "name");
    QCommandLineOption reopenModOpt("reopen-modified", "The reopened set has unsaved changes.");
    for (QCommandLineOption* o : {&reopenOpt, &reopenPathOpt, &reopenNameOpt, &reopenModOpt})
        o->setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOptions({sizeOpt, shotsOpt, reopenOpt, reopenPathOpt, reopenNameOpt, reopenModOpt});
    cli.process(app);

    // A restart (--reopen) and screenshot runs never hand over: the old window is on its way out.
    const bool standalone = cli.isSet(shotsOpt) || cli.isSet(reopenOpt) || qEnvironmentVariableIsSet("SESSIONS_NEW_INSTANCE");
    if (!standalone && handToRunningInstance(cli.positionalArguments().value(0))) return 0;

    sessions::MainWindow w;
    const QStringList wh = cli.value(sizeOpt).split('x');
    if (wh.size() == 2) w.resize(wh[0].toInt(), wh[1].toInt());
    else if (!cli.isSet(shotsOpt)) w.restoreWindowState();
    w.show();

    QLocalServer server;
    if (!cli.isSet(shotsOpt)) {
        QLocalServer::removeServer(instanceName());   // left behind by a crash, or by the window a restart replaces
        server.listen(instanceName());
        QObject::connect(&server, &QLocalServer::newConnection, &w, [&server, &w] {
            while (QLocalSocket* c = server.nextPendingConnection()) {
                QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
                QObject::connect(c, &QLocalSocket::readyRead, c, [c, &w] {
                    while (c->canReadLine()) {
                        const QString line = QString::fromUtf8(c->readLine()).trimmed();
                        c->write("ok\n");
                        w.setWindowState(w.windowState() & ~Qt::WindowMinimized);
                        w.raise();
                        w.activateWindow();
                        if (line.startsWith("open\t")) {
                            const QString path = line.mid(5);
                            QTimer::singleShot(0, &w, [&w, path] { w.fileOpenPath(path); });
                        }
                    }
                });
            }
        });
    }

    if (cli.isSet(reopenOpt)) {
        QString error;
        if (!w.reopenSnapshot(cli.value(reopenOpt), cli.value(reopenPathOpt), cli.value(reopenNameOpt), cli.isSet(reopenModOpt), &error))
            QMessageBox::warning(&w, "Sessions", "Couldn't reopen the set after restarting.\n\n" + error);
    } else if (!cli.positionalArguments().isEmpty()) {
        w.fileOpenPath(cli.positionalArguments().first());   // also a double-clicked .sessions file (Exec=sessions %f)
    } else if (!cli.isSet(shotsOpt)) {
        w.openStartupSet();   // Preferences > Startup: the demo (already there), an empty set or the last one
    }

    if (cli.isSet(shotsOpt)) {
        const QString dir = cli.value(shotsOpt);
        QTimer::singleShot(300, &w, [&w, dir] { w.captureScreenshots(dir); QApplication::quit(); });
    }
    return app.exec();
}
