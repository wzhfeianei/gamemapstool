import 'dart:async';
import 'dart:math';
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
    required this.cpu,
    required this.iconBytes,
  });

  final int pid;
  final String name;
  final double cpu;
  final Uint8List? iconBytes;

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
    <String, String>{'value': 'auto', 'label': '自动'},
    <String, String>{'value': 'printWindow', 'label': 'PrintWindow'},
    <String, String>{'value': 'wgc', 'label': 'Graphics Capture'},
  ];
  String _selectedCaptureMode = 'auto';

  Uint8List? _imageBytes;
  String? _errorMessage;
  DateTime? _lastCapturedAt;
  int? _lastCaptureDurationMs;
  Timer? _autoTimer;
  bool _autoEnabled = false;
  bool _captureInProgress = false;
  int _intervalMs = 1000;
  int _rotationQuarterTurns = 0;
  bool _flipHorizontal = false;
  bool _grayscale = false;

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
      final List<Object?>? result =
          await _channel.invokeMethod<List<Object?>>('listProcesses');
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
        final Object? cpuValue = item['cpu'];
        final Object? iconValue = item['icon'];
        final int pid = pidValue is int
            ? pidValue
            : pidValue is num
                ? pidValue.toInt()
                : 0;
        final String name =
            nameValue is String ? nameValue.trim() : nameValue?.toString() ?? '';
        final double cpu = cpuValue is double
            ? cpuValue
            : cpuValue is num
                ? cpuValue.toDouble()
                : 0.0;
        final Uint8List? icon =
            iconValue is Uint8List ? iconValue : null;
        if (pid > 0 && name.isNotEmpty) {
          processes.add(
            ProcessEntry(pid: pid, name: name, cpu: cpu, iconBytes: icon),
          );
        }
      }
      final int? selectedPid = _selectedProcess?.pid;
      setState(() {
        _processList = processes;
        if (selectedPid != null) {
          for (final ProcessEntry entry in processes) {
            if (entry.pid == selectedPid) {
              _selectedProcess = entry;
              break;
            }
          }
        }
        _selectedProcess ??= processes.isNotEmpty ? processes.first : null;
      });
    } on PlatformException catch (e) {
      if (!mounted) {
        return;
      }
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
        }
      });
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

  void _startAutoCapture() {
    if (_autoEnabled) {
      return;
    }
    setState(() {
      _autoEnabled = true;
      _errorMessage = null;
    });
    _autoTimer?.cancel();
    _autoTimer = Timer.periodic(Duration(milliseconds: _intervalMs), (_) {
      _captureOnce();
    });
  }

  void _stopAutoCapture() {
    _autoTimer?.cancel();
    setState(() {
      _autoEnabled = false;
    });
  }

  void _updateInterval(int value) {
    setState(() {
      _intervalMs = value;
    });
    if (_autoEnabled) {
      _startAutoCapture();
    }
  }

  void _resetImageOps() {
    setState(() {
      _rotationQuarterTurns = 0;
      _flipHorizontal = false;
      _grayscale = false;
    });
  }

  ColorFilter? _buildColorFilter() {
    if (!_grayscale) {
      return null;
    }
    return const ColorFilter.matrix(<double>[
      0.2126, 0.7152, 0.0722, 0, 0,
      0.2126, 0.7152, 0.0722, 0, 0,
      0.2126, 0.7152, 0.0722, 0, 0,
      0, 0, 0, 1, 0,
    ]);
  }

  Widget _buildImagePreview() {
    if (_imageBytes == null || _imageBytes!.isEmpty) {
      return Container(
        alignment: Alignment.center,
        color: Colors.grey.shade100,
        child: const Text('暂无截图'),
      );
    }
    final ColorFilter? filter = _buildColorFilter();
    Widget image = Image.memory(
      _imageBytes!,
      key: ValueKey<int>(_imageVersion),
      gaplessPlayback: true,
      fit: BoxFit.contain,
    );
    if (filter != null) {
      image = ColorFiltered(colorFilter: filter, child: image);
    }
    image = Transform(
      alignment: Alignment.center,
      transform: Matrix4.identity()
        ..rotateZ(_rotationQuarterTurns * (pi / 2))
        ..scaleByDouble(_flipHorizontal ? -1.0 : 1.0, 1.0, 1.0, 1.0),
      child: image,
    );
    return InteractiveViewer(
      minScale: 0.2,
      maxScale: 5,
      child: Center(child: image),
    );
  }

  Widget _buildProcessItem(ProcessEntry entry) {
    final Widget icon = entry.iconBytes != null && entry.iconBytes!.isNotEmpty
        ? Image.memory(entry.iconBytes!, width: 20, height: 20)
        : const Icon(Icons.apps, size: 20);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        icon,
        const SizedBox(width: 8),
        ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 220),
          child: Text(
            entry.name,
            overflow: TextOverflow.ellipsis,
          ),
        ),
        const SizedBox(width: 8),
        Text('${entry.cpu.toStringAsFixed(1)}%'),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final String statusText = _lastCapturedAt == null
        ? '尚未截图'
        : '最近截图：${_lastCapturedAt!.toLocal().toIso8601String()} 用时：${_lastCaptureDurationMs ?? 0} ms';
    return Scaffold(
      appBar: AppBar(
        title: const Text('游戏截图工具'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                Expanded(
                  child: DropdownButtonFormField<ProcessEntry>(
                    key: ValueKey<int>(_selectedProcess?.pid ?? 0),
                    initialValue: _selectedProcess,
                    items: _processList
                        .map((ProcessEntry entry) =>
                            DropdownMenuItem<ProcessEntry>(
                              value: entry,
                              child: _buildProcessItem(entry),
                            ))
                        .toList(),
                    onChanged: (ProcessEntry? value) {
                      setState(() {
                        _selectedProcess = value;
                      });
                    },
                    decoration: const InputDecoration(
                      labelText: '进程',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                SizedBox(
                  width: 180,
                  child: DropdownButtonFormField<String>(
                    initialValue: _selectedCaptureMode,
                    isExpanded: true,
                    items: _captureModes
                        .map((Map<String, String> mode) =>
                            DropdownMenuItem<String>(
                              value: mode['value'],
                              child: Text(
                                mode['label'] ?? '',
                                overflow: TextOverflow.ellipsis,
                              ),
                            ))
                        .toList(),
                    onChanged: (String? value) {
                      if (value == null) {
                        return;
                      }
                      setState(() {
                        _selectedCaptureMode = value;
                      });
                    },
                    decoration: const InputDecoration(
                      labelText: '截图方式',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                OutlinedButton.icon(
                  onPressed: _processLoading ? null : _refreshProcessList,
                  icon: const Icon(Icons.refresh),
                  label: Text(_processLoading ? '刷新中' : '刷新'),
                ),
                const SizedBox(width: 12),
                OutlinedButton.icon(
                  onPressed: () async {
                    final int? updatedInterval = await Navigator.of(context).push(
                      MaterialPageRoute<int>(
                        builder: (BuildContext context) => SettingsPage(
                          intervalMs: _intervalMs,
                        ),
                      ),
                    );
                    if (updatedInterval != null) {
                      _updateInterval(updatedInterval);
                    }
                  },
                  icon: const Icon(Icons.settings),
                  label: const Text('配置'),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                ElevatedButton(
                  onPressed: _captureInProgress ? null : _captureOnce,
                  child: Text(_captureInProgress ? '截图中...' : '手动截图'),
                ),
                const SizedBox(width: 12),
                FilledButton(
                  onPressed: _autoEnabled ? _stopAutoCapture : _startAutoCapture,
                  child: Text(_autoEnabled ? '停止自动截图' : '开始自动截图'),
                ),
                const SizedBox(width: 12),
                OutlinedButton(
                  onPressed: () {
                    setState(() {
                      _imageBytes = null;
                      _errorMessage = null;
                      _lastCapturedAt = null;
                    });
                  },
                  child: const Text('清除图片'),
                ),
                const Spacer(),
                Text(statusText),
              ],
            ),
            const SizedBox(height: 12),
            Wrap(
              spacing: 12,
              runSpacing: 8,
              children: [
                OutlinedButton.icon(
                  onPressed: () {
                    setState(() {
                      _rotationQuarterTurns =
                          (_rotationQuarterTurns + 1) % 4;
                    });
                  },
                  icon: const Icon(Icons.rotate_right),
                  label: const Text('旋转90°'),
                ),
                OutlinedButton.icon(
                  onPressed: () {
                    setState(() {
                      _flipHorizontal = !_flipHorizontal;
                    });
                  },
                  icon: const Icon(Icons.flip),
                  label: Text(_flipHorizontal ? '取消水平翻转' : '水平翻转'),
                ),
                OutlinedButton.icon(
                  onPressed: () {
                    setState(() {
                      _grayscale = !_grayscale;
                    });
                  },
                  icon: const Icon(Icons.filter_b_and_w),
                  label: Text(_grayscale ? '取消灰度' : '灰度'),
                ),
                OutlinedButton.icon(
                  onPressed: _resetImageOps,
                  icon: const Icon(Icons.restart_alt),
                  label: const Text('重置操作'),
                ),
              ],
            ),
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

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.intervalMs});

  final int intervalMs;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final TextEditingController _intervalController;
  String? _errorMessage;

  @override
  void initState() {
    super.initState();
    _intervalController =
        TextEditingController(text: widget.intervalMs.toString());
  }

  @override
  void dispose() {
    _intervalController.dispose();
    super.dispose();
  }

  void _save() {
    final int? value = int.tryParse(_intervalController.text.trim());
    if (value == null || value <= 0) {
      setState(() {
        _errorMessage = '请输入大于0的毫秒数';
      });
      return;
    }
    Navigator.of(context).pop(value);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('配置'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: _intervalController,
              decoration: const InputDecoration(
                labelText: '自动截图间隔（毫秒）',
                border: OutlineInputBorder(),
              ),
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
            ),
            if (_errorMessage != null) ...[
              const SizedBox(height: 8),
              Text(
                _errorMessage!,
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              ),
            ],
            const SizedBox(height: 16),
            Align(
              alignment: Alignment.centerRight,
              child: FilledButton(
                onPressed: _save,
                child: const Text('保存'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
