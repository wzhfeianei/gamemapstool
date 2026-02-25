import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';
import 'image_search.dart';

abstract class WorkerMessage {
  final int id;
  WorkerMessage(this.id);
}

class SearchRequestMessage extends WorkerMessage {
  final Uint8List imageBytes;
  final List<SearchRequestStruct> requests;
  final int width;
  final int height;

  SearchRequestMessage(int id, this.imageBytes, this.requests,
      {this.width = 0, this.height = 0})
      : super(id);
}

class LoadTemplateMessage extends WorkerMessage {
  final String path;
  LoadTemplateMessage(int id, this.path) : super(id);
}

class ReleaseTemplateMessage extends WorkerMessage {
  final int templateId;
  ReleaseTemplateMessage(int id, this.templateId) : super(id);
}

class ReleaseAllTemplatesMessage extends WorkerMessage {
  ReleaseAllTemplatesMessage(int id) : super(id);
}

class WorkerResponse {
  final int id;
  final dynamic result;
  final String? error;

  WorkerResponse(this.id, this.result, {this.error});
}

class ImageSearchWorker {
  Isolate? _isolate;
  SendPort? _sendPort;
  final _receivePort = ReceivePort();
  final _completers = <int, Completer<dynamic>>{};
  int _nextId = 0;
  bool _isReady = false;
  final _readyCompleter = Completer<void>();

  Future<void> init() async {
    if (_isolate != null) return;

    _isolate = await Isolate.spawn(_isolateEntryPoint, _receivePort.sendPort);

    _receivePort.listen((message) {
      if (message is SendPort) {
        _sendPort = message;
        _isReady = true;
        _readyCompleter.complete();
      } else if (message is WorkerResponse) {
        final completer = _completers.remove(message.id);
        if (completer != null) {
          if (message.error != null) {
            completer.completeError(message.error!);
          } else {
            completer.complete(message.result);
          }
        }
      }
    });

    await _readyCompleter.future;
  }

  Future<List<SearchResultStruct>> findImagesBatch(
    Uint8List imageBytes,
    List<SearchRequestStruct> requests, {
    int width = 0,
    int height = 0,
  }) async {
    if (!_isReady) await _readyCompleter.future;

    final id = _nextId++;
    final completer = Completer<List<SearchResultStruct>>();
    _completers[id] = completer;

    _sendPort!.send(SearchRequestMessage(id, imageBytes, requests,
        width: width, height: height));
    return completer.future;
  }

  Future<int> loadTemplate(String path) async {
    if (!_isReady) await _readyCompleter.future;

    final id = _nextId++;
    final completer = Completer<int>();
    _completers[id] = completer;

    _sendPort!.send(LoadTemplateMessage(id, path));
    return completer.future;
  }

  Future<void> releaseTemplate(int templateId) async {
    if (!_isReady) await _readyCompleter.future;

    final id = _nextId++;
    final completer = Completer<void>();
    _completers[id] = completer;

    _sendPort!.send(ReleaseTemplateMessage(id, templateId));
    return completer.future;
  }

  Future<void> releaseAllTemplates() async {
    if (!_isReady) await _readyCompleter.future;

    final id = _nextId++;
    final completer = Completer<void>();
    _completers[id] = completer;

    _sendPort!.send(ReleaseAllTemplatesMessage(id));
    return completer.future;
  }

  void dispose() {
    _receivePort.close();
    _isolate?.kill();
    _isolate = null;
    _isReady = false;
    _completers.clear();
  }

  static void _isolateEntryPoint(SendPort sendPort) {
    final receivePort = ReceivePort();
    sendPort.send(receivePort.sendPort);

    // Initialize NativeImageSearch in the worker isolate
    final searcher = NativeImageSearch();

    receivePort.listen((message) {
      if (message is SearchRequestMessage) {
        try {
          final results = searcher.findImagesBatch(
            message.imageBytes,
            message.requests,
            width: message.width,
            height: message.height,
          );
          sendPort.send(WorkerResponse(message.id, results));
        } catch (e) {
          sendPort.send(WorkerResponse(message.id, null, error: e.toString()));
        }
      } else if (message is LoadTemplateMessage) {
        try {
          final templateId = searcher.loadTemplate(message.path);
          sendPort.send(WorkerResponse(message.id, templateId));
        } catch (e) {
          sendPort.send(WorkerResponse(message.id, null, error: e.toString()));
        }
      } else if (message is ReleaseTemplateMessage) {
        try {
          searcher.releaseTemplate(message.templateId);
          sendPort.send(WorkerResponse(message.id, null));
        } catch (e) {
          sendPort.send(WorkerResponse(message.id, null, error: e.toString()));
        }
      } else if (message is ReleaseAllTemplatesMessage) {
        try {
          searcher.releaseAllTemplates();
          sendPort.send(WorkerResponse(message.id, null));
        } catch (e) {
          sendPort.send(WorkerResponse(message.id, null, error: e.toString()));
        }
      }
    });
  }
}
