import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

const sampleRate = 44100;
const bpms = [85, 100, 115, 130, 145, 160];

void main() {
  final output = Directory('assets/audio')..createSync(recursive: true);
  for (final bpm in bpms) {
    final seconds = 8 * 60 / bpm;
    final sampleCount = (sampleRate * seconds).round();
    final pcm = Int16List(sampleCount);
    for (var i = 0; i < sampleCount; i++) {
      final time = i / sampleRate;
      final beat = (time * bpm / 60) % 1;
      final halfBeat = (time * bpm / 30) % 1;
      final kick = sin(2 * pi * (58 + bpm / 8) * time) * exp(-beat * 18);
      final tick = sin(2 * pi * 880 * time) * exp(-halfBeat * 35) * .22;
      final bass = sin(2 * pi * (96 + bpm / 4) * time) * .12;
      pcm[i] = ((kick * .62 + tick + bass) * 32767).round().clamp(-32768, 32767);
    }
    File('${output.path}/demo_$bpm.wav').writeAsBytesSync(_wav(pcm));
  }
}

Uint8List _wav(Int16List samples) {
  final dataSize = samples.length * 2;
  final bytes = ByteData(44 + dataSize);
  void text(int offset, String value) {
    for (var i = 0; i < value.length; i++) {
      bytes.setUint8(offset + i, value.codeUnitAt(i));
    }
  }

  text(0, 'RIFF');
  bytes.setUint32(4, 36 + dataSize, Endian.little);
  text(8, 'WAVE');
  text(12, 'fmt ');
  bytes.setUint32(16, 16, Endian.little);
  bytes.setUint16(20, 1, Endian.little);
  bytes.setUint16(22, 1, Endian.little);
  bytes.setUint32(24, sampleRate, Endian.little);
  bytes.setUint32(28, sampleRate * 2, Endian.little);
  bytes.setUint16(32, 2, Endian.little);
  bytes.setUint16(34, 16, Endian.little);
  text(36, 'data');
  bytes.setUint32(40, dataSize, Endian.little);
  for (var i = 0; i < samples.length; i++) {
    bytes.setInt16(44 + i * 2, samples[i], Endian.little);
  }
  return bytes.buffer.asUint8List();
}
