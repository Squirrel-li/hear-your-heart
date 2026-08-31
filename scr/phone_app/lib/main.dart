import 'package:flutter/material.dart';

import 'app_controller.dart';
import 'domain.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final controller = AppController();
  await controller.initialize();
  runApp(HeartMusicApp(controller: controller));
}

class HeartMusicApp extends StatelessWidget {
  const HeartMusicApp({super.key, required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: '聽見你的心',
      theme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        scaffoldBackgroundColor: const Color(0xFF071A22),
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF31D7D0),
          brightness: Brightness.dark,
          surface: const Color(0xFF102B35),
          error: const Color(0xFFFF9D57),
        ),
        cardTheme: const CardThemeData(color: Color(0xFF102B35), elevation: 0, margin: EdgeInsets.zero),
        navigationBarTheme: const NavigationBarThemeData(backgroundColor: Color(0xFF0B222B), indicatorColor: Color(0xFF19525A)),
      ),
      home: AppShell(controller: controller),
    );
  }
}

class AppShell extends StatefulWidget {
  const AppShell({super.key, required this.controller});
  final AppController controller;

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  int index = 0;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: widget.controller,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          backgroundColor: Colors.transparent,
          title: const Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text('聽見你的心', style: TextStyle(fontWeight: FontWeight.w800)),
            Text('HEART-SYNC MUSIC', style: TextStyle(fontSize: 10, letterSpacing: 1.8)),
          ]),
          actions: [Padding(padding: const EdgeInsets.only(right: 16), child: _StatusPill(connected: widget.controller.connected))],
        ),
        body: IndexedStack(
          index: index,
          children: [
            DashboardScreen(controller: widget.controller),
            PlayerScreen(controller: widget.controller),
            SettingsScreen(controller: widget.controller),
          ],
        ),
        bottomNavigationBar: NavigationBar(
          selectedIndex: index,
          onDestinationSelected: (value) => setState(() => index = value),
          destinations: const [
            NavigationDestination(icon: Icon(Icons.monitor_heart_outlined), selectedIcon: Icon(Icons.monitor_heart), label: '儀表板'),
            NavigationDestination(icon: Icon(Icons.headphones_outlined), selectedIcon: Icon(Icons.headphones), label: '播放'),
            NavigationDestination(icon: Icon(Icons.tune_outlined), selectedIcon: Icon(Icons.tune), label: '設定'),
          ],
        ),
      ),
    );
  }
}

