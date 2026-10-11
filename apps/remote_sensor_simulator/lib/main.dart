import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'ble_bridge.dart';
import 'ppg_simulator.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const RemoteSensorSimulatorApp());
}

class RemoteSensorSimulatorApp extends StatelessWidget {
  const RemoteSensorSimulatorApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        debugShowCheckedModeBanner: false,
        title: 'HUB8735 Remote Sensor Simulator',
        theme: ThemeData(
          colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal, brightness: Brightness.dark),
          useMaterial3: true,
          scaffoldBackgroundColor: const Color(0xff101719),
        ),
        home: const SimulatorPage(),
      );
}

const _profileLabels = ['FFF0 / FFF1 / FFF2', '524CACC0… / D002 / D001'];

class SimulatorPage extends StatefulWidget {
  const SimulatorPage({super.key});

  @override
  State<SimulatorPage> createState() => _SimulatorPageState();
}

class _SimulatorPageState extends State<SimulatorPage> {
  final _bridge = BleBridge();
  final _nameController = TextEditingController(text: 'BLE_sensor_ITAP');
  final _scrollController = ScrollController();

  StreamSubscription<Map<dynamic, dynamic>>? _events;
  List<PpgDataSet> _dataSets = const [];
  PpgPlayback? _playback;
  BleStatus _ble = const BleStatus();
  final List<String> _writeLog = [];
  String? _message;
  String? _loadError;
  bool _loading = true;
  int _selectedSource = 0;

  @override
  void initState() {
    super.initState();
    _events = _bridge.events.listen(_onBleEvent, onError: (Object error) {
      if (mounted) setState(() => _message = 'BLE event error: $error');
    });
    unawaited(_initialize());
  }

  Future<void> _initialize() async {
    try {
      _dataSets = await PpgDataLoader.load(rootBundle.loadString);
      final map = await _bridge.initialize();
      _ble = BleStatus.fromMap(map);
      _createPlayback(_dataSets.first);
    } catch (error) {
      _loadError = '$error';
    }
    if (mounted) setState(() => _loading = false);
  }

  void _createPlayback(PpgDataSet dataSet) {
    _playback?.dispose();
    _playback = PpgPlayback(
      dataSet: dataSet,
      send: _sendPayload,
      onChanged: () {
        if (mounted) setState(() {});
      },
    );
  }

  Future<bool> _sendPayload(Uint8List bytes) async {
    try {
      final result = await _bridge.notify(bytes);
      if (!result.ok) {
        _message = _friendlyError(result.error);
      }
      return result.ok;
    } catch (error) {
      _message = '通知失敗：$error';
      return false;
    }
  }

  void _onBleEvent(Map<dynamic, dynamic> event) {
    final type = event['type'];
    if (type == 'fff2_write') {
      final time = TimeOfDay.now().format(context);
      final hex = event['hex'] as String? ?? '';
      setState(() {
        _writeLog.insert(0, '$time  $hex');
        if (_writeLog.length > 100) _writeLog.removeLast();
      });
      return;
    }
    if (mounted) {
      setState(() {
        _ble = BleStatus.fromMap(event);
        if (_ble.error != null) _message = _friendlyError(_ble.error);
      });
    }
  }

  String _friendlyError(String? error) => switch (error) {
        'not_connected' => '未連線或未訂閱 Notify，資料未消耗。',
        'permission_required' => '請允許附近裝置權限後重新啟動 App。',
        'bluetooth_disabled' => '請先開啟手機藍牙。',
        'ble_unsupported' => '手機不支援 BLE Peripheral。',
        'not_subscribed' => 'HUB8735 尚未訂閱 FFF1，資料未消耗。',
        null => 'BLE 操作失敗。',
        _ => error,
      };

  Future<void> _applyName() async {
    final name = _nameController.text.trim();
    if (name.isEmpty || name.length > 20 || name.contains(RegExp(r'[\r\n]'))) {
      setState(() => _message = '廣播名稱需為 1–20 個字元，且不可含換行。');
      return;
    }
    try {
      final map = await _bridge.setName(name);
      if (mounted) {
        setState(() {
          _ble = BleStatus.fromMap(map);
          _message = '已套用廣播名稱：$name';
        });
      }
    } catch (error) {
      if (mounted) setState(() => _message = '套用名稱失敗：$error');
    }
  }

