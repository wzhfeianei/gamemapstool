#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <variant>

#include "win32_window.h"

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <d3d11.h>
#include <wrl/client.h>

// Custom message for capture completion
#define WM_CAPTURE_COMPLETE (WM_USER + 101)

class CaptureTexture {
 public:
  CaptureTexture(flutter::TextureRegistrar* texture_registrar);
  ~CaptureTexture();

  int64_t id() const { return texture_id_; }

  void UpdateFrame(const uint8_t* data, size_t width, size_t height, size_t row_pitch, bool force_opaque = false);

  const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width, size_t height);

  bool GetContent(std::vector<uint8_t>* output, size_t* width, size_t* height);
  
  void GetSize(size_t* width, size_t* height);

 private:
  flutter::TextureRegistrar* texture_registrar_;
  std::unique_ptr<flutter::TextureVariant> texture_;
  int64_t texture_id_ = -1;
  std::mutex mutex_;
  std::vector<uint8_t> buffer_;
  size_t width_ = 0;
  size_t height_ = 0;
  std::unique_ptr<FlutterDesktopPixelBuffer> pixel_buffer_;
};

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
  std::unique_ptr<flutter::PluginRegistrarWindows> plugin_registrar_;
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

  // GDI Capture
  std::atomic<bool> gdi_capturing_ = false;
  std::thread gdi_capture_thread_;
  void GdiCaptureLoop(HWND hwnd, std::string mode);

  void StartCaptureSession(const flutter::MethodCall<flutter::EncodableValue>& call,
                           std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void StopCaptureSession(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void GetCaptureFrame(const flutter::MethodCall<flutter::EncodableValue>& call, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void GetLastFrame(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void GetTextureId(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
                      winrt::Windows::Foundation::IInspectable const& args);

  std::unique_ptr<CaptureTexture> capture_texture_;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
