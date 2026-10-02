add_library(lhc_cli STATIC
    auth_command.cc
    confirm_hook.cc
    daemon_client.cc
    daemon_commands.cc
    output.cc
    probe_command.cc
)
target_include_directories(lhc_cli PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_cli PUBLIC
    lhc_analysis
    lhc_protocol
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
)
