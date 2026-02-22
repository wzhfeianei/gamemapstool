#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <map>
#include <vector>
#include <mutex>

#include "win32_window.h"

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <d3d11.h>
#include <wrl/client.h>

// Custom message for capture completion
#define WM_CAPTURE_COMPLETE (WM_USER + 101)

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window {
 public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project);
  virtual ~FlutterWindow();

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      capture_channel_;

  // Pending capture results
  int next_capture_id_ = 1;
  std::map<int, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>
      pending_results_;

  // WGC Session State
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  
  // COM pointers for efficient access in OnFrameArrived
  Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
  D3D11_TEXTURE2D_DESC staging_desc_ = {};

  std::vector<uint8_t> last_frame_data_;
  std::mutex frame_mutex_;
  bool is_capturing_ = false;
  winrt::event_token frame_arrived_token_;

  void StartCaptureSession(const flutter::MethodCall<flutter::EncodableValue>& call,
                           std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void StopCaptureSession(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void GetCaptureFrame(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
                      winrt::Windows::Foundation::IInspectable const& args);
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
