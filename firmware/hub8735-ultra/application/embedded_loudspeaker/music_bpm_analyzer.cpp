#include "music_bpm_analyzer.h"

#include <FreeRTOS.h>

#include "debug.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {
constexpr const char* TAG = "BPM";
constexpr uint32_t ANALYSIS_SECONDS = 60;
constexpr uint32_t MIN_ANALYSIS_SECONDS = 8;
constexpr unsigned FRAMES_PER_STEP = 8;
constexpr unsigned MAX_OPERATIONS_PER_STEP = 64;
constexpr uint32_t MAX_DECODE_ERRORS = 200;  // ~5 s of frames; beyond this the file is not usable
constexpr uint32_t MIDDLE_START_MIN_BYTES = 2u * 1024 * 1024;
}

MusicBpmAnalyzer::MusicBpmAnalyzer()
    : decoder_(nullptr),
      analyzerReady_(false),
      cursor_(encoded_),
      bytes_(0),
      eof_(false),
      sampleRate_(0),
      frames_(0),
      active_(false),
      decodeErrors_(0),
      startOffset_(0),
      startMs_(0),
      featureUs_(0),
      pcmHash_(0),
      error_(nullptr)
{
}

bool MusicBpmAnalyzer::begin(AmebaFatFS& fs, const char* filename)
{
    cancel();
    error_ = nullptr;
    if (filename == nullptr || filename[0] == '\0') {
        error_ = "empty filename";
        LOGE(TAG, "begin: %s", error_);
        return false;
    }

    const char* root = fs.getRootPath();
    if (root == nullptr) {
        error_ = "SD card is not mounted";
        LOGE(TAG, "begin: %s", error_);
        return false;
    }
    char path[128];
    const int pathLength = snprintf(path, sizeof(path), "%s%s", root, filename);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(path)) {
        error_ = "MP3 path is too long";
        LOGE(TAG, "begin: %s (%s)", error_, filename);
        return false;
    }
    if (!file_.open(path)) {
        error_ = "cannot open MP3";
        LOGE(TAG, "begin: %s: %s", error_, path);
        return false;
    }

    // ponytail: The SDK has no decoder free/reset API; keep one context for
    // audioTask lifetime to avoid per-file leaks. Add cleanup when the SDK exposes it.
    if (decoder_ == nullptr) decoder_ = MP3InitDecoder();
    if (decoder_ == nullptr) {
        file_.close();
        error_ = "MP3 decoder allocation failed";
        LOGE(TAG, "begin: %s (free heap %u)", error_, static_cast<unsigned>(xPortGetFreeHeapSize()));
        return false;
    }

    // Skip the ID3v2 tag (cover art can contain false sync words) and, for long
    // files, start about one third in so a quiet intro does not skew the estimate.
    const uint32_t fileSize = file_.size();
    uint32_t start = 0;
    uint8_t header[10];
    bool id3Found = false;
    if (file_.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        memcmp(header, "ID3", 3) == 0 && (header[6] | header[7] | header[8] | header[9]) < 0x80) {
        start = 10u + ((static_cast<uint32_t>(header[6]) << 21) |
                       (static_cast<uint32_t>(header[7]) << 14) |
                       (static_cast<uint32_t>(header[8]) << 7) | header[9]);
        if ((header[5] & 0x10) != 0) start += 10;
        id3Found = true;
    }
    bool oneThirdIn = false;
    if (fileSize > MIDDLE_START_MIN_BYTES && fileSize / 3 > start) {
        start = fileSize / 3;
        oneThirdIn = true;
    }
    if (start >= fileSize || !file_.seek(start)) {
        file_.close();
        error_ = "cannot seek to audio data";
        LOGE(TAG, "begin: %s (size %lu, start %lu)", error_, static_cast<unsigned long>(fileSize),
             static_cast<unsigned long>(start));
        return false;
    }
    LOGD(TAG, "open %s: size=%lu, ID3 %s, start offset=%lu (%s)", filename, static_cast<unsigned long>(fileSize),
         id3Found ? "skipped" : "none", static_cast<unsigned long>(start),
         oneThirdIn ? "one-third in" : (start > 0 ? "after ID3" : "file start"));
    decodeErrors_ = 0;
    startOffset_ = start;
    startMs_ = millis();
    featureUs_ = 0;
    pcmHash_ = 2166136261u;

    cursor_ = encoded_;
    bytes_ = 0;
    eof_ = false;
    sampleRate_ = 0;
    frames_ = 0;
    active_ = true;
    return true;
}

