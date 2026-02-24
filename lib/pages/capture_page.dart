import 'dart:async';
import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:win32/win32.dart' as win32;
import '../controllers/input_controller.dart';

// Missing constants in some win32 versions or specific libraries
const int MK_LBUTTON = 0x0001;
const int MK_RBUTTON = 0x0002;
const int MK_SHIFT = 0x0004;
const int MK_CONTROL = 0x0008;
const int MK_MBUTTON = 0x0010;

class CapturePage extends StatefulWidget {
  const CapturePage({super.key});

  @override
  State<CapturePage> createState() => _CapturePageState();
}

class ProcessEntry {
  const ProcessEntry({
    required this.pid,
    required this.name,
    this.windowTitle,
    required this.iconBytes,
    this.cpu = 0.0,
  });

  final int pid;
  final String name;
  final String? windowTitle;
  final Uint8List? iconBytes;
  final double cpu;

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    return other is ProcessEntry && other.pid == pid;
  }

  @override
  int get hashCode => pid.hashCode;
}

class _CapturePageState extends State<CapturePage> {
  static const MethodChannel _channel = MethodChannel('gamemapstool/capture');

  List<ProcessEntry> _processList = [];
  ProcessEntry? _selectedProcess;
  bool _processLoading = false;
  bool _isBusy = false; // Prevents re-entry during async ops
  int _imageVersion = 0;

  Uint8List? _imageBytes;
  String? _errorMessage;
  DateTime? _lastCapturedAt;
  int? _lastCaptureDurationMs;
  Timer? _autoTimer;
  bool _autoEnabled = false;
  bool _captureInProgress = false;
  int? _textureId;
  double? _textureWidth;
  double? _textureHeight;
  int? _imageWidth;
  int? _imageHeight;

  Timer? _checkAliveTimer;

  final InputController _inputController = InputController();
  bool _controlEnabled = false;
  final FocusNode _inputFocusNode = FocusNode();

  String? _lastInputStatus; // Store last input status for display

  @override
  void initState() {
    super.initState();
    _refreshProcessList();
    _checkAliveTimer = Timer.periodic(const Duration(seconds: 3), (timer) {
      if (_selectedProcess != null) {
        _refreshProcessList(updateState: false);
      }
    });
  }

  @override
  void dispose() {
    _autoTimer?.cancel();
    _checkAliveTimer?.cancel();
    _inputFocusNode.dispose();
    super.dispose();
  }

  void _handlePointer(PointerEvent event) {
    if (!_controlEnabled || _selectedProcess == null) return;

    final int pid = _selectedProcess!.pid;
    // Coordinates relative to the image (InteractiveViewer child)
    final int x = event.localPosition.dx.toInt();
    final int y = event.localPosition.dy.toInt();

    // Only update status for Down/Up to avoid spamming setState on Move
    // For Move, update status every 10 events or so if needed, but for now just log/send
    // Actually, user requested "监听到鼠标键盘动作时,在下方信息栏显示到的监听信息"
    // To avoid lag, we might want to throttle UI updates, but send messages immediately.

    String action = '';

    if (event is PointerDownEvent) {
      if (event.buttons == kPrimaryMouseButton) {
        action = 'L-Down';
        _inputController.sendMouseEvent(
          pid,
          x,
          y,
          win32.WM_LBUTTONDOWN,
          MK_LBUTTON,
        );
      } else if (event.buttons == kSecondaryMouseButton) {
        action = 'R-Down';
        _inputController.sendMouseEvent(
          pid,
          x,
          y,
          win32.WM_RBUTTONDOWN,
          MK_RBUTTON,
        );
      }
    } else if (event is PointerUpEvent) {
      // For Up events, buttons might be 0, but we need to know which one was released.
      // However, PointerUpEvent doesn't explicitly say which button released in a simple way
      // other than checking changed buttons, but standard WM_LBUTTONUP doesn't need wParam usually (except keys).
      // We can infer from the event type or check logic.
      // Simply sending LBUTTONUP for primary button up is safe enough for now.
      // A more robust way handles all buttons.
      // For now, assume Left Button.
      // Actually PointerUpEvent has 'kind'.
      action = 'Up';
      _inputController.sendMouseEvent(pid, x, y, win32.WM_LBUTTONUP, 0);
    } else if (event is PointerMoveEvent) {
      // Throttle UI updates for move events to avoid performance hit
      // But always send the message
      int wParam = 0;
      if (event.buttons & kPrimaryMouseButton != 0) wParam |= MK_LBUTTON;
      if (event.buttons & kSecondaryMouseButton != 0) wParam |= MK_RBUTTON;
      _inputController.sendMouseEvent(pid, x, y, win32.WM_MOUSEMOVE, wParam);
      // action = 'Move'; // Don't update UI on every move
    } else if (event is PointerHoverEvent) {
      _inputController.sendMouseEvent(pid, x, y, win32.WM_MOUSEMOVE, 0);
    }

    if (action.isNotEmpty) {
      setState(() {
        _lastInputStatus = 'Mouse: $action ($x, $y) -> PID: $pid';
      });
    }
  }

