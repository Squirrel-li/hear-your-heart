#include "music_genre_classifier.h"

#include <FreeRTOS.h>

#include "debug.h"
#include "StreamIO.h"
#include "AudioStream.h"
#include "NNAudioClassification.h"

#include <new>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

extern "C" {
#include "mmf2_module.h"
#include "module_array.h"
#include "avcodec.h"
}
#include "SD_Model.h"  // yamnet_from_sd；只在 LoadNNModelSource=SD 時由 SDK 使用

namespace {

constexpr const char* TAG = "GENRE";
constexpr uint32_t NN_RATE = 16000;           // YAMNet 需要 16 kHz mono
constexpr uint32_t ANALYZE_SECONDS = 15;
constexpr uint32_t MIN_SECONDS = 8;           // 少於此長度不可靠
constexpr uint32_t PCM_FRAME_BYTES = 1920;    // array_module 每個 frame 60 ms
constexpr uint32_t MIDDLE_START_MIN_BYTES = 1024u * 1024;
constexpr uint32_t STREAM_TIMEOUT_MS = ANALYZE_SECONDS * 1000 + 3000;
constexpr uint32_t STREAM_IDLE_MS = 2500;     // 已有結果後，這麼久沒有新視窗就視為串流結束
constexpr unsigned FRAMES_PER_STEP = 8;
constexpr unsigned MAX_OPERATIONS_PER_STEP = 64;
constexpr uint32_t MAX_DECODE_ERRORS = 200;

// 自訓 c5u11 模型（tools/yamnet_genre，SD 上的 NN_MDL/yamnet.nb）的 softmax 輸出順序；標籤須 < MUSIC_GENRE_MAX。
constexpr int GENRE_COUNT = 5;
const char* const GENRE_LABELS[GENRE_COUNT] = {"Classical", "Electronic", "Hip-Hop", "Rock", "Other"};
// SDK 前處理的輸入緩衝跨歌曲保留，每首前 2 個視窗含上一首（或開機時的靜音）樣本，不計分。
constexpr uint32_t WARMUP_WINDOWS = 2;

// NN 結果 callback 在 NN 的 task 上執行；audioTask 只讀取。
volatile uint32_t windowCount = 0;   // 含 warm-up 視窗
volatile uint32_t lastWindowMs = 0;
volatile uint32_t badOutputSize = 0;  // 模型輸出類別數不是 5（SD 上放錯模型）
volatile float genreScore[GENRE_COUNT];  // 計分視窗的機率（0..1）累加

float halfToFloat(uint16_t h)
{
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x3FF;
    float value;
    if (exponent == 0) value = ldexpf(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = 65504.0f;
    else value = ldexpf(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return (h & 0x8000) ? -value : value;
}

// 取代 SDK 的 yamnet_postprocess（521 類、只留機率 > 0.5）：直接累計模型原始輸出（fp16 softmax）。
// 不產生 SDK 結果（回傳 0 筆），曲風由 streamStep 取平均後決定。在 NN 的 task 上執行。
int genrePostprocess(void* tensorOut, nn_tensor_param_t* param, void* res)
{
    (void)res;
    const uint16_t* probs = tensorOut != nullptr ? *static_cast<const uint16_t* const*>(tensorOut) : nullptr;
    if (probs == nullptr || param == nullptr) return 0;
    if (param->dim[0].size[0] != static_cast<uint32_t>(GENRE_COUNT)) {
        ++badOutputSize;
    } else if (windowCount >= WARMUP_WINDOWS) {
        for (int i = 0; i < GENRE_COUNT; ++i) genreScore[i] += halfToFloat(probs[i]);
    }
    ++windowCount;
    lastWindowMs = millis();
    return 0;
}

// 包住 SDK 的 array_module，讓 RAM 中的 PCM 像麥克風一樣接到 StreamIO。
// ponytail: array_module 沒有公開文件，參數來自實測；若 SDK 更新需重新驗證。
class PcmArray : public MMFModule {
public:
    bool begin(int16_t* data, uint32_t samples)
    {
        _p_mmf_context = mm_module_open(&array_module);
        if (_p_mmf_context == nullptr) return false;
        array_params_t params = {};
        params.type = AVMEDIA_TYPE_AUDIO;
        params.codec_id = AV_CODEC_ID_PCM_RAW;
        params.mode = ARRAY_MODE_ONCE;
        params.u.a.channel = 1;
        params.u.a.samplerate = NN_RATE;
        params.u.a.sample_bit_length = 16;
        params.u.a.frame_size = PCM_FRAME_BYTES;
        array_t array = {};
        array.data_addr = reinterpret_cast<uint32_t>(data);
        array.data_len = samples * sizeof(int16_t);
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_SET_PARAMS, reinterpret_cast<int>(&params));
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_SET_ARRAY, reinterpret_cast<int>(&array));
        mm_module_ctrl(_p_mmf_context, MM_CMD_SET_QUEUE_LEN, 6);
        mm_module_ctrl(_p_mmf_context, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_APPLY, 0);
        return true;
    }
    void start() { mm_module_ctrl(_p_mmf_context, CMD_ARRAY_STREAMING, 1); }
    void end()
    {
        if (_p_mmf_context == nullptr) return;
        if (mm_module_close(_p_mmf_context) == nullptr) _p_mmf_context = nullptr;
    }
};

AudioSetting nnAudioSetting(NN_RATE, 1, USE_AUDIO_LEFT_DMIC);  // 只用來描述 PCM 格式
NNAudioClassification audioNN;
bool nnStarted = false;
PcmArray* pcmSource = nullptr;
StreamIO* streamer = nullptr;

}  // namespace

