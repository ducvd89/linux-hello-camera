add_library(lhc_store STATIC
    migrate.cc
    process.cc
    systemd_creds.cc
    template.cc
    template_store.cc
)
target_include_directories(lhc_store PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lhc_store PUBLIC
    lhc_core
    lhc_face
    nlohmann_json::nlohmann_json
)