  Future<void> _applyProfile(int profile) async {
    try {
      final map = await _bridge.setProfile(profile);
      if (mounted) {
        setState(() {
          _ble = BleStatus.fromMap(map);
          _message = '已切換 UUID：${_profileLabels[profile]}（需重新開始廣播）。';
        });
      }
    } catch (error) {
      if (mounted) setState(() => _message = '切換 UUID 失敗：$error');
    }
  }

  Future<void> _startAdvertising() async {
    try {
      final map = await _bridge.startAdvertising();
      if (mounted) setState(() => _ble = BleStatus.fromMap(map));
    } catch (error) {
      if (mounted) setState(() => _message = '開始廣播失敗：$error');
    }
  }

  Future<void> _stopAdvertising() async {
    try {
      final map = await _bridge.stopAdvertising();
      if (mounted) setState(() => _ble = BleStatus.fromMap(map));
    } catch (error) {
      if (mounted) setState(() => _message = '停止廣播失敗：$error');
    }
  }

  void _selectSource(int index) {
    _selectedSource = index;
    _playback?.selectDataSet(_dataSets[index]);
    setState(() => _message = '已停止傳送並切換至 ${_dataSets[index].source.label}。');
  }

  Future<void> _sendNext() async {
    final sent = await _playback?.sendNext() ?? false;
    if (!sent && mounted && _playback?.lastSent == null && _message == null) {
      setState(() => _message = '目前沒有可傳送的資料。');
    }
  }

  Future<void> _playOnce() async {
    await _playback?.playOnce();
  }

  Future<void> _playLoop() async {
    await _playback?.playLoop();
  }

  void _stopTransmission() {
    _playback?.stop();
    setState(() => _message = '已停止傳送。');
  }

  @override
  void dispose() {
    _events?.cancel();
    _playback?.dispose();
    _nameController.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Scaffold(body: Center(child: CircularProgressIndicator()));
    }
    if (_loadError != null) {
      return Scaffold(
        appBar: AppBar(title: const Text('Remote Sensor Simulator')),
        body: Center(child: Padding(padding: const EdgeInsets.all(24), child: Text(_loadError!))),
      );
    }

