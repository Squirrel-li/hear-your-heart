import 'dart:async';
import 'dart:math';

enum ActivityLevel { idle, light, active }
enum FitnessGoal { fatBurn, endurance, recovery }
enum SimulationScenario { warmup, aerobic, endurance, overload }
enum HeartZone { zone1, zone2, zone3, zone4, zone5 }
enum MusicMode { motivation, coolDown }

extension ActivityLevelText on ActivityLevel {
  String get label => switch (this) {
    ActivityLevel.idle => '休息',
    ActivityLevel.light => '輕度',
    ActivityLevel.active => '活躍',
  };
}

extension FitnessGoalText on FitnessGoal {
  String get label => switch (this) {
    FitnessGoal.fatBurn => '減脂',
    FitnessGoal.endurance => '耐力',
    FitnessGoal.recovery => '恢復',
  };
}

extension ScenarioText on SimulationScenario {
  String get label => switch (this) {
    SimulationScenario.warmup => '暖身',
    SimulationScenario.aerobic => '有氧',
    SimulationScenario.endurance => '耐力',
    SimulationScenario.overload => '過載',
  };
}

extension HeartZoneText on HeartZone {
  String get label => 'Zone ${index + 1}';
}

class SensorSnapshot {
  const SensorSnapshot({
    required this.timestamp,
    required this.hr,
    required this.respiratoryRate,
    required this.rmssd,
    required this.sdnn,
    required this.activity,
    required this.valid,
  });

  final DateTime timestamp;
  final int hr;
  final int respiratoryRate;
  final double rmssd;
  final double sdnn;
  final ActivityLevel activity;
  final bool valid;
}

abstract interface class SensorDataSource {
  Stream<SensorSnapshot> get snapshots;
  bool get isRunning;
  void start();
  void stop();
  void dispose();
}

class SimulatedSensorDataSource implements SensorDataSource {
  final _controller = StreamController<SensorSnapshot>.broadcast();
  Timer? _timer;
  int _tick = 0;
  int _lastRr = 14;

  SimulationScenario scenario = SimulationScenario.aerobic;
  bool manual = false;
  int manualHr = 110;
  int manualRr = 18;
  ActivityLevel manualActivity = ActivityLevel.light;

  @override
  Stream<SensorSnapshot> get snapshots => _controller.stream;
  @override
  bool get isRunning => _timer != null;

  @override
  void start() {
    if (_timer != null) return;
    _emit();
    _timer = Timer.periodic(const Duration(seconds: 1), (_) => _emit());
  }

  @override
  void stop() {
    _timer?.cancel();
    _timer = null;
  }

  void setScenario(SimulationScenario value) {
    scenario = value;
    manual = false;
    _tick = 0;
    _emit();
  }

  void setManual({int? hr, int? rr, ActivityLevel? activity}) {
    manual = true;
    manualHr = hr ?? manualHr;
    manualRr = rr ?? manualRr;
    manualActivity = activity ?? manualActivity;
    _emit();
  }

  void _emit() {
    if (_controller.isClosed) return;
    _tick++;
    final (baseHr, baseRr, activity) = switch (scenario) {
      SimulationScenario.warmup => (88, 14, ActivityLevel.light),
      SimulationScenario.aerobic => (118, 19, ActivityLevel.light),
      SimulationScenario.endurance => (146, 24, ActivityLevel.active),
      SimulationScenario.overload => (178, 32, ActivityLevel.active),
    };
    final hr = manual ? manualHr : baseHr + (sin(_tick / 3) * 3).round();
    if (manual || _tick == 1 || _tick % 10 == 0) {
      _lastRr = manual ? manualRr : baseRr + (sin(_tick / 7) * 2).round();
    }
    _controller.add(SensorSnapshot(
      timestamp: DateTime.now(),
      hr: hr,
      respiratoryRate: _lastRr,
      rmssd: max(18, 62 - hr * 0.22),
      sdnn: max(24, 78 - hr * 0.25),
      activity: manual ? manualActivity : activity,
      valid: true,
    ));
  }

  @override
  void dispose() {
    stop();
    _controller.close();
  }
}

class Track {
  const Track({required this.title, required this.bpm, required this.energy, required this.style, required this.assetPath});
  final String title;
  final int bpm;
  final double energy;
  final String style;
  final String assetPath;
}

