add_executable(lhc_tests
    test_main.cc
    test_camera.cc
    test_config.cc
    test_daemon.cc
    test_liveness.cc
    test_matching.cc
    test_storage.cc
)
target_link_libraries(lhc_tests PRIVATE lhc_analysis lhc_daemon lhc_cli lhc_store)
set_target_properties(lhc_tests PROPERTIES BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}")
add_test(NAME lhc_tests COMMAND lhc_tests)

# A daemon for tests and hand checks: the real engine and server with a fake TPM and, when the
# configured camera is a recorded session folder, a replay instead of V4L2. Never installed.
add_executable(lhc_test_daemon test_daemon_main.cc)
target_link_libraries(lhc_test_daemon PRIVATE lhc_daemon CLI11::CLI11)
set_target_properties(lhc_test_daemon PROPERTIES BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}")

# enroll -> list -> auth -> remove -> clear through the helper's client code against that daemon,
# on the real models and recorded frames. Skipped (exit 77) where they are not available.
add_executable(lhc_integration test_integration.cc)
target_link_libraries(lhc_integration PRIVATE lhc_daemon lhc_cli)
target_compile_definitions(lhc_integration PRIVATE
    TEST_DAEMON="$<TARGET_FILE:lhc_test_daemon>"
    TEST_MODELS_DIR="${LHC_MODELS_DIR}"
)
set_target_properties(lhc_integration PROPERTIES BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}")
add_dependencies(lhc_integration lhc_test_daemon)
add_test(NAME lhc_integration COMMAND lhc_integration)
set_tests_properties(lhc_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
