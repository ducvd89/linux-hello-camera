add_library(lhc_camera STATIC
    frame.cc
    v4l2_camera.cc
    camera_stream.cc
    replay_source.cc
)
target_include_directories(lhc_camera PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_camera PUBLIC
    lhc_core
    lhc_face
    yaml-cpp::yaml-cpp
    PkgConfig::TURBOJPEG
    spdlog::spdlog_header_only
)
