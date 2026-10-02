# The PAM module only forks and execs the helper, so it needs nothing but libpam.
add_library(pam_linux_hello_camera MODULE pam.cc)
set_target_properties(pam_linux_hello_camera PROPERTIES PREFIX "")
target_compile_definitions(pam_linux_hello_camera PRIVATE LHC_HELPER_PATH="${LHC_HELPER_PATH}")
target_link_libraries(pam_linux_hello_camera PRIVATE ${PAM_LIB})

# The helper holds all the heavy code. Its RUNPATH is $ORIGIN so it finds the bundled
# libonnxruntime installed beside it.
add_executable(linux-hello-camera-helper helper.cc)
target_compile_definitions(linux-hello-camera-helper PRIVATE LHC_VERSION="${LHC_VERSION}")
target_link_libraries(linux-hello-camera-helper PRIVATE lhc_cli CLI11::CLI11)
set_target_properties(linux-hello-camera-helper PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN"
)