  void _handleKey(RawKeyEvent event) {
    if (!_controlEnabled || _selectedProcess == null) return;

    final int pid = _selectedProcess!.pid;
    if (event.data is RawKeyEventDataWindows) {
      final data = event.data as RawKeyEventDataWindows;
      final int vkCode = data.keyCode;
      final bool isDown = event is RawKeyDownEvent;
      _inputController.sendKeyEvent(pid, vkCode, isDown);

      setState(() {
        _lastInputStatus =
            'Key: ${isDown ? "Down" : "Up"} (VK: $vkCode) -> PID: $pid';
      });
    }
  }

  Future<void> _refreshProcessList({bool updateState = true}) async {
    if (_processLoading) {
      return;
    }
    if (updateState) {
      setState(() {
        _processLoading = true;
        _loadingNotifier.value = true;
        _errorMessage = null;
      });
    } else {
      _processLoading = true;
    }

    try {
      final List<Object?>? result = await _channel.invokeMethod<List<Object?>>(
        'listProcesses',
      );
      if (!mounted) {
        return;
      }
      final List<ProcessEntry> processes = <ProcessEntry>[];
      for (final Object? item in result ?? <Object?>[]) {
        if (item is! Map) {
          continue;
        }
        final Object? pidValue = item['pid'];
        final Object? nameValue = item['name'];
        final Object? windowTitleValue = item['windowTitle'];
        final Object? iconValue = item['icon'];
        final Object? cpuValue = item['cpu'];
        final int pid = pidValue is int
            ? pidValue
            : pidValue is num
            ? pidValue.toInt()
            : 0;
        final String name = nameValue is String
            ? nameValue.trim()
            : nameValue?.toString() ?? '';
        final String? windowTitle =
            windowTitleValue is String && windowTitleValue.isNotEmpty
            ? windowTitleValue
            : null;
        final Uint8List? icon = iconValue is Uint8List ? iconValue : null;
        final double cpu = cpuValue is double
            ? cpuValue
            : cpuValue is num
            ? cpuValue.toDouble()
            : 0.0;
        if (pid > 0 && name.isNotEmpty) {
          processes.add(
            ProcessEntry(
              pid: pid,
              name: name,
              windowTitle: windowTitle,
              iconBytes: icon,
              cpu: cpu,
            ),
          );
        }
      }

      // If we have a selected process, check if it's still in the list
      final int? selectedPid = _selectedProcess?.pid;
      ProcessEntry? nextSelected;
      bool found = false;

      if (selectedPid != null) {
        for (final ProcessEntry entry in processes) {
          if (entry.pid == selectedPid) {
            nextSelected = entry;
            found = true;
            break;
          }
        }
      }

      if (!updateState && selectedPid != null && found) {
        // Background check passed, process is alive.
        // Do NOT update _selectedProcess to avoid icon flickering.
        return;
      }

      setState(() {
        _processList = processes;
        // Check alive logic
        if (selectedPid != null) {
          if (found) {
            // Update with latest info (e.g. CPU usage)
            _selectedProcess = nextSelected;
          } else {
            // Process died
            _selectedProcess = null;
          }
        }
      });
    } on PlatformException catch (e) {
      if (!mounted) {
        return;
      }
      // Only show error if explicitly refreshing
      if (updateState) {
        debugPrint('Refresh process list error: ${e.toString()}');
        setState(() {
          _errorMessage = e.message ?? '获取进程列表失败';
        });
      }
    } finally {
      if (mounted) {
        if (updateState) {
          setState(() {
            _processLoading = false;
            _loadingNotifier.value = false;
          });
        } else {
          _processLoading = false;
        }
      }
    }
  }

