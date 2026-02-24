import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:win32/win32.dart';

// Define a struct to pass data to the EnumWindows callback
final class _SearchContext extends Struct {
  @Int32()
  external int pid;

  // We'll store potential matches here.
  // For simplicity, let's just store the best one found so far.
  // Criteria: visible, has title, no parent.
  @Int32()
  external int bestHwnd;

  @Int32()
  external int fallbackHwnd;
}

// Top-level callback function for EnumWindows
int _enumWindowsCallback(int hwnd, int lParam) {
  final context = Pointer<_SearchContext>.fromAddress(lParam);
  final pid = context.ref.pid;

  final processIdPtr = calloc<DWORD>();
  try {
    GetWindowThreadProcessId(hwnd, processIdPtr);
    if (processIdPtr.value == pid) {
      if (IsWindowVisible(hwnd) != 0) {
        // If we found a visible window, keep it as fallback
        if (context.ref.fallbackHwnd == 0) {
          context.ref.fallbackHwnd = hwnd;
        }

        // Check if it has a title
        final length = GetWindowTextLength(hwnd);
        if (length > 0) {
          // Check if it's a top-level window (no parent)
          if (GetParent(hwnd) == 0) {
            context.ref.bestHwnd = hwnd;
            // Don't stop enumeration immediately, we might find a better one?
            // Actually, EnumWindows order is Z-order usually. Topmost first.
            // If we found a visible, titled, top-level window, it's likely the main one.
            return FALSE;
          }
        }
      }
    }
  } finally {
    calloc.free(processIdPtr);
  }
  return TRUE; // Continue enumeration
}

class InputController {
  int? _cachedPid;
  int? _cachedHwnd;

  /// Find the main window handle (HWND) for a given process ID (PID).
  /// Returns 0 if not found.
  int getHwndForPid(int pid) {
    if (_cachedPid == pid &&
        _cachedHwnd != null &&
        IsWindow(_cachedHwnd!) != 0) {
      return _cachedHwnd!;
    }

    _cachedPid = null;
    _cachedHwnd = null;

    final hwnd = _findWindowForPid(pid);
    if (hwnd != 0) {
      _cachedPid = pid;
      _cachedHwnd = hwnd;

      // Log found window info for debugging
      final titleLength = GetWindowTextLength(hwnd);
      if (titleLength > 0) {
        final titleBuffer = wsalloc(titleLength + 1);
        GetWindowText(hwnd, titleBuffer, titleLength + 1);
        // print('InputController: Found HWND $hwnd for PID $pid, Title: ${titleBuffer.toDartString()}');
        free(titleBuffer);
      } else {
        // print('InputController: Found HWND $hwnd for PID $pid (No Title)');
      }
    } else {
      // print('InputController: No HWND found for PID $pid');
    }
    return hwnd;
  }

  int _findWindowForPid(int targetPid) {
    final context = calloc<_SearchContext>();
    context.ref.pid = targetPid;
    context.ref.bestHwnd = 0;
    context.ref.fallbackHwnd = 0;

    final callback = NativeCallable<WNDENUMPROC>.isolateLocal(
      _enumWindowsCallback,
      exceptionalReturn: 0,
    );

    try {
      EnumWindows(callback.nativeFunction, context.address);
      // Prefer best match, then fallback
      return context.ref.bestHwnd != 0
          ? context.ref.bestHwnd
          : context.ref.fallbackHwnd;
    } finally {
      callback.close();
      calloc.free(context);
    }
  }

  void sendMouseEvent(int pid, int x, int y, int msg, int wParam) {
    final hwnd = getHwndForPid(pid);
    if (hwnd == 0) {
      print('InputController: HWND not found for PID $pid');
      return;
    }

    // Make lParam: (y << 16) | (x & 0xFFFF)
    final lParam = (y << 16) | (x & 0xFFFF);
    final result = PostMessage(hwnd, msg, wParam, lParam);
    if (result == 0) {
      print(
        'InputController: PostMessage failed for PID $pid, HWND $hwnd, msg $msg',
      );
    }
  }

  void sendKeyEvent(int pid, int vkCode, bool isDown) {
    final hwnd = getHwndForPid(pid);
    if (hwnd == 0) {
      print('InputController: HWND not found for PID $pid');
      return;
    }

    final msg = isDown ? WM_KEYDOWN : WM_KEYUP;

    // Construct lParam for WM_KEYDOWN/UP
    int scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    int lParam = 1; // Repeat count
    lParam |= (scanCode << 16);

    if (!isDown) {
      lParam |= (1 << 30); // Previous key state
      lParam |= (1 << 31); // Transition state
    }

    final result = PostMessage(hwnd, msg, vkCode, lParam);
    if (result == 0) {
      print(
        'InputController: PostMessage key failed for PID $pid, HWND $hwnd, vk $vkCode',
      );
    }
  }
}
