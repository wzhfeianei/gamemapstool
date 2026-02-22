#include "flutter_window.h"

#include <optional>
#include <algorithm>
#include <cwctype>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

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

ULONGLONG FileTimeToUint64(const FILETIME& time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

bool QueryProcessTime(DWORD pid, ULONGLONG* time) {
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!handle) {
    return false;
  }
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;
  const BOOL ok =
      GetProcessTimes(handle, &creation_time, &exit_time, &kernel_time,
                      &user_time);
  CloseHandle(handle);
  if (!ok) {
    return false;
  }
  *time = FileTimeToUint64(kernel_time) + FileTimeToUint64(user_time);
  return true;
}

std::unordered_map<DWORD, ULONGLONG> CaptureProcessTimes() {
  std::unordered_map<DWORD, ULONGLONG> times;
  const std::vector<ProcessRecord> records = EnumerateProcesses();
  for (const auto& record : records) {
    ULONGLONG value = 0;
    if (QueryProcessTime(record.pid, &value)) {
      times[record.pid] = value;
    }
  }
  return times;
}

double ComputeCpuPercent(ULONGLONG delta_time, ULONGLONG elapsed_time,
                         DWORD processor_count) {
  if (elapsed_time == 0 || processor_count == 0) {
    return 0.0;
  }
  const double usage =
      (static_cast<double>(delta_time) / static_cast<double>(elapsed_time)) *
      100.0 / static_cast<double>(processor_count);
  if (usage < 0.0) {
    return 0.0;
  }
  return usage;
}

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
    entry.icon_bytes.clear();
    auto start_it = start_times.find(record.pid);
    auto end_it = end_times.find(record.pid);
    if (start_it != start_times.end() && end_it != end_times.end()) {
      ULONGLONG delta = end_it->second - start_it->second;
      entry.cpu = ComputeCpuPercent(delta, elapsed_time, processor_count);
    }
    std::wstring path;
    if (GetProcessImagePath(record.pid, &path)) {
      ExtractIconPng(path, &entry.icon_bytes);
    }
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const ProcessEntry& left, const ProcessEntry& right) {
              if (left.cpu == right.cpu) {
                return left.name < right.name;
              }
              return left.cpu > right.cpu;
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
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  hr = d3d_device->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr)) {
    if (error) {
      *error = L"Failed to create staging texture";
    }
    if (apartment_inited) {
      winrt::uninit_apartment();
    }
    return false;
  }
  d3d_context->CopyResource(staging.Get(), texture.get());
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
      static_cast<uint64_t>(mapped.RowPitch) * desc.Height;
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
      desc.Width, desc.Height, GUID_WICPixelFormat32bppBGRA,
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
  BOOL ok = PrintWindow(hwnd, hdc_mem, 0x00000002);
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
  BOOL ok = PrintWindow(hwnd, hdc_mem, 0x00000002);
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

FlutterWindow::~FlutterWindow() {}

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
  capture_channel_ = std::make_unique<
      flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "gamemapstool/capture",
      &flutter::StandardMethodCodec::GetInstance());
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
        if (call.method_name() == "getCaptureFrame") {
          GetCaptureFrame(std::move(result));
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
        if (capture_mode == "printWindow") {
          captured = CaptureWindowToPngBytesPrintWindow(hwnd, &png_bytes, &error);
        } else if (capture_mode == "wgc") {
          captured = CaptureWindowToPngBytesWgc(hwnd, &png_bytes, &error);
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
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
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
    result->Error("not-found", "Target window not found");
    return;
  }

  StopCaptureSession(nullptr);

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr, 0, D3D11_SDK_VERSION, &d3d11_device_, nullptr, &d3d11_context_);
  
  if (FAILED(hr)) {
    result->Error("d3d-error", "Failed to create D3D device");
    return;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  hr = d3d11_device_.As(&dxgi_device);
  if (FAILED(hr)) {
     result->Error("dxgi-error", "Failed to get DXGI device");
     return;
  }

  winrt::com_ptr<IInspectable> inspectable_device;
  hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable_device.put());
  if (FAILED(hr)) {
     result->Error("winrt-error", "Failed to create WinRT device");
     return;
  }

  device_ = inspectable_device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

  auto interop = winrt::get_activation_factory<
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
      IGraphicsCaptureItemInterop>();

  hr = interop->CreateForWindow(
      hwnd,
      winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
      winrt::put_abi(item_));

  if (FAILED(hr) || !item_) {
    result->Error("capture-item-error", "Failed to create capture item");
    return;
  }

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

  try {
    session_.StartCapture();
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        is_capturing_ = true;
    }
    result->Success();
  } catch (...) {
    StopCaptureSession(nullptr);
    result->Error("start-failed", "Failed to start capture");
  }
}

