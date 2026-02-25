import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

// 定义 C 结构体对应的 Dart 结构
base class SearchResult extends Struct {
  @Int32()
  external int x;

  @Int32()
  external int y;

  @Double()
  external double score;
}

// 定义 C 函数类型
typedef LoadTemplateC = Int32 Function(Pointer<Utf8> path);
typedef LoadTemplateDart = int Function(Pointer<Utf8> path);

typedef ReleaseTemplateC = Void Function(Int32 id);
typedef ReleaseTemplateDart = void Function(int id);

typedef ReleaseAllTemplatesC = Void Function();
typedef ReleaseAllTemplatesDart = void Function();

typedef FindImageC = SearchResult Function(
    Int32 id, Int32 x, Int32 y, Int32 w, Int32 h, Double threshold);
typedef FindImageDart = SearchResult Function(
    int id, int x, int y, int w, int h, double threshold);

typedef DebugSaveLastCaptureC = Void Function(Pointer<Utf8> path);
typedef DebugSaveLastCaptureDart = void Function(Pointer<Utf8> path);

// 批量查找接口定义
typedef FindImagesBatchC = Void Function(
    Pointer<Uint8> imageBytes, Int32 length,
    Int32 width, Int32 height, Int32 stride,
    Pointer<SearchRequest> requests, Int32 count,
    Pointer<SearchResultItem> results);
typedef FindImagesBatchDart = void Function(
    Pointer<Uint8> imageBytes, int length,
    int width, int height, int stride,
    Pointer<SearchRequest> requests, int count,
    Pointer<SearchResultItem> results);

class NativeImageSearch {
  static NativeImageSearch? _instance;
  late DynamicLibrary _lib;

  late LoadTemplateDart _loadTemplate;
  late ReleaseTemplateDart _releaseTemplate;
  late ReleaseAllTemplatesDart _releaseAllTemplates;
  late FindImageDart _findImage;
  late DebugSaveLastCaptureDart _debugSaveLastCapture;
  late FindImagesBatchDart _findImagesBatch;

  factory NativeImageSearch() {
    _instance ??= NativeImageSearch._internal();
    return _instance!;
  }

  NativeImageSearch._internal() {
    // 加载 DLL
    // 在 Debug 模式下，DLL 通常位于构建目录或与 exe 同级
    // 在 Release 模式下，DLL 与 exe 同级
    try {
      if (Platform.isWindows) {
        _lib = DynamicLibrary.open('native_image_search.dll');
      } else {
        throw UnsupportedError('This plugin only supports Windows');
      }

      // 绑定函数
      _loadTemplate = _lib
          .lookupFunction<LoadTemplateC, LoadTemplateDart>('load_template');
      _releaseTemplate = _lib
          .lookupFunction<ReleaseTemplateC, ReleaseTemplateDart>('release_template');
      _releaseAllTemplates = _lib
          .lookupFunction<ReleaseAllTemplatesC, ReleaseAllTemplatesDart>('release_all_templates');
      _findImage = _lib
          .lookupFunction<FindImageC, FindImageDart>('find_image');
      _debugSaveLastCapture = _lib
          .lookupFunction<DebugSaveLastCaptureC, DebugSaveLastCaptureDart>('debug_save_last_capture');
      _findImagesBatch = _lib
          .lookupFunction<FindImagesBatchC, FindImagesBatchDart>('find_images_batch');
          
    } catch (e) {
      print('Failed to load native_image_search.dll: $e');
      // 可以选择抛出异常或降级处理
      rethrow;
    }
  }

