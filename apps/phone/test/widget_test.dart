import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:hear_your_heart/app_controller.dart';
import 'package:hear_your_heart/main.dart';

void main() {
  testWidgets('three primary screens are reachable', (tester) async {
    final controller = AppController();
    addTearDown(controller.dispose);
    await tester.pumpWidget(HeartMusicApp(controller: controller));
    expect(find.text('即時心率'), findsOneWidget);

    await tester.tap(find.byIcon(Icons.headphones_outlined));
    await tester.pumpAndSettle();
    expect(find.byIcon(Icons.graphic_eq_rounded), findsOneWidget);

    await tester.tap(find.byIcon(Icons.tune_outlined));
    await tester.pumpAndSettle();
    expect(find.text('個人設定'), findsOneWidget);
  });
}
