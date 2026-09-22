# softcam (MIT, https://github.com/tshino/softcam), vendored from commit 5113173.
# LocalCam patches: camera name "LocalCam", own CLSID (softcam.cpp), own shared-memory names (FrameBuffer.cpp),
# and the sender's calls wait at most 100 ms for the frame lock instead of forever (FrameBuffer.cpp, Misc.cpp).
# Build files are replaced by this CMakeLists; sources are otherwise unchanged.

add_library(softcam SHARED
    src/softcam/softcam.cpp
    src/softcam/softcam.def
    src/softcamcore/DShowSoftcam.cpp
    src/softcamcore/FrameBuffer.cpp
    src/softcamcore/Misc.cpp
    src/softcamcore/SenderAPI.cpp
    src/softcamcore/Watchdog.cpp
    src/baseclasses/amfilter.cpp
    src/baseclasses/amvideo.cpp
    src/baseclasses/combase.cpp
    src/baseclasses/dllentry.cpp
    src/baseclasses/dllsetup.cpp
    src/baseclasses/mtype.cpp
    src/baseclasses/source.cpp
    src/baseclasses/winutil.cpp
    src/baseclasses/wxdebug.cpp
    src/baseclasses/wxlist.cpp
    src/baseclasses/wxutil.cpp
)
target_include_directories(softcam PRIVATE src src/baseclasses INTERFACE src/softcam)
target_compile_definitions(softcam PRIVATE UNICODE _UNICODE)
target_link_libraries(softcam PRIVATE strmiids winmm)
