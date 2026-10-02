# Detection (YOLOv8n-face), recognition (EdgeFace), optional AI anti-spoofing (MiniFAS /
# MobileNetV3), all on ONNX Runtime, plus the stb image code they share.
add_library(lhc_face STATIC
    common/onnx_session.cc
    detection/face_detection.cc
    detection/utils.cc
    recognition/face_recognition.cc
    antispoofing/face_as.cc
    enrolled_faces.cc
    stb_impl.cc
)
target_include_directories(lhc_face PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${STB_INCLUDE_DIRS}
)
target_include_directories(lhc_face SYSTEM PUBLIC ${ONNXRUNTIME_INCLUDE_DIRS})
target_link_libraries(lhc_face PUBLIC
    lhc_core
    ${ONNXRUNTIME_LIB}
)
