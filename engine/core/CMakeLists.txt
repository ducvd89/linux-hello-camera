add_library(lhc_core STATIC
    base64.cc
    config.cc
    file_util.cc
    hardware.cc
)
target_include_directories(lhc_core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_definitions(lhc_core PRIVATE
    LHC_CONFIG_PATH="${LHC_CONFIG_PATH}"
    LHC_FACES_DIR="${LHC_FACES_DIR}"
    LHC_TEMPLATES_DIR="${LHC_TEMPLATES_DIR}"
    LHC_SOCKET_PATH="${LHC_SOCKET_PATH}"
    LHC_CONFIRM_HOOK="${LHC_CONFIRM_HOOK}"
    LHC_MODELS_DIR="${LHC_MODELS_DIR}"
)
target_link_libraries(lhc_core PUBLIC yaml-cpp::yaml-cpp)
