add_library(lhc_analysis STATIC
    authenticator.cc
    crop_selector.cc
    frame_analyzer.cc
    match_tracker.cc
    models.cc
)
target_include_directories(lhc_analysis PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_analysis PUBLIC
    lhc_core
    lhc_camera
    lhc_liveness
    lhc_face
    spdlog::spdlog_header_only
)
