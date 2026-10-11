import 'dart:async';
import 'dart:typed_data';

class PpgSource {
  const PpgSource(this.label, this.assetPath);

  final String label;
  final String assetPath;
}

class PpgFrame {
  const PpgFrame({required this.bytes, required this.timestamp, required this.sequence});

  final Uint8List bytes;
  final int timestamp;
  final int sequence;
  int get type => bytes[4];

  Uint8List payload({int timestampOffset = 0, int sequenceOffset = 0}) {
    final result = Uint8List.fromList(bytes);
    _writeUint32(result, 0, timestamp + timestampOffset);
    _writeUint32(result, 5, sequence + sequenceOffset);
    return result;
  }
}

class PpgDataSet {
  const PpgDataSet({required this.source, required this.frames});

  final PpgSource source;
  final List<PpgFrame> frames;
}

class PpgDataLoader {
  static const sources = <PpgSource>[
    PpgSource('真實資料（capture）', 'assets/testdata/capture.txt'),
    PpgSource('模擬：暖身', 'assets/testdata/ppg_warmup.txt'),
    PpgSource('模擬：燃脂', 'assets/testdata/ppg_fat_burn.txt'),
    PpgSource('模擬：有氧', 'assets/testdata/ppg_aerobic.txt'),
    PpgSource('模擬：無氧', 'assets/testdata/ppg_anaerobic.txt'),
    PpgSource('模擬：最大強度', 'assets/testdata/ppg_maximal.txt'),
    PpgSource('IMU：靜止', 'assets/testdata/gsensor_rest.txt'),
    PpgSource('IMU：步行', 'assets/testdata/gsensor_walk.txt'),
    PpgSource('IMU：跑步', 'assets/testdata/gsensor_run.txt'),
  ];

  static Future<List<PpgDataSet>> load(Future<String> Function(String path) read) async {
    final result = <PpgDataSet>[];
    for (final source in sources) {
      result.add(PpgDataSet(source: source, frames: parse(source, await read(source.assetPath))));
    }
    return result;
  }

  static List<PpgFrame> parse(PpgSource source, String text) {
    final frames = <PpgFrame>[];
    var lineNumber = 0;
    for (final rawLine in text.split(RegExp(r'\r?\n'))) {
      lineNumber++;
      final line = rawLine.trim();
      if (line.isEmpty || line.startsWith('#')) continue;

      final fields = line.split(RegExp(r'\s+'));
      if (fields.length < 5) {
        throw FormatException(source.label + ': 第 ' + lineNumber.toString() + ' 行缺少 frame header');
      }
      final parsed = Uint8List(fields.length);
      for (var i = 0; i < fields.length; i++) {
        if (!RegExp(r'^[0-9a-fA-F]{2}$').hasMatch(fields[i])) {
          throw FormatException(source.label + ': 第 ' + lineNumber.toString() + ' 行有無效 HEX');
        }
        parsed[i] = int.parse(fields[i], radix: 16);
      }

      final type = parsed[4];
      final frameLength = switch (type) {
        0x01 => 20,
        0x02 || 0x03 => 15,
        _ => 0,
      };
      final legacyPpgLength = type == 0x01 && fields.length == 21;
      if (frameLength == 0 || (!legacyPpgLength && fields.length != frameLength)) {
        throw FormatException(source.label + ': 第 ' + lineNumber.toString() + ' 行的 frame 長度不符');
      }

      // 舊 PPG 測資多一個未使用尾位元組；線上只送協定定義的 20 bytes。
      final bytes = Uint8List.fromList(parsed.sublist(0, frameLength));
      final frame = PpgFrame(
        bytes: bytes,
        timestamp: _readUint32(bytes, 0),
        sequence: _readUint32(bytes, 5),
      );
      if (frames.isEmpty) {
        if (frame.type == 0x03) {
          throw FormatException(source.label + ': IMU 資料必須先送 Acc');
        }
      } else {
        final previous = frames.last;
        if (frames.first.type == 0x01) {
          if (frame.type != 0x01) {
            throw FormatException(source.label + ': PPG 資料不能混入其他 frame');
          }
          if (frame.timestamp <= previous.timestamp) {
            throw FormatException(source.label + ': timestamp 必須遞增');
          }
          if (frame.sequence != previous.sequence + 1) {
            throw FormatException(source.label + ': sequence 必須連續遞增');
          }
        } else if (previous.type == 0x02 && frame.type == 0x03) {
          if (frame.timestamp != previous.timestamp || frame.sequence != previous.sequence) {
            throw FormatException(source.label + ': Acc/Gyro 必須共用 timestamp 與 sequence');
          }
        } else if (previous.type == 0x03 && frame.type == 0x02) {
          if (frame.timestamp <= previous.timestamp) {
            throw FormatException(source.label + ': 下一筆 IMU timestamp 必須遞增');
          }
          if (frame.sequence != previous.sequence + 1) {
            throw FormatException(source.label + ': 下一筆 IMU sequence 必須連續遞增');
          }
        } else {
          throw FormatException(source.label + ': IMU frame 必須依 Acc、Gyro 順序成對');
        }
      }
      frames.add(frame);
    }
    if (frames.isEmpty) throw FormatException(source.label + ': 沒有資料');
    if (frames.first.type == 0x02 && frames.last.type != 0x03) {
      throw FormatException(source.label + ': 最後一筆 IMU 缺少 Gyro frame');
    }
    return frames;
  }
}

class PpgPayload {
  const PpgPayload({
    required this.bytes,
    required this.timestamp,
    required this.sequence,
    required this.rawIndex,
    required this.cycle,
  });

