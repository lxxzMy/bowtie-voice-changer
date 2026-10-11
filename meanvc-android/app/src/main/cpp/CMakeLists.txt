cmake_minimum_required(VERSION 3.22)
project(meanvc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ONNX Runtime shared lib extracted from the official onnxruntime-android AAR
# (1.22.0 — must match the headers in ort_include/). Loaded at runtime via
# System.loadLibrary("onnxruntime") BEFORE our lib.
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libonnxruntime.so)

add_library(meanvc SHARED meanvc_jni.cpp)
target_include_directories(meanvc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ort_include)
target_link_libraries(meanvc onnxruntime aaudio log android)
target_compile_options(meanvc PRIVATE -O2 -fexceptions -frtti)