MusicBpmAnalyzer::StepResult MusicBpmAnalyzer::step(uint16_t& bpm)
{
    if (!active_) return fail("analysis is not active");

    unsigned framesThisStep = 0;
    unsigned operations = 0;
    bool drained = false;  // EOF reached and no decodable data left
    while (framesThisStep < FRAMES_PER_STEP &&
           operations < MAX_OPERATIONS_PER_STEP) {
        ++operations;
        if (sampleRate_ != 0 && frames_ >= sampleRate_ * ANALYSIS_SECONDS) {
            return finish(bpm);
        }

        if (!eof_ && bytes_ < MAINBUF_SIZE) {
            if (bytes_ > 0) memmove(encoded_, cursor_, static_cast<size_t>(bytes_));
            cursor_ = encoded_;
            const int got = file_.read(encoded_ + bytes_, sizeof(encoded_) - bytes_);
            if (got < 0) return fail("SD read failed");
            if (got == 0) eof_ = true;
            else bytes_ += got;
        }

        if (bytes_ <= 4) {
            if (eof_) { drained = true; break; }
            continue;
        }

        const int offset = MP3FindSyncWord(cursor_, bytes_);
        if (offset < 0) {
            if (eof_) { drained = true; break; }
            const int keep = bytes_ < 3 ? bytes_ : 3;
            cursor_ += bytes_ - keep;
            bytes_ = keep;
            continue;
        }

        cursor_ += offset;
        bytes_ -= offset;
        uint8_t* const before = cursor_;
        const int beforeBytes = bytes_;
        const int decodeError = MP3Decode(decoder_, &cursor_, &bytes_, decoded_, 0);
        if (decodeError == ERR_MP3_INDATA_UNDERFLOW ||
            decodeError == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof_) { drained = true; break; }
            if (cursor_ == before && bytes_ == beforeBytes && bytes_ >= MAINBUF_SIZE) {
                ++cursor_;
                --bytes_;
            }
            continue;
        }
        if (decodeError != ERR_MP3_NONE) {
            if (++decodeErrors_ <= 3 || decodeErrors_ % 100 == 0) {
                LOGT(TAG, "decode error %d (total %lu), resync", decodeError,
                     static_cast<unsigned long>(decodeErrors_));
            }
            if (bytes_ > 0) {
                ++cursor_;
                --bytes_;
            }
            continue;
        }

        MP3FrameInfo info{};
        MP3GetLastFrameInfo(decoder_, &info);
        if (info.samprate < 8000 || info.samprate > 48000 ||
            (info.nChans != 1 && info.nChans != 2) || info.bitsPerSample != 16 ||
            info.outputSamps <= 0 || info.outputSamps > 2304 ||
            info.outputSamps % info.nChans != 0 ||
            (sampleRate_ != 0 && sampleRate_ != static_cast<uint32_t>(info.samprate))) {
            // Helix can report success yet leave empty frame info (e.g. a Xing/info frame or a
            // bit-reservoir gap right after a seek). Skip it like a decode error; only give up
            // when the file keeps producing them.
            if (++decodeErrors_ > MAX_DECODE_ERRORS) {
                LOGW(TAG, "bad frame: rate=%d chans=%d bits=%d samples=%d (expected rate %lu); too many errors",
                     info.samprate, info.nChans, info.bitsPerSample, info.outputSamps,
                     static_cast<unsigned long>(sampleRate_));
                return fail("unsupported or inconsistent MP3 frame");
            }
            LOGT(TAG, "bad frame skipped: rate=%d chans=%d samples=%d (errors %lu)", info.samprate, info.nChans,
                 info.outputSamps, static_cast<unsigned long>(decodeErrors_));
            if (cursor_ == before && bytes_ > 0) {  // nothing consumed: avoid spinning on the same bytes
                ++cursor_;
                --bytes_;
            }
            continue;
        }

        if (!analyzerReady_) {
            sampleRate_ = static_cast<uint32_t>(info.samprate);
            if (!analyzer_.begin(sampleRate_, ANALYSIS_SECONDS)) {
                LOGE(TAG, "tempo analyzer allocation failed (free heap %u)", static_cast<unsigned>(xPortGetFreeHeapSize()));
                return fail("tempo analyzer allocation failed");
            }
            analyzerReady_ = true;
            LOGD(TAG, "first frame: %lu Hz, %d ch, %d kbps; analysing up to %lu s",
                 static_cast<unsigned long>(sampleRate_), info.nChans, info.bitrate / 1000,
                 static_cast<unsigned long>(ANALYSIS_SECONDS));
        }

        const int sampleCount = info.outputSamps / info.nChans;
        for (int i = 0; i < sampleCount; ++i) {
            int32_t sample = decoded_[i * info.nChans];
            if (info.nChans == 2) {
                sample = (sample + decoded_[i * 2 + 1]) / 2;
            }
            const uint16_t u = static_cast<uint16_t>(sample);
            pcmHash_ = ((pcmHash_ ^ (u & 0xff)) * 16777619u ^ (u >> 8)) * 16777619u;
            mono_[i] = sample / 32768.0f;
        }
        const uint32_t t0 = micros();
        analyzer_.process(mono_, sampleCount);
        featureUs_ += micros() - t0;
        frames_ += static_cast<uint32_t>(sampleCount);
        ++framesThisStep;
    }

    if (drained) return finish(bpm);
    if (sampleRate_ != 0 && frames_ >= sampleRate_ * ANALYSIS_SECONDS) {
        return finish(bpm);
    }
    return StepResult::InProgress;
}

