import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:gamemapstool/main.dart';

void main() {
  testWidgets('Capture page renders core controls', (
    WidgetTester tester,
  ) async {
    TestWidgetsFlutterBinding.ensureInitialized();
    const MethodChannel channel = MethodChannel('gamemapstool/capture');
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) async {
          if (call.method == 'listProcesses') {
            return <Map<String, Object?>>[
              <String, Object?>{
                'pid': 100,
                'name': 'game.exe',
                'cpu': 32.5,
                'icon': Uint8List(0),
              },
              <String, Object?>{
                'pid': 200,
                'name': 'demo.exe',
                'cpu': 10.0,
                'icon': Uint8List(0),
              },
            ];
          }
          return null;
        });
    await tester.pumpWidget(const MyApp());
    await tester.pumpAndSettle();
    expect(find.text('手动截图'), findsOneWidget);
    expect(find.text('开始自动截图'), findsOneWidget);
    expect(find.text('截图方式'), findsOneWidget);
    expect(find.text('旋转90°'), findsOneWidget);
    expect(find.text('灰度'), findsOneWidget);
    await tester.tap(find.text('灰度'));
    await tester.pump();
    expect(find.text('取消灰度'), findsOneWidget);
    await tester.tap(find.byIcon(Icons.rotate_right));
    await tester.pump();
    expect(find.text('配置'), findsOneWidget);
    await tester.tap(find.text('配置'));
    await tester.pumpAndSettle();
    expect(find.text('自动截图间隔（毫秒）'), findsOneWidget);
    await tester.tap(find.text('保存'));
    await tester.pump();
  });
}