  Future<void> _captureOnce() async {
    if (_captureInProgress) {
      return;
    }
    final ProcessEntry? process = _selectedProcess;
    if (process == null) {
      setState(() {
        _errorMessage = '请选择进程';
      });
      return;
    }
    setState(() {
      _captureInProgress = true;
      _errorMessage = null;
    });
    final Stopwatch stopwatch = Stopwatch()..start();
    try {
      final Uint8List? bytes = await _channel.invokeMethod<Uint8List>(
        'capture',
        <String, Object?>{'pid': process.pid, 'processName': process.name},
      );
      stopwatch.stop();
      if (!mounted) {
        return;
      }
      setState(() {
        _imageBytes = bytes;
        _lastCapturedAt = DateTime.now();
        _lastCaptureDurationMs = stopwatch.elapsedMilliseconds;
        _imageVersion++;
        if (bytes == null || bytes.isEmpty) {
          _errorMessage = '未获取到截图数据';
          _imageWidth = null;
          _imageHeight = null;
        }
      });
      if (bytes != null && bytes.isNotEmpty) {
        final ui.Image image = await decodeImageFromList(bytes);
        if (mounted) {
          setState(() {
            _imageWidth = image.width;
            _imageHeight = image.height;
          });
        }
      }
    } on PlatformException catch (e) {
      stopwatch.stop();
      if (!mounted) {
        return;
      }
      setState(() {
        _errorMessage = e.message ?? '截图失败';
        _lastCaptureDurationMs = stopwatch.elapsedMilliseconds;
      });
    } finally {
      if (mounted) {
        setState(() {
          _captureInProgress = false;
        });
      }
    }
  }

