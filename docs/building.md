# Building Sessions

Sessions uses C++17, CMake and Qt 6 Widgets/Svg/Network. Windows additionally uses Qt Multimedia. The application runs without plugin hosting; instruments and effects are built in.

## Linux

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-svg-dev libpulse-dev libasound2-dev libsndfile1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/sessions
QT_QPA_PLATFORM=offscreen SESSIONS_AUDIO=none ctest --test-dir build --parallel 4 --output-on-failure
```

Some distributions call the SVG development package `libqt6svg6-dev`. PulseAudio/PipeWire provides desktop audio; JACK is loaded at runtime for low-latency duplex routing. ALSA provides MIDI. `libsndfile1` adds FLAC and other supported codecs. AppImage builds from Ubuntu 24.04 require an equally recent glibc; use Flatpak on older distributions.

Every normal Linux build also creates a timestamped `.deb` in `dist`. Set `-DSESSIONS_BUILD_DEB=OFF` to disable that. `-DSESSIONS_BUILD_TESTS=OFF` builds only the application. Install a package with `sudo apt install ./dist/sessions_*.deb`.

## Windows

Use an MSYS2 **UCRT64** shell with GCC, CMake/Ninja, `qt6-base`, `qt6-svg`, `qt6-multimedia` and `libsndfile` from the same UCRT64 repository. The exact packages and commands are in [.github/workflows/build.yml](../.github/workflows/build.yml).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSESSIONS_BUILD_TESTS=OFF -DSESSIONS_BUILD_DEB=OFF
cmake --build build --parallel 4
bash scripts/package-windows.sh
```

The ZIP contains Sessions, shared Qt libraries, platform/TLS plugins and libsndfile/codecs. Windows uses Qt's shared audio backend and WinMM MIDI, with input selection in Preferences. Allow microphone access in Windows for recording. ASIO, WASAPI exclusive mode and Linux JACK port routing are not provided by this initial Windows backend. Nektar/APC integrations are established on Linux; Windows controller-port names and hardware behaviour need physical testing.

For development, `SESSIONS_AUDIO=none` runs a silent clock, `SESSIONS_NO_UPDATES=1` suppresses the startup request, and `sessions --screenshots DIR` captures the actual UI. These do not change the normal user's preferences.
