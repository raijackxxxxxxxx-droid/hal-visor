cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/face-detect.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ONNX Runtime (YuNet顔検出用, Windows x64のみ): 公式リリースzipを取得してリンクし、
# 実行時DLLをプラグインDLLの隣へコピーする
if(WIN32)
  include(FetchContent)
  FetchContent_Declare(
    onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip
    URL_HASH SHA256=78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581
  )
  FetchContent_MakeAvailable(onnxruntime)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${onnxruntime_SOURCE_DIR}/include)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
    COMMENT "Copy onnxruntime.dll next to the plugin DLL"
    VERBATIM
  )
endif()
