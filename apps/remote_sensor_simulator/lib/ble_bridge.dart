import 'package:flutter/services.dart';

class BleStatus {
  const BleStatus({
    this.bluetoothAvailable = false,
    this.bluetoothEnabled = false,
    this.advertising = false,
    this.connected = false,
    this.subscribed = false,
    this.name = 'BLE_watch_ITAP',
    this.profile = 0,
    this.error,
  });

  final bool bluetoothAvailable;
  final bool bluetoothEnabled;
  final bool advertising;
  final bool connected;
  final bool subscribed;
  final String name;
  final int profile;
  final String? error;

  factory BleStatus.fromMap(Map<dynamic, dynamic> map) => BleStatus(
        bluetoothAvailable: map['bluetoothAvailable'] == true,
        bluetoothEnabled: map['bluetoothEnabled'] == true,
        advertising: map['advertising'] == true,
        connected: map['connected'] == true,
        subscribed: map['subscribed'] == true,
        name: map['name'] as String? ?? 'BLE_watch_ITAP',
        profile: map['profile'] as int? ?? 0,
        error: map['error'] as String?,
      );
}

class BleResult {
  const BleResult(this.ok, {this.error});

  final bool ok;
  final String? error;

  factory BleResult.from(dynamic value) {
    if (value is Map) {
      return BleResult(value['ok'] == true, error: value['error'] as String?);
    }
    return const BleResult(false, error: 'BLE bridge 回傳格式錯誤');
  }
}

class BleBridge {
  static const _methods = MethodChannel('com.itap.remote_sensor_simulator/ble');
  static const _events = EventChannel('com.itap.remote_sensor_simulator/events');

  Stream<Map<dynamic, dynamic>> get events => _events
      .receiveBroadcastStream()
      .where((event) => event is Map)
      .cast<Map<dynamic, dynamic>>();

  Future<Map<dynamic, dynamic>> initialize() async =>
      _map(await _methods.invokeMethod('initialize'));

  Future<Map<dynamic, dynamic>> setName(String name) async =>
      _map(await _methods.invokeMethod('setName', {'name': name}));

  Future<Map<dynamic, dynamic>> setProfile(int profile) async =>
      _map(await _methods.invokeMethod('setProfile', {'profile': profile}));

  Future<Map<dynamic, dynamic>> startAdvertising() async =>
      _map(await _methods.invokeMethod('startAdvertising'));

  Future<Map<dynamic, dynamic>> stopAdvertising() async =>
      _map(await _methods.invokeMethod('stopAdvertising'));

  Future<BleResult> notify(Uint8List bytes) async =>
      BleResult.from(await _methods.invokeMethod('notify', {'bytes': bytes}));

  Map<dynamic, dynamic> _map(dynamic value) => value is Map ? value : const {};
}


