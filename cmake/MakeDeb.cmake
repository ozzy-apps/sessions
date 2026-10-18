# Builds dist/sessions_<version>+<timestamp>_<arch>.deb from an existing build.
# Run by the `deb` target after every build that changes the app:
#   cmake -DBUILD_DIR=... -DOUT_DIR=... -DVERSION=... -DMAINTAINER=... -DSTAMP=... -P MakeDeb.cmake
# The timestamp makes each package newer than the last, so installing it
# always upgrades. Only the newest .deb is kept in dist/.

cmake_minimum_required(VERSION 3.16)

execute_process(COMMAND dpkg --print-architecture OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP stamp "%Y%m%d%H%M%S" UTC)
set(full_version "${VERSION}+${stamp}")
set(stage "${BUILD_DIR}/deb-stage")

# 1. Install into a staging root.
file(REMOVE_RECURSE "${stage}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env DESTDIR=${stage} ${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix /usr --strip
    OUTPUT_QUIET RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
    message(FATAL_ERROR "MakeDeb: install step failed")
endif()
execute_process(COMMAND chmod -R go-w,go+rX "${stage}")   # 0755 dirs / binaries, 0644 files, whatever the umask

# Double-clicking a .sessions file needs both the MIME type and a desktop entry that
# claims it. No maintainer scripts: dpkg's triggers for /usr/share/mime/packages,
# /usr/share/applications and the hicolor icons refresh the caches on install.
foreach(f usr/share/mime/packages/sessions.xml usr/share/applications/sessions.desktop
          usr/share/icons/hicolor/scalable/mimetypes/application-x-sessions-set.svg)
    if(NOT EXISTS "${stage}/${f}")
        message(FATAL_ERROR "MakeDeb: /${f} is missing from the package")
    endif()
endforeach()
file(STRINGS "${stage}/usr/share/applications/sessions.desktop" mime_line REGEX "^MimeType=.*application/x-sessions-set;")
if(NOT mime_line)
    message(FATAL_ERROR "MakeDeb: sessions.desktop doesn't claim application/x-sessions-set")
endif()
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
    execute_process(COMMAND ${DESKTOP_FILE_VALIDATE} "${stage}/usr/share/applications/sessions.desktop"
                    OUTPUT_VARIABLE dfv ERROR_VARIABLE dfv RESULT_VARIABLE rv)
    if(NOT rv EQUAL 0)
        message(WARNING "MakeDeb: desktop-file-validate: ${dfv}")
    endif()
endif()

# 2. Library dependencies, the way debhelper finds them. dpkg-shlibdeps wants a
#    debian/control next to it, so give it a throwaway one.
set(shl "${BUILD_DIR}/deb-shlibs")
file(REMOVE_RECURSE "${shl}")
file(MAKE_DIRECTORY "${shl}/debian")
file(WRITE "${shl}/debian/control" "Source: sessions\n\nPackage: sessions\nArchitecture: any\n")
execute_process(
    COMMAND dpkg-shlibdeps -O "${stage}/usr/bin/sessions"
    WORKING_DIRECTORY "${shl}"
    OUTPUT_VARIABLE shlibs ERROR_VARIABLE shlibs_err RESULT_VARIABLE rv
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(rv EQUAL 0 AND shlibs MATCHES "shlibs:Depends=(.*)")
    set(depends "${CMAKE_MATCH_1}")
else()
    message(WARNING "MakeDeb: dpkg-shlibdeps failed (${shlibs_err}); using a fixed dependency list")
    set(depends "libqt6widgets6 (>= 6.4), libqt6svg6 (>= 6.4), libpulse0")
endif()

# 3. Control file.
file(GLOB_RECURSE files "${stage}/*")
set(bytes 0)
foreach(f IN LISTS files)
    file(SIZE "${f}" sz)
    math(EXPR bytes "${bytes} + ${sz}")
endforeach()
math(EXPR kib "(${bytes} + 1023) / 1024")
file(MAKE_DIRECTORY "${stage}/DEBIAN")
file(WRITE "${stage}/DEBIAN/control"
"Package: sessions
Version: ${full_version}
Section: sound
Priority: optional
Architecture: ${arch}
Maintainer: ${MAINTAINER}
Installed-Size: ${kib}
Depends: ${depends}
Recommends: libsndfile1
Description: Clip-based music sequencer
 Sessions is a clip-launching music sequencer:
 a Session grid for launching clips, an Arrangement timeline, a note editor,
 instruments and effects, and a mixer with send and return tracks.
 Work is saved as .sessions sets, which open from the file manager.
 Audio clips are warped to the tempo; WAV and AIFF load built in, and FLAC,
 Ogg Vorbis, Opus and MP3 load through libsndfile when it is installed.
")

# 4. Build the package; keep only the newest one in dist/.
file(MAKE_DIRECTORY "${OUT_DIR}")
file(GLOB old "${OUT_DIR}/sessions_*.deb")
if(old)
    file(REMOVE ${old})
endif()
set(deb "${OUT_DIR}/sessions_${full_version}_${arch}.deb")
execute_process(COMMAND dpkg-deb --root-owner-group --build "${stage}" "${deb}" OUTPUT_QUIET RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
    message(FATAL_ERROR "MakeDeb: dpkg-deb failed")
endif()
file(TOUCH "${STAMP}")
message(STATUS "Built ${deb}")
