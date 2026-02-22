import 'dart:async';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

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
  int _imageVersion = 0;
  final List<Map<String, String>> _captureModes = [
    <String, String>{'value': 'bitblt', 'label': 'BitBlt'},
    <String, String>{'value': 'printWindow', 'label': 'PrintWindow'},
    <String, String>{'value': 'wgc', 'label': 'Graphics Capture'},
  ];
  String _selectedCaptureMode = 'wgc';

  Uint8List? _imageBytes;
  String? _errorMessage;
  DateTime? _lastCapturedAt;
  int? _lastCaptureDurationMs;
  Timer? _autoTimer;
  bool _autoEnabled = false;
  bool _captureInProgress = false;
  int _intervalMs = 1000;
  int? _textureId;
  double? _textureWidth;
  double? _textureHeight;
  int? _imageWidth;
  int? _imageHeight;

  @override
  void initState() {
    super.initState();
    _refreshProcessList();
  }

  @override
  void dispose() {
    _autoTimer?.cancel();
    super.dispose();
  }

  Future<void> _refreshProcessList() async {
    if (_processLoading) {
      return;
    }
    setState(() {
      _processLoading = true;
      _errorMessage = null;
    });
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
      if (selectedPid != null) {
        for (final ProcessEntry entry in processes) {
          if (entry.pid == selectedPid) {
            nextSelected = entry;
            break;
          }
        }
      }
      setState(() {
        _processList = processes;
        _selectedProcess =
            nextSelected ?? (processes.isNotEmpty ? processes.first : null);
      });
    } on PlatformException catch (e) {
      if (!mounted) {
        return;
      }
      debugPrint('Refresh process list error: ${e.toString()}');
      setState(() {
        _errorMessage = e.message ?? '获取进程列表失败';
      });
    } finally {
      if (mounted) {
        setState(() {
          _processLoading = false;
        });
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
        <String, Object?>{
          'pid': process.pid,
          'processName': process.name,
          'mode': _selectedCaptureMode,
        },
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
    if (_autoEnabled) {
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
      _autoEnabled = true;
      _errorMessage = null;
    });

    try {
      await _channel.invokeMethod('startCaptureSession', <String, Object?>{
        'pid': process.pid,
        'processName': process.name,
        'mode': _selectedCaptureMode,
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

      _captureLoop();
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() {
        _autoEnabled = false;
        _errorMessage = e.message ?? '启动捕获会话失败';
      });
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
    try {
      if (_textureId != null) {
        final Uint8List? lastFrame = await _channel.invokeMethod(
          'getLastFrame',
        );
        if (lastFrame != null && mounted) {
          setState(() {
            _imageBytes = lastFrame;
          });
        }
      }
    } catch (e) {
      debugPrint('Get last frame error: $e');
    }

    if (!mounted) return;
    setState(() {
      _autoEnabled = false;
      _textureId = null;
    });
    try {
      await _channel.invokeMethod('stopCaptureSession');
    } catch (e) {
      debugPrint('Stop capture session error: $e');
    }
  }

  Widget _buildImagePreview() {
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
      return InteractiveViewer(
        constrained: false,
        minScale: 0.2,
        maxScale: 5,
        child: Center(child: image),
      );
    }
    if (_imageBytes == null || _imageBytes!.isEmpty) {
      return Container(
        alignment: Alignment.center,
        color: Colors.grey.shade100,
        child: const Text('暂无截图'),
      );
    }
    Widget image = Image.memory(
      _imageBytes!,
      key: ValueKey<int>(_imageVersion),
      gaplessPlayback: true,
      fit: BoxFit.none,
    );
    return InteractiveViewer(
      constrained: false,
      minScale: 0.2,
      maxScale: 5,
      child: Center(child: image),
    );
  }

  Widget _buildProcessItem(ProcessEntry entry) {
    final Widget icon = entry.iconBytes != null && entry.iconBytes!.isNotEmpty
        ? Image.memory(entry.iconBytes!, width: 20, height: 20)
        : const Icon(Icons.apps, size: 20);

    final String displayName =
        entry.windowTitle != null && entry.windowTitle!.isNotEmpty
        ? '${entry.windowTitle} ${entry.name}'
        : entry.name;

    return Row(
      children: [
        icon,
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            '$displayName [CPU: ${entry.cpu.toStringAsFixed(1)}%]',
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(fontWeight: FontWeight.w500),
          ),
        ),
      ],
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
            '分辨率：${_imageWidth}x${_imageHeight} 用时：${_lastCaptureDurationMs ?? 0} ms';
      } else {
        statusText = '用时：${_lastCaptureDurationMs ?? 0} ms';
      }
    }

    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                Expanded(
                  child: SizedBox(
                    height: 40,
                    child: DropdownButtonFormField<ProcessEntry>(
                      key: ValueKey<int>(_selectedProcess?.pid ?? 0),
                      initialValue: _selectedProcess,
                      isExpanded: true,
                      isDense: true,
                      decoration: InputDecoration(
                        labelText: '进程',
                        border: const OutlineInputBorder(),
                        contentPadding: const EdgeInsets.symmetric(
                          horizontal: 10,
                          vertical: 8,
                        ),
                        suffixIcon: IconButton(
                          icon: _processLoading
                              ? const SizedBox(
                                  width: 24,
                                  height: 24,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                  ),
                                )
                              : const Icon(Icons.refresh),
                          onPressed: _processLoading
                              ? null
                              : _refreshProcessList,
                          tooltip: '刷新进程列表',
                        ),
                      ),
                      items: _processList
                          .map(
                            (ProcessEntry entry) =>
                                DropdownMenuItem<ProcessEntry>(
                                  value: entry,
                                  child: _buildProcessItem(entry),
                                ),
                          )
                          .toList(),
                      onChanged: (ProcessEntry? value) {
                        setState(() {
                          _selectedProcess = value;
                        });
                      },
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                SizedBox(
                  width: 140,
                  height: 40,
                  child: DropdownButtonFormField<String>(
                    initialValue: _selectedCaptureMode,
                    isExpanded: true,
                    isDense: true,
                    decoration: const InputDecoration(
                      labelText: '截图方式',
                      border: OutlineInputBorder(),
                      contentPadding: EdgeInsets.symmetric(
                        horizontal: 10,
                        vertical: 8,
                      ),
                    ),
                    items: _captureModes
                        .map(
                          (Map<String, String> mode) =>
                              DropdownMenuItem<String>(
                                value: mode['value'],
                                child: Text(
                                  mode['label'] ?? '',
                                  overflow: TextOverflow.ellipsis,
                                ),
                              ),
                        )
                        .toList(),
                    onChanged: (String? value) {
                      if (value == null) {
                        return;
                      }
                      setState(() {
                        _selectedCaptureMode = value;
                      });
                    },
                  ),
                ),
                const SizedBox(width: 12),
                ElevatedButton(
                  onPressed: _captureInProgress ? null : _captureOnce,
                  child: Text(_captureInProgress ? '截图中...' : '手动截图'),
                ),
                const SizedBox(width: 12),
                FilledButton(
                  onPressed: _autoEnabled
                      ? _stopAutoCapture
                      : _startAutoCapture,
                  child: Text(_autoEnabled ? '停止实时捕获' : '开始实时捕获'),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(children: [Expanded(child: Text(statusText))]),
            if (_errorMessage != null) ...[
              const SizedBox(height: 8),
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  _errorMessage!,
                  style: TextStyle(color: Theme.of(context).colorScheme.error),
                ),
              ),
            ],
            const SizedBox(height: 12),
            Expanded(
              child: Container(
                width: double.infinity,
                decoration: BoxDecoration(
                  border: Border.all(color: Colors.grey.shade300),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: _buildImagePreview(),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
