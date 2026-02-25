#define NOMINMAX
#include "image_search.h"
#include <windows.h>
#include <opencv2/opencv.hpp>
#include <map>
#include <mutex>
#include <vector>
#include <string>

// 全局变量管理模板
static std::map<int, cv::Mat> g_templates;
static int g_nextTemplateId = 1;
static std::mutex g_mutex;
static cv::Mat g_lastCapture;

// GDI 屏幕截图辅助函数
// 将屏幕特定区域截图并转换为 cv::Mat
// 返回的 cv::Mat 为 BGR (8UC3) 格式
cv::Mat CaptureScreen(int x, int y, int w, int h) {
    HDC hScreenDC = GetDC(NULL);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);

    // 如果未指定宽高，获取全屏尺寸 (仅主屏幕)
    if (w <= 0 || h <= 0) {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }

    // 创建位图
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    // 截图 (SRCCOPY)
    // 注意：x, y 可以是负数（多显示器情况），GetDC(NULL) 覆盖整个虚拟屏幕
    BitBlt(hMemoryDC, 0, 0, w, h, hScreenDC, x, y, SRCCOPY);

    // 关键修正：必须在调用 GetDIBits 前将位图选出设备上下文
    SelectObject(hMemoryDC, hOldBitmap);

    // 获取位图信息
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负值表示自上而下 (Top-down)
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; // 32位色深 (BGRA)
    bi.bmiHeader.biCompression = BI_RGB;

    // 创建 cv::Mat 来存储像素数据 (BGRA)
    cv::Mat mat(h, w, CV_8UC4);
    
    // 从位图中获取像素数据
    // GetDIBits 即使 hBitmap 未被选入 DC，也可以工作，只要 hBitmap 有效
    GetDIBits(hMemoryDC, hBitmap, 0, h, mat.data, &bi, DIB_RGB_COLORS);

    // 清理 GDI 资源
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    // 转换为 BGR (OpenCV 默认使用 BGR，且 matchTemplate 通常不需要 Alpha 通道)
    cv::Mat result;
    if (!mat.empty()) {
        cv::cvtColor(mat, result, cv::COLOR_BGRA2BGR);
    }
    
    // 保存最后一次截图用于调试
    g_lastCapture = result.clone();

    return result;
}