class DashboardScreen extends StatelessWidget {
  const DashboardScreen({super.key, required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final snapshot = controller.snapshot;
    final decision = controller.decision;
    final warning = decision != null && decision.observedZone.index >= HeartZone.zone4.index;
    return ListView(
      key: const PageStorageKey('dashboard'),
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
      children: [
        if (warning) _WarningCard(zone: decision.observedZone) else _HeroCard(controller: controller),
        const SizedBox(height: 12),
        Row(children: [
          Expanded(child: _MetricCard(icon: Icons.air, label: '呼吸率', value: '${snapshot?.respiratoryRate ?? '--'}', unit: 'BrPM')),
          const SizedBox(width: 12),
          Expanded(child: _MetricCard(icon: Icons.directions_run, label: '活動', value: snapshot?.activity.label ?? '--', unit: '狀態')),
        ]),
        const SizedBox(height: 12),
        _SectionCard(
          title: '目前決策',
          child: Column(children: [
            _InfoRow(label: '音樂模式', value: decision?.mode == MusicMode.coolDown ? '放鬆降載' : '節奏激勵'),
            _InfoRow(label: '目標 BPM', value: '${decision?.targetBpm ?? '--'}'),
            _InfoRow(label: '目前曲目', value: controller.currentTrack.title),
            _InfoRow(label: '訓練建議', value: controller.recommendation),
          ]),
        ),
        const SizedBox(height: 12),
        _SimulatorCard(controller: controller),
      ],
    );
  }
}

class PlayerScreen extends StatelessWidget {
  const PlayerScreen({super.key, required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final track = controller.currentTrack;
    final totalMs = controller.duration.inMilliseconds;
    final progress = totalMs <= 0 ? 0.0 : (controller.position.inMilliseconds / totalMs).clamp(0.0, 1.0);
    return ListView(
      key: const PageStorageKey('player'),
      padding: const EdgeInsets.fromLTRB(20, 12, 20, 28),
      children: [
        AspectRatio(
          aspectRatio: 1.25,
          child: Container(
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(28),
              gradient: const LinearGradient(colors: [Color(0xFF17606A), Color(0xFF102B35), Color(0xFF7A4A27)], begin: Alignment.topLeft, end: Alignment.bottomRight),
            ),
            child: const Icon(Icons.graphic_eq_rounded, size: 112, color: Color(0xFF79F2EB)),
          ),
        ),
        const SizedBox(height: 22),
        Text(track.title, textAlign: TextAlign.center, style: Theme.of(context).textTheme.headlineSmall?.copyWith(fontWeight: FontWeight.w800)),
        const SizedBox(height: 6),
        Text('${track.bpm} BPM  ·  ${track.style.toUpperCase()}  ·  Energy ${(track.energy * 100).round()}%', textAlign: TextAlign.center, style: TextStyle(color: Theme.of(context).colorScheme.primary)),
        const SizedBox(height: 20),
        LinearProgressIndicator(value: progress, minHeight: 5, borderRadius: BorderRadius.circular(8)),
        const SizedBox(height: 16),
        Row(mainAxisAlignment: MainAxisAlignment.center, children: [
          IconButton(iconSize: 34, tooltip: '上一首', onPressed: () => controller.nextTrack(-1), icon: const Icon(Icons.skip_previous_rounded)),
          const SizedBox(width: 18),
          FilledButton(
            onPressed: controller.togglePlayback,
            style: FilledButton.styleFrom(shape: const CircleBorder(), padding: const EdgeInsets.all(20)),
            child: Icon(controller.isPlaying ? Icons.pause_rounded : Icons.play_arrow_rounded, size: 42),
          ),
          const SizedBox(width: 18),
          IconButton(iconSize: 34, tooltip: '下一首', onPressed: controller.nextTrack, icon: const Icon(Icons.skip_next_rounded)),
        ]),
        const SizedBox(height: 24),
        Text('示範曲目', style: Theme.of(context).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
        const SizedBox(height: 8),
        ...demoTracks.map((item) => ListTile(
          selected: item == track,
          contentPadding: EdgeInsets.zero,
          leading: CircleAvatar(child: Text('${item.bpm}')),
          title: Text(item.title),
          subtitle: Text('${item.style} · Energy ${(item.energy * 100).round()}%'),
          trailing: item == track ? const Icon(Icons.equalizer) : const Icon(Icons.play_arrow),
          onTap: () => controller.selectTrack(item),
        )),
      ],
    );
  }
}

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key, required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    return ListView(
      key: const PageStorageKey('settings'),
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 28),
      children: [
        _SectionCard(
          title: '個人設定',
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            Text('年齡  ${controller.age} 歲', style: Theme.of(context).textTheme.titleMedium),
            Slider(value: controller.age.toDouble(), min: 10, max: 100, divisions: 90, label: '${controller.age}', onChanged: controller.setAge),
            const SizedBox(height: 8),
            const Text('健身目標'),
            const SizedBox(height: 8),
            SegmentedButton<FitnessGoal>(
              segments: FitnessGoal.values.map((goal) => ButtonSegment(value: goal, label: Text(goal.label))).toList(),
              selected: {controller.goal},
              onSelectionChanged: (selection) => controller.setGoal(selection.first),
            ),
          ]),
        ),
        const SizedBox(height: 12),
        _SectionCard(
          title: '資料來源',
          child: Column(children: [
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('軟體模擬感測器'),
              subtitle: Text(controller.sensor.isRunning ? '每秒輸出 HR；每 10 秒更新 RR' : '已暫停，3 秒後顯示失聯'),
              value: controller.sensor.isRunning,
              onChanged: controller.setSimulationEnabled,
            ),
            const Divider(),
            const _InfoRow(label: '介面', value: 'SensorDataSource'),
            const _InfoRow(label: '未來接入', value: 'BLE adapter → SensorSnapshot'),
            const _InfoRow(label: '目前來源', value: 'SimulatedSensorDataSource'),
          ]),
        ),
        const SizedBox(height: 12),
        _SectionCard(
          title: '使用提醒',
          child: Text('本 APP 為專題展示用途，Zone 4/5 提示不是醫療診斷。感到不適時應立即停止運動並尋求專業協助。', style: TextStyle(color: Theme.of(context).colorScheme.onSurfaceVariant, height: 1.5)),
        ),
      ],
    );
  }
}

class _HeroCard extends StatelessWidget {
  const _HeroCard({required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final hr = controller.snapshot?.hr;
    final zone = controller.decision?.observedZone;
    return Container(
      padding: const EdgeInsets.all(22),
      decoration: BoxDecoration(borderRadius: BorderRadius.circular(24), gradient: const LinearGradient(colors: [Color(0xFF164B56), Color(0xFF102B35)])),
      child: Row(children: [
        Container(padding: const EdgeInsets.all(18), decoration: const BoxDecoration(color: Color(0x2631D7D0), shape: BoxShape.circle), child: const Icon(Icons.favorite_rounded, color: Color(0xFFFF826E), size: 42)),
        const SizedBox(width: 20),
        Expanded(child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('即時心率', style: TextStyle(color: Colors.white70)),
          Text('${hr ?? '--'}', style: const TextStyle(fontSize: 48, fontWeight: FontWeight.w900, height: 1.05)),
          Text('BPM  ·  ${zone?.label ?? '等待資料'}  ·  ${(controller.hrPercent * 100).round()}% HRmax'),
        ])),
      ]),
    );
  }
}

