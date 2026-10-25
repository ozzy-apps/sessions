# Third-party notices

Sessions is built with Qt 6 (Core, Gui, Widgets, Svg and Network; Multimedia on Windows), dynamically linked under LGPL terms. Qt source, license texts and relinking information are available at https://www.qt.io/licensing/open-source-lgpl-obligations and https://code.qt.io/. Binary packages must retain the shipped Qt license texts and shared libraries; users may replace compatible Qt libraries.

You may replace or modify the LGPL libraries and reverse engineer Sessions to debug modifications to those libraries, as required by the applicable LGPL. Sessions' original source/artwork remain All rights reserved. The bundled `licenses` directory includes LGPL/GPL reference texts; sample and runtime-specific notices remain with their respective files.

Linux audio uses PulseAudio/PipeWire and optional JACK/ALSA. Portable packages include libsndfile and codec dependencies under their respective LGPL/BSD licenses. Windows audio uses Qt Multimedia's Windows backend; MIDI uses Windows Multimedia services. Runtime dependencies remain separate shared libraries.

The built-in AVL Drumkits recordings are by Glen MacArthur; attribution and licenses are in `share/sessions/drums`. The optional six-library drum pack carries its own exact provenance and CC0/CC BY 4.0 notices in `drum-content/licenses`. Those recordings keep their original licenses independently of Sessions' software license. No Bitwig or Nektar integration code is included.

See the corresponding installed library's license/source package for the exact Qt, PulseAudio, ALSA, libsndfile and codec versions in a build. Release workflows retain license directories from the runtime packages. This file accompanies, and does not replace, those license texts.
