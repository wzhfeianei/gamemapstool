#include "flutter_window.h"
#include "utils.h"

#include <optional>
#include <algorithm>
#include <cwctype>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>
#include <chrono>

#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <wrl/client.h>

#ifndef __IDirect3DDxgiInterfaceAccess_INTERFACE_DEFINED__
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
    IDirect3DDxgiInterfaceAccess : IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};
#endif

#include "flutter/generated_plugin_registrant.h"

CaptureTexture::CaptureTexture(flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {
  texture_ = std::make_unique<flutter::TextureVariant>(
      flutter::PixelBufferTexture([this](size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
        return this->CopyPixelBuffer(width, height);
      }));
  texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
}

CaptureTexture::~CaptureTexture() {
  texture_registrar_->UnregisterTexture(texture_id_);
}

void CaptureTexture::UpdateFrame(const uint8_t* data, size_t width, size_t height, size_t row_pitch, bool force_opaque) {
  if (!data) return;
  const std::lock_guard<std::mutex> lock(mutex_);
  
  if (width_ != width || height_ != height) {
    width_ = width;
    height_ = height;
    buffer_.resize(width * height * 4);
    pixel_buffer_ = std::make_unique<FlutterDesktopPixelBuffer>();
    pixel_buffer_->width = width;
    pixel_buffer_->height = height;
    pixel_buffer_->release_callback = nullptr;
    pixel_buffer_->release_context = nullptr;
  }

  if (buffer_.size() != width * height * 4) return;

  // Copy with stride handling and color swizzling (BGRA -> RGBA)
  // Source is BGRA (from D3D11), Destination is RGBA (Flutter standard on Windows).
  // Note: Flutter's PixelBufferTexture on Windows expects RGBA by default if we can't specify otherwise.
  if (row_pitch == width * 4) {
      uint8_t* dst = buffer_.data();
      const uint8_t* src = data;
      size_t total_pixels = width * height;
      for (size_t i = 0; i < total_pixels; ++i) {
          dst[i * 4 + 0] = src[i * 4 + 2]; // R
          dst[i * 4 + 1] = src[i * 4 + 1]; // G
          dst[i * 4 + 2] = src[i * 4 + 0]; // B
          dst[i * 4 + 3] = force_opaque ? 255 : src[i * 4 + 3]; // A
      }
  } else {
      for (size_t y = 0; y < height; ++y) {
          uint8_t* dst_row = buffer_.data() + y * width * 4;
          const uint8_t* src_row = data + y * row_pitch;
          for (size_t x = 0; x < width; ++x) {
              dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // R
              dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G
              dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // B
              dst_row[x * 4 + 3] = force_opaque ? 255 : src_row[x * 4 + 3]; // A
          }
      }
  }

  texture_registrar_->MarkTextureFrameAvailable(texture_id_);
}

const FlutterDesktopPixelBuffer* CaptureTexture::CopyPixelBuffer(size_t width, size_t height) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (buffer_.empty()) return nullptr;
  
  pixel_buffer_->buffer = buffer_.data();
  pixel_buffer_->width = width_;
  pixel_buffer_->height = height_;
  return pixel_buffer_.get();
}

bool CaptureTexture::GetContent(std::vector<uint8_t>* output, size_t* width, size_t* height) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (buffer_.empty()) return false;
  *output = buffer_;
  *width = width_;
  *height = height_;
  return true;
}

void CaptureTexture::GetSize(size_t* width, size_t* height) {
  const std::lock_guard<std::mutex> lock(mutex_);
  *width = width_;
  *height = height_;
}

namespace {
using Microsoft::WRL::ComPtr;

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int size_needed =
      MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size_needed <= 0) {
    return std::wstring();
  }
  std::wstring result(static_cast<size_t>(size_needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], size_needed);
  return result;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int size_needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
  if (size_needed <= 0) {
    return std::string();
  }
  std::string result(static_cast<size_t>(size_needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], size_needed,
                      nullptr, nullptr);
  return result;
}

std::wstring ToLower(const std::wstring& value) {
  std::wstring result = value;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return result;
}

bool TryFindProcessId(const std::wstring& process_name, DWORD* pid) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return false;
  }
  const std::wstring target_lower = ToLower(process_name);
  do {
    if (ToLower(entry.szExeFile) == target_lower) {
      *pid = entry.th32ProcessID;
      CloseHandle(snapshot);
      return true;
    }
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return false;
}

struct ProcessRecord {
  DWORD pid;
  DWORD parent_pid;
  std::wstring name;
};

struct ProcessEntry {
  DWORD pid;
  std::wstring name;
  double cpu;
  std::vector<uint8_t> icon_bytes;
};

std::vector<ProcessRecord> EnumerateProcesses() {
  std::vector<ProcessRecord> records;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return records;
  }
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return records;
  }
  do {
    ProcessRecord record;
    record.pid = entry.th32ProcessID;
    record.parent_pid = entry.th32ParentProcessID;
    record.name = entry.szExeFile;
    if (!record.name.empty()) {
      records.push_back(record);
    }
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return records;
}

// CPU calculation helpers restored



bool GetProcessImagePath(DWORD pid, std::wstring* path) {
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!handle) {
    return false;
  }
  std::wstring buffer(1024, L'\0');
  DWORD size = static_cast<DWORD>(buffer.size());
  const BOOL ok =
      QueryFullProcessImageNameW(handle, 0, buffer.data(), &size);
  CloseHandle(handle);
  if (!ok || size == 0) {
    return false;
  }
  buffer.resize(size);
  *path = buffer;
  return true;
}

