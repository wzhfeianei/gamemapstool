import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;
import 'dart:ffi' hide Size;
import 'package:ffi/ffi.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:win32/win32.dart' as win32;
import '../controllers/input_controller.dart';
import '../native/image_search.dart';
import '../native/image_search_worker.dart';

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
  static const double _defaultTextureWidth = 1920;
  static const double _defaultTextureHeight = 1080;

  List<ProcessEntry> _processList = [];
  ProcessEntry? _selectedProcess;
  bool _processLoading = false;
  bool _isBusy = false;
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
  int? _lastResizeWidth;
  int? _lastResizeHeight;

  Timer? _checkAliveTimer;

  final InputController _inputController = InputController();
  bool _controlEnabled = false;
  final FocusNode _inputFocusNode = FocusNode();

  String? _lastInputStatus;

  // Search State
  Uint8List? _debugRoiBytes;

  // Auto Task State
  bool _autoTaskEnabled = false;
  Timer? _autoTaskTimer;
  final Map<String, int> _taskTemplateIds = {};
  final Map<int, Size> _taskTemplateSizes = {};
  List<SearchResultStruct> _taskSearchResults = [];

  // Auto Task Search ROI
  int _searchRoiX = 0;
  int _searchRoiY = 0;
  int _searchRoiW = -1;
  int _searchRoiH = -1;

  final ImageSearchWorker _imageWorker = ImageSearchWorker();

  void updateSearchRoi(int x, int y, int w, int h) {
    setState(() {
      _searchRoiX = x;
      _searchRoiY = y;
      _searchRoiW = w;
      _searchRoiH = h;
    });
  }

  @override
  void initState() {
    super.initState();
    _imageWorker.init();
    _refreshProcessList();
    _checkAliveTimer = Timer.periodic(const Duration(seconds: 3), (timer) {
      if (_selectedProcess != null) {
        _refreshProcessList(updateState: false);
      }
    });
  }

  @override
  void dispose() {
    _imageWorker.dispose();
    _autoTimer?.cancel();
    _checkAliveTimer?.cancel();
    _autoTaskTimer?.cancel();
    _inputFocusNode.dispose();
    super.dispose();
  }

  void _handlePointer(PointerEvent event) {
    if (!_controlEnabled || _selectedProcess == null) return;

    final int pid = _selectedProcess!.pid;
    final int x = event.localPosition.dx.toInt();
    final int y = event.localPosition.dy.toInt();

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
      action = 'Up';
      _inputController.sendMouseEvent(pid, x, y, win32.WM_LBUTTONUP, 0);
    } else if (event is PointerMoveEvent) {
      int wParam = 0;
      if (event.buttons & kPrimaryMouseButton != 0) wParam |= MK_LBUTTON;
      if (event.buttons & kSecondaryMouseButton != 0) wParam |= MK_RBUTTON;
      _inputController.sendMouseEvent(pid, x, y, win32.WM_MOUSEMOVE, wParam);
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
        return;
      }

      setState(() {
        _processList = processes;
        if (selectedPid != null) {
          if (found) {
            _selectedProcess = nextSelected;
          } else {
            _selectedProcess = null;
          }
        }
      });
    } on PlatformException catch (e) {
      if (!mounted) {
        return;
      }
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

  Future<void> _resizePreviewWindow() async {
    if (!_autoEnabled) return;
    final double? width = _textureWidth;
    final double? height = _textureHeight;
    if (width == null || height == null || width <= 0 || height <= 0) {
      return;
    }
    final double devicePixelRatio = MediaQuery.of(context).devicePixelRatio;
    final int targetWidth = (width / devicePixelRatio).round();
    final int targetHeight = (height / devicePixelRatio).round();
    if (_lastResizeWidth == targetWidth && _lastResizeHeight == targetHeight) {
      return;
    }
    _lastResizeWidth = targetWidth;
    _lastResizeHeight = targetHeight;
    try {
      await _channel.invokeMethod('resizePreviewWindow', <String, Object?>{
        'width': targetWidth,
        'height': targetHeight,
      });
    } catch (e) {}
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
            await _resizePreviewWindow();
          }
        }
      } catch (e) {
        debugPrint('Failed to get texture ID: $e');
      }

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

    if (_textureId != null &&
        _textureWidth != null &&
        _textureWidth! > 0 &&
        _textureHeight != null &&
        _textureHeight! > 0) {
      return;
    }

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

            if (w != null && w > 0 && h != null && h > 0) {
              await _resizePreviewWindow();
              return;
            }
          }
        }
      }
    } catch (e) {}

    if (!mounted || !_autoEnabled) return;

    Future<void>.delayed(const Duration(milliseconds: 100), _captureLoop);
  }

  Future<void> _stopAutoCapture() async {
    if (_isBusy) return;
    setState(() {
      _isBusy = true;
    });

    try {
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
        } catch (e) {}
      }

      await _channel.invokeMethod('stopCaptureSession');
    } catch (e) {
      debugPrint('Stop capture session error: $e');
    } finally {
      if (mounted) {
        setState(() {
          _autoEnabled = false;
          _textureId = null;
          _lastResizeWidth = null;
          _lastResizeHeight = null;
          _isBusy = false;
        });
      }
    }
  }

  String _resolveResourcePath(String filename) {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    String path = '$exeDir\\yuanshen\\$filename';
    if (!File(path).existsSync()) {
      path = 'yuanshen/$filename';
    }
    return path;
  }

  Future<void> _startAutoTask() async {
    if (_autoTaskEnabled) return;
    final process = _selectedProcess;
    if (process == null) {
      setState(() => _errorMessage = '请先选择进程');
      return;
    }
    setState(() => _errorMessage = null);

    final templates = ['juqing.png', 'tiaoguo.png', 'f.png'];
    _taskTemplateIds.clear();
    _taskTemplateSizes.clear();

    try {
      for (final name in templates) {
        final path = _resolveResourcePath(name);
        if (!File(path).existsSync()) {
          throw '资源文件缺失: $name\n路径: $path';
        }

        final bytes = await File(path).readAsBytes();
        final codec = await ui.instantiateImageCodec(bytes);
        final frame = await codec.getNextFrame();
        final size = Size(
          frame.image.width.toDouble(),
          frame.image.height.toDouble(),
        );
        frame.image.dispose();

        final id = await _imageWorker.loadTemplate(path);
        if (id <= 0) {
          throw '加载模板失败: $name (ID: $id)';
        }
        _taskTemplateIds[name] = id;
        _taskTemplateSizes[id] = size;
      }
    } catch (e) {
      await _stopAutoTask();
      setState(() => _errorMessage = e.toString());
      return;
    }

    if (!_autoEnabled) {
      await _startAutoCapture();
    }

    setState(() {
      _autoTaskEnabled = true;
    });

    _autoTaskTimer = Timer.periodic(const Duration(milliseconds: 1000), (
      timer,
    ) {
      _autoTaskLoop();
    });
  }

  Future<void> _stopAutoTask() async {
    _autoTaskTimer?.cancel();
    _autoTaskTimer = null;

    try {
      await _channel.invokeMethod('closeOverlay');
    } catch (e) {
      debugPrint('Close overlay error: $e');
    }

    for (final id in _taskTemplateIds.values) {
      await _imageWorker.releaseTemplate(id);
    }
    _taskTemplateIds.clear();
    _taskTemplateSizes.clear();

    setState(() {
      _autoTaskEnabled = false;
      _taskSearchResults = [];
    });
  }

  Future<void> _autoTaskLoop() async {
    if (!_autoTaskEnabled || _selectedProcess == null) return;

    try {
      final Uint8List? imageBytes = await _channel.invokeMethod<Uint8List>(
        'getLastFrame',
      );

      if (imageBytes == null || imageBytes.isEmpty) return;

      final juqingId = _taskTemplateIds['juqing.png'];
      final tiaoId = _taskTemplateIds['tiaoguo.png'];
      final fId = _taskTemplateIds['f.png'];

      if (juqingId == null) return;

      final results = await _imageWorker.findImagesBatch(imageBytes, [
        SearchRequestStruct(
          juqingId,
          threshold: 0.7,
          roiX: _searchRoiX,
          roiY: _searchRoiY,
          roiW: _searchRoiW,
          roiH: _searchRoiH,
        ),
      ]);

      List<SearchResultStruct> currentResults = [];

      // 检查是否找到剧情图片
      bool foundJuqing = false;
      for (final res in results) {
        if (res.score >= 0.7) {
          currentResults.add(res);
          if (res.templateId == juqingId) {
            foundJuqing = true;
          }
        }
      }

      if (foundJuqing) {
        final subRequests = <SearchRequestStruct>[];
        if (tiaoId != null) {
          subRequests.add(
            SearchRequestStruct(
              tiaoId,
              threshold: 0.5,
              roiX: 900,
              roiY: 1000,
              roiW: 1100,
              roiH: 1100,
            ),
          );
        }
        if (fId != null) {
          subRequests.add(
            SearchRequestStruct(
              fId,
              threshold: 0.7,
              roiX: 1000,
              roiY: 400,
              roiW: 1500,
              roiH: 1100,
            ),
          );
        }

        if (subRequests.isNotEmpty) {
          final subResults = await _imageWorker.findImagesBatch(
            imageBytes,
            subRequests,
          );

          bool foundSub = false;
          for (final res in subResults) {
            if (res.score >= 0.7) {
              currentResults.add(res);
              foundSub = true;
            }
          }

          if (foundSub) {
            _inputController.sendKeyEvent(_selectedProcess!.pid, 0x46, true);
            await Future.delayed(const Duration(milliseconds: 50));
            _inputController.sendKeyEvent(_selectedProcess!.pid, 0x46, false);
          }
        }
      }

      // Update Overlay
      List<Map<String, dynamic>> overlayRects = [];
      for (final res in currentResults) {
        final size = _taskTemplateSizes[res.templateId];
        if (size != null) {
          overlayRects.add({
            'x': res.x,
            'y': res.y,
            'width': size.width.toInt(),
            'height': size.height.toInt(),
          });
        }
      }

      // try {
      //   await _channel.invokeMethod('updateOverlay', {'rects': overlayRects});
      // } catch (e) {
      //   debugPrint('Update overlay error: $e');
      // }

      if (mounted) {
        setState(() {
          _taskSearchResults = currentResults;
        });
      }
    } catch (e) {
      debugPrint('Auto task error: $e');
    }
  }

  Future<void> _showSearchDialog() async {
    final process = _selectedProcess;
    if (process == null) {
      setState(() => _errorMessage = '请先选择进程');
      return;
    }

    final exeDir = File(Platform.resolvedExecutable).parent.path;
    String imagePath = '$exeDir\\yuanshen\\juqing.png';

    if (!File(imagePath).existsSync()) {
      imagePath = 'yuanshen/juqing.png';
    }

    final bool fileExists = File(imagePath).existsSync();

    await showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('图片查找测试 (ROI)'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '目标资源: yuanshen/juqing.png',
              style: const TextStyle(fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 4),
            Text(
              '实际路径: $imagePath',
              style: TextStyle(fontSize: 10, color: Colors.grey),
            ),
            Text(
              '状态: ${fileExists ? "✅ 文件存在" : "❌ 文件不存在"}',
              style: TextStyle(color: fileExists ? Colors.green : Colors.red),
            ),
            const SizedBox(height: 12),

            if (_debugRoiBytes != null) ...[
              const Text(
                '上次查找的 ROI 区域截图:',
                style: TextStyle(fontWeight: FontWeight.bold),
              ),
              const SizedBox(height: 4),
              Container(
                height: 150,
                width: double.infinity,
                decoration: BoxDecoration(
                  border: Border.all(color: Colors.grey),
                ),
                child: Image.memory(_debugRoiBytes!, fit: BoxFit.contain),
              ),
            ] else
              const Text('暂无调试截图'),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('关闭'),
          ),
          ElevatedButton(
            onPressed: !fileExists
                ? null
                : () async {
                    Navigator.pop(context);

                    try {
                      final templateId = await _imageWorker.loadTemplate(
                        imagePath,
                      );
                      if (templateId <= 0) {
                        setState(
                          () => _errorMessage = '加载模板失败 (ID: $templateId)',
                        );
                        _showSearchDialog();
                        return;
                      }

                      final stopwatchCapture = Stopwatch()..start();
                      final Uint8List? imageBytes = await _channel
                          .invokeMethod<Uint8List>('capture', <String, Object?>{
                            'pid': process.pid,
                            'processName': process.name,
                          });
                      stopwatchCapture.stop();

                      if (imageBytes == null || imageBytes.isEmpty) {
                        setState(() => _errorMessage = 'WGC 截图失败或返回为空');
                        await _imageWorker.releaseTemplate(templateId);
                        _showSearchDialog();
                        return;
                      }

                      final stopwatchSearch = Stopwatch()..start();

                      final request = SearchRequestStruct(
                        templateId,
                        roiX: 0,
                        roiY: 0,
                        roiW: -1,
                        roiH: -1,
                        threshold: 0.8,
                      );

                      final results = await _imageWorker.findImagesBatch(
                        imageBytes,
                        [request],
                        width: 0,
                        height: 0,
                      );
                      stopwatchSearch.stop();

                      await _imageWorker.releaseTemplate(templateId);

                      Uint8List? debugBytes;
                      final debugFile = File('debug_last_batch_source.png');
                      if (debugFile.existsSync()) {
                        debugBytes = await debugFile.readAsBytes();
                      }

                      setState(() {
                        _debugRoiBytes = debugBytes;
                        if (results.isNotEmpty && results[0].score >= 0.8) {
                          final res = results[0];
                          _errorMessage =
                              '✅ 找到目标! (${res.x}, ${res.y}) 置信度: ${res.score.toStringAsFixed(2)}\n'
                              '截图耗时: ${stopwatchCapture.elapsedMilliseconds}ms\n'
                              '查找耗时: ${stopwatchSearch.elapsedMilliseconds}ms';
                        } else {
                          double maxScore = results.isNotEmpty
                              ? results[0].score
                              : 0.0;
                          _errorMessage =
                              '❌ 未找到目标 (最高分: ${maxScore.toStringAsFixed(2)})\n'
                              '截图耗时: ${stopwatchCapture.elapsedMilliseconds}ms\n'
                              '查找耗时: ${stopwatchSearch.elapsedMilliseconds}ms';
                        }
                      });

                      if (mounted) _showSearchDialog();
                    } catch (e) {
                      setState(() => _errorMessage = '搜索异常: $e');
                      if (mounted) _showSearchDialog();
                    }
                  },
            child: const Text('查找'),
          ),
        ],
      ),
    );
  }

  Widget _buildImagePreview() {
    final double devicePixelRatio = MediaQuery.of(context).devicePixelRatio;
    const Widget loadingOverlay = ColoredBox(
      color: Colors.black12,
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 8),
            Text('正在初始化画面...'),
          ],
        ),
      ),
    );
    Widget content;
    if (_textureId != null) {
      final bool hasTextureSize =
          _textureWidth != null &&
          _textureWidth! > 0 &&
          _textureHeight != null &&
          _textureHeight! > 0;
      final double rawWidth = hasTextureSize
          ? _textureWidth!
          : _defaultTextureWidth;
      final double rawHeight = hasTextureSize
          ? _textureHeight!
          : _defaultTextureHeight;
      final double renderWidth = rawWidth / devicePixelRatio;
      final double renderHeight = rawHeight / devicePixelRatio;
      content = SizedBox(
        width: renderWidth,
        height: renderHeight,
        child: Stack(
          fit: StackFit.expand,
          children: [
            Texture(textureId: _textureId!),
            if (_taskSearchResults.isNotEmpty)
              CustomPaint(
                painter: SearchResultPainter(
                  _taskSearchResults,
                  _taskTemplateSizes,
                  _taskTemplateIds,
                ),
              ),
            if (!hasTextureSize) loadingOverlay,
          ],
        ),
      );
    } else if (_autoEnabled) {
      final double renderWidth = _defaultTextureWidth / devicePixelRatio;
      final double renderHeight = _defaultTextureHeight / devicePixelRatio;
      content = SizedBox(
        width: renderWidth,
        height: renderHeight,
        child: loadingOverlay,
      );
    } else if (_imageBytes == null || _imageBytes!.isEmpty) {
      return Container(
        alignment: Alignment.center,
        color: Colors.grey.shade100,
        child: const Text('暂无截图'),
      );
    } else {
      content = Stack(
        children: [
          Image.memory(
            _imageBytes!,
            key: ValueKey<int>(_imageVersion),
            gaplessPlayback: true,
            fit: BoxFit.none,
          ),
          if (_taskSearchResults.isNotEmpty)
            Positioned.fill(
              child: CustomPaint(
                painter: SearchResultPainter(
                  _taskSearchResults,
                  _taskTemplateSizes,
                  _taskTemplateIds,
                ),
              ),
            ),
        ],
      );
    }

    content = Listener(
      onPointerDown: _handlePointer,
      onPointerUp: _handlePointer,
      onPointerMove: _handlePointer,
      onPointerHover: _handlePointer,
      child: content,
    );

    final bool enableTransform = !_controlEnabled && !_autoEnabled;
    return RawKeyboardListener(
      focusNode: _inputFocusNode,
      onKey: _handleKey,
      autofocus: _controlEnabled,
      child: InteractiveViewer(
        constrained: false,
        minScale: 1,
        maxScale: 1,
        panEnabled: enableTransform,
        scaleEnabled: enableTransform,
        child: Center(child: content),
      ),
    );
  }

  Widget _buildProcessItem(ProcessEntry entry) {
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
                                onTap: () {},
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
              _lastInputStatus ?? statusText,
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
                IconButton(
                  icon: Icon(
                    _autoTaskEnabled
                        ? Icons.assignment_turned_in
                        : Icons.assignment,
                  ),
                  color: _autoTaskEnabled ? Colors.blue : null,
                  onPressed: _autoTaskEnabled ? _stopAutoTask : _startAutoTask,
                  tooltip: _autoTaskEnabled ? '停止自动任务' : '开始自动任务',
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
                  icon: const Icon(Icons.image_search),
                  onPressed: _showSearchDialog,
                  tooltip: '图片查找测试',
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

          _buildStatusBar(statusText),
        ],
      ),
    );
  }
}