class _WarningCard extends StatelessWidget {
  const _WarningCard({required this.zone});
  final HeartZone zone;

  @override
  Widget build(BuildContext context) => Semantics(
    liveRegion: true,
    child: Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(color: const Color(0xFF5A2D20), borderRadius: BorderRadius.circular(22), border: Border.all(color: const Color(0xFFFF9D57))),
      child: Row(children: [
        const Icon(Icons.warning_amber_rounded, color: Color(0xFFFFB47D), size: 42),
        const SizedBox(width: 16),
        Expanded(child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('${zone.label} 高負荷', style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w800)),
          Text(zone == HeartZone.zone5 ? '已立即切換降載音樂，請降低運動強度。' : '持續監測中，系統將優先選擇放鬆節奏。'),
        ])),
      ]),
    ),
  );
}

class _SimulatorCard extends StatelessWidget {
  const _SimulatorCard({required this.controller});
  final AppController controller;

  @override
  Widget build(BuildContext context) => _SectionCard(
    title: '感測器模擬',
    child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Wrap(spacing: 8, runSpacing: 8, children: SimulationScenario.values.map((scenario) => ChoiceChip(
        label: Text(scenario.label),
        selected: !controller.sensor.manual && controller.sensor.scenario == scenario,
        onSelected: (_) => controller.setScenario(scenario),
      )).toList()),
      SwitchListTile(contentPadding: EdgeInsets.zero, title: const Text('手動輸入'), value: controller.sensor.manual, onChanged: controller.setManualMode),
      if (controller.sensor.manual) ...[
        Text('HR ${controller.sensor.manualHr} BPM'),
        Slider(value: controller.sensor.manualHr.toDouble(), min: 50, max: 210, divisions: 160, onChanged: controller.setManualHr),
        Text('RR ${controller.sensor.manualRr} BrPM'),
        Slider(value: controller.sensor.manualRr.toDouble(), min: 8, max: 40, divisions: 32, onChanged: controller.setManualRr),
        DropdownButtonFormField<ActivityLevel>(
          initialValue: controller.sensor.manualActivity,
          decoration: const InputDecoration(labelText: '活動狀態', border: OutlineInputBorder()),
          items: ActivityLevel.values.map((level) => DropdownMenuItem(value: level, child: Text(level.label))).toList(),
          onChanged: (value) { if (value != null) controller.setManualActivity(value); },
        ),
      ],
    ]),
  );
}

class _MetricCard extends StatelessWidget {
  const _MetricCard({required this.icon, required this.label, required this.value, required this.unit});
  final IconData icon;
  final String label;
  final String value;
  final String unit;

  @override
  Widget build(BuildContext context) => Card(
    child: Padding(
      padding: const EdgeInsets.all(16),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(icon, color: Theme.of(context).colorScheme.primary),
        const SizedBox(height: 18),
        Text(label, style: const TextStyle(color: Colors.white60)),
        Text(value, style: const TextStyle(fontSize: 24, fontWeight: FontWeight.w800), maxLines: 1),
        Text(unit, style: const TextStyle(fontSize: 11, color: Colors.white54)),
      ]),
    ),
  );
}

class _SectionCard extends StatelessWidget {
  const _SectionCard({required this.title, required this.child});
  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) => Card(
    child: Padding(
      padding: const EdgeInsets.all(18),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(title, style: Theme.of(context).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w800)),
        const SizedBox(height: 14),
        child,
      ]),
    ),
  );
}

class _InfoRow extends StatelessWidget {
  const _InfoRow({required this.label, required this.value});
  final String label;
  final String value;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 6),
    child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
      SizedBox(width: 86, child: Text(label, style: const TextStyle(color: Colors.white60))),
      Expanded(child: Text(value, textAlign: TextAlign.right)),
    ]),
  );
}

class _StatusPill extends StatelessWidget {
  const _StatusPill({required this.connected});
  final bool connected;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
    decoration: BoxDecoration(color: connected ? const Color(0x2231D7D0) : const Color(0x33FF9D57), borderRadius: BorderRadius.circular(30)),
    child: Row(children: [
      Icon(connected ? Icons.sensors : Icons.sensors_off, size: 15, color: connected ? const Color(0xFF31D7D0) : const Color(0xFFFF9D57)),
      const SizedBox(width: 5),
      Text(connected ? '模擬器連線' : '感測器失聯', style: const TextStyle(fontSize: 11)),
    ]),
  );
}
