#ifndef RUNNER_OVERLAY_WINDOW_H_
#define RUNNER_OVERLAY_WINDOW_H_

#include <windows.h>
#include <vector>
#include <mutex>

struct OverlayRect {
    int x;
    int y;
    int width;
    int height;
    COLORREF color;
};

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    bool Create();
    void Destroy();
    void UpdatePosition(HWND target_hwnd);
    void SetRects(const std::vector<OverlayRect>& rects);
    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void OnPaint(HDC hdc);

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    std::vector<OverlayRect> rects_;
    std::mutex mutex_;
    
    // Cached target state
    HWND target_hwnd_ = nullptr;
    RECT target_rect_ = {};
};

#endif // RUNNER_OVERLAY_WINDOW_H_
