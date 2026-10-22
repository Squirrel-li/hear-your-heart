import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../lib/ppg_simulator.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('loads PPG (0x01) and combined acc+gyro (0x02) frames in protocol format', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);

    expect(dataSets, hasLength(10));
    expect(dataSets.first.frames, hasLength(51));
    var step = 0;
    for (final dataSet in dataSets.where((d) => d.frames.first.type != 0x05)) {
      final type = dataSet.frames.first.type;
      final length = type == 0x01 ? 20 : 21;
      step = type == 0x01 ? 5000 : 1000;
      expect(dataSet.frames, hasLength(type == 0x01 ? 51 : 60));
      for (var i = 0; i < dataSet.frames.length; i++) {
        expect(dataSet.frames[i].type, type);
        expect(dataSet.frames[i].bytes.length, length);
        if (i == 0) continue;
        expect(dataSet.frames[i].timestamp - dataSet.frames[i - 1].timestamp, step);
        expect(dataSet.frames[i].sequence - dataSet.frames[i - 1].sequence, 1);
      }
    }
  });

  test('loop boundary keeps timestamp and sequence continuous', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);
    final dataSet = dataSets.first;
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
    expect(next.bytes.length, 20);

    for (var i = 0; i < 20; i++) {
      if (i < 4 || (i >= 5 && i < 9)) continue;
      expect(next.bytes[i], dataSet.frames.first.bytes[i]);
    }

    final imuData = dataSets[6];
    final imu = PpgCursor(imuData);
    PpgPayload? lastImu;
    for (var i = 0; i < imuData.frames.length; i++) {
      lastImu = imu.peek(loop: true);
      imu.advance(loop: true);
    }
    final nextImu = imu.peek(loop: true)!;
    expect(nextImu.bytes[4], 0x02);
    expect(nextImu.timestamp, lastImu!.timestamp + 1000);
    expect(nextImu.sequence, lastImu.sequence + 1);
  });

  test('loads PPG raw batch (0x05) and counts sequence by samples', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);
    final raw = dataSets.last;
    expect(raw.frames.first.type, 0x05);
    expect(raw.frames, hasLength(1253));
    for (var i = 0; i < raw.frames.length; i++) {
      expect(raw.frames[i].bytes.length, 18);
      expect(raw.frames[i].samples, 2);
      expect(raw.frames[i].sequence, i * 2);
    }

    final cursor = PpgCursor(raw)..source = 1;
    PpgPayload? last;
    for (var i = 0; i < raw.frames.length; i++) {
      last = cursor.peek(loop: true);
      cursor.advance(loop: true);
    }
    final next = cursor.peek(loop: true)!;
    expect(next.bytes[4], 0x15);
    expect(next.sequence, last!.sequence + 2);
  });

  test('rejects 0x05 frames whose length is not 10 + 4N', () {
    const source = PpgSource('bad', 'x');
    expect(() => PpgDataLoader.parse(source, '00 00 00 38 05 00 00 00 00 01 0A 0E 0F'), throwsFormatException);
    expect(() => PpgDataLoader.parse(source, '00 00 00 38 05 00 00 00 00 01'), throwsFormatException);
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

  test('source nibble marks watch frames without changing msgType', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);
    final cursor = PpgCursor(dataSets.first)..source = 1;
    expect(cursor.peek(loop: false)!.bytes[4], 0x11);
    cursor.source = 0;
    expect(cursor.peek(loop: false)!.bytes[4], 0x01);
    expect(dataSets.first.frames.first.type, 0x01);  // stored frame untouched
  });
}
