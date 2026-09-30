#include <Arduino.h>
#include "AmebaFatFS.h"
#include "mp3dec.h"
#include <MiniBpm.h>
#include <new>
#include <string.h>

// Put the MP3 in the SD card root. Analysis uses its first 60 seconds.
static const char *song = "music1.mp3";
static uint8_t encoded[4096];       // Helix needs at least MAINBUF_SIZE bytes.
static int16_t decoded[1152 * 2];   // Largest stereo MP3 output frame.
static float mono[1152];
static AmebaFatFS fs;

static bool analyzeMp3(const char *filename, double &bpm)
{
    const String path = String(fs.getRootPath()) + filename;
    File file = fs.open(path);
    if (!file) return false;
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) { file.close(); return false; }
    breakfastquay::MiniBPM *analyzer = nullptr;
    uint8_t *cursor = encoded;
    int bytes = 0;
    bool eof = false;
    uint32_t frames = 0;
    uint32_t sampleRate = 0;
    unsigned decodedFrames = 0;
    bool failed = false;

    while (!failed && ((sampleRate == 0 && (!eof || bytes > 4)) ||
                       (sampleRate != 0 && frames < sampleRate * 60u))) {
        if (!eof && bytes < MAINBUF_SIZE) {
            memmove(encoded, cursor, bytes);
            cursor = encoded;
            const int got = file.read(encoded + bytes, sizeof(encoded) - bytes);
            if (got < 0) { failed = true; break; }
            if (got == 0) eof = true;
            bytes += got;
        }
        if (bytes <= 4) {
            if (eof) break;
            continue;
        }
        const int offset = MP3FindSyncWord(cursor, bytes);
        if (offset < 0) {
            // A four-byte sync header may straddle two SD reads.
            if (eof) break;
            const int keep = bytes < 3 ? bytes : 3;
            cursor += bytes - keep;
            bytes = keep;
            continue;
        }
        cursor += offset;
        bytes -= offset;
        uint8_t *const before = cursor;
        const int beforeBytes = bytes;
        const int error = MP3Decode(decoder, &cursor, &bytes, decoded, 0);
        if (error == ERR_MP3_INDATA_UNDERFLOW ||
            error == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof) break;
            if (cursor == before && bytes == beforeBytes && bytes >= MAINBUF_SIZE) {
                ++cursor;
                --bytes;
            }
            continue;
        }
        if (error != ERR_MP3_NONE) {
            if (bytes > 0) { ++cursor; --bytes; }
            continue;
        }
        MP3FrameInfo info;
        MP3GetLastFrameInfo(decoder, &info);
        if (info.samprate < 8000 || info.samprate > 48000 ||
            (info.nChans != 1 && info.nChans != 2) || info.bitsPerSample != 16 ||
            info.outputSamps < 0 || info.outputSamps > 2304 ||
            info.outputSamps % info.nChans != 0 ||
            (sampleRate && sampleRate != (uint32_t)info.samprate)) {
            failed = true;
            break;
        }
        if (!analyzer) {
            sampleRate = info.samprate;
            analyzer = new (std::nothrow) breakfastquay::MiniBPM((float)sampleRate);
            if (!analyzer) { failed = true; break; }
        }
        const int count = info.outputSamps / info.nChans;
        for (int i = 0; i < count; ++i) {
            int32_t value = decoded[i * info.nChans];
            if (info.nChans == 2) value = (value + decoded[i * 2 + 1]) / 2;
            mono[i] = value / 32768.0f;
        }
        analyzer->process(mono, count);
        frames += count;
        if (++decodedFrames % 32u == 0) delay(1);
    }
    file.close();
    bpm = !failed && analyzer && frames >= sampleRate * 8u
              ? analyzer->estimateTempo() : 0.0;
    delete analyzer;
    return bpm > 0.0;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    if (!fs.begin()) {
        Serial.println("SD card initialization failed");
        return;
    }
    double bpm = 0;
    if (analyzeMp3(song, bpm)) {
        Serial.print(song);
        Serial.print(": ");
        Serial.print(bpm, 1);
        Serial.println(" BPM");
    } else {
        Serial.println("MP3 BPM analysis failed");
    }
}

void loop() {}