MusicGenreClassifier::MusicGenreClassifier()
    : phase_(Phase::Idle),
      decoder_(nullptr),
      cursor_(encoded_),
      bytes_(0),
      eof_(false),
      sampleRate_(0),
      decodeErrors_(0),
      pcm_(nullptr),
      pcmSamples_(0),
      step_(1.0f),
      phase_acc_(0.0f),
      sum_(0.0f),
      sumCount_(0),
      streamStartMs_(0),
      audioStart_(0),
      middleStart_(false),
      error_(nullptr)
{
}

bool MusicGenreClassifier::begin(AmebaFatFS& fs, const char* filename)
{
    cancel();
    error_ = nullptr;
    const char* root = fs.getRootPath();
    if (filename == nullptr || filename[0] == '\0' || root == nullptr) {
        error_ = "empty filename or SD not mounted";
        return false;
    }
    char path[128];
    const int pathLength = snprintf(path, sizeof(path), "%s%s", root, filename);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(path)) {
        error_ = "MP3 path is too long";
        return false;
    }
    if (!file_.open(path)) {
        error_ = "cannot open MP3";
        return false;
    }
    // ponytail: The SDK has no decoder free API; keep one context for audioTask lifetime.
    if (decoder_ == nullptr) decoder_ = MP3InitDecoder();
    if (decoder_ == nullptr) {
        closeFile();
        error_ = "MP3 decoder allocation failed";
        return false;
    }
    pcm_ = static_cast<int16_t*>(malloc(NN_RATE * ANALYZE_SECONDS * sizeof(int16_t)));
    if (pcm_ == nullptr) {
        closeFile();
        error_ = "PCM buffer allocation failed";
        LOGE(TAG, "begin: %s (free heap %u)", error_, static_cast<unsigned>(xPortGetFreeHeapSize()));
        return false;
    }

    // 跳過 ID3v2（封面圖可能含假的 sync word）；長檔從約 1/4 處開始取樣，避開靜音或單一樂器的前奏。
    // ponytail: 以檔案大小估算位置，VBR 檔可能偏離；需要精準時改用時間索引。
    const uint32_t fileSize = file_.size();
    uint32_t start = 0;
    uint8_t header[10];
    if (file_.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        memcmp(header, "ID3", 3) == 0 && (header[6] | header[7] | header[8] | header[9]) < 0x80) {
        start = 10u + ((static_cast<uint32_t>(header[6]) << 21) |
                       (static_cast<uint32_t>(header[7]) << 14) |
                       (static_cast<uint32_t>(header[8]) << 7) | header[9]);
        if ((header[5] & 0x10) != 0) start += 10;
    }
    audioStart_ = start;
    middleStart_ = fileSize > MIDDLE_START_MIN_BYTES && fileSize / 4 > start;
    if (middleStart_) start = fileSize / 4;
    if (start >= fileSize || !file_.seek(start)) {
        cancel();
        error_ = "cannot seek to audio data";
        return false;
    }
    LOGD(TAG, "open %s: size=%lu start=%lu", filename, static_cast<unsigned long>(fileSize),
         static_cast<unsigned long>(start));

    cursor_ = encoded_;
    bytes_ = 0;
    eof_ = false;
    sampleRate_ = 0;
    decodeErrors_ = 0;
    pcmSamples_ = 0;
    phase_acc_ = 0.0f;
    sum_ = 0.0f;
    sumCount_ = 0;
    phase_ = Phase::Decoding;
    return true;
}

