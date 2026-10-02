add_library(lhc_liveness STATIC liveness.cc)
target_include_directories(lhc_liveness PUBLIC ${CMAKE_SOURCE_DIR})
