# 高性能图片查找方案技术文档

## 1. 方案架构
本项目采用 **Flutter + C++ Native Plugin (OpenCV)** 的混合架构，旨在实现Windows平台下的高性能游戏图像识别与自动化。

### 核心组件
*   **Flutter (Dart)**: 负责 UI 展示、业务逻辑控制、资源管理以及 WGC (Windows Graphics Capture) 截屏流的获取。
*   **Native Plugin (C++)**: 封装 OpenCV 算法，负责高性能的图像解码、ROI (感兴趣区域) 切割、模板匹配。
*   **Dart FFI**: 作为两者之间的通信桥梁，支持传递原始内存指针，极大降低数据拷贝开销。

## 2. 关键特性

### 2.1 极致性能优化
*   **批量处理 (Batch Processing)**: 通过 `find_images_batch` 接口，支持一次传入一张大图（屏幕截图）和多个查找任务。C++ 内部循环处理，避免了多次 FFI 调用和多次图像解码的开销。
*   **智能格式识别**: C++ 接口自动识别输入数据格式：
    *   **Raw BGRA**: 如果提供了宽高和 stride，直接将内存映射为 `cv::Mat`，实现**零拷贝**加载（推荐用于 WGC 原始流）。
    *   **PNG/JPG**: 如果未提供宽高，自动调用 `imdecode` 解码（兼容性好）。
*   **ROI 区域锁定**: 支持在查找时指定 `(x, y, w, h)` 区域，仅在局部进行模板匹配，计算量通常减少 90% 以上。

### 2.2 资源管理策略
*   **外部化资源**: 图片资源不打入 `assets` 包，而是存放在项目根目录下的子文件夹（如 `yuanshen/`）。
*   **动态加载**: 支持通过相对路径或绝对路径加载模板，方便后续实现在线更新资源包，无需重新编译 App。
*   **自动定位**: 代码实现了在 Debug（项目目录）和 Release（exe 同级目录）模式下的自动路径回退查找机制。

### 2.3 调试与可视化
*   **调试截图**: 每次查找操作（无论是单次还是批量），C++ 都会将实际用于匹配的图像保存为临时文件（`debug_last_roi.png` 或 `debug_last_batch_source.png`）。
*   **UI 回显**: Flutter 界面会读取并显示这些调试截图，帮助开发者快速排查“截屏黑屏”、“区域截偏”或“颜色空间不一致”等问题。

## 3. 开发与构建环境

### 3.1 依赖环境
*   **Flutter SDK**: >= 3.10.0
*   **Visual Studio 2022**: 需要安装 "Desktop development with C++" 工作负载。
*   **OpenCV 4.x**:
    *   **推荐方式**: 使用 Scoop 安装 (`scoop install opencv`)。
    *   **路径**: 脚本会自动在 `C:/Users/<User>/scoop/apps/opencv/current/x64/vc16` 查找。
    *   **环境变量**: 如果安装在其他位置，需设置 `OPENCV_DIR` 环境变量指向 `build` 目录。

### 3.2 构建系统 (CMake)
*   **自动化配置**: `windows/native_lib/CMakeLists.txt` 已配置为自动查找 OpenCV。
*   **编码修复**: 强制使用 UTF-8 编译 (`/utf-8`)，解决中文注释导致的编译错误。
*   **符号冲突**: 预定义 `NOMINMAX`，解决 Windows SDK 与 std::min/max 的冲突。

### 3.3 打包与发布 (Release)
*   **DLL 自动分发**: `windows/runner/CMakeLists.txt` 配置了 Post-Build 脚本。
    *   编译时：自动将 `opencv_world4xxx.dll` 复制到 `build/.../runner/Release`。
    *   打包时：自动剔除 Debug 版本的 DLL (`*d.dll`)，显著减小安装包体积。
*   **零依赖**: 最终用户**不需要安装 OpenCV 或 Python**，解压即可运行。

## 4. API 接口说明 (Dart)

### `NativeImageSearch` 类
*   `loadTemplate(String path)`: 加载模板图片到 C++ 内存，返回 ID。
*   `releaseTemplate(int id)`: 释放模板内存。
*   `findImage(...)`: 单次查找（支持 ROI）。
*   `findImagesBatch(...)`: **[推荐]** 批量查找。
    *   输入：`Uint8List imageBytes` (屏幕截图)。
    *   输入：`List<SearchRequestStruct>` (包含多个 templateId 和各自的 ROI)。
    *   输出：`List<SearchResultStruct>` (每个目标的坐标和置信度)。

## 5. 后续维护注意事项
1.  **更新 OpenCV**: 如果升级 OpenCV 版本，需确保 Scoop 路径或环境变量同步更新，CMake 会自动适配新的版本号。
2.  **WGC 接入**: 当接入 WGC 原始流时，确保传递正确的 `width`, `height` 和 `stride` 给 `findImagesBatch`，以启用 Raw BGRA 高速模式。
3.  **资源路径**: 发布时，请确保 `yuanshen` 等资源文件夹已复制到 `gamemapstool.exe` 的同级目录下。
