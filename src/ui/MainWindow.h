#pragma once
// MainWindow: menu bar and control bar on top; browser | (Session or
// Arrangement) in the middle; Info View | (Clip View or Device View) at the
// bottom. Owns the global shortcuts (Tab, Shift+Tab, F11, Space, B, Delete,
// Ctrl+D, Ctrl+Z, Ctrl+1/2…) and the current set's file (File menu, title).

#include "core/SetFile.h"
#include <QMainWindow>
#include <QVector>
#include "ArrangementView.h"
#include <QHash>
#include <QSet>
#include <QList>
#include <memory>
#include <functional>

class QStackedWidget;
class QMenu;
class QLabel;
class QToolButton;
class QDialog;
class QSplitter;
class QTimer;

namespace sessions {

class Project;
class Transport;
namespace audio { class Engine; struct RenderOptions; class MidiInput; }
class MapOverlay;
class ControlBar;
class Browser;
class SessionView;
class ArrangementView;
class ClipView;
class DeviceView;
class InfoView;
class ThemeEditor;
class OverviewBar;
class ControlSurface;
class ApcKey25;
class RecoveryManager;
struct ArrangementClip;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Dev helper: `sessions --screenshots DIR` grabs each view and exits.
    void captureScreenshots(const QString& dir);

    QDialog* aboutDialog();   // Help > About: the logo in the theme's ink, the words, the versions (the caller shows it)
    bool sessionViewShown() const;
    bool clipViewShown() const;
    Project*         project() const { return m_project; }
    Transport*       transport() const { return m_transport; }
    ArrangementView* arrangementView() const { return m_arrangement; }
    SessionView*     sessionView() const { return m_session; }
    DeviceView*      deviceView() const { return m_deviceView; }
    bool             midiKeyboard() const { return m_midiKeys; }
    audio::MidiInput* midiInput() const { return m_midi; }
    audio::Engine*   engine() const { return m_engine; }
    Mapper*          mapper() const { return m_mapper; }
    ControlSurface*  controlSurface() const { return m_surface; }
    ApcKey25*        apcKey25() const { return m_apc; }
    Browser*         browser() const { return m_browser; }
    ClipView*        clipView() const { return m_clipView; }
    MapOverlay*      mapOverlay() const { return m_overlay; }
    // MIDI Map / Key Map mode (Ctrl+M / Ctrl+K; Esc leaves): clicks select a control instead of using it.
    void             setMapMode(Mapper::Mode mode);

    // ---- the set. These never ask about unsaved changes and show no dialogs;
    // the File menu asks first (see maybeSave) and reports errors.
    void    newSet();
    bool    openSet(const QString& path, QString* error = nullptr);
    bool    saveSet(const QString& path, QString* error = nullptr);
    // After a View > Zoom restart: open the snapshot as the set it was taken from
    // (its path, name and unsaved state), then delete the snapshot.
    bool    reopenSnapshot(const QString& snapshot, const QString& path, const QString& name, bool modified, QString* error = nullptr);
    QString setPath() const { return m_setPath; }   // empty until the set is saved
    QString setName() const { return m_setName; }
    bool    setModified() const;                    // unsaved changes (the title shows a *)
    void    enableRecovery(const QString& root = {});
    void    saveRecoveryNow();
    bool    offerRecovery(bool startup = false);
    bool    recoverSet(const QString& file, QString* error = nullptr);
    RecoveryManager* recoveryManager() const { return m_recovery; }
    // File > Open Recent, a path on the command line, a double-clicked .sessions file:
    // asks about unsaved changes, then opens and reports failures.
    void    fileOpenPath(const QString& path);
    // Window size, view and panels as you left them (QSettings "window/..."), and what to
    // open when nothing is given on the command line (Preferences > Startup).
    void    restoreWindowState();
    void    saveWindowState() const;
    void    openStartupSet();