// 1/4 處可能落在假的 sync word 上（解碼錯誤超過上限）：改從 ID3 之後重來一次，只退回一次。
bool MusicGenreClassifier::retryFromStart()
{
    if (!middleStart_ || !file_.seek(audioStart_)) return false;
    LOGD(TAG, "decode failed at 1/4 of the file; retrying from byte %lu", static_cast<unsigned long>(audioStart_));
    middleStart_ = false;
    cursor_ = encoded_;
    bytes_ = 0;
    eof_ = false;
    sampleRate_ = 0;
    decodeErrors_ = 0;
    pcmSamples_ = 0;
    phase_acc_ = 0.0f;
    sum_ = 0.0f;
    sumCount_ = 0;
    return true;
}

MusicGenreClassifier::StepResult MusicGenreClassifier::step(char genre[MUSIC_GENRE_MAX])
{
    switch (phase_) {
    case Phase::Decoding: return decodeStep();
    case Phase::Streaming: return streamStep(genre);
    default: return fail("classification is not active");
    }
}

MusicGenreClassifier::StepResult MusicGenreClassifier::decodeStep()
{
    const uint32_t capacity = NN_RATE * ANALYZE_SECONDS;
    unsigned framesThisStep = 0;
    unsigned operations = 0;
    bool drained = false;
    while (framesThisStep < FRAMES_PER_STEP && operations < MAX_OPERATIONS_PER_STEP &&
           pcmSamples_ < capacity) {
        ++operations;
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
        const int error = MP3Decode(decoder_, &cursor_, &bytes_, decoded_, 0);
        if (error == ERR_MP3_INDATA_UNDERFLOW || error == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof_) { drained = true; break; }
            if (cursor_ == before && bytes_ == beforeBytes && bytes_ >= MAINBUF_SIZE) {
                ++cursor_;
                --bytes_;
            }
            continue;
        }
        if (error != ERR_MP3_NONE) {
            if (++decodeErrors_ > MAX_DECODE_ERRORS) {
                if (retryFromStart()) continue;
                return fail("too many decode errors");
            }
            if (bytes_ > 0) { ++cursor_; --bytes_; }
            continue;
        }
        MP3FrameInfo info{};
        MP3GetLastFrameInfo(decoder_, &info);
        if (info.samprate < 8000 || info.samprate > 48000 ||
            (info.nChans != 1 && info.nChans != 2) || info.bitsPerSample != 16 ||
            info.outputSamps <= 0 || info.outputSamps > 2304 ||
            info.outputSamps % info.nChans != 0 ||
            (sampleRate_ != 0 && sampleRate_ != static_cast<uint32_t>(info.samprate))) {
            // seek 之後 bit reservoir 缺資料時 Helix 可能回成功但 frame info 為空：當作錯誤略過。
            if (++decodeErrors_ > MAX_DECODE_ERRORS) {
                if (retryFromStart()) continue;
                return fail("unsupported or inconsistent MP3 frame");
            }
            if (cursor_ == before && bytes_ > 0) { ++cursor_; --bytes_; }
            continue;
        }
        if (sampleRate_ == 0) {
            sampleRate_ = static_cast<uint32_t>(info.samprate);
            step_ = static_cast<float>(sampleRate_) / NN_RATE;
        }
        // 混成單聲道後以區間平均降到 16 kHz（比直接抽點少混疊）。
        const int count = info.outputSamps / info.nChans;
        for (int i = 0; i < count && pcmSamples_ < capacity; ++i) {
            int32_t value = decoded_[i * info.nChans];
            if (info.nChans == 2) value = (value + decoded_[i * 2 + 1]) / 2;
            sum_ += value;
            ++sumCount_;
            phase_acc_ += 1.0f;
            if (phase_acc_ >= step_) {
                phase_acc_ -= step_;
                pcm_[pcmSamples_++] = static_cast<int16_t>(sum_ / sumCount_);
                sum_ = 0.0f;
                sumCount_ = 0;
            }
        }
        ++framesThisStep;
    }

    if (pcmSamples_ >= capacity || drained) {
        if (pcmSamples_ < NN_RATE * MIN_SECONDS) return fail("not enough audio");
        closeFile();  // 串流只需要 RAM 中的 PCM
        if (!startStreaming()) return fail(error_ != nullptr ? error_ : "NN start failed");
        phase_ = Phase::Streaming;
    }
    return StepResult::InProgress;
}

