import 'dart:async';

import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'domain.dart';

class AppController extends ChangeNotifier {
  AppController({SimulatedSensorDataSource? sensor, DecisionEngine? decisionEngine, AudioPlayer? audioPlayer})
      : sensor = sensor ?? SimulatedSensorDataSource(),
        _engine = decisionEngine ?? DecisionEngine(),
        _player = audioPlayer ?? AudioPlayer();

  final SimulatedSensorDataSource sensor;
  final DecisionEngine _engine;
  final AudioPlayer _player;
  SharedPreferencesAsync? _preferences;

  StreamSubscription<SensorSnapshot>? _sensorSubscription;
  StreamSubscription<Duration>? _positionSubscription;
  StreamSubscription<Duration>? _durationSubscription;
  StreamSubscription<PlayerState>? _playerSubscription;
  Timer? _freshnessTimer;

  int age = 30;
  FitnessGoal goal = FitnessGoal.endurance;
  SensorSnapshot? snapshot;
  DecisionResult? decision;
  Track currentTrack = demoTracks[2];
  DateTime? trackStartedAt;
  DateTime? lastValidAt;
  Duration position = Duration.zero;
  Duration duration = const Duration(seconds: 12);
  bool isPlaying = false;

  bool get connected => !DecisionEngine.isStale(lastValidAt, DateTime.now());
  int get hrMax => DecisionEngine.hrMax(age);
  double get hrPercent => snapshot == null ? 0 : snapshot!.hr / hrMax;

  Future<void> initialize() async {
    _preferences = SharedPreferencesAsync();
    age = (await _preferences!.getInt('age') ?? 30).clamp(1, 120);
    final savedGoal = await _preferences!.getString('goal');
    goal = FitnessGoal.values.where((value) => value.name == savedGoal).firstOrNull ?? FitnessGoal.endurance;
    await _player.setReleaseMode(ReleaseMode.loop);
    _positionSubscription = _player.onPositionChanged.listen((value) {
      position = value;
      notifyListeners();
    });
    _durationSubscription = _player.onDurationChanged.listen((value) {
      duration = value;
      notifyListeners();
    });
    _playerSubscription = _player.onPlayerStateChanged.listen((value) {
      isPlaying = value == PlayerState.playing;
      notifyListeners();
    });
    _sensorSubscription = sensor.snapshots.listen(_onSnapshot);
    _freshnessTimer = Timer.periodic(const Duration(seconds: 1), (_) => notifyListeners());
    sensor.start();
  }

  void _onSnapshot(SensorSnapshot value) {
    snapshot = value;
    if (!value.valid) {
      notifyListeners();
      return;
    }
    lastValidAt = value.timestamp;
    final next = _engine.process(
      snapshot: value,
      age: age,
      now: value.timestamp,
      tracks: demoTracks,
      currentTrack: currentTrack,
      trackStartedAt: trackStartedAt,
    );
    decision = next;
    if (next.trackChanged) _selectTrack(next.track, autoplay: isPlaying);
    notifyListeners();
  }

  Future<void> _selectTrack(Track track, {required bool autoplay}) async {
    currentTrack = track;
    trackStartedAt = DateTime.now();
    position = Duration.zero;
    if (autoplay) {
      await _player.play(AssetSource(track.assetPath));
    } else {
      await _player.setSource(AssetSource(track.assetPath));
    }
    notifyListeners();
  }

  Future<void> togglePlayback() async {
    if (isPlaying) {
      await _player.pause();
    } else {
      trackStartedAt ??= DateTime.now();
      await _player.play(AssetSource(currentTrack.assetPath));
    }
  }

  Future<void> nextTrack([int direction = 1]) async {
    final index = demoTracks.indexOf(currentTrack);
    final rawNext = (index + direction) % demoTracks.length;
    final next = rawNext < 0 ? rawNext + demoTracks.length : rawNext;
    await _selectTrack(demoTracks[next], autoplay: isPlaying);
  }

  void selectTrack(Track track) => nextTrack(demoTracks.indexOf(track) - demoTracks.indexOf(currentTrack));
  void setScenario(SimulationScenario value) => sensor.setScenario(value);

  void setManualMode(bool value) {
    value ? sensor.setManual() : sensor.setScenario(sensor.scenario);
    notifyListeners();
  }

  void setManualHr(double value) => sensor.setManual(hr: value.round());
  void setManualRr(double value) => sensor.setManual(rr: value.round());
  void setManualActivity(ActivityLevel value) => sensor.setManual(activity: value);

  void setSimulationEnabled(bool value) {
    value ? sensor.start() : sensor.stop();
    notifyListeners();
  }

  Future<void> setAge(double value) async {
    age = value.round().clamp(1, 120);
    await _preferences?.setInt('age', age);
    notifyListeners();
  }

  Future<void> setGoal(FitnessGoal value) async {
    goal = value;
    await _preferences?.setString('goal', value.name);
    notifyListeners();
  }

  String get recommendation => switch (goal) {
    FitnessGoal.fatBurn => '維持 Zone 2，讓呼吸穩定並延長運動時間。',
    FitnessGoal.endurance => '以 Zone 2–3 穩態訓練為主，避免頻繁衝高。',
    FitnessGoal.recovery => '保持 Zone 1，搭配低 BPM 音樂逐步降載。',
  };

  @override
  void dispose() {
    _freshnessTimer?.cancel();
    _sensorSubscription?.cancel();
    _positionSubscription?.cancel();
    _durationSubscription?.cancel();
    _playerSubscription?.cancel();
    sensor.dispose();
    _player.dispose();
    super.dispose();
  }
}
