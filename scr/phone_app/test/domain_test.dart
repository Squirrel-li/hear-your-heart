import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:hear_your_heart/domain.dart';

SensorSnapshot sample(int hr, DateTime time, {ActivityLevel activity = ActivityLevel.light}) => SensorSnapshot(
  timestamp: time,
  hr: hr,
  respiratoryRate: 18,
  rmssd: 40,
  sdnn: 50,
  activity: activity,
  valid: true,
);

void main() {
  test('HRmax and all five zone boundaries are deterministic', () {
    expect(DecisionEngine.hrMax(20), 200);
    expect(DecisionEngine.zoneFor(119, 20), HeartZone.zone1);
    expect(DecisionEngine.zoneFor(120, 20), HeartZone.zone2);
    expect(DecisionEngine.zoneFor(140, 20), HeartZone.zone3);
    expect(DecisionEngine.zoneFor(160, 20), HeartZone.zone4);
    expect(DecisionEngine.zoneFor(180, 20), HeartZone.zone5);
  });

  test('normal zone changes require ten seconds', () {
    final engine = DecisionEngine();
    final start = DateTime(2026);
    engine.process(snapshot: sample(100, start), age: 20, now: start, tracks: demoTracks);
    final pending = engine.process(snapshot: sample(130, start), age: 20, now: start, tracks: demoTracks);
    final earlyTime = start.add(const Duration(seconds: 9));
    final early = engine.process(snapshot: sample(130, earlyTime), age: 20, now: earlyTime, tracks: demoTracks);
    final settledTime = start.add(const Duration(seconds: 10));
    final settled = engine.process(snapshot: sample(130, settledTime), age: 20, now: settledTime, tracks: demoTracks);
    expect(pending.activeZone, HeartZone.zone1);
    expect(early.activeZone, HeartZone.zone1);
    expect(settled.activeZone, HeartZone.zone2);
  });

  test('track is held for thirty seconds', () {
    final engine = DecisionEngine();
    final start = DateTime(2026);
    final current = demoTracks.last;
    final early = engine.process(snapshot: sample(120, start), age: 20, now: start, tracks: demoTracks, currentTrack: current, trackStartedAt: start);
    final readyTime = start.add(const Duration(seconds: 31));
    final ready = engine.process(snapshot: sample(120, readyTime), age: 20, now: readyTime, tracks: demoTracks, currentTrack: current, trackStartedAt: start);
    expect(early.track, current);
    expect(early.trackChanged, isFalse);
    expect(ready.trackChanged, isTrue);
    expect(ready.track.bpm, 115);
  });

  test('Zone 5 immediately selects cool-down music', () {
    final engine = DecisionEngine();
    final start = DateTime(2026);
    engine.process(snapshot: sample(120, start), age: 20, now: start, tracks: demoTracks);
    final overload = start.add(const Duration(seconds: 1));
    final result = engine.process(snapshot: sample(190, overload), age: 20, now: overload, tracks: demoTracks, currentTrack: demoTracks.last, trackStartedAt: start);
    expect(result.activeZone, HeartZone.zone5);
    expect(result.mode, MusicMode.coolDown);
    expect(result.track.style, 'relax');
    expect(result.trackChanged, isTrue);
  });

  test('sensor is stale only after three seconds', () {
    final start = DateTime(2026);
    expect(DecisionEngine.isStale(start, start.add(const Duration(seconds: 3))), isFalse);
    expect(DecisionEngine.isStale(start, start.add(const Duration(milliseconds: 3001))), isTrue);
  });

  test('activity resolves equal-BPM ties by energy', () {
    const tracks = [
      Track(title: 'low', bpm: 120, energy: .2, style: 'drive', assetPath: 'low.wav'),
      Track(title: 'high', bpm: 120, energy: .8, style: 'drive', assetPath: 'high.wav'),
    ];
    expect(DecisionEngine.selectTrack(tracks: tracks, targetBpm: 120, mode: MusicMode.motivation, activity: ActivityLevel.idle).title, 'low');
    expect(DecisionEngine.selectTrack(tracks: tracks, targetBpm: 120, mode: MusicMode.motivation, activity: ActivityLevel.active).title, 'high');
  });

  test('catalog points to six generated WAV assets', () {
    expect(demoTracks, hasLength(6));
    for (final track in demoTracks) {
      final file = File('assets/${track.assetPath}');
      expect(file.existsSync(), isTrue, reason: track.assetPath);
      expect(file.lengthSync(), greaterThan(44));
    }
  });
}