  /// 加载模板图片，返回 ID
  /// 如果 ID <= 0 表示加载失败
  int loadTemplate(String imagePath) {
    final pathPtr = imagePath.toNativeUtf8();
    try {
      return _loadTemplate(pathPtr);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// 释放指定模板
  void releaseTemplate(int id) {
    _releaseTemplate(id);
  }

  /// 释放所有模板
  void releaseAllTemplates() {
    _releaseAllTemplates();
  }

  /// 在指定区域查找图片
  /// [x], [y], [w], [h] 定义查找区域 (ROI)
  /// 如果 [w] 或 [h] <= 0，则查找全屏
  /// [threshold] 匹配阈值 (0.0 - 1.0)
  /// 返回 SearchResultResult (x, y, score)
  SearchResult findImage(int templateId,
      {int x = 0, int y = 0, int w = -1, int h = -1, double threshold = 0.9}) {
    return _findImage(templateId, x, y, w, h, threshold);
  }
  
  /// 调试：保存最后一次截图
  void debugSaveLastCapture(String path) {
    final pathPtr = path.toNativeUtf8();
    try {
      _debugSaveLastCapture(pathPtr);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// 批量查找图片
  /// [imageBytes] 源图片数据 (PNG/JPG 或 Raw BGRA)
  /// [width], [height] 如果是 Raw 数据，必须提供宽高；如果是压缩数据，传 0
  /// [requests] 任务列表
  /// 返回: 结果列表 (与 requests 一一对应)
  List<SearchResultStruct> findImagesBatch(
    Uint8List imageBytes, 
    List<SearchRequestStruct> requests,
    {int width = 0, int height = 0}
  ) {
    if (requests.isEmpty) return [];

    final int count = requests.length;
    final int stride = width * 4; // 假设标准 stride

    // 分配内存
    final imgPtr = calloc<Uint8>(imageBytes.length);
    final imgList = imgPtr.asTypedList(imageBytes.length);
    imgList.setAll(0, imageBytes);

    final reqPtr = calloc<SearchRequest>(count);
    for (int i = 0; i < count; i++) {
      final req = reqPtr[i];
      req.templateId = requests[i].templateId;
      req.roiX = requests[i].roiX;
      req.roiY = requests[i].roiY;
      req.roiW = requests[i].roiW;
      req.roiH = requests[i].roiH;
      req.threshold = requests[i].threshold;
    }

    final resPtr = calloc<SearchResultItem>(count);

    try {
      _findImagesBatch(
        imgPtr, imageBytes.length, 
        width, height, stride, 
        reqPtr, count, 
        resPtr
      );

      return List.generate(count, (i) {
        final item = resPtr[i];
        return SearchResultStruct(
          templateId: item.templateId,
          x: item.x,
          y: item.y,
          score: item.score
        );
      });

    } finally {
      calloc.free(imgPtr);
      calloc.free(reqPtr);
      calloc.free(resPtr);
    }
  }
}

// 纯 Dart 类用于批量请求 (避免暴露 FFI Struct)
class SearchResultStruct {
  final int templateId;
  final int x;
  final int y;
  final double score;

  SearchResultStruct({
    required this.templateId, 
    required this.x, 
    required this.y, 
    required this.score
  });
}

class SearchRequestStruct {
  final int templateId;
  final int roiX, roiY, roiW, roiH;
  final double threshold;

  SearchRequestStruct(this.templateId, {
    this.roiX = 0, this.roiY = 0, this.roiW = -1, this.roiH = -1, 
    this.threshold = 0.9
  });
}

// FFI 结构体定义 (与 C++ 对应)
base class SearchRequest extends Struct {
  @Int32() external int templateId;
  @Int32() external int roiX;
  @Int32() external int roiY;
  @Int32() external int roiW;
  @Int32() external int roiH;
  @Double() external double threshold;
}

base class SearchResultItem extends Struct {
  @Int32() external int templateId;
  @Int32() external int x;
  @Int32() external int y;
  @Double() external double score;
}
class ImageTemplate {
  final int id;
  final String path;
  bool _disposed = false;

  ImageTemplate._(this.id, this.path);

  static ImageTemplate? load(String path) {
    final id = NativeImageSearch().loadTemplate(path);
    if (id <= 0) return null;
    return ImageTemplate._(id, path);
  }

  /// 在指定区域查找
  SearchResult? find({
    int x = 0, 
    int y = 0, 
    int w = -1, 
    int h = -1, 
    double threshold = 0.9
  }) {
    if (_disposed) throw StateError('Template already disposed');
    
    final result = NativeImageSearch().findImage(id, x: x, y: y, w: w, h: h, threshold: threshold);
    if (result.score >= threshold) {
      return result;
    }
    return null;
  }

  void dispose() {
    if (!_disposed) {
      NativeImageSearch().releaseTemplate(id);
      _disposed = true;
    }
  }
}
