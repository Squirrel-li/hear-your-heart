import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../lib/ppg_simulator.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('loads PPG and paired Acc/Gyro frames in protocol format', () async {
    final dataSets = await PpgDataLoader.load(rootBundle.loadString);

    expect(dataSets, hasLength(9));
    expect(dataSets.first.frames, hasLength(51));
    for (final dataSet in dataSets) {
      if (dataSet.frames.first.type == 0x01) {
        expect(dataSet.frames, hasLength(51));
        expect(dataSet.frames.every((frame) => frame.bytes.length == 20), isTrue);
        for (var i = 1; i < dataSet.frames.length; i++) {
          expect(dataSet.frames[i].type, 0x01);
          expect(dataSet.frames[i].timestamp - dataSet.frames[i - 1].timestamp, 5000);
          expect(dataSet.frames[i].sequence - dataSet.frames[i - 1].sequence, 1);
        }
      } else {
        expect(dataSet.frames, hasLength(102));
        for (var i = 0; i < dataSet.frames.length; i += 2) {
          final acc = dataSet.frames[i];
          final gyro = dataSet.frames[i + 1];
          expect(acc.type, 0x02);
          expect(gyro.type, 0x03);
          expect(acc.bytes.length, 15);
          expect(gyro.bytes.length, 15);
          expect(gyro.timestamp, acc.timestamp);
          expect(gyro.sequence, acc.sequence);
          if (i + 2 < dataSet.frames.length) {
            expect(dataSet.frames[i + 2].timestamp - gyro.timestamp, 1000);
            expect(dataSet.frames[i + 2].sequence - gyro.sequence, 1);
          }
        }
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
