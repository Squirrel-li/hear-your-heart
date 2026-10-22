// Dump bpm_tempo candidates + features for training (train.py). Decodes exactly like music_bpm_analyzer.cpp
// (Helix fixed-point decoder, same 4 KB read/resync loop): skip ID3v2, start 1/3 in for files > 2 MB, mono mix,
// at most 60 s. The SDK ships Helix only as libhmp3.a, so build Helix from the FB port with the SDK headers:
//   git clone https://github.com/FrankBoesing/Arduino-Teensy-Codec-lib fb
//   mkdir helix && cp fb/mp3/*.c fb/mp3/doc/polyphase.c helix/ && cp <SDK>/system/component/audio/3rdparty/hmp3/*.h helix/
//   In helix/assembly.h make REV16 return ((value >> 8) & 0xff) | ((value << 8) & 0xff00): the SDK's C version
//   keeps bits above 15 and decodes noise on a PC (fixed, it matches ffmpeg within 1 LSB).
//   (cd helix && gcc -O2 -c -I. -include mp3dec.h -include assembly.h *.c && ar rcs libhelix.a *.o)
//   g++ -std=c++11 -O2 -Ihelix -I../../application/embedded_loudspeaker dump.cpp helix/libhelix.a -o bpm_tempo_dump
//   ./bpm_tempo_dump a.mp3 b.mp3 ... > features.tsv     (one line per candidate: path, bpm, features)
// With -DDUMP_ESTIMATE it uses bpm_tempo_model.h and prints one line per file: path, estimated BPM, and the
// "pcm" hash/sample count that the HUB prints in its BPM timing log, to check both decoders give the same PCM.
#include "mp3dec.h"
#ifndef DUMP_ESTIMATE
#define BPM_TEMPO_NO_MODEL
#endif
#include "bpm_tempo.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    static bpm_tempo::Candidate cand[bpm_tempo::MAX_CANDIDATES];
    static uint8_t encoded[4096];
    static int16_t decoded[1152 * 2];
    static float mono[1152];
    bpm_tempo::Analyzer an;
    HMP3Decoder dec = MP3InitDecoder();
    for (int a = 1; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb");
        if (f == nullptr) { printf("%s\tOPEN_FAIL\n", argv[a]); continue; }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t h[10];
        long start = 0;
        if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3) && (h[6] | h[7] | h[8] | h[9]) < 0x80) {
            start = 10 + ((h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9]);
            if (h[5] & 0x10) start += 10;
        }
        if (size > 2L * 1024 * 1024 && size / 3 > start) start = size / 3;
        fseek(f, start, SEEK_SET);
        uint8_t* cursor = encoded;
        int bytes = 0;
        bool eof = false;
        unsigned rate = 0, frames = 0, errors = 0, decodedFrames = 0;
        uint32_t hash = 2166136261u;  // FNV-1a over the mono int16 samples, as in the firmware log
        while (rate == 0 || frames < rate * 60) {
            if (!eof && bytes < MAINBUF_SIZE) {
                if (bytes > 0) memmove(encoded, cursor, bytes);
                cursor = encoded;
                const size_t got = fread(encoded + bytes, 1, sizeof encoded - bytes, f);
                if (got == 0) eof = true;
                else bytes += static_cast<int>(got);
            }
            if (bytes <= 4) {
                if (eof) break;
                continue;
            }
            const int offset = MP3FindSyncWord(cursor, bytes);
            if (offset < 0) {
                if (eof) break;
                const int keep = bytes < 3 ? bytes : 3;
                cursor += bytes - keep;
                bytes = keep;
                continue;
            }
            cursor += offset;
            bytes -= offset;
            uint8_t* const before = cursor;
            const int beforeBytes = bytes;
            const int err = MP3Decode(dec, &cursor, &bytes, decoded, 0);
            if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
                if (eof) break;
                if (cursor == before && bytes == beforeBytes && bytes >= MAINBUF_SIZE) { ++cursor; --bytes; }
                continue;
            }
            if (err != ERR_MP3_NONE) {
                ++errors;
                if (bytes > 0) { ++cursor; --bytes; }
                continue;
            }
            MP3FrameInfo info{};
            MP3GetLastFrameInfo(dec, &info);
            if (info.samprate < 8000 || info.samprate > 48000 || (info.nChans != 1 && info.nChans != 2) ||
                info.bitsPerSample != 16 || info.outputSamps <= 0 || info.outputSamps > 2304 ||
                info.outputSamps % info.nChans != 0 || (rate != 0 && rate != static_cast<unsigned>(info.samprate))) {
                if (++errors > 200) { rate = 0; break; }
                if (cursor == before && bytes > 0) { ++cursor; --bytes; }
                continue;
            }
            if (++decodedFrames <= 3) continue;  // decoder state from the previous file; same as the firmware
            if (rate == 0) {
                rate = info.samprate;
                if (!an.begin(rate, 60)) { rate = 0; break; }
            }
            const int s = info.outputSamps / info.nChans;
            for (int i = 0; i < s; ++i) {
                int32_t v = decoded[i * info.nChans];
                if (info.nChans == 2) v = (v + decoded[i * 2 + 1]) / 2;
                const uint16_t u = static_cast<uint16_t>(v);
                hash = ((hash ^ (u & 0xff)) * 16777619u ^ (u >> 8)) * 16777619u;
                mono[i] = v / 32768.0f;
            }
            an.process(mono, s);
            frames += s;
        }
        fclose(f);
        const int count = rate != 0 && frames >= rate * 8 ? an.analyze(cand) : 0;
#ifdef DUMP_ESTIMATE
        printf("%s\t%.2f\tpcm %08x/%u\t%u errors\n", argv[a], bpm_tempo::estimate(cand, count), hash, frames, errors);
        an.release();
        continue;
#endif
        if (count == 0) printf("%s\tNONE\n", argv[a]);
        for (int i = 0; i < count; ++i) {
            printf("%s\t%.3f", argv[a], cand[i].bpm);
            for (int j = 0; j < bpm_tempo::FEATURES; ++j) printf("\t%.5g", cand[i].features[j]);
            printf("\n");
        }
        an.release();
    }
    return 0;
}