class SearchResultPainter extends CustomPainter {
  final List<SearchResultStruct> results;
  final Map<int, Size> templateSizes;
  final Map<String, int> templateIds;

  SearchResultPainter(this.results, this.templateSizes, this.templateIds);

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 3.0;

    for (final res in results) {
      if (res.templateId == templateIds['juqing.png']) {
        paint.color = Colors.green;
      } else {
        paint.color = Colors.red;
      }

      final templateSize = templateSizes[res.templateId] ?? const Size(50, 50);

      canvas.drawRect(
        Rect.fromLTWH(
          res.x.toDouble(),
          res.y.toDouble(),
          templateSize.width,
          templateSize.height,
        ),
        paint,
      );

      final textSpan = TextSpan(
        text: res.templateId == templateIds['juqing.png'] ? '剧情' : '交互',
        style: TextStyle(
          color: paint.color,
          fontSize: 14,
          fontWeight: FontWeight.bold,
          backgroundColor: Colors.black54,
        ),
      );
      final textPainter = TextPainter(
        text: textSpan,
        textDirection: TextDirection.ltr,
      );
      textPainter.layout();
      textPainter.paint(
        canvas,
        Offset(res.x.toDouble(), res.y.toDouble() - 20),
      );
    }
  }

  @override
  bool shouldRepaint(covariant SearchResultPainter oldDelegate) {
    return oldDelegate.results != results;
  }
}
