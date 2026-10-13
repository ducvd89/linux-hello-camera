add_library(lhc_camera STATIC
    frame.cc
    v4l2_camera.cc
    camera_stream.cc
    frame_grabber.cc
    replay_source.cc
)
find_package(Threads REQUIRED)
target_include_directories(lhc_camera PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_camera PUBLIC
    lhc_core
    lhc_face
    yaml-cpp::yaml-cpp
    PkgConfig::TURBOJPEG
    Threads::Threads
    spdlog::spdlog_header_only
)