MusicBpmAnalyzer::StepResult MusicBpmAnalyzer::finish(uint16_t& bpm)
{
    // shortcut: 估計在這一次 step 內完成，期間 audioTask 不處理命令；若 timing log 的 estimate 超過約 1 秒再拆段。
    const uint32_t estimateStartMs = millis();
    float estimate = 0.0f;
    int candidateCount = 0;
    if (analyzerReady_ && sampleRate_ != 0 && frames_ >= sampleRate_ * MIN_ANALYSIS_SECONDS) {
        bpm_tempo::Candidate* candidates = static_cast<bpm_tempo::Candidate*>(
            malloc(sizeof(bpm_tempo::Candidate) * bpm_tempo::MAX_CANDIDATES));
        if (candidates == nullptr) return fail("candidate buffer allocation failed");
        candidateCount = analyzer_.analyze(candidates);
        estimate = bpm_tempo::estimate(candidates, candidateCount);
        free(candidates);
    }
    const uint32_t nowMs = millis();
    const uint32_t featureMs = featureUs_ / 1000;
    const uint32_t totalMs = nowMs - startMs_;
    const uint32_t estimateMs = nowMs - estimateStartMs;
    LOGI(TAG, "timing: total %lu ms = decode+SD %lu + features %lu + estimate %lu; %lu s of audio, %d candidates, "
              "estimate %d.%02d BPM, %lu decode errors, pcm %08lx/%lu",
         static_cast<unsigned long>(totalMs), static_cast<unsigned long>(totalMs - featureMs - estimateMs),
         static_cast<unsigned long>(featureMs), static_cast<unsigned long>(estimateMs),
         static_cast<unsigned long>(sampleRate_ != 0 ? frames_ / sampleRate_ : 0), candidateCount,
         static_cast<int>(estimate), static_cast<int>((estimate - static_cast<int>(estimate)) * 100),
         static_cast<unsigned long>(decodeErrors_), static_cast<unsigned long>(pcmHash_),
         static_cast<unsigned long>(frames_));
    if (!isfinite(estimate) || estimate < 30.0f || estimate > 300.0f) {
        return fail("not enough audio or no BPM estimate");
    }

    const unsigned rounded = static_cast<unsigned>(estimate + 0.5);
    if (rounded < 30 || rounded > 300) {
        return fail("estimated BPM is outside the catalog range");
    }

    bpm = static_cast<uint16_t>(rounded);
    closeCurrentFile();
    return StepResult::Complete;
}

MusicBpmAnalyzer::StepResult MusicBpmAnalyzer::fail(const char* reason)
{
    error_ = reason;
    LOGD(TAG, "fail: %s (frames=%lu, decode errors=%lu)", reason, static_cast<unsigned long>(frames_),
         static_cast<unsigned long>(decodeErrors_));
    closeCurrentFile();
    return StepResult::Failed;
}

void MusicBpmAnalyzer::cancel()
{
    closeCurrentFile();
    error_ = nullptr;
}

void MusicBpmAnalyzer::closeCurrentFile()
{
    if (file_) file_.close();
    analyzer_.release();
    analyzerReady_ = false;
    cursor_ = encoded_;
    bytes_ = 0;
    eof_ = false;
    sampleRate_ = 0;
    frames_ = 0;
    active_ = false;
}