const demoTracks = <Track>[
  Track(title: '晨光舒展', bpm: 85, energy: .24, style: 'relax', assetPath: 'audio/demo_85.wav'),
  Track(title: '平穩呼吸', bpm: 100, energy: .34, style: 'relax', assetPath: 'audio/demo_100.wav'),
  Track(title: '城市步頻', bpm: 115, energy: .45, style: 'drive', assetPath: 'audio/demo_115.wav'),
  Track(title: '穩態巡航', bpm: 130, energy: .62, style: 'drive', assetPath: 'audio/demo_130.wav'),
  Track(title: '向前加速', bpm: 145, energy: .78, style: 'drive', assetPath: 'audio/demo_145.wav'),
  Track(title: '極速節拍', bpm: 160, energy: .90, style: 'drive', assetPath: 'audio/demo_160.wav'),
];

class DecisionResult {
  const DecisionResult({required this.observedZone, required this.activeZone, required this.mode, required this.targetBpm, required this.track, required this.trackChanged});
  final HeartZone observedZone;
  final HeartZone activeZone;
  final MusicMode mode;
  final int targetBpm;
  final Track track;
  final bool trackChanged;
}

class DecisionEngine {
  DecisionEngine({this.zoneDwell = const Duration(seconds: 10), this.minimumTrackTime = const Duration(seconds: 30)});

  final Duration zoneDwell;
  final Duration minimumTrackTime;
  HeartZone? _activeZone;
  HeartZone? _pendingZone;
  DateTime? _pendingSince;

  static int hrMax(int age) => 220 - age.clamp(1, 120);

  static HeartZone zoneFor(int hr, int age) {
    final ratio = hr / hrMax(age);
    if (ratio < .60) return HeartZone.zone1;
    if (ratio < .70) return HeartZone.zone2;
    if (ratio < .80) return HeartZone.zone3;
    if (ratio < .90) return HeartZone.zone4;
    return HeartZone.zone5;
  }

  static bool isStale(DateTime? lastValidAt, DateTime now) => lastValidAt == null || now.difference(lastValidAt) > const Duration(seconds: 3);

  static Track selectTrack({required List<Track> tracks, required int targetBpm, required MusicMode mode, required ActivityLevel activity}) {
    final style = mode == MusicMode.coolDown ? 'relax' : 'drive';
    final candidates = tracks.where((track) => track.style == style).toList();
    candidates.sort((a, b) {
      final bpmOrder = (a.bpm - targetBpm).abs().compareTo((b.bpm - targetBpm).abs());
      if (bpmOrder != 0) return bpmOrder;
      return activity == ActivityLevel.active ? b.energy.compareTo(a.energy) : a.energy.compareTo(b.energy);
    });
    return candidates.first;
  }

  DecisionResult process({
    required SensorSnapshot snapshot,
    required int age,
    required DateTime now,
    required List<Track> tracks,
    Track? currentTrack,
    DateTime? trackStartedAt,
  }) {
    final observed = zoneFor(snapshot.hr, age);
    _activeZone ??= observed;
    if (observed == _activeZone) {
      _pendingZone = null;
      _pendingSince = null;
    } else if (observed == HeartZone.zone5) {
      _activeZone = observed;
      _pendingZone = null;
      _pendingSince = null;
    } else if (_pendingZone != observed) {
      _pendingZone = observed;
      _pendingSince = now;
    } else if (now.difference(_pendingSince!) >= zoneDwell) {
      _activeZone = observed;
      _pendingZone = null;
      _pendingSince = null;
    }

    final active = _activeZone!;
    final mode = active.index >= HeartZone.zone4.index ? MusicMode.coolDown : MusicMode.motivation;
    final target = active.index >= HeartZone.zone4.index ? (snapshot.hr * .75).round() : snapshot.hr;
    final desired = selectTrack(tracks: tracks, targetBpm: target, mode: mode, activity: snapshot.activity);
    final mayChange = currentTrack == null || active == HeartZone.zone5 || (trackStartedAt != null && now.difference(trackStartedAt) >= minimumTrackTime);
    final changed = mayChange && desired != currentTrack;
    return DecisionResult(
      observedZone: observed,
      activeZone: active,
      mode: mode,
      targetBpm: target,
      track: changed ? desired : currentTrack ?? desired,
      trackChanged: changed,
    );
  }
}
