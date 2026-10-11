#ifndef MUSIC_BPM_ANALYZER_H
#define MUSIC_BPM_ANALYZER_H

#include <stdint.h>

#include "AmebaFatFS.h"
#include "mp3dec.h"
// Arduino.h defines min()/max() macros that break the C++ standard headers MiniBpm pulls in.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include <algorithm>
#include <MiniBpm.h>
#pragma pop_macro("max")
#pragma pop_macro("min")

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
    breakfastquay::MiniBPM* analyzer_;
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
    const char* error_;
};

#endif