bool EncodeWicBitmapToPng(IWICImagingFactory* factory, IWICBitmap* wic_bitmap,
                          std::vector<uint8_t>* output,
                          std::wstring* error) {
  ComPtr<IStream> stream;
  HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create stream";
    }
    return false;
  }
  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create PNG encoder";
    }
    return false;
  }
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to init PNG encoder";
    }
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(&frame, &props);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create frame";
    }
    return false;
  }
  hr = frame->Initialize(props.Get());
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to init frame";
    }
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  hr = wic_bitmap->GetSize(&width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    if (error) {
      *error = L"Failed to get bitmap info";
    }
    return false;
  }
  frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&format);
  hr = frame->WriteSource(wic_bitmap, nullptr);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to write frame data";
    }
    return false;
  }
  hr = frame->Commit();
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to commit frame";
    }
    return false;
  }
  hr = encoder->Commit();
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to commit PNG";
    }
    return false;
  }
  STATSTG stat;
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
    if (error) {
      *error = L"Failed to read PNG data";
    }
    return false;
  }
  const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
  if (size == 0) {
    if (error) {
      *error = L"Invalid PNG data";
    }
    return false;
  }
  output->resize(size);
  LARGE_INTEGER start;
  start.QuadPart = 0;
  stream->Seek(start, STREAM_SEEK_SET, nullptr);
  ULONG read = 0;
  hr = stream->Read(output->data(), size, &read);
  if (FAILED(hr) || read != size) {
    if (error) {
      *error = L"Failed to read PNG data";
    }
    return false;
  }
  return true;
}

bool EncodeBitmapToPng(HBITMAP bitmap, std::vector<uint8_t>* output,
                       std::wstring* error) {
  HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to init encoder";
    }
    if (SUCCEEDED(com_init)) {
      CoUninitialize();
    }
    return false;
  }
  ComPtr<IWICBitmap> wic_bitmap;
  hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha,
                                        &wic_bitmap);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to convert bitmap";
    }
    if (SUCCEEDED(com_init)) {
      CoUninitialize();
    }
    return false;
  }
  const bool ok =
      EncodeWicBitmapToPng(factory.Get(), wic_bitmap.Get(), output, error);
  if (SUCCEEDED(com_init)) {
    CoUninitialize();
  }
  return ok;
}

bool ExtractIconPng(const std::wstring& path, std::vector<uint8_t>* output) {
  SHFILEINFOW file_info;
  if (!SHGetFileInfoW(path.c_str(), 0, &file_info, sizeof(file_info),
                      SHGFI_ICON | SHGFI_SMALLICON)) {
    return false;
  }
  HICON icon = file_info.hIcon;
  if (!icon) {
    return false;
  }
  const int size = 32;
  HDC screen_dc = GetDC(nullptr);
  HDC mem_dc = CreateCompatibleDC(screen_dc);
  HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, size, size);
  HGDIOBJ old_object = SelectObject(mem_dc, bitmap);
  RECT rect;
  rect.left = 0;
  rect.top = 0;
  rect.right = size;
  rect.bottom = size;
  FillRect(mem_dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  DrawIconEx(mem_dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
  SelectObject(mem_dc, old_object);
  DestroyIcon(icon);
  ReleaseDC(nullptr, screen_dc);
  DeleteDC(mem_dc);
  std::wstring error;
  const bool ok = EncodeBitmapToPng(bitmap, output, &error);
  DeleteObject(bitmap);
  return ok;
}

ULONGLONG FileTimeToUint64(const FILETIME& time) {
  return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) |
         time.dwLowDateTime;
}

bool QueryProcessTime(DWORD pid, ULONGLONG* time) {
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
  if (!handle) {
    return false;
  }
  FILETIME creation_time, exit_time, kernel_time, user_time;
  const BOOL ok = GetProcessTimes(handle, &creation_time, &exit_time,
                                  &kernel_time, &user_time);
  CloseHandle(handle);
  if (!ok) {
    return false;
  }
  *time = FileTimeToUint64(kernel_time) + FileTimeToUint64(user_time);
  return true;
}

std::unordered_map<DWORD, ULONGLONG> CaptureProcessTimes() {
  std::unordered_map<DWORD, ULONGLONG> times;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return times;
  }
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      ULONGLONG time;
      if (QueryProcessTime(entry.th32ProcessID, &time)) {
        times[entry.th32ProcessID] = time;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return times;
}

double ComputeCpuPercent(ULONGLONG delta_time, ULONGLONG elapsed_time,
                         DWORD processor_count) {
  if (elapsed_time == 0) {
    return 0.0;
  }
  const double cpu = static_cast<double>(delta_time) /
                     static_cast<double>(elapsed_time) /
                     static_cast<double>(processor_count);
  return cpu * 100.0;
}

std::vector<ProcessEntry> ListProcessesDetailed() {
  FILETIME start_time;
  FILETIME end_time;
  GetSystemTimeAsFileTime(&start_time);
  const auto start_times = CaptureProcessTimes();
  Sleep(200);
  GetSystemTimeAsFileTime(&end_time);
  const auto end_times = CaptureProcessTimes();
  const ULONGLONG elapsed_time =
      FileTimeToUint64(end_time) - FileTimeToUint64(start_time);
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  const DWORD processor_count = system_info.dwNumberOfProcessors;

  const std::vector<ProcessRecord> records = EnumerateProcesses();
  std::vector<ProcessEntry> entries;
  entries.reserve(records.size());
  for (const auto& record : records) {
    ProcessEntry entry;
    entry.pid = record.pid;
    entry.name = record.name;
    entry.cpu = 0.0;
    
    auto it_start = start_times.find(record.pid);
    auto it_end = end_times.find(record.pid);
    if (it_start != start_times.end() && it_end != end_times.end()) {
        ULONGLONG delta = 0;
        if (it_end->second > it_start->second) {
            delta = it_end->second - it_start->second;
        }
        entry.cpu = ComputeCpuPercent(delta, elapsed_time, processor_count);
    }
    
    // Filter: Only show processes with CPU > 0
    // if (entry.cpu <= 0.0) {
    //     continue;
    // }

    entry.icon_bytes.clear();
    std::wstring path;
    if (GetProcessImagePath(record.pid, &path)) {
      ExtractIconPng(path, &entry.icon_bytes);
    }
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const ProcessEntry& left, const ProcessEntry& right) {
              return left.cpu > right.cpu; // Sort by CPU usage descending
            });
  return entries;
}

