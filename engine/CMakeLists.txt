cmake_minimum_required(VERSION 3.20)

# The version comes from -DLHC_VERSION=... (the PKGBUILD), else this default.
set(LHC_VERSION "0.0.0" CACHE STRING "Version reported by the helper")
project(linux-hello-camera LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS        NO)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Where the helper looks for things (DESIGN.md). These are fixed at build time:
# the helper runs as root from PAM and must not take them from its environment.
# The one exception is the models directory, which probe and probe-dir also
# read from $LHC_MODELS_DIR (see core/config.cc), for calibration.
set(LHC_MODELS_DIR "/usr/share/linux-hello-camera/models" CACHE PATH "Models directory")
set(LHC_CONFIG_PATH "/etc/linux-hello-camera/config.yaml" CACHE FILEPATH "System config file")
set(LHC_FACES_DIR "/var/lib/linux-hello-camera/faces" CACHE PATH "Face crops left by 0.8 (migrate reads and deletes them)")
set(LHC_TEMPLATES_DIR "/var/lib/linux-hello-camera/templates" CACHE PATH "Face templates, one file per user")
set(LHC_SOCKET_PATH "/run/linux-hello-camera/engine.sock" CACHE FILEPATH "Daemon socket")
set(LHC_CONFIRM_HOOK "/usr/lib/linux-hello-camera/confirm-hook" CACHE FILEPATH "Confirmation hook run by auth")

include(GNUInstallDirs)
set(LHC_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/linux-hello-camera/linux-hello-camera-helper")
include(Dependencies.cmake)

find_package(Threads REQUIRED)

add_subdirectory(core)
add_subdirectory(camera)
add_subdirectory(liveness)
add_subdirectory(face)
add_subdirectory(store)
add_subdirectory(analysis)
add_subdirectory(cli)
add_subdirectory(daemon)
add_subdirectory(pam)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Everything the PAM stack runs lives under lib/linux-hello-camera: the helper
# and the bundled ONNX Runtime next to it (RUNPATH $ORIGIN).
set(LHC_LIBEXEC_DIR "${CMAKE_INSTALL_LIBDIR}/linux-hello-camera")
install(TARGETS linux-hello-camera-helper linux-hello-camerad RUNTIME DESTINATION "${LHC_LIBEXEC_DIR}")
file(GLOB LHC_ONNXRUNTIME_LIBS "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so.*")
install(FILES ${LHC_ONNXRUNTIME_LIBS} DESTINATION "${LHC_LIBEXEC_DIR}")
install(TARGETS pam_linux_hello_camera LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/security")

# systemd units: socket activation for the daemon.
set(LHC_LIBEXEC_FULL_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/linux-hello-camera")
foreach(unit linux-hello-camerad.socket linux-hello-camerad.service)
    configure_file(systemd/${unit}.in ${CMAKE_BINARY_DIR}/${unit} @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/${unit} DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system")
endforeach()
