#include "MainWindow.h"
#include "ArrangementView.h"
#include "ClipView.h"
#include "InfoView.h"
#include "SessionView.h"
#include "audio/TrackRender.h"
#include "core/Model.h"
#include "core/Transport.h"
#include "widgets/DragValue.h"
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>

namespace sessions {
void MainWindow::captureMidi() {
    QString error;
    const bool arrangement = !sessionViewShown();
    auto clip = m_transport->captureMidi(m_project->selectedTrack, arrangement,
                                        m_session->grid()->selectedScene(), &error);
    if (!clip) { m_info->show("Capture MIDI", error); return; }
    selectClip(clip);
    showClipView(true);
    m_info->show("Captured MIDI", QString("%1 notes").arg(clip->notes.size()));
}

QDialog* MainWindow::bounceDialog() {
    const int track = m_project->selectedTrack;
    const Track* source = m_project->trackAt(track);
    if (!source || track >= m_project->tracks.size()) return nullptr;
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle("Bounce Track to Audio");
    dialog->setMinimumWidth(440);
    dialog->setObjectName("bounceTrackDialog");
    auto* form = new QFormLayout(dialog);
    form->addRow("Track", new QLabel(source->name, dialog));
    auto* range = new QComboBox(dialog);
    range->setObjectName("bounceRange");
    double start = 0, end = 0;
    if (source->isGroup()) {
        const auto span = m_project->groupArrangementSpan(track);
        start = span.start; end = span.end;
    } else {
        start = 1e18;
        for (const auto& a : source->arrangement) { start = std::min(start, a.start); end = std::max(end, a.start + a.length); }
        if (start > end) start = 0;
    }
    if (m_arrangement->hasTimeSelection()) range->addItem("Time selection", QVariantList{m_arrangement->selectionStart(), m_arrangement->selectionEnd()});
    range->addItem("Whole track", QVariantList{start, end});
    range->addItem("Loop region", QVariantList{m_transport->loopStart(), m_transport->loopStart() + m_transport->loopLength()});
    form->addRow("Range", range);
    auto* tail = new DragValue(dialog);
    tail->setObjectName("bounceTail");
    tail->setRange(0, 30); tail->setStep(1); tail->setDefault(2);
    tail->setValue(QSettings().value("render/bounceTail", 2).toDouble(), false);
    tail->setFormatter([](double v) { return QString("%1 s").arg(v, 0, 'f', 0); });
    tail->setAccentText(false);
    form->addRow("Effect tail", tail);
    auto* mute = new QCheckBox("Mute source and activate bounce", dialog);
    mute->setObjectName("bounceMuteSource");
    mute->setChecked(QSettings().value("render/bounceMuteSource", false).toBool());
    form->addRow(mute);
    auto* help = new QLabel("32-bit float, after track effects, fader and pan. Parent group and Master effects apply during playback. Return effects are separate. The source stays editable; undo restores both tracks.", dialog);
    help->setWordWrap(true);
    help->setMaximumWidth(410);
    form->addRow(help);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Bounce");
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    form->addRow(buttons);
    dialog->setProperty("sourceTrackId", source->id);
    return dialog;
}

void MainWindow::bounceSelectedTrack() {
    if (m_transport->recordingInProgress()) { m_info->show("Bounce Track", "Stop recording before bouncing."); return; }
    std::unique_ptr<QDialog> dialog(bounceDialog());
    if (!dialog) { m_info->show("Bounce Track", "Select an Arrangement track or group."); return; }
    if (dialog->exec() != QDialog::Accepted) return;
    const auto range = dialog->findChild<QComboBox*>("bounceRange")->currentData().toList();
    const double tail = dialog->findChild<DragValue*>("bounceTail")->value();
    const bool mute = dialog->findChild<QCheckBox*>("bounceMuteSource")->isChecked();
    audio::RenderOptions options;
    options.startBeat = range.value(0).toDouble();
    options.lengthBeats = range.value(1).toDouble() - options.startBeat;
    options.tailSeconds = tail;
    const int sourceId = dialog->property("sourceTrackId").toInt();
    const int source = m_project->indexOfTrack(sourceId);
    if (source < 0) return;
    const QString path = m_transport->recordingPath(m_project->tracks[source].name + " Bounce");
    QProgressDialog progress("Bouncing…", "Cancel", 0, 1000, this);
    progress.setWindowTitle("Bounce Track to Audio");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QString error;
    const int inserted = audio::bounceTrack(*m_project, sourceId, path, options, mute, &error, [&](double f) {
        progress.setValue(int(f * 1000)); QCoreApplication::processEvents(); return !progress.wasCanceled();
    });
    progress.reset();
    if (inserted < 0) {
        if (error != "Cancelled.") QMessageBox::warning(this, "Bounce Track", error);
        return;
    }
    QSettings().setValue("render/bounceTail", tail);
    QSettings().setValue("render/bounceMuteSource", mute);
    selectTrack(inserted);
    showSession(false);
    selectClip(m_project->tracks[inserted].arrangement.front().clip);
    showClipView(true);
    m_info->show("Bounced to audio", mute ? "Source muted. Ctrl+Z restores both tracks." : "Bounce starts muted. Activate it to audition.");
}
} // namespace sessions