    // ---- samples and audio files (no dialogs; the menus and drops report problems)
    // Browser double-click: a clip on the selected audio track, or into a MIDI track's Sampler.
    void    loadSample(const QString& path);
    // One undo step. A Drum Rack on the track (or `deviceId` itself) takes the sample on its selected
    // pad; otherwise the track's Sampler does (a new one replaces the instrument). `where` says which.
    bool    loadIntoSampler(int track, int deviceId, const QString& path, QString* error = nullptr, QString* where = nullptr);
    QStringList missingSamples() const;   // files the set refers to that can't be found
    double  songLength() const;           // beats: the end of the last clip on the timeline
    // File > Export Audio…: renders the Arrangement offline into a WAV.
    bool    exportAudio(const QString& path, const audio::RenderOptions& options, QString* error = nullptr,
                        const std::function<bool(double)>& progress = {});
    // File > Collect All and Save: copies the samples that live outside the set's folder into
    // <set folder>/Samples/Imported and points the set at the copies (one undo step).
    // Returns how many files were copied, or -1 (and why).
    int     collectSamples(QString* error = nullptr);

protected:
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;   // "Save changes?" on quit
    void dragEnterEvent(QDragEnterEvent*) override;   // a .sessions file dropped on the window opens it
    void dropEvent(QDropEvent*) override;
    // Tab / Shift+Tab are caught app-wide before Qt's focus chain eats them.
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    // File menu
    void fileNew();
    void fileOpen();
    bool fileSave();
    bool fileSaveAs();
    bool maybeSave();   // false when you cancel
    void fileExportAudio();
    bool fileCollectAllAndSave();
    void showPreferences();
    void showShortcuts();
    bool loadSetFile(const QString& file, QString* error);   // replaces the set; nothing changes on failure
    void applyExtras(const setfile::Extras& x);
    void setReplaced(const QString& path, const QString& name, bool keepRecovery = false);
    setfile::Extras currentExtras() const;
    void updateTitle();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    QString setDialogDir() const;
    void restartReopeningSet();   // View > Zoom
    void showSession(bool session);
    void showClipView(bool clip);
    void toggleFullScreen();
    void rebuildThemeMenu();   // View > Theme: the built-in themes, yours, Customize…
    void showThemeEditor();
    void buildMenus();
    // Edit commands that act on whichever view has focus.
    void copySelection();
    void pasteSelection();
    void duplicateSelection();
    void renameSelection();
    void insertTrack(TrackType type);
    void loadSampleNow(const QString& path);   // loadSample's second half, once the file is in memory
    void groupSelectedTracks();
    void ungroupSelectedTrack();
    // MIDI from keyboards and controllers: mappings first, else notes for the armed tracks.
    void midiNoteOn(int channel, int pitch, int velocity);
    void midiNoteOff(int channel, int pitch);
    void midiControl(int channel, int controller, int value);
    void midiPitchBend(int channel, int value);      // to the tracks the keyboard plays
    void midiPressure(int channel, int value);
    void releaseMidiNote(int key);
    void showHeldKeys();                             // the piano roll lights the keys held down
    // Computer MIDI keyboard.
    void setMidiKeyboard(bool on);
    bool midiKey(QKeyEvent* k, bool press);
    void releaseMidiKeys();
    QList<int> keyboardTargets() const;
    void updateKeyboardInfo();
    void selectTrack(int track);
    void selectClip(std::shared_ptr<struct Clip> clip);
    void updateDetailTitle();
    enum class Area { Session, Arrangement, Roll, Sample, Devices, Other };
    Area focusedArea() const;
    void runCommand(const QString& command);   // a view's context menu asked for a window-wide command (0.22)
    void saveClipToLibrary();                  // the selected clip into the User Library (asks for a name)
    void loadLibraryClip(const QString& path); // a browser clip onto the selected track (the view that shows)

