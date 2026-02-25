cmake_minimum_required(VERSION 3.14)

project(native_image_search LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制 MSVC 使用 UTF-8 编码读取源文件，解决中文乱码导致的语法错误
if(MSVC)
    add_compile_options(/utf-8)
    # 禁用一些常见的 MSVC 警告，防止干扰
    add_compile_options(/wd4819) 
endif()

# 查找 OpenCV 包
# 注意：如果 CMake 找不到 OpenCV，请设置环境变量 OpenCV_DIR 指向 OpenCV 的 build 目录
# 优先查找 Scoop 安装的 OpenCV
list(APPEND CMAKE_PREFIX_PATH "C:/Users/Administrator/scoop/apps/opencv/current")
list(APPEND CMAKE_PREFIX_PATH "C:/Users/Administrator/scoop/apps/opencv/current/x64/vc16/lib")

find_package(OpenCV REQUIRED)

# 添加源文件
add_library(native_image_search SHARED
    image_search.cpp
    image_search.h
)

# 链接库
target_link_libraries(native_image_search PRIVATE 
    ${OpenCV_LIBS}
    gdi32
    user32
)

# 定义导出宏
target_compile_definitions(native_image_search PRIVATE IMAGE_SEARCH_EXPORTS)

# 设置输出目录，使其与 Runner 在同一目录 (方便调试)
set_target_properties(native_image_search PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runner/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runner/$<CONFIG>"
)

# 注意：这里移除了 install 命令，统一在 runner/CMakeLists.txt 中处理


