import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../lib/ppg_simulator.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('loads six datasets with 51 21-byte frames and big-endian fields', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);

    expect(dataSets, hasLength(6));
    for (final dataSet in dataSets) {
      expect(dataSet.frames, hasLength(51));
      expect(dataSet.frames.every((frame) => frame.bytes.length == 21), isTrue);
      for (var i = 1; i < dataSet.frames.length; i++) {
        expect(dataSet.frames[i].timestamp - dataSet.frames[i - 1].timestamp, 5000);
        expect(dataSet.frames[i].sequence - dataSet.frames[i - 1].sequence, 1);
      }
    }
  });

  test('loop boundary keeps timestamp and sequence continuous', () async {
    final dataSet = (await PpgDataLoader.load(rootBundle.loadString)).first;
    final cursor = PpgCursor(dataSet);
    PpgPayload? last;

    for (var i = 0; i < dataSet.frames.length; i++) {
      last = cursor.peek(loop: true);
      expect(last, isNotNull);
      cursor.advance(loop: true);
    }

    final next = cursor.peek(loop: true)!;
    expect(next.rawIndex, 0);
    expect(next.cycle, 1);
    expect(next.timestamp, last!.timestamp + 5000);
    expect(next.sequence, last.sequence + 1);
    expect(next.bytes.length, 21);

    for (var i = 0; i < 21; i++) {
      if (i < 4 || (i >= 5 && i < 9)) continue;
      expect(next.bytes[i], dataSet.frames.first.bytes[i]);
    }
  });

  test('switching dataset resets cursor position and loop offsets', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);
    final cursor = PpgCursor(dataSets.first);
    cursor.advance(loop: false);
    expect(cursor.peek(loop: false)!.rawIndex, 1);

    cursor.reset(dataSets.last);
    final first = cursor.peek(loop: false)!;
    expect(first.rawIndex, 0);
    expect(first.cycle, 0);
    expect(first.timestamp, dataSets.last.frames.first.timestamp);
    expect(first.sequence, dataSets.last.frames.first.sequence);
  });
}