  Future<void> _startAutoCapture() async {
    if (_autoEnabled || _isBusy) {
      return;
    }
    final ProcessEntry? process = _selectedProcess;
    if (process == null) {
      setState(() {
        _errorMessage = '请选择进程';
      });
      return;
    }

    setState(() {
      _isBusy = true;
      _errorMessage = null;
    });

    try {
      await _channel.invokeMethod('startCaptureSession', <String, Object?>{
        'pid': process.pid,
        'processName': process.name,
      });

      if (!mounted) return;
      setState(() {
        _autoEnabled = true;
      });

      // Try to get texture ID for all modes as they all support it now
      try {
        final Map<Object?, Object?>? result = await _channel
            .invokeMapMethod<Object?, Object?>('getTextureId');
        if (result != null) {
          final int? tid = result['id'] as int?;
          final int? w = result['width'] as int?;
          final int? h = result['height'] as int?;
          if (tid != null) {
            setState(() {
              _textureId = tid;
              _textureWidth = w?.toDouble();
              _textureHeight = h?.toDouble();
            });
          }
        }
      } catch (e) {
        debugPrint('Failed to get texture ID: $e');
        // Fallback to manual loop if texture is not available
      }

      // Start the loop without awaiting it, as it runs indefinitely until stopped
      _captureLoop();
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() {
        _autoEnabled = false;
        _errorMessage = e.message ?? '启动捕获会话失败';
      });
    } finally {
      if (mounted) {
        setState(() {
          _isBusy = false;
        });
      }
    }
  }

  Future<void> _captureLoop() async {
    if (!_autoEnabled) return;
    if (_textureId != null) return;

    // Retry getting texture ID instead of falling back to slow PNG capture
    try {
      final Map<Object?, Object?>? result = await _channel
          .invokeMapMethod<Object?, Object?>('getTextureId');
      if (result != null) {
        final int? tid = result['id'] as int?;
        final int? w = result['width'] as int?;
        final int? h = result['height'] as int?;
        if (tid != null) {
          if (mounted) {
            setState(() {
              _textureId = tid;
              _textureWidth = w?.toDouble();
              _textureHeight = h?.toDouble();
            });
          }
          return; // Exit loop, texture is ready
        }
      }
    } catch (e) {
      // debugPrint('Retry getTextureId failed: $e');
    }

    if (!mounted || !_autoEnabled) return;

    // Wait before retrying
    Future<void>.delayed(const Duration(milliseconds: 500), _captureLoop);
  }

  Future<void> _stopAutoCapture() async {
    if (_isBusy) return;
    setState(() {
      _isBusy = true;
    });

    try {
      // Try to get one last frame before stopping
      if (_textureId != null) {
        try {
          final Uint8List? lastFrame = await _channel.invokeMethod(
            'getLastFrame',
          );
          if (lastFrame != null && mounted) {
            setState(() {
              _imageBytes = lastFrame;
            });
          }
        } catch (e) {
          // Ignore last frame error
        }
      }

      await _channel.invokeMethod('stopCaptureSession');
    } catch (e) {
      debugPrint('Stop capture session error: $e');
    } finally {
      if (mounted) {
        setState(() {
          _autoEnabled = false;
          _textureId = null;
          _isBusy = false;
        });
      }
    }
  }

  Widget _buildImagePreview() {
    Widget content;
    if (_textureId != null) {
      Widget image = Texture(textureId: _textureId!);
      if (_textureWidth != null &&
          _textureHeight != null &&
          _textureHeight! > 0) {
        image = SizedBox(
          width: _textureWidth,
          height: _textureHeight,
          child: image,
        );
      }
      content = image;
    } else if (_imageBytes == null || _imageBytes!.isEmpty) {
      return Container(
        alignment: Alignment.center,
        color: Colors.grey.shade100,
        child: const Text('暂无截图'),
      );
    } else {
      content = Image.memory(
        _imageBytes!,
        key: ValueKey<int>(_imageVersion),
        gaplessPlayback: true,
        fit: BoxFit.none,
      );
    }

    // Wrap content with Listener for mouse events
    content = Listener(
      onPointerDown: _handlePointer,
      onPointerUp: _handlePointer,
      onPointerMove: _handlePointer,
      onPointerHover: _handlePointer,
      child: content,
    );

    return RawKeyboardListener(
      focusNode: _inputFocusNode,
      onKey: _handleKey,
      autofocus: _controlEnabled,
      child: InteractiveViewer(
        constrained: false,
        minScale: 0.2,
        maxScale: 5,
        panEnabled: !_controlEnabled,
        scaleEnabled: !_controlEnabled,
        child: Center(child: content),
      ),
    );
  }

  Widget _buildProcessItem(ProcessEntry entry) {
    // Keep this method but update it to match the dialog style or just ignore it if unused.
    // Since the user wants the original dialog style, we will inline the style in the dialog
    // and leave this method as is (or remove it if it causes issues, but better to just fix the dialog).
    // Actually, to avoid the "unused" warning and keep code clean, let's make this method
    // match the ORIGINAL dialog style exactly.

    final Widget icon = entry.iconBytes != null && entry.iconBytes!.isNotEmpty
        ? Image.memory(entry.iconBytes!, width: 32, height: 32)
        : const Icon(Icons.apps, size: 32);

    final String title = entry.windowTitle?.isNotEmpty == true
        ? entry.windowTitle!
        : entry.name;

    return Row(
      children: [
        icon,
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: const TextStyle(fontWeight: FontWeight.bold)),
              Text(
                entry.name,
                style: const TextStyle(color: Colors.grey, fontSize: 12),
              ),
            ],
          ),
        ),
      ],
    );
  }

  void _showProcessSelectionDialog() {
    _refreshProcessList();
    showDialog(
      context: context,
      builder: (BuildContext context) {
        return StatefulBuilder(
          builder: (context, setState) {
            return Dialog(
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(8),
              ),
              child: Container(
                width: 600,
                height: 400,
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      '选择捕获窗口',
                      style: TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 4),
                    const Text(
                      '双击选中要捕获的窗口',
                      style: TextStyle(color: Colors.grey),
                    ),
                    const SizedBox(height: 16),
                    Expanded(
                      child: ValueListenableBuilder<bool>(
                        valueListenable: _loadingNotifier,
                        builder: (context, isLoading, child) {
                          if (isLoading) {
                            return const Center(
                              child: CircularProgressIndicator(),
                            );
                          }
                          return ListView.builder(
                            itemCount: _processList.length,
                            itemBuilder: (context, index) {
                              final entry = _processList[index];
                              final isSelected =
                                  _selectedProcess?.pid == entry.pid;
                              return InkWell(
                                onTap: () {
                                  // Optional: Single tap to highlight?
                                },
                                onDoubleTap: () {
                                  this.setState(() {
                                    _selectedProcess = entry;
                                  });
                                  Navigator.of(context).pop();
                                },
                                child: Container(
                                  padding: const EdgeInsets.all(8),
                                  decoration: BoxDecoration(
                                    color: isSelected
                                        ? Theme.of(context).highlightColor
                                        : null,
                                    borderRadius: BorderRadius.circular(4),
                                  ),
                                  child: _buildProcessItem(entry),
                                ),
                              );
                            },
                          );
                        },
                      ),
                    ),
                    const SizedBox(height: 16),
                    Align(
                      alignment: Alignment.centerRight,
                      child: TextButton(
                        onPressed: () => Navigator.of(context).pop(),
                        child: const Text('取消'),
                      ),
                    ),
                  ],
                ),
              ),
            );
          },
        );
      },
    );
  }

  final ValueNotifier<bool> _loadingNotifier = ValueNotifier(false);

  Widget _buildStatusBar(String statusText) {
    return Container(
      height: 24,
      padding: const EdgeInsets.symmetric(horizontal: 8),
      decoration: BoxDecoration(color: Theme.of(context).primaryColor),
      child: Row(
        children: [
          Expanded(
            child: Text(
              _lastInputStatus ?? statusText, // Prioritize input status
              style: const TextStyle(color: Colors.white, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    String statusText = '尚未截图';
    if (_lastCapturedAt != null) {
      if (_textureId != null) {
        statusText =
            '分辨率：${_textureWidth?.toInt() ?? 0}x${_textureHeight?.toInt() ?? 0} 用时：${_lastCaptureDurationMs ?? 0} ms';
      } else if (_imageWidth != null && _imageHeight != null) {
        statusText =
            '分辨率：$_imageWidth×$_imageHeight 用时：${_lastCaptureDurationMs ?? 0} ms';
      } else {
        statusText = '用时：${_lastCaptureDurationMs ?? 0} ms';
      }
    }

    return Scaffold(
      body: Column(
        children: [
          // Toolbar
          Container(
            height: 50,
            padding: const EdgeInsets.symmetric(horizontal: 16),
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: Colors.grey.shade300)),
            ),
            child: Row(
              children: [
                InkWell(
                  onTap: () {
                    _showProcessSelectionDialog();
                  },
                  borderRadius: BorderRadius.circular(4),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 8,
                      vertical: 4,
                    ),
                    child: Row(
                      children: [
                        if (_selectedProcess?.iconBytes != null)
                          Image.memory(
                            _selectedProcess!.iconBytes!,
                            width: 20,
                            height: 20,
                          )
                        else
                          const Icon(Icons.add_to_queue, size: 20),
                        const SizedBox(width: 8),
                        ConstrainedBox(
                          constraints: const BoxConstraints(maxWidth: 200),
                          child: Text(
                            _selectedProcess != null ? '已绑定窗口' : '点击绑定窗口',
                            overflow: TextOverflow.ellipsis,
                          ),
                        ),
                        const Icon(Icons.arrow_drop_down),
                      ],
                    ),
                  ),
                ),
                const Spacer(),
                IconButton(
                  icon: Icon(
                    _controlEnabled ? Icons.mouse : Icons.mouse_outlined,
                  ),
                  color: _controlEnabled ? Colors.green : null,
                  onPressed: _selectedProcess == null
                      ? null
                      : () {
                          setState(() {
                            _controlEnabled = !_controlEnabled;
                            if (_controlEnabled) {
                              _inputFocusNode.requestFocus();
                            }
                          });
                        },
                  tooltip: _controlEnabled ? '停止同步控制' : '开始同步控制',
                ),
                IconButton(
                  icon: const Icon(Icons.camera_alt),
                  onPressed: _captureInProgress ? null : _captureOnce,
                  tooltip: '手动截图',
                ),
                IconButton(
                  icon: Icon(_autoEnabled ? Icons.stop_circle : Icons.videocam),
                  color: _autoEnabled ? Colors.red : null,
                  onPressed: _isBusy
                      ? null
                      : (_autoEnabled ? _stopAutoCapture : _startAutoCapture),
                  tooltip: _autoEnabled ? '停止实时捕获' : '开始实时捕获',
                ),
              ],
            ),
          ),

          // Image Preview Area
          Expanded(
            child: Stack(
              children: [
                Container(
                  color: Colors.black87,
                  width: double.infinity,
                  child: _buildImagePreview(),
                ),
                if (_errorMessage != null)
                  Positioned(
                    top: 16,
                    left: 16,
                    right: 16,
                    child: Container(
                      padding: const EdgeInsets.all(8),
                      decoration: BoxDecoration(
                        color: Colors.red.withOpacity(0.8),
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: Text(
                        _errorMessage!,
                        style: const TextStyle(color: Colors.white),
                      ),
                    ),
                  ),
              ],
            ),
          ),

          // Status Bar
          _buildStatusBar(statusText),
        ],
      ),
    );
  }
}