    final playback = _playback!;
    final last = playback.lastSent;
    return Scaffold(
      appBar: AppBar(
        title: const Text('HUB8735 Remote Sensor'),
        bottom: const PreferredSize(
          preferredSize: Size.fromHeight(20),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Padding(
              padding: EdgeInsets.only(left: 16, bottom: 8),
              child: Text('BLE Peripheral / PPG Simulator', style: TextStyle(fontSize: 12)),
            ),
          ),
        ),
      ),
      body: SafeArea(
        child: ListView(
          controller: _scrollController,
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 28),
          children: [
            _Section(
              title: 'BLE 狀態',
              child: Column(
                children: [
                  _StateRow(
                    label: '藍牙',
                    value: !_ble.bluetoothAvailable
                        ? '不支援 BLE Peripheral'
                        : (_ble.bluetoothEnabled ? '已開啟' : '未開啟'),
                    good: _ble.bluetoothAvailable && _ble.bluetoothEnabled,
                  ),
                  _StateRow(label: '廣播', value: _ble.advertising ? '進行中' : '已停止', good: _ble.advertising),
                  _StateRow(label: '連線', value: _ble.connected ? '已連線' : '未連線', good: _ble.connected),
                  _StateRow(
                    label: 'Notify 訂閱',
                    value: _ble.subscribed ? '已訂閱 Notify' : '未訂閱',
                    good: _ble.subscribed,
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: '廣播名稱',
              child: Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: _nameController,
                      maxLength: 20,
                      decoration: const InputDecoration(
                        labelText: 'Local name',
                        counterText: '',
                        border: OutlineInputBorder(),
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  FilledButton(onPressed: _applyName, child: const Text('套用')),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: 'UUID 設定',
              child: SegmentedButton<int>(
                segments: [
                  for (var i = 0; i < _profileLabels.length; i++)
                    ButtonSegment(value: i, label: Text(_profileLabels[i], style: const TextStyle(fontSize: 12))),
                ],
                selected: {_ble.profile},
                onSelectionChanged: (value) => _applyProfile(value.first),
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: '廣播控制',
              child: Row(
                children: [
                  Expanded(
                    child: FilledButton.icon(
                      onPressed: _ble.advertising ? null : _startAdvertising,
                      icon: const Icon(Icons.bluetooth_searching),
                      label: const Text('開始廣播'),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _ble.advertising ? _stopAdvertising : null,
                      icon: const Icon(Icons.stop),
                      label: const Text('停止廣播'),
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: '資料來源',
              child: DropdownButtonFormField<PpgDataSet>(
                initialValue: _dataSets[_selectedSource],
                isExpanded: true,
                decoration: const InputDecoration(border: OutlineInputBorder()),
                items: [
                  for (final dataSet in _dataSets)
                    DropdownMenuItem(value: dataSet, child: Text(dataSet.source.label)),
                ],
                onChanged: (value) {
                  if (value != null) _selectSource(_dataSets.indexOf(value));
                },
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: '傳送控制',
              child: Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  FilledButton.tonalIcon(
                    onPressed: playback.isPlaying ? null : _sendNext,
                    icon: const Icon(Icons.skip_next),
                    label: const Text('傳送下一筆'),
                  ),
                  FilledButton.tonalIcon(
                    onPressed: playback.isPlaying ? null : _playOnce,
                    icon: const Icon(Icons.play_arrow),
                    label: const Text('播放一次'),
                  ),
                  FilledButton.tonalIcon(
                    onPressed: playback.isPlaying ? null : _playLoop,
                    icon: const Icon(Icons.repeat),
                    label: const Text('循環播放'),
                  ),
                  OutlinedButton.icon(
                    onPressed: playback.isPlaying ? _stopTransmission : null,
                    icon: const Icon(Icons.stop_circle_outlined),
                    label: const Text('停止傳送'),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: '傳送進度',
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  _InfoRow(label: '目前筆數', value: '${last == null ? 0 : last.rawIndex + 1}'),
                  _InfoRow(label: '總筆數', value: '${playback.dataSet.frames.length}'),
                  _InfoRow(label: '傳送數量', value: '${playback.sentCount}'),
                  _InfoRow(
                    label: '模式',
                    value: playback.isPlaying ? (playback.isLooping ? '循環播放中' : '播放一次') : '停止',
                  ),
                  const Divider(),
                  const Text('最近一筆 HEX'),
                  const SizedBox(height: 6),
                  SelectableText(
                    last == null ? '尚未傳送' : _formatHex(last.bytes),
                    style: const TextStyle(fontFamily: 'monospace', fontSize: 12, height: 1.45),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _Section(
              title: 'RX 寫入日誌',
              trailing: TextButton(
                onPressed: _writeLog.isEmpty ? null : () => setState(_writeLog.clear),
                child: const Text('清除日誌'),
              ),
              child: SizedBox(
                height: 150,
                child: _writeLog.isEmpty
                    ? const Center(child: Text('尚無 RX 寫入'))
                    : ListView.builder(
                        itemCount: _writeLog.length,
                        itemBuilder: (context, index) => Padding(
                          padding: const EdgeInsets.only(bottom: 5),
                          child: SelectableText(
                            _writeLog[index],
                            style: const TextStyle(fontFamily: 'monospace', fontSize: 11),
                          ),
                        ),
                      ),
              ),
            ),
            if (_message != null) ...[
              const SizedBox(height: 10),
              Card(
                color: Theme.of(context).colorScheme.surfaceContainerHighest,
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Text(_message!, semanticsLabel: _message!),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _Section extends StatelessWidget {
  const _Section({required this.title, required this.child, this.trailing});

  final String title;
  final Widget child;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) => Card(
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      title,
                      style: Theme.of(context).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w700),
                    ),
                  ),
                  if (trailing != null) trailing!,
                ],
              ),
              const SizedBox(height: 12),
              child,
            ],
          ),
        ),
      );
}

class _StateRow extends StatelessWidget {
  const _StateRow({required this.label, required this.value, required this.good});

  final String label;
  final String value;
  final bool good;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          children: [
            Icon(good ? Icons.check_circle : Icons.circle_outlined, size: 18, color: good ? Colors.tealAccent : Colors.white54),
            const SizedBox(width: 8),
            Text(label),
            const Spacer(),
            Flexible(child: Text(value, textAlign: TextAlign.right)),
          ],
        ),
      );
}

class _InfoRow extends StatelessWidget {
  const _InfoRow({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(
          children: [
            Text(label, style: const TextStyle(color: Colors.white60)),
            const Spacer(),
            Flexible(child: Text(value, textAlign: TextAlign.right)),
          ],
        ),
      );
}

String _formatHex(Uint8List bytes) => bytes.map((value) => value.toRadixString(16).padLeft(2, '0').toUpperCase()).join(' ');