  final Uint8List bytes;
  final int timestamp;
  final int sequence;
  final int rawIndex;
  final int cycle;
}

class PpgCursor {
  PpgCursor(this.dataSet);

  PpgDataSet dataSet;
  int nextIndex = 0;
  int cycle = 0;

  int get _timestampStep {
    for (var i = 1; i < dataSet.frames.length; i++) {
      final step = dataSet.frames[i].timestamp - dataSet.frames[i - 1].timestamp;
      if (step > 0) return step;
    }
    return 5000;
  }

  int get _sequenceStep {
    for (var i = 1; i < dataSet.frames.length; i++) {
      final step = dataSet.frames[i].sequence - dataSet.frames[i - 1].sequence;
      if (step > 0) return step;
    }
    return 1;
  }

  int get _cycleTimestampSpan =>
      dataSet.frames.last.timestamp - dataSet.frames.first.timestamp + _timestampStep;

  int get _cycleSequenceSpan =>
      dataSet.frames.last.sequence - dataSet.frames.first.sequence + _sequenceStep;

  PpgPayload? peek({required bool loop}) {
    if (dataSet.frames.isEmpty) return null;
    var index = nextIndex;
    var cycleForPayload = cycle;
    if (index >= dataSet.frames.length) {
      if (!loop) return null;
      index = 0;
      cycleForPayload++;
    }
    final frame = dataSet.frames[index];
    final timestampOffset = cycleForPayload * _cycleTimestampSpan;
    final sequenceOffset = cycleForPayload * _cycleSequenceSpan;
    return PpgPayload(
      bytes: frame.payload(
        timestampOffset: timestampOffset,
        sequenceOffset: sequenceOffset,
      ),
      timestamp: frame.timestamp + timestampOffset,
      sequence: frame.sequence + sequenceOffset,
      rawIndex: index,
      cycle: cycleForPayload,
    );
  }

  void advance({required bool loop}) {
    if (nextIndex >= dataSet.frames.length) {
      if (!loop) return;
      nextIndex = 0;
      cycle++;
    }
    nextIndex++;
  }

  int? delayAfter(PpgPayload payload, {required bool loop}) {
    if (payload.rawIndex + 1 < dataSet.frames.length) {
      return dataSet.frames[payload.rawIndex + 1].timestamp -
          dataSet.frames[payload.rawIndex].timestamp;
    }
    return loop ? _timestampStep : null;
  }

  void reset([PpgDataSet? data]) {
    if (data != null) dataSet = data;
    nextIndex = 0;
    cycle = 0;
  }
}

class PpgPlayback {
  PpgPlayback({
    required PpgDataSet dataSet,
    required this.send,
    this.onChanged,
  }) : cursor = PpgCursor(dataSet);

  final Future<bool> Function(Uint8List bytes) send;
  final void Function()? onChanged;
  late final PpgCursor cursor;

  Timer? _timer;
  bool _playing = false;
  bool _loop = false;
  PpgPayload? lastSent;
  int sentCount = 0;

  bool get isPlaying => _playing;
  bool get isLooping => _loop;
  PpgDataSet get dataSet => cursor.dataSet;

  Future<bool> sendNext() async {
    if (_playing) return false;
    final payload = cursor.peek(loop: false);
    if (payload == null) {
      onChanged?.call();
      return false;
    }
    final sent = await send(payload.bytes);
    if (sent) {
      cursor.advance(loop: false);
      _record(payload);
    }
    onChanged?.call();
    return sent;
  }

  Future<void> playOnce() => _play(loop: false);

  Future<void> playLoop() => _play(loop: true);

  Future<void> _play({required bool loop}) async {
    stop(notify: false);
    cursor.reset();
    lastSent = null;
    sentCount = 0;
    _loop = loop;
    _playing = true;
    onChanged?.call();
    await _sendCurrent();
  }

  Future<void> _sendCurrent() async {
    if (!_playing) return;
    final payload = cursor.peek(loop: _loop);
    if (payload == null) {
      stop();
      return;
    }
    final sent = await send(payload.bytes);
    if (!_playing) return;
    if (!sent) {
      stop();
      return;
    }
    cursor.advance(loop: _loop);
    _record(payload);
    onChanged?.call();
    final delay = cursor.delayAfter(payload, loop: _loop);
    if (delay == null) {
      _playing = false;
      onChanged?.call();
      return;
    }
    _timer = Timer(Duration(milliseconds: delay < 0 ? 0 : delay), () {
      _timer = null;
      unawaited(_sendCurrent());
    });
  }

  void stop({bool notify = true}) {
    _timer?.cancel();
    _timer = null;
    _playing = false;
    if (notify) onChanged?.call();
  }

  void selectDataSet(PpgDataSet dataSet) {
    stop(notify: false);
    cursor.reset(dataSet);
    lastSent = null;
    sentCount = 0;
    onChanged?.call();
  }

  void _record(PpgPayload payload) {
    lastSent = payload;
    sentCount++;
  }

  void dispose() => stop(notify: false);
}

int _readUint32(Uint8List bytes, int offset) =>
    (bytes[offset] << 24) |
    (bytes[offset + 1] << 16) |
    (bytes[offset + 2] << 8) |
    bytes[offset + 3];

void _writeUint32(Uint8List bytes, int offset, int value) {
  bytes[offset] = (value >> 24) & 0xff;
  bytes[offset + 1] = (value >> 16) & 0xff;
  bytes[offset + 2] = (value >> 8) & 0xff;
  bytes[offset + 3] = value & 0xff;
}
