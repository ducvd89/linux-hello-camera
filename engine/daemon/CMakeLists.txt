# The wire format, shared by the daemon and the helper's client code.
add_library(lhc_protocol STATIC protocol.cc)
target_include_directories(lhc_protocol PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_protocol PUBLIC nlohmann_json::nlohmann_json)

add_library(lhc_daemon STATIC
    authorization.cc
    daemon_main.cc
    engine.cc
    rate_limiter.cc
    server.cc
)
target_include_directories(lhc_daemon PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_daemon PUBLIC
    lhc_analysis
    lhc_protocol
    lhc_store
    nlohmann_json::nlohmann_json
    Threads::Threads
)

add_executable(linux-hello-camerad linux_hello_camerad.cc)
target_compile_definitions(linux-hello-camerad PRIVATE LHC_VERSION="${LHC_VERSION}")
target_link_libraries(linux-hello-camerad PRIVATE lhc_daemon CLI11::CLI11)
set_target_properties(linux-hello-camerad PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN"
)
