#pragma once
// ClipView: clip settings on the left in narrow boxes side by side,
// the editor on the right (the PianoRoll for MIDI clips, the SampleEditor for audio clips).
// The round L / ♪ / E switches at the bottom left show or hide the Launch, Notes (or Sample)
// and Envelopes boxes (remembered in QSettings clipView/launch, notes, envelopes); the Clip box always shows.
//  - Clip: name, signature, start (the start marker), position (the loop's), length, loop, grid, Duplicate Loop, Clear
//  - Launch: launch mode, legato, quantization, follow actions
//  - Notes (MIDI): transpose, :2 / x2, reverse, invert, legato, scale + Fold
//  - Sample (audio): file, Warp on/off, warp mode and its slices, original tempo (:2 / x2),
//    gain, transpose, reverse; Takes swaps the editor for the TakesEditor (loop-recorded clips)
//  - Envelopes: pick a parameter (Mixer or a device, then the parameter); Show swaps the
//    editor for the EnvelopeEditor, where the clip's envelope for it is drawn
// Note tools act on the selected notes, or the whole clip when none are selected.

#include <QWidget>
#include <memory>
#include <functional>

class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QCheckBox;
class QStackedWidget;
class QToolButton;

namespace sessions {

class Project;
class Transport;
class PianoRoll;
class SampleEditor;
class TakesEditor;
class EnvelopeEditor;
class DragValue;
class ClipOverview;
struct Clip;
struct Note;
using ClipPtr = std::shared_ptr<Clip>;

class ClipView : public QWidget {
    Q_OBJECT
public:
    ClipView(Project* project, Transport* transport, QWidget* parent = nullptr);
    void setClip(ClipPtr clip);
    PianoRoll* pianoRoll() const { return m_roll; }
    SampleEditor* sampleEditor() const { return m_sampleEditor; }
    TakesEditor*  takesEditor() const { return m_takesEditor; }
    EnvelopeEditor* envelopeEditor() const { return m_envEditor; }
    void showEnvelope(bool on);   // the EnvelopeEditor instead of the note / sample editor
    void showTakes(bool on);      // the Takes view (audio clips with takes) instead of the sample editor
    // The editor's viewport in clip beats (the piano roll's or the sample editor's), for the clip
    // overviews to outline and drag; false while another editor (envelopes, takes) shows.
    bool editorViewport(double& first, double& last) const;
    void setEditorViewport(double first, double last);
    ClipOverview* overviewStrip() const { return m_strip; }   // the clip in miniature over the editor (0.21): drag to scroll / zoom, click to centre

signals:
    void commandRequested(const QString& command);   // the editors' menus and the Save button (0.22): "saveClip", "cut", "copy", "paste", "showSample"

private:
    void refresh();
    QString gridLabel() const;
    QWidget* buildClipTab();
    QWidget* buildLaunchTab();
    QWidget* buildNotesTab();
    QWidget* buildSampleTab();
    QWidget* buildEnvelopeTab();
    void refreshEnvelopeChoosers();
    void setSampleBpm(double bpm);   // rescales the warp markers (and the length) around the first marker
    bool audio() const;
    void editNotes(const QString& label, const std::function<void(QVector<Note>& notes, const QList<int>& which)>& fn);
    void editClip(const QString& label, int mergeWhat, const std::function<void(Clip&)>& fn);

    Project*   m_project;
    ClipPtr    m_clip;
    PianoRoll* m_roll;
    SampleEditor* m_sampleEditor;
    TakesEditor*  m_takesEditor;
    EnvelopeEditor* m_envEditor;
    QStackedWidget* m_editors;
    ClipOverview* m_strip = nullptr;
    QWidget*   m_props;
    QWidget*   m_launchBox = nullptr;
    QWidget*   m_notesBox = nullptr;   // shown for MIDI clips
    QWidget*   m_sampleBox = nullptr;  // shown for audio clips
    QWidget*   m_envBox = nullptr;
    QToolButton* m_launchToggle = nullptr;   // round switches: which boxes show (QSettings clipView/*)
    QToolButton* m_notesToggle = nullptr;
    QToolButton* m_envToggle = nullptr;
    void       showBoxes();
    bool       m_refreshing = false;

    // Clip tab
    QLineEdit* m_name;
    DragValue* m_length;
    DragValue* m_start;      // Clip::start (0.23)
    DragValue* m_position;   // Clip::loopStart
    QPushButton* m_loop;
    QPushButton* m_grid;
    QPushButton* m_dupLoop;
    QPushButton* m_clear;
    QPushButton* m_save = nullptr;   // Save Clip to User Library (0.22)
    // Launch tab
    QComboBox* m_mode;
    QCheckBox* m_legato;
    QComboBox* m_quant;
    QCheckBox* m_follow;
    QComboBox* m_actionA;
    QComboBox* m_actionB;
    DragValue* m_chance;
    DragValue* m_followTime;
    // Notes tab
    QPushButton* m_scaleOn;
    QComboBox* m_root;
    QComboBox* m_scale;
    QPushButton* m_fold;
    // Sample tab
    QLabel*    m_file;
    QComboBox* m_warp;
    QPushButton* m_warpOn;
    QComboBox* m_slices;
    QComboBox* m_beatsTail = nullptr;
    QLabel*    m_beatsTailLabel = nullptr;
    QLabel*    m_warpExtraLabel = nullptr;   // the row under Warp: what the mode offers
    DragValue* m_grainSize = nullptr;        // Tones / Texture
    DragValue* m_flux = nullptr;             // Texture
    DragValue* m_formants = nullptr;         // Complex Pro
    DragValue* m_bpm;
    QLabel*    m_tempoInfo = nullptr;   // the tempo found in the audio and how the clip sits against the set's
    DragValue* m_gain;
    DragValue* m_transpose;
    QPushButton* m_reverse;
    QPushButton* m_takesShow;
    // Envelopes tab
    QComboBox* m_envDevice;   // Mixer, then the track's devices
    QComboBox* m_envParam;    // that one's parameters (data: the target)
    QPushButton* m_envShow;
    QPushButton* m_envClear;
    QLabel*    m_envList;
};

} // namespace sessions
