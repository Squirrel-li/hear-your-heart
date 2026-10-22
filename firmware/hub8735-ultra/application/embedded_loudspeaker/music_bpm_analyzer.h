#ifndef MUSIC_BPM_ANALYZER_H
#define MUSIC_BPM_ANALYZER_H

#include <stdint.h>

#include "AmebaFatFS.h"
#include "mp3dec.h"
#include "bpm_tempo.h"

class MusicBpmAnalyzer {
public:
    enum class StepResult : uint8_t {
        InProgress,
        Complete,
        Failed
    };

    MusicBpmAnalyzer();
    bool begin(AmebaFatFS& fs, const char* filename);
    StepResult step(uint16_t& bpm);
    void cancel();
    const char* error() const { return error_; }

private:
    StepResult finish(uint16_t& bpm);
    StepResult fail(const char* reason);
    void closeCurrentFile();

    File file_;
    HMP3Decoder decoder_;
    bpm_tempo::Analyzer analyzer_;
    bool analyzerReady_;
    uint8_t encoded_[4096];
    int16_t decoded_[1152 * 2];
    float mono_[1152];
    uint8_t* cursor_;
    int bytes_;
    bool eof_;
    uint32_t sampleRate_;
    uint32_t frames_;
    bool active_;
    uint32_t decodeErrors_;
    uint32_t startOffset_;
    uint32_t startMs_;     // 計時 log：整首、特徵（STFT）與最後估計各花多少時間
    uint32_t featureUs_;
    uint32_t pcmHash_;     // FNV-1a over the mono int16 samples; compare with tools/bpm_tempo/dump.cpp -DDUMP_ESTIMATE
    const char* error_;
};

#endif