void FlutterWindow::StopCaptureSession(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    is_capturing_ = false;
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

void FlutterWindow::GetCaptureFrame(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  std::vector<uint8_t> data;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    data = last_frame_data_;
  }
  if (data.empty()) {
      result->Success(flutter::EncodableValue(std::vector<uint8_t>()));
  } else {
      result->Success(flutter::EncodableValue(data));
  }
}

void FlutterWindow::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const& args) {

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
    if (FAILED(hr)) return;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    
    // 3. Check/Update staging texture (protected by lock)
    if (!staging_texture_ || 
        staging_desc_.Width != desc.Width || 
        staging_desc_.Height != desc.Height ||
        staging_desc_.Format != desc.Format) {
        
        staging_texture_ = nullptr;
        D3D11_TEXTURE2D_DESC new_desc = desc;
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
    local_context->CopyResource(local_staging.Get(), texture.Get());
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = local_context->Map(local_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return;
    
    int width = desc.Width;
    int height = desc.Height;
    int src_stride = mapped.RowPitch;
    // 24-bit BMP stride must be a multiple of 4 bytes
    int dst_stride = (width * 3 + 3) & ~3;
    int data_size = dst_stride * height;
    int file_size = 54 + data_size;
    
    std::vector<uint8_t> bmp(file_size);
    
    // BITMAPFILEHEADER
    bmp[0] = 'B'; bmp[1] = 'M';
    *(int32_t*)&bmp[2] = file_size;
    *(int32_t*)&bmp[6] = 0;
    *(int32_t*)&bmp[10] = 54;
    
    // BITMAPINFOHEADER
    *(int32_t*)&bmp[14] = 40;
    *(int32_t*)&bmp[18] = width;
    *(int32_t*)&bmp[22] = -height; // Top-down
    *(int16_t*)&bmp[26] = 1;
    *(int16_t*)&bmp[28] = 24; // 24-bit BGR (no alpha)
    *(int32_t*)&bmp[30] = 0; // BI_RGB
    *(int32_t*)&bmp[34] = data_size;
    *(int32_t*)&bmp[38] = 0;
    *(int32_t*)&bmp[42] = 0;
    *(int32_t*)&bmp[46] = 0;
    *(int32_t*)&bmp[50] = 0;
    
    uint8_t* src_base = (uint8_t*)mapped.pData;
    uint8_t* dst_base = &bmp[54];
    
    for (int y = 0; y < height; ++y) {
        uint8_t* src_row = src_base + y * src_stride;
        uint8_t* dst_row = dst_base + y * dst_stride;
        for (int x = 0; x < width; ++x) {
            // BGRA -> BGR
            dst_row[x * 3 + 0] = src_row[x * 4 + 0]; // B
            dst_row[x * 3 + 1] = src_row[x * 4 + 1]; // G
            dst_row[x * 3 + 2] = src_row[x * 4 + 2]; // R
        }
    }
    
    local_context->Unmap(local_staging.Get(), 0);
    
    // 7. Re-acquire lock to update shared state
    lock.lock();
    last_frame_data_ = std::move(bmp);
}
