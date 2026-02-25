#include "overlay_window.h"
#include <dwmapi.h>

OverlayWindow::OverlayWindow() {}

OverlayWindow::~OverlayWindow() {
    Destroy();
}

bool OverlayWindow::Create() {
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"GameMapsToolOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);

    RegisterClassEx(&wc);

    // Create layered, transparent, topmost window
    // WS_EX_LAYERED: Allows transparency
    // WS_EX_TRANSPARENT: Click-through (mouse events pass through)
    // WS_EX_TOPMOST: Always on top
    // WS_EX_TOOLWINDOW: No taskbar icon
    hwnd_ = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"Overlay",
        WS_POPUP,
        0, 0, 100, 100,
        nullptr, nullptr, wc.hInstance, this
    );

    if (!hwnd_) return false;

    // Set transparency key (black is transparent)
    SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 0, LWA_COLORKEY);

    return true;
}

void OverlayWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OverlayWindow::UpdatePosition(HWND target_hwnd) {
    if (!hwnd_ || !target_hwnd || !IsWindow(target_hwnd)) return;

    target_hwnd_ = target_hwnd;

    RECT rect;
    // Try to get precise frame bounds (excluding shadow)
    HRESULT hr = DwmGetWindowAttribute(target_hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(hr)) {
        GetWindowRect(target_hwnd, &rect);
    }

    // Only update if changed
    if (memcmp(&rect, &target_rect_, sizeof(RECT)) != 0) {
        target_rect_ = rect;
        SetWindowPos(hwnd_, HWND_TOPMOST, 
            rect.left, rect.top, 
            rect.right - rect.left, rect.bottom - rect.top, 
            SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void OverlayWindow::SetRects(const std::vector<OverlayRect>& rects) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rects_ = rects;
    }
    if (hwnd_ && visible_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
    }
}

void OverlayWindow::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
}

void OverlayWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    OverlayWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lparam;
        self = (OverlayWindow*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (OverlayWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    if (self) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                self->OnPaint(hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                // Return 1 to indicate handled, avoids flickering
                // But we use NULL_BRUSH so it might not matter
                return 1;
        }
    }

    return DefWindowProc(hwnd, message, wparam, lparam);
}

void OverlayWindow::OnPaint(HDC hdc) {
    // Double buffering to prevent flicker
    RECT clientRect;
    GetClientRect(hwnd_, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

    // Fill with black (transparent color key)
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memDC, &clientRect, blackBrush);
    DeleteObject(blackBrush);

    // Draw rects
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Create Pen (Red, 3px)
        HPEN hPen = CreatePen(PS_SOLID, 3, RGB(255, 0, 0));
        HGDIOBJ oldPen = SelectObject(memDC, hPen);
        HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));

        for (const auto& r : rects_) {
            Rectangle(memDC, r.x, r.y, r.x + r.width, r.y + r.height);
        }

        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(hPen);
    }

    // Blit to screen
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}