bool MusicGenreClassifier::startStreaming()
{
    windowCount = 0;
    lastWindowMs = 0;
    badOutputSize = 0;
    for (int i = 0; i < GENRE_COUNT; ++i) genreScore[i] = 0;

    // 模型來源由 Arduino 選單 LoadNNModelSource 決定（此處設為 SD）。掃描期間只開一次。
    if (!nnStarted) {
        audioNN.configAudio(nnAudioSetting);
        yamnet_from_sd.postprocess = genrePostprocess;  // 須在 begin() 之前
        audioNN.modelSelect(AUDIO_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_YAMNET);
        audioNN.begin();
        nnStarted = true;
    }
    pcmSource = new (std::nothrow) PcmArray();
    streamer = new (std::nothrow) StreamIO(1, 1);
    if (pcmSource == nullptr || streamer == nullptr || !pcmSource->begin(pcm_, pcmSamples_)) {
        error_ = "array module start failed";
        stopStreaming();
        return false;
    }
    streamer->registerInput(*pcmSource);
    streamer->registerOutput(audioNN);
    if (streamer->begin() != 0) {
        error_ = "StreamIO link start failed";
        stopStreaming();
        return false;
    }
    streamStartMs_ = millis();
    pcmSource->start();
    LOGD(TAG, "streaming %lu samples to NN", static_cast<unsigned long>(pcmSamples_));
    return true;
}

MusicGenreClassifier::StepResult MusicGenreClassifier::streamStep(char genre[MUSIC_GENRE_MAX])
{
    const uint32_t now = millis();
    const uint32_t elapsed = now - streamStartMs_;
    const uint32_t windows = windowCount;
    const bool idle = windows > 0 && now - lastWindowMs > STREAM_IDLE_MS;
    if (elapsed < STREAM_TIMEOUT_MS && !idle) return StepResult::InProgress;

    if (badOutputSize > 0) return fail("model output is not 5 genres (wrong NN_MDL/yamnet.nb?)");
    if (windows <= WARMUP_WINDOWS) return fail("NN produced too few windows");

    // 與離線評估相同：各視窗 softmax 取平均，取最高者。
    const uint32_t scored = windows - WARMUP_WINDOWS;
    int best = 0;
    int percent[GENRE_COUNT];
    for (int i = 0; i < GENRE_COUNT; ++i) {
        percent[i] = static_cast<int>(genreScore[i] / scored * 100.0f + 0.5f);
        if (genreScore[i] > genreScore[best]) best = i;
    }
    LOGI(TAG, "%lu windows in %lu ms -> %s (Classical %d%%, Electronic %d%%, Hip-Hop %d%%, Rock %d%%, Other %d%%)",
         static_cast<unsigned long>(scored), static_cast<unsigned long>(elapsed), GENRE_LABELS[best],
         percent[0], percent[1], percent[2], percent[3], percent[4]);
    strncpy(genre, GENRE_LABELS[best], MUSIC_GENRE_MAX - 1);
    genre[MUSIC_GENRE_MAX - 1] = '\0';
    cancel();
    return StepResult::Complete;
}

void MusicGenreClassifier::stopStreaming()
{
    if (streamer != nullptr) {
        streamer->end();
        delete streamer;
        streamer = nullptr;
    }
    if (pcmSource != nullptr) {
        pcmSource->end();
        delete pcmSource;
        pcmSource = nullptr;
    }
}

MusicGenreClassifier::StepResult MusicGenreClassifier::fail(const char* reason)
{
    error_ = reason;
    LOGD(TAG, "fail: %s (samples=%lu, decode errors=%lu)", reason,
         static_cast<unsigned long>(pcmSamples_), static_cast<unsigned long>(decodeErrors_));
    cancel();
    error_ = reason;  // cancel() 會清掉
    return StepResult::Failed;
}

void MusicGenreClassifier::cancel()
{
    stopStreaming();
    closeFile();
    free(pcm_);
    pcm_ = nullptr;
    pcmSamples_ = 0;
    phase_ = Phase::Idle;
    error_ = nullptr;
}

void MusicGenreClassifier::shutdown()
{
    cancel();
    if (nnStarted) {
        audioNN.end();
        nnStarted = false;
    }
}

void MusicGenreClassifier::closeFile()
{
    if (file_) file_.close();
}