struct WindowSearchContext {
  DWORD pid;
  std::wstring title_filter_lower;
  HWND hwnd;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lparam) {
  WindowSearchContext* context =
      reinterpret_cast<WindowSearchContext*>(lparam);
  DWORD window_pid = 0;
  GetWindowThreadProcessId(hwnd, &window_pid);
  if (window_pid != context->pid) {
    return TRUE;
  }
  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }
  if (!context->title_filter_lower.empty()) {
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
      return TRUE;
    }
    std::wstring title(static_cast<size_t>(length), L'\0');
    GetWindowTextW(hwnd, &title[0], length + 1);
    if (ToLower(title).find(context->title_filter_lower) ==
        std::wstring::npos) {
      return TRUE;
    }
  }
  context->hwnd = hwnd;
  return FALSE;
}

HWND FindWindowForProcess(DWORD pid, const std::wstring& title_filter) {
  WindowSearchContext context;
  context.pid = pid;
  context.title_filter_lower = ToLower(title_filter);
  context.hwnd = nullptr;
  EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&context));
  return context.hwnd;
}

std::unordered_map<DWORD, std::vector<DWORD>> BuildProcessChildrenMap(
    const std::vector<ProcessRecord>& records) {
  std::unordered_map<DWORD, std::vector<DWORD>> result;
  for (const auto& record : records) {
    result[record.parent_pid].push_back(record.pid);
  }
  return result;
}

std::vector<DWORD> CollectProcessTreePids(
    DWORD root_pid,
    const std::unordered_map<DWORD, std::vector<DWORD>>& children_map) {
  std::vector<DWORD> stack;
  std::vector<DWORD> result;
  std::set<DWORD> visited;
  stack.push_back(root_pid);
  while (!stack.empty()) {
    DWORD pid = stack.back();
    stack.pop_back();
    if (!visited.insert(pid).second) {
      continue;
    }
    result.push_back(pid);
    auto it = children_map.find(pid);
    if (it != children_map.end()) {
      for (DWORD child : it->second) {
        stack.push_back(child);
      }
    }
  }
  return result;
}

std::vector<DWORD> FindPidsByName(const std::wstring& process_name,
                                  const std::vector<ProcessRecord>& records) {
  std::vector<DWORD> result;
  const std::wstring target = ToLower(process_name);
  for (const auto& record : records) {
    if (ToLower(record.name) == target) {
      result.push_back(record.pid);
    }
  }
  return result;
}

int64_t WindowArea(HWND hwnd) {
  RECT rect;
  if (!GetClientRect(hwnd, &rect)) {
    return 0;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    return 0;
  }
  return static_cast<int64_t>(width) * static_cast<int64_t>(height);
}

struct WindowPickContext {
  const std::set<DWORD>* pids;
  HWND best;
  int64_t best_area;
};