extern "C" {

    EXPORT int load_template(const char* imagePath) {
        if (!imagePath) return -1;
        
        // 读取图片 (IMREAD_COLOR 忽略 Alpha 通道，IMREAD_UNCHANGED 保留)
        // 通常 matchTemplate 使用 BGR 图像
        cv::Mat templ = cv::imread(imagePath, cv::IMREAD_COLOR);
        if (templ.empty()) {
            return -2; // 读取失败
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        int id = g_nextTemplateId++;
        g_templates[id] = templ;
        return id;
    }

    EXPORT void release_template(int templateId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_templates.erase(templateId);
    }

    EXPORT void release_all_templates() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_templates.clear();
        g_nextTemplateId = 1;
    }

    EXPORT SearchResult find_image(int templateId, int x, int y, int w, int h, double threshold) {
        SearchResult result = { -1, -1, 0.0 };

        cv::Mat templ;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_templates.find(templateId);
            if (it == g_templates.end()) {
                return result; // 模板不存在
            }
            templ = it->second;
        }

        // 截图 (ROI)
        cv::Mat screen = CaptureScreen(x, y, w, h);
        
        // 强制保存调试截图 (方便 Dart 层回显)
        // 保存到临时文件，例如 debug_last_roi.png
        if (!screen.empty()) {
            cv::imwrite("debug_last_roi.png", screen);
        }

        // 检查尺寸
        if (screen.empty() || screen.rows < templ.rows || screen.cols < templ.cols) {
            return result; // 屏幕区域比模板还小或截图失败
        }

        // 模板匹配
        cv::Mat matchResult;
        // TM_CCOEFF_NORMED 是最常用的归一化相关系数匹配法
        // 结果范围 [-1, 1]，越接近 1 越匹配
        cv::matchTemplate(screen, templ, matchResult, cv::TM_CCOEFF_NORMED);

        // 寻找最大值位置
        double minVal, maxVal;
        cv::Point minLoc, maxLoc;
        cv::minMaxLoc(matchResult, &minVal, &maxVal, &minLoc, &maxLoc);

        if (maxVal >= threshold) {
            // 返回的坐标是相对于屏幕左上角的绝对坐标
            // 截图区域左上角是 (x, y)，匹配结果是在截图内的相对坐标 maxLoc
            result.x = x + maxLoc.x;
            result.y = y + maxLoc.y;
            result.score = maxVal;
        }

        return result;
    }
    
    EXPORT void debug_save_last_capture(const char* path) {
        if (!g_lastCapture.empty() && path) {
            cv::imwrite(path, g_lastCapture);
        }
    }

    EXPORT void find_images_batch(
        uint8_t* imageBytes, int length, 
        int width, int height, int stride,
        SearchRequest* requests, int count, 
        SearchResultItem* results
    ) {
        if (!imageBytes || length <= 0 || !requests || !results || count <= 0) {
            return;
        }

        cv::Mat sourceImage;

        // 1. 解码或构造源图片
        if (width > 0 && height > 0) {
            // Raw BGRA 模式
            // 注意：OpenCV 默认使用 BGR，而 WGC/Flutter 通常是 BGRA
            // 如果不需要 Alpha，可以直接当作 4 通道处理，或者 cvtColor
            // 这里我们假设 stride 是标准的 width * 4，且格式为 BGRA (8UC4)
            // 如果 stride 不是标准的，可能需要调整 step
            sourceImage = cv::Mat(height, width, CV_8UC4, imageBytes, stride > 0 ? stride : cv::Mat::AUTO_STEP);
            
            // matchTemplate 需要 BGR 或灰度，通常不需要 Alpha
            // 转换为 BGR
            cv::cvtColor(sourceImage, sourceImage, cv::COLOR_BGRA2BGR);
        } else {
            // 压缩图片模式 (PNG/JPG)
            // 先将数据包装成 vector 或 Mat
            std::vector<uint8_t> buffer(imageBytes, imageBytes + length);
            sourceImage = cv::imdecode(buffer, cv::IMREAD_COLOR);
        }

        if (sourceImage.empty()) {
            return; // 图片无效
        }
        
        // 保存调试图
        cv::imwrite("debug_last_batch_source.png", sourceImage);

        // 2. 批量处理任务
        for (int i = 0; i < count; i++) {
            // 注意：C++ 指针偏移
            SearchRequest& req = requests[i];
            SearchResultItem& res = results[i];
            
            // 初始化结果
            res.templateId = req.templateId;
            res.x = -1;
            res.y = -1;
            res.score = 0.0;

            // 获取模板
            cv::Mat templ;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_templates.find(req.templateId);
                if (it == g_templates.end()) {
                    continue; // 模板不存在
                }
                templ = it->second;
            }

            // 处理 ROI
            cv::Mat searchArea;
            int offsetX = 0;
            int offsetY = 0;

            if (req.roiW > 0 && req.roiH > 0) {
                // 确保 ROI 在图片范围内
                int roiX = std::max(0, req.roiX);
                int roiY = std::max(0, req.roiY);
                // 确保不越界
                int maxW = sourceImage.cols - roiX;
                int maxH = sourceImage.rows - roiY;
                
                if (maxW <= 0 || maxH <= 0) {
                     continue; // ROI 完全在图片外
                }
                
                int roiW = std::min(req.roiW, maxW);
                int roiH = std::min(req.roiH, maxH);

                if (roiW < templ.cols || roiH < templ.rows) {
                    continue; // ROI 太小
                }

                // 切割子图 (零拷贝)
                searchArea = sourceImage(cv::Rect(roiX, roiY, roiW, roiH));
                offsetX = roiX;
                offsetY = roiY;
            } else {
                // 全图搜索
                searchArea = sourceImage;
            }

            // 检查尺寸
            if (searchArea.rows < templ.rows || searchArea.cols < templ.cols) {
                continue;
            }

            // 匹配
            cv::Mat matchResult;
            cv::matchTemplate(searchArea, templ, matchResult, cv::TM_CCOEFF_NORMED);

            double minVal, maxVal;
            cv::Point minLoc, maxLoc;
            cv::minMaxLoc(matchResult, &minVal, &maxVal, &minLoc, &maxLoc);

            if (maxVal >= req.threshold) {
                res.x = offsetX + maxLoc.x;
                res.y = offsetY + maxLoc.y;
                res.score = maxVal;
            }
        }
    }
}