    Project*        m_project;
    audio::Engine*  m_engine;
    Transport*      m_transport;
    ControlBar*     m_controlBar;
    Browser*        m_browser;
    QStackedWidget* m_mainStack;
    SessionView*    m_session;
    ArrangementView* m_arrangement;
    QStackedWidget* m_detailStack;
    ClipView*       m_clipView;
    DeviceView*     m_deviceView;
    InfoView*       m_info = nullptr;
    OverviewBar*    m_overview = nullptr;   // the bottom strip: the clip and the device chain in miniature
    QLabel*         m_detailTitle;
    QToolButton*    m_clipTab;               // the Clip / Device tabs, on their side between the Info View and the detail (0.21)
    QToolButton*    m_deviceTab;
    class ClipOverview* m_headerClip = nullptr;   // the clip in miniature over the Clip View: drag to scroll / zoom its editor
    void            cutSelection();
    QVector<ArrangementView::ClipPiece> m_pieces;   // Ctrl+C / Ctrl+X of a time selection in the Arrangement: its material, by lane
    QAction*        m_autoAction = nullptr;
    QAction*        m_keysAction = nullptr;
    QAction*        m_browserAction = nullptr;
    QSplitter*      m_vsplit = nullptr;    // main views over the detail pane
    QSplitter*      m_hsplit = nullptr;    // the browser beside the main views (drag the divider for a wider browser)
    QWidget*        m_bottom = nullptr;    // the detail pane: Info View, and the Clip View or the Device View
    int             m_clipPaneHeight = 0;  // the Clip View's pane height (the Device View's is fixed: just what the devices need)
    // The detail pane's height (0.37): the Device View's is fixed; the Clip View's is at least what its boxes need
    // (clipPaneMinHeight) and otherwise the height you dragged it to, applied when the pane switches to it
    // (`resize`), never while it already shows (selecting another clip leaves your drag alone).
    void            fitDetailPane(bool clip, bool resize = true);
    int             clipPaneMinHeight() const;
    int             devicePaneHeight() const;
    std::shared_ptr<struct Clip> m_clipboard;
    bool            m_midiKeys = false;
    int             m_octave = 3;           // C3 = MIDI 60
    double          m_velocity = 100 / 127.0;
    struct HeldKey { int pitch; QList<int> tracks; };
    QHash<int, HeldKey> m_heldKeys;         // Qt key -> the note it's holding

    // MIDI input and mappings
    audio::MidiInput* m_midi = nullptr;
    ControlSurface* m_surface = nullptr;
    ApcKey25*       m_apc = nullptr;
    bool            m_apcLightsFailed = false;   // the Info View said why the pads stay dark
    void            updateApc();      // ports and profile (MIDI ports came or went, Preferences)
    void            updateApcBox();   // the Session View's outline of the pads
    Mapper*         m_mapper = nullptr;
    MapOverlay*     m_overlay = nullptr;
    QAction*        m_keyMapAction = nullptr;
    QAction*        m_midiMapAction = nullptr;
    QHash<int, QList<int>> m_midiHeld;      // (channel << 8 | pitch) -> the tracks it plays
    QSet<int>       m_sustained;            // released while the sustain pedal was down
    bool            m_sustain = false;

    // the set's file
    QString         m_setPath;
    int             m_previewRequest = 0;   // the browser preview: only the latest request plays
    QString         m_setName = "Demo";
    setfile::Extras m_savedExtras;          // loop, quantization, cue... as last saved or opened
    bool            m_forcedModified = false;   // reopened after a restart with unsaved changes
    bool            m_closeConfirmed = false;   // quitting was confirmed (or needs no asking)
    bool            m_saveInProgress = false;
    RecoveryManager* m_recovery = nullptr;
    QTimer*          m_recoveryTimer = nullptr;
    QAction*         m_backupsAction = nullptr;
    QMenu*          m_recentMenu = nullptr;
    QMenu*          m_themeMenu = nullptr;
    ThemeEditor*    m_themeEditor = nullptr;
};

} // namespace sessions