HWND FindBestWindowForPids(const std::set<DWORD>& pids) {
  WindowPickContext context;
  context.pids = &pids;
  context.best = nullptr;
  context.best_area = 0;
  EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        auto* context = reinterpret_cast<WindowPickContext*>(lparam);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (context->pids->find(window_pid) == context->pids->end()) {
          return TRUE;
        }
        if (!IsWindowVisible(hwnd)) {
          return TRUE;
        }
        int64_t area = WindowArea(hwnd);
        if (area <= 0) {
          return TRUE;
        }
        if (!context->best || area > context->best_area) {
          context->best = hwnd;
          context->best_area = area;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  return context.best;
}

bool CaptureWindowToPngBytesWgc(HWND hwnd, std::vector<uint8_t>* output,
                                std::wstring* error) {
  if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
    if (error) {
      *error = L"Graphics capture not supported";
    }
    return false;
  }
  bool apartment_inited = false;
  try {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    apartment_inited = true;
  } catch (const winrt::hresult_error& e) {
    if (error) {
      *error = e.message().c_str();
    }
    return false;
  }
  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID3D11DeviceContext> d3d_context;
  D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, feature_levels,
      ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &d3d_device,
      &feature_level, &d3d_context);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create D3D device";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  ComPtr<IDXGIDevice> dxgi_device;
  hr = d3d_device.As(&dxgi_device);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to query DXGI device";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  winrt::com_ptr<IInspectable> inspectable_device;
  hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(),
                                            inspectable_device.put());
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create WinRT device";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  auto direct3d_device =
      inspectable_device
          .as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  auto interop =
      winrt::get_activation_factory<
          winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
          IGraphicsCaptureItemInterop>();
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  hr = interop->CreateForWindow(
      hwnd,
      winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
      winrt::put_abi(item));
  if (FAILED(hr) || !item) {
    if (error) {
      *error = L"Failed to create capture item";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  const auto size = item.Size();
  if (size.Width <= 0 || size.Height <= 0) {
    if (error) {
      *error = L"Invalid window size";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  auto frame_pool =
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
          direct3d_device,
          winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
              B8G8R8A8UIntNormalized,
          1, size);
  auto session = frame_pool.CreateCaptureSession(item);
  winrt::handle frame_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};
  if (!frame_event) {
    session.Close();
    frame_pool.Close();
    if (error) {
      *error = L"Failed to create capture event";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  auto token =
      frame_pool.FrameArrived([&](auto&, auto&) { SetEvent(frame_event.get()); });
  session.StartCapture();
  const DWORD wait = WaitForSingleObject(frame_event.get(), 1000);
  frame_pool.FrameArrived(token);
  if (wait != WAIT_OBJECT_0) {
    session.Close();
    frame_pool.Close();
    if (error) {
      *error = L"Capture timeout";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  auto frame = frame_pool.TryGetNextFrame();
  session.Close();
  frame_pool.Close();
  if (!frame) {
    if (error) {
      *error = L"Failed to get capture frame";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  auto surface = frame.Surface();
  auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  hr = access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void());
  if (FAILED(hr) || !texture) {
    if (error) {
      *error = L"Failed to read capture texture";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  // Calculate crop
  UINT client_width = desc.Width;
  UINT client_height = desc.Height;
  UINT offset_x = 0;
  UINT offset_y = 0;

  if (IsWindow(hwnd)) {
       RECT client_rect;
       if (GetClientRect(hwnd, &client_rect)) {
           POINT pt = {0, 0};
           ClientToScreen(hwnd, &pt);
           
           RECT window_rect;
           if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect, sizeof(window_rect)) != S_OK) {
               GetWindowRect(hwnd, &window_rect);
           }
           
           int off_x = pt.x - window_rect.left;
           int off_y = pt.y - window_rect.top;
           int c_w = client_rect.right - client_rect.left;
           int c_h = client_rect.bottom - client_rect.top;
           
           if (off_x < 0) off_x = 0;
           if (off_y < 0) off_y = 0;
           if (off_x + c_w > (int)desc.Width) c_w = desc.Width - off_x;
           if (off_y + c_h > (int)desc.Height) c_h = desc.Height - off_y;

           client_width = (UINT)c_w;
           client_height = (UINT)c_h;
           offset_x = (UINT)off_x;
           offset_y = (UINT)off_y;
       }
  }

  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Width = client_width;
  staging_desc.Height = client_height;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  hr = d3d_device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create staging texture";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  
  D3D11_BOX src_box;
  src_box.left = offset_x;
  src_box.top = offset_y;
  src_box.front = 0;
  src_box.right = offset_x + client_width;
  src_box.bottom = offset_y + client_height;
  src_box.back = 1;
  d3d_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.get(), 0, &src_box);

  D3D11_MAPPED_SUBRESOURCE mapped;
  hr = d3d_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to map capture texture";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  const uint64_t buffer_size =
      static_cast<uint64_t>(mapped.RowPitch) * staging_desc.Height;
  if (buffer_size == 0 || buffer_size > static_cast<uint64_t>(UINT_MAX)) {
    d3d_context->Unmap(staging.Get(), 0);
    if (error) {
      *error = L"Invalid capture buffer";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<IWICImagingFactory> factory;
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    d3d_context->Unmap(staging.Get(), 0);
    if (error) {
      *error = L"Failed to init encoder";
    }
    if (SUCCEEDED(com_init)) {
      CoUninitialize();
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  ComPtr<IWICBitmap> wic_bitmap;
  hr = factory->CreateBitmapFromMemory(
      staging_desc.Width, staging_desc.Height, GUID_WICPixelFormat32bppBGRA,
      mapped.RowPitch, static_cast<UINT>(buffer_size),
      static_cast<BYTE*>(mapped.pData), &wic_bitmap);
  d3d_context->Unmap(staging.Get(), 0);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to convert capture bitmap";
    }
    if (SUCCEEDED(com_init)) {
      CoUninitialize();
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  const bool ok =
      EncodeWicBitmapToPng(factory.Get(), wic_bitmap.Get(), output, error);
  if (SUCCEEDED(com_init)) {
    CoUninitialize();
  }
  if (apartment_inited) {
    winrt::uninit_apartment();
  }
  return ok;
}

bool CaptureWindowToPngBytes(HWND hwnd, std::vector<uint8_t>* output,
                             std::wstring* error) {
  RECT rect;
  if (!GetClientRect(hwnd, &rect)) {
    if (error) {
      *error = L"Failed to get window size";
    }
    return false;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    if (error) {
      *error = L"Invalid window size";
    }
    return false;
  }
  HDC hdc_window = GetDC(hwnd);
  if (!hdc_window) {
    if (error) {
      *error = L"Failed to get window DC";
    }
    return false;
  }
  HDC hdc_mem = CreateCompatibleDC(hdc_window);
  HBITMAP bitmap = CreateCompatibleBitmap(hdc_window, width, height);
  if (!hdc_mem || !bitmap) {
    if (error) {
      *error = L"Failed to create bitmap";
    }
    if (bitmap) {
      DeleteObject(bitmap);
    }
    if (hdc_mem) {
      DeleteDC(hdc_mem);
    }
    ReleaseDC(hwnd, hdc_window);
    return false;
  }
  HGDIOBJ old_object = SelectObject(hdc_mem, bitmap);
  // Manual capture: Prefer PrintWindow as it handles most windows better (including partially obscured ones)
  // Fallback to BitBlt if PrintWindow fails, then to WGC.
  BOOL ok = PrintWindow(hwnd, hdc_mem, 0x00000003); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
  if (!ok) {
      // Try standard PrintWindow
      ok = PrintWindow(hwnd, hdc_mem, 0x00000001); // PW_CLIENTONLY
  }
  if (!ok) {
      // Try BitBlt as last resort for GDI
      ok = BitBlt(hdc_mem, 0, 0, width, height, hdc_window, 0, 0, SRCCOPY | CAPTUREBLT);
  }

  if (!ok) {
    SelectObject(hdc_mem, old_object);
    ReleaseDC(hwnd, hdc_window);
    DeleteObject(bitmap);
    DeleteDC(hdc_mem);
    return CaptureWindowToPngBytesWgc(hwnd, output, error);
  }
  SelectObject(hdc_mem, old_object);
  ReleaseDC(hwnd, hdc_window);
  const bool encoded = EncodeBitmapToPng(bitmap, output, error);
  DeleteObject(bitmap);
  DeleteDC(hdc_mem);
  return encoded;
}

bool CaptureWindowToPngBytesPrintWindow(HWND hwnd, std::vector<uint8_t>* output,
                                        std::wstring* error) {
  RECT rect;
  if (!GetClientRect(hwnd, &rect)) {
    if (error) {
      *error = L"Failed to get window size";
    }
    return false;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    if (error) {
      *error = L"Invalid window size";
    }
    return false;
  }
  HDC hdc_window = GetDC(hwnd);
  if (!hdc_window) {
    if (error) {
      *error = L"Failed to get window DC";
    }
    return false;
  }
  HDC hdc_mem = CreateCompatibleDC(hdc_window);
  HBITMAP bitmap = CreateCompatibleBitmap(hdc_window, width, height);
  if (!hdc_mem || !bitmap) {
    if (error) {
      *error = L"Failed to create bitmap";
    }
    if (bitmap) {
      DeleteObject(bitmap);
    }
    if (hdc_mem) {
      DeleteDC(hdc_mem);
    }
    ReleaseDC(hwnd, hdc_window);
    return false;
  }
  HGDIOBJ old_object = SelectObject(hdc_mem, bitmap);
  BOOL ok = PrintWindow(hwnd, hdc_mem, 0x00000003); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
  if (!ok) {
    SelectObject(hdc_mem, old_object);
    ReleaseDC(hwnd, hdc_window);
    DeleteObject(bitmap);
    DeleteDC(hdc_mem);
    if (error) {
      *error = L"PrintWindow failed";
    }
    return false;
  }
  SelectObject(hdc_mem, old_object);
  ReleaseDC(hwnd, hdc_window);
  const bool encoded = EncodeBitmapToPng(bitmap, output, error);
  DeleteObject(bitmap);
  DeleteDC(hdc_mem);
  return encoded;
}
}  

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {
  // StopCaptureSession(nullptr);
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  
  try {
      auto registrar_ref = flutter_controller_->engine()->GetRegistrarForPlugin("gamemapstool_internal");
      plugin_registrar_ = std::make_unique<flutter::PluginRegistrarWindows>(registrar_ref);

      capture_texture_ = std::make_unique<CaptureTexture>(
          plugin_registrar_->texture_registrar());

      capture_channel_ = std::make_unique<
          flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "gamemapstool/capture",
          &flutter::StandardMethodCodec::GetInstance());
  } catch (const std::exception& e) {
      std::cerr << "Exception in OnCreate setup: " << e.what() << std::endl;
      return false;
  } catch (...) {
      std::cerr << "Unknown exception in OnCreate setup" << std::endl;
      return false;
  }
  capture_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) {
        if (call.method_name() == "startCaptureSession") {
          StartCaptureSession(call, std::move(result));
          return;
        }
        if (call.method_name() == "stopCaptureSession") {
          StopCaptureSession(std::move(result));
          return;
        }
        if (call.method_name() == "getTextureId") {
          GetTextureId(std::move(result));
          return;
        }
        if (call.method_name() == "getCaptureFrame") {
          GetCaptureFrame(call, std::move(result));
          return;
        }
        if (call.method_name() == "getLastFrame") {
          GetLastFrame(std::move(result));
          return;
        }
        if (call.method_name() == "listProcesses") {
          const std::vector<ProcessEntry> processes = ListProcessesDetailed();
          flutter::EncodableList list;
          list.reserve(processes.size());
          for (const auto& entry : processes) {
            flutter::EncodableMap item;
            item[flutter::EncodableValue("pid")] =
                flutter::EncodableValue(static_cast<int64_t>(entry.pid));
            item[flutter::EncodableValue("name")] =
                flutter::EncodableValue(WideToUtf8(entry.name));
            item[flutter::EncodableValue("cpu")] =
                flutter::EncodableValue(entry.cpu);
            item[flutter::EncodableValue("icon")] =
                flutter::EncodableValue(entry.icon_bytes);
            list.emplace_back(item);
          }
          result->Success(flutter::EncodableValue(list));
          return;
        }
        if (call.method_name() != "capture") {
          result->NotImplemented();
          return;
        }
        const auto* args =
            std::get_if<flutter::EncodableMap>(call.arguments());
        if (!args) {
          result->Error("bad-args", "Missing arguments");
          return;
        }
        DWORD target_pid = 0;
        bool has_pid = false;
        auto pid_it = args->find(flutter::EncodableValue("pid"));
        if (pid_it != args->end()) {
          if (std::holds_alternative<int32_t>(pid_it->second)) {
            target_pid =
                static_cast<DWORD>(std::get<int32_t>(pid_it->second));
            has_pid = target_pid != 0;
          } else if (std::holds_alternative<int64_t>(pid_it->second)) {
            target_pid =
                static_cast<DWORD>(std::get<int64_t>(pid_it->second));
            has_pid = target_pid != 0;
          }
        }
        std::wstring process_name;
        std::string capture_mode = "auto";
        auto mode_it = args->find(flutter::EncodableValue("mode"));
        if (mode_it != args->end() &&
      std::holds_alternative<std::string>(mode_it->second)) {
    capture_mode = std::get<std::string>(mode_it->second);
  }
  if (!has_pid) {
          auto process_it = args->find(flutter::EncodableValue("processName"));
          if (process_it == args->end() ||
              !std::holds_alternative<std::string>(process_it->second)) {
            result->Error("bad-args", "Invalid processName");
            return;
          }
          std::string process_name_utf8 =
              std::get<std::string>(process_it->second);
          process_name = Utf8ToWide(process_name_utf8);
        }
        const std::vector<ProcessRecord> records = EnumerateProcesses();
        const auto children_map = BuildProcessChildrenMap(records);
        std::set<DWORD> candidate_pids;
        if (has_pid) {
          const std::vector<DWORD> subtree =
              CollectProcessTreePids(target_pid, children_map);
          candidate_pids.insert(subtree.begin(), subtree.end());
        } else {
          const std::vector<DWORD> pids =
              FindPidsByName(process_name, records);
          if (pids.empty()) {
            result->Error("not-found", "Target process not found");
            return;
          }
          for (DWORD pid : pids) {
            const std::vector<DWORD> subtree =
                CollectProcessTreePids(pid, children_map);
            candidate_pids.insert(subtree.begin(), subtree.end());
          }
        }
        HWND hwnd = FindBestWindowForPids(candidate_pids);
        if (!hwnd) {
          result->Error("not-found", "Target window not found");
          return;
        }
        std::vector<uint8_t> png_bytes;
        std::wstring error;
        bool captured = false;
        if (capture_mode == "wgc") {
          captured = CaptureWindowToPngBytesWgc(hwnd, &png_bytes, &error);
        } else if (capture_mode == "printWindow") {
          captured = CaptureWindowToPngBytesPrintWindow(hwnd, &png_bytes, &error);
        } else {
          captured = CaptureWindowToPngBytes(hwnd, &png_bytes, &error);
        }
        if (!captured) {
          result->Error("capture-failed",
                        error.empty() ? "Capture failed" : WideToUtf8(error));
          return;
        }
        result->Success(flutter::EncodableValue(png_bytes));
      });
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
    case WM_CAPTURE_COMPLETE:
      start_task_running_ = false;
      if (pending_start_result_) {
        if (wparam == 1) { // Success
          pending_start_result_->Success();
        } else { // Failure
          pending_start_result_->Error("start-failed", pending_start_error_.empty() ? "Capture failed" : pending_start_error_);
        }
        pending_start_result_ = nullptr;
      }
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

void FlutterWindow::GdiCaptureLoop(HWND hwnd, std::string mode) {
  // 1. Initial setup
  HDC hdc_window = GetDC(hwnd);
  if (!hdc_window) {
      // Fallback if we can't get DC
      return;
  }
  HDC hdc_mem = CreateCompatibleDC(hdc_window);
  HBITMAP hbitmap = nullptr;
  HGDIOBJ old_object = nullptr;
  void* bits = nullptr;
  std::vector<uint8_t> buffer;
  int last_width = 0;
  int last_height = 0;

  // Common BMI for 32-bit RGB
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  while (gdi_capturing_) {
    try {
      auto start_time = std::chrono::steady_clock::now();

      // 2. Check window validity
      if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      // 3. Get Window Size
      RECT rect;
      if (!GetClientRect(hwnd, &rect)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
      }
      int width = rect.right - rect.left;
      int height = rect.bottom - rect.top;

      if (width <= 0 || height <= 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
      }

      // 4. Recreate bitmap if size changed or first run
      if (!hbitmap || width != last_width || height != last_height) {
          if (old_object) SelectObject(hdc_mem, old_object);
          if (hbitmap) DeleteObject(hbitmap);

          bmi.bmiHeader.biWidth = width;
          bmi.bmiHeader.biHeight = -height; // Top-down

          // Always use DIB Section for direct access and best performance
          // This avoids GetDIBits overhead and potential failure points
          hbitmap = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

          if (!hbitmap) {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
              continue;
          }
          old_object = SelectObject(hdc_mem, hbitmap);
          last_width = width;
          last_height = height;
      }

      // 5. Perform Capture
      bool success = false;
      if (mode == "bitblt") {
          success = BitBlt(hdc_mem, 0, 0, width, height, hdc_window, 0, 0, SRCCOPY | CAPTUREBLT);
      } else {
          // PrintWindow with PW_CLIENTONLY | PW_RENDERFULLCONTENT (0x3)
          success = PrintWindow(hwnd, hdc_mem, 0x3);
          if (!success) {
              success = PrintWindow(hwnd, hdc_mem, 0x2);
          }
      }

      if (success && capture_texture_) {
          // 6. Update Texture
          // Direct access via bits pointer (DIB Section)
          // Alpha channel is often 0 for GDI capture, so force_opaque=true is critical
          capture_texture_->UpdateFrame(static_cast<uint8_t*>(bits), width, height, width * 4, true);
      }

      // 7. Frame pacing (Aim for ~30 FPS = 33ms)
      auto end_time = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
      if (elapsed < 33) {
          std::this_thread::sleep_for(std::chrono::milliseconds(33 - elapsed));
      }
    } catch (...) {
      // Prevent thread crash, just sleep and retry
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // 8. Cleanup
  if (old_object) SelectObject(hdc_mem, old_object);
  if (hbitmap) DeleteObject(hbitmap);
  if (hdc_mem) DeleteDC(hdc_mem);
  if (hdc_window) ReleaseDC(hwnd, hdc_window);
}

void FlutterWindow::StartCaptureSession(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

  const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
  if (!args) {
    result->Error("bad-args", "Missing arguments");
    return;
  }

  DWORD target_pid = 0;
  bool has_pid = false;
  auto pid_it = args->find(flutter::EncodableValue("pid"));
  if (pid_it != args->end()) {
    if (std::holds_alternative<int32_t>(pid_it->second)) {
      target_pid = static_cast<DWORD>(std::get<int32_t>(pid_it->second));
      has_pid = target_pid != 0;
    } else if (std::holds_alternative<int64_t>(pid_it->second)) {
      target_pid = static_cast<DWORD>(std::get<int64_t>(pid_it->second));
      has_pid = target_pid != 0;
    }
  }

  std::wstring process_name;
  if (!has_pid) {
    auto process_it = args->find(flutter::EncodableValue("processName"));
    if (process_it != args->end() && std::holds_alternative<std::string>(process_it->second)) {
       process_name = Utf8ToWide(std::get<std::string>(process_it->second));
    }
  }

  std::string capture_mode = "wgc";
  auto mode_it = args->find(flutter::EncodableValue("mode"));
  if (mode_it != args->end() && std::holds_alternative<std::string>(mode_it->second)) {
      capture_mode = std::get<std::string>(mode_it->second);
  }
  
  if (start_task_running_) {
    result->Error("busy", "Capture session is starting");
    return;
  }
  start_task_running_ = true;

  // Store the result to be completed later
  pending_start_result_ = std::move(result);
  pending_start_error_.clear();

  // Spawn a thread to perform the setup asynchronously
  std::thread([this, target_pid, has_pid, process_name, capture_mode]() {
    try {
      // 1. Stop previous session (this might block joining threads)
      StopCaptureSession(nullptr);

      // 2. Find target window
      const std::vector<ProcessRecord> records = EnumerateProcesses();
      const auto children_map = BuildProcessChildrenMap(records);
      std::set<DWORD> candidate_pids;
      if (has_pid) {
        const std::vector<DWORD> subtree = CollectProcessTreePids(target_pid, children_map);
        candidate_pids.insert(subtree.begin(), subtree.end());
      } else if (!process_name.empty()) {
        const std::vector<DWORD> pids = FindPidsByName(process_name, records);
        for (DWORD pid : pids) {
          const std::vector<DWORD> subtree = CollectProcessTreePids(pid, children_map);
          candidate_pids.insert(subtree.begin(), subtree.end());
        }
      }
      
      HWND hwnd = FindBestWindowForPids(candidate_pids);
      if (!hwnd) {
        throw std::runtime_error("Target window not found");
      }

      // 3. Start Capture
      if (capture_mode != "wgc") {
          gdi_capturing_ = true;
          gdi_capture_thread_ = std::thread(&FlutterWindow::GdiCaptureLoop, this, hwnd, capture_mode);
      } else {
          HRESULT hr = D3D11CreateDevice(
              nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
              nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &d3d11_context_);
          
          if (FAILED(hr)) throw std::runtime_error("Failed to create D3D device");

          ComPtr<IDXGIDevice> dxgi_device;
          hr = d3d11_device_.As(&dxgi_device);
          if (FAILED(hr)) throw std::runtime_error("Failed to get DXGI device");

          winrt::com_ptr<IInspectable> inspectable_device;
          hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable_device.put());
          if (FAILED(hr)) throw std::runtime_error("Failed to create WinRT device");

          device_ = inspectable_device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

          auto interop = winrt::get_activation_factory<
              winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
              IGraphicsCaptureItemInterop>();

          hr = interop->CreateForWindow(
              hwnd,
              winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
              winrt::put_abi(item_));

          if (FAILED(hr) || !item_) throw std::runtime_error("Failed to create capture item");

          auto size = item_.Size();

          try {
            frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                device_,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1, size);
          } catch (...) {
             frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
                device_,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1, size);
          }

          session_ = frame_pool_.CreateCaptureSession(item_);
          session_.IsCursorCaptureEnabled(false);

          frame_arrived_token_ = frame_pool_.FrameArrived({this, &FlutterWindow::OnFrameArrived});

          current_capture_hwnd_ = hwnd;
          session_.StartCapture();
          {
              std::lock_guard<std::mutex> lock(frame_mutex_);
              is_capturing_ = true;
          }
      }

      // Success
      PostMessage(GetHandle(), WM_CAPTURE_COMPLETE, 1, 0);

    } catch (const std::exception& e) {
      pending_start_error_ = e.what();
      PostMessage(GetHandle(), WM_CAPTURE_COMPLETE, 0, 0);
    } catch (...) {
      pending_start_error_ = "Unknown error during capture setup";
      PostMessage(GetHandle(), WM_CAPTURE_COMPLETE, 0, 0);
    }
  }).detach();
}

void FlutterWindow::StopCaptureSession(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (gdi_capturing_) {
    gdi_capturing_ = false;
    if (gdi_capture_thread_.joinable()) {
      gdi_capture_thread_.join();
    }
  }

  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    is_capturing_ = false;
    current_capture_hwnd_ = nullptr;
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    if (frame_pool_) {
      frame_pool_.Close();
      frame_pool_ = nullptr;
    }
    item_ = nullptr;
    device_ = nullptr;
    d3d11_context_ = nullptr;
    staging_texture_ = nullptr;
    last_frame_data_.clear();
  }
  if (result) {
    result->Success();
  }
}

void FlutterWindow::GetCaptureFrame(const flutter::MethodCall<flutter::EncodableValue>& call, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  // 1. Try fast path if actively capturing
  if (is_capturing_ && capture_texture_) {
    std::vector<uint8_t> buffer;
    size_t width, height;
    if (capture_texture_->GetContent(&buffer, &width, &height)) {
       HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
       ComPtr<IWICImagingFactory> factory;
       HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
       if (SUCCEEDED(hr)) {
           ComPtr<IWICBitmap> wic_bitmap;
           hr = factory->CreateBitmapFromMemory(
               static_cast<UINT>(width), static_cast<UINT>(height),
               GUID_WICPixelFormat32bppRGBA,
               static_cast<UINT>(width * 4),
               static_cast<UINT>(buffer.size()),
               buffer.data(),
               &wic_bitmap);
           if (SUCCEEDED(hr)) {
               std::vector<uint8_t> png_bytes;
               std::wstring error;
               if (EncodeWicBitmapToPng(factory.Get(), wic_bitmap.Get(), &png_bytes, &error)) {
                   if (SUCCEEDED(com_init)) CoUninitialize();
                   result->Success(flutter::EncodableValue(png_bytes));
                   return;
               }
           }
       }
       if (SUCCEEDED(com_init)) CoUninitialize();
    }
  }

  // 2. Slow path (manual capture or fallback)
  const auto* arguments = std::get_if<flutter::EncodableMap>(call.arguments());
  if (!arguments) {
      result->Error("invalid_arguments", "Arguments must be a map");
      return;
  }
  
  int64_t pid = 0;
  auto pid_it = arguments->find(flutter::EncodableValue("pid"));
  if (pid_it != arguments->end()) {
      if (std::holds_alternative<int32_t>(pid_it->second))
          pid = std::get<int32_t>(pid_it->second);
      else if (std::holds_alternative<int64_t>(pid_it->second))
          pid = std::get<int64_t>(pid_it->second);
  }

  if (pid <= 0) {
      result->Error("invalid_pid", "PID must be positive");
      return;
  }

  std::set<DWORD> pids;
  pids.insert(static_cast<DWORD>(pid));
  HWND hwnd = FindBestWindowForPids(pids);
  
  if (!hwnd) {
      result->Error("window_not_found", "Could not find window for PID");
      return;
  }

  std::vector<uint8_t> png_bytes;
  std::wstring error;
  // Try WGC capture first
  if (CaptureWindowToPngBytesWgc(hwnd, &png_bytes, &error)) {
      result->Success(flutter::EncodableValue(png_bytes));
      return;
  }
  
  // Try GDI capture as fallback
  if (CaptureWindowToPngBytes(hwnd, &png_bytes, &error)) {
      result->Success(flutter::EncodableValue(png_bytes));
      return;
  }

  // Convert wstring error to string (basic conversion)
  std::string error_str = Utf8FromUtf16(error.c_str());
  result->Error("capture_failed", error_str.empty() ? "Failed to capture window" : error_str);
}

void FlutterWindow::GetTextureId(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (capture_texture_) {
    size_t width = 0;
    size_t height = 0;
    capture_texture_->GetSize(&width, &height);
    
    flutter::EncodableMap map;
    map[flutter::EncodableValue("id")] = flutter::EncodableValue(capture_texture_->id());
    map[flutter::EncodableValue("width")] = flutter::EncodableValue((int64_t)width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue((int64_t)height);
    
    result->Success(flutter::EncodableValue(map));
  } else {
    result->Error("NO_TEXTURE", "Capture texture not initialized");
  }
}

void FlutterWindow::GetLastFrame(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (!capture_texture_) {
    result->Error("NO_TEXTURE", "Capture texture not initialized");
    return;
  }

  std::vector<uint8_t> buffer;
  size_t width, height;
  if (!capture_texture_->GetContent(&buffer, &width, &height)) {
    result->Error("NO_CONTENT", "No content in texture");
    return;
  }

  HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
     if (SUCCEEDED(com_init)) CoUninitialize();
     result->Error("WIC_ERROR", "Failed to create WIC factory");
     return;
  }

  ComPtr<IWICBitmap> wic_bitmap;
  hr = factory->CreateBitmapFromMemory(
      static_cast<UINT>(width), static_cast<UINT>(height),
      GUID_WICPixelFormat32bppRGBA,
      static_cast<UINT>(width * 4),
      static_cast<UINT>(buffer.size()),
      buffer.data(),
      &wic_bitmap);

  if (FAILED(hr)) {
     if (SUCCEEDED(com_init)) CoUninitialize();
     result->Error("WIC_ERROR", "Failed to create bitmap from memory");
     return;
  }

  std::vector<uint8_t> output;
  std::wstring error;
  if (EncodeWicBitmapToPng(factory.Get(), wic_bitmap.Get(), &output, &error)) {
      result->Success(flutter::EncodableValue(output));
  } else {
      result->Error("ENCODE_ERROR", Utf8FromUtf16(error.c_str()));
  }

  if (SUCCEEDED(com_init)) CoUninitialize();
}

void FlutterWindow::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const& args) {

    try {
        // 1. Acquire lock to safely access members and check state
        std::unique_lock<std::mutex> lock(frame_mutex_);
        if (!is_capturing_ || !d3d11_context_ || !d3d11_device_) return;
        
        // 2. Get the frame (must be done before releasing lock? No, sender is thread safe, but safe to do here)
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        auto surface = frame.Surface();
        auto interop_surface = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = interop_surface->GetInterface(IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
             // If device lost, we might need to handle it. For now, just log and return.
             // OutputDebugStringA("Failed to get texture interface\n");
             return;
        }

        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        
        // Calculate Crop Region
        UINT client_width = desc.Width;
        UINT client_height = desc.Height;
        UINT offset_x = 0;
        UINT offset_y = 0;

        if (current_capture_hwnd_ && IsWindow(current_capture_hwnd_)) {
             RECT client_rect;
             if (GetClientRect(current_capture_hwnd_, &client_rect)) {
                 POINT pt = {0, 0};
                 ClientToScreen(current_capture_hwnd_, &pt);
                 
                 RECT window_rect;
                 if (DwmGetWindowAttribute(current_capture_hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect, sizeof(window_rect)) != S_OK) {
                     GetWindowRect(current_capture_hwnd_, &window_rect);
                 }
                 
                 int off_x = pt.x - window_rect.left;
                 int off_y = pt.y - window_rect.top;
                 int c_w = client_rect.right - client_rect.left;
                 int c_h = client_rect.bottom - client_rect.top;
                 
                 if (off_x < 0) off_x = 0;
                 if (off_y < 0) off_y = 0;
                 if (off_x + c_w > (int)desc.Width) c_w = desc.Width - off_x;
                 if (off_y + c_h > (int)desc.Height) c_h = desc.Height - off_y;

                 client_width = (UINT)c_w;
                 client_height = (UINT)c_h;
                 offset_x = (UINT)off_x;
                 offset_y = (UINT)off_y;
             }
        }

        // 3. Check/Update staging texture (protected by lock)
        if (!staging_texture_ || 
            staging_desc_.Width != client_width || 
            staging_desc_.Height != client_height ||
            staging_desc_.Format != desc.Format) {
            
            staging_texture_ = nullptr;
            D3D11_TEXTURE2D_DESC new_desc = desc;
            new_desc.Width = client_width;
            new_desc.Height = client_height;
            new_desc.BindFlags = 0;
            new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            new_desc.Usage = D3D11_USAGE_STAGING;
            new_desc.MiscFlags = 0;
            
            hr = d3d11_device_->CreateTexture2D(&new_desc, nullptr, &staging_texture_);
            if (FAILED(hr)) return;
            staging_desc_ = new_desc;
        }

        // 4. Create local copies to keep objects alive while unlocked
        ComPtr<ID3D11DeviceContext> local_context = d3d11_context_;
        ComPtr<ID3D11Texture2D> local_staging = staging_texture_;

        // 5. Unlock to allow UI thread to call GetCaptureFrame without blocking
        lock.unlock();

        // 6. Perform heavy operations (GPU Copy, Map, Memcpy)
        // Use CopySubresourceRegion to crop
        D3D11_BOX src_box;
        src_box.left = offset_x;
        src_box.top = offset_y;
        src_box.front = 0;
        src_box.right = offset_x + client_width;
        src_box.bottom = offset_y + client_height;
        src_box.back = 1;
        
        local_context->CopySubresourceRegion(local_staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &src_box);
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = local_context->Map(local_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return;
        
        if (capture_texture_) {
            capture_texture_->UpdateFrame((uint8_t*)mapped.pData, client_width, client_height, mapped.RowPitch);
        }
        
        local_context->Unmap(local_staging.Get(), 0);
    } catch (...) {
        // Catch all exceptions to prevent crash from winrt or other issues
        // OutputDebugStringA("Exception in OnFrameArrived\n");
    }
}
