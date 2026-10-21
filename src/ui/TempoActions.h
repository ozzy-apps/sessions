#pragma once

#include "core/Model.h"
#include <QMenu>
#include <algorithm>
#include <cmath>
#include <functional>

namespace sessions {

inline QString sourceTempoRange(const Clip& clip) {
    if (clip.detectedBpm <= 0) return {};
    double low = clip.sampleBpm(), high = low;
    for (int i = 1; i < clip.warpMarkers.size(); ++i) {
        const auto& a = clip.warpMarkers[i - 1];
        const auto& b = clip.warpMarkers[i];
        if (b.seconds <= a.seconds || b.beat <= a.beat) continue;
        const double bpm = 60 * (b.beat - a.beat) / (b.seconds - a.seconds);
        if (!std::isfinite(bpm)) continue;
        low = std::min(low, bpm);
        high = std::max(high, bpm);
    }
    return high - low > std::max(0.5, 0.005 * clip.sampleBpm())
        ? QString("%1–%2").arg(low, 0, 'f', 1).arg(high, 0, 'f', 1) : QString();
}

// Shared by the source-tempo field and waveform menu. These are explicit repairs:
// loading a set must never replace its saved timing or a user's marker edits.
inline void addSourceTempoActions(QMenu& menu, Project* project, const ClipPtr& clip,
                                  const std::function<void()>& changed = {}) {
    const bool editable = clip && clip->sample && clip->takes.isEmpty();
    const bool stems = editable && project->matchingStemClips(clip).size() > 1;
    auto add = [&](const QString& text, const char* name, bool enabled, const std::function<bool()>& run) {
        QAction* action = menu.addAction(text, &menu, [run, changed] { if (run() && changed) changed(); });
        action->setObjectName(name);
        action->setEnabled(enabled);
    };
    add("Re-detect Tempo", "redetectSourceTempo", editable,
        [project, clip] { return project->redetectClipTempo(clip); });
    add("Re-detect Matching Stems", "redetectStemTempo", stems,
        [project, clip] { return project->redetectClipTempo(clip, true); });
    add(stems ? "Use Project Tempo for Matching Stems" : "Use Project Tempo", "useProjectSourceTempo", editable,
        [project, clip, stems] { return project->setClipSourceBpm(clip, project->tempo, stems); });
    add("Apply Source Tempo to Matching Stems", "applySourceTempoToStems", stems && clip->detectedBpm > 0,
        [project, clip] { return project->setClipSourceBpm(clip, clip->sampleBpm(), true); });
}

} // namespace sessions
