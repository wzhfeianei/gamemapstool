#ifndef IMAGE_SEARCH_H
#define IMAGE_SEARCH_H

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#include <cstdint>

// 结构体定义在 extern "C" 之外，避免 C++ 特性干扰 C 链接
// 但为了 ABI 兼容，我们使用标准的 POD 类型
struct SearchResult {
    int x;
    int y;
    double score;
};

extern "C" {
    // 初始化：加载模板图片并返回 ID
    // imagePath: 图片绝对路径
    // 返回值: templateId (>0 成功, <=0 失败)
    EXPORT int load_template(const char* imagePath);

    // 释放特定模板
    EXPORT void release_template(int templateId);

    // 释放所有模板资源
    EXPORT void release_all_templates();

    // 在指定区域查找图片
    // templateId: load_template 返回的 ID
    // x, y, w, h: 屏幕区域 (全屏传 w=-1, h=-1)
    // threshold: 匹配阈值 (0.0 - 1.0, 推荐 0.8-0.9)
    EXPORT SearchResult find_image(int templateId, int x, int y, int w, int h, double threshold);

    // 调试用：保存最后一次截图到文件 (方便查看截图是否正确)
    EXPORT void debug_save_last_capture(const char* path);

    // 批量任务结构体
    struct SearchRequest {
        int templateId;
        int roiX;
        int roiY;
        int roiW;
        int roiH;
        double threshold;
    };

    struct SearchResultItem {
        int templateId;
        int x;
        int y;
        double score;
    };

    // 批量查找
    // imageBytes: 图片数据指针 (可以是 PNG/JPG 压缩数据，也可以是 BGRA 原始像素)
    // length: 数据长度
    // width: 图片宽度 (如果传 >0，则视为 raw BGRA 数据；如果传 0，视为 PNG/JPG)
    // height: 图片高度
    // stride: 每行字节数 (通常 width * 4)
    // requests: 任务数组指针
    // count: 任务数量
    // results: 输出数组指针 (由调用者分配，长度需 >= count)
    EXPORT void find_images_batch(
        uint8_t* imageBytes, int length, 
        int width, int height, int stride,
        SearchRequest* requests, int count, 
        SearchResultItem* results
    );
}

#endif // IMAGE_SEARCH_H
