cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
target_link_libraries(${BINARY_NAME} PRIVATE native_image_search)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib" "windowscodecs.lib" "gdi32.lib" "shell32.lib" "d3d11.lib" "dxgi.lib" "windowsapp.lib" "coremessaging.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# === OpenCV DLL Auto Copy ===
# 自动查找并复制 OpenCV DLL 到输出目录
# 根据构建类型 (Debug/Release) 选择正确的 DLL，减少包体积
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    # Debug 模式：优先查找带 'd' 后缀的 DLL
    file(GLOB OpenCV_DLLS "C:/Users/Administrator/scoop/apps/opencv/current/x64/vc16/bin/opencv_world*d.dll")
else()
    # Release/Profile 模式：只保留 release DLL (排除 *d.dll)
    # 策略：先找出所有的 DLL，然后剔除掉以 d.dll 结尾的
    file(GLOB ALL_DLLS "C:/Users/Administrator/scoop/apps/opencv/current/x64/vc16/bin/opencv_world*.dll")
    set(OpenCV_DLLS "")
    foreach(DLL_PATH ${ALL_DLLS})
        # 简单字符串匹配：如果文件名不以 "d.dll" 结尾，则认为是 Release 版
        string(REGEX MATCH "d\\.dll$" IS_DEBUG "${DLL_PATH}")
        if(NOT IS_DEBUG)
            list(APPEND OpenCV_DLLS ${DLL_PATH})
        endif()
    endforeach()
endif()

if(OpenCV_DLLS)
    message(STATUS "Found OpenCV DLLs for Runner (${CMAKE_BUILD_TYPE}): ${OpenCV_DLLS}")
    
    # 1. 开发模式：复制到构建目录
    add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OpenCV_DLLS}
        $<TARGET_FILE_DIR:${BINARY_NAME}>
    )
    
    # 2. 发布模式：安装到最终包
    install(FILES ${OpenCV_DLLS} DESTINATION "${CMAKE_INSTALL_PREFIX}")
endif()

# === Native Lib Install ===
# 安装自定义的 DLL 到最终包
install(TARGETS native_image_search
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}"
)
