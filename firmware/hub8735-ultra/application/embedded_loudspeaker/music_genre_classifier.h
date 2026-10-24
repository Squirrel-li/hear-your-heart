#ifndef MUSIC_GENRE_CLASSIFIER_H
#define MUSIC_GENRE_CLASSIFIER_H

#include <stdint.h>

#include "AmebaFatFS.h"
#include "mp3dec.h"
#include "music_library.h"

// 以 NPU（YAMNet，模型由 SD 載入）辨識 MP3 的曲風。介面比照 MusicBpmAnalyzer，
// 由 audioTask 在播放的空檔呼叫，不新增 task。
// 流程：解碼一段 MP3 -> 16 kHz mono PCM（RAM）-> NN -> 累計各視窗的曲風分數 -> 取最高者。
class MusicGenreClassifier {
public:
    enum class StepResult : uint8_t {
        InProgress,
        Complete,  // genre 已填入：Classical / Electronic / Hip-Hop / Rock / Other
        Failed     // 解碼或 NN 失敗；genre 維持空，之後可重試
    };

    MusicGenreClassifier();
    bool begin(AmebaFatFS& fs, const char* filename);
    StepResult step(char genre[MUSIC_GENRE_MAX]);
    void cancel();     // 結束目前這首：關檔、停止串流、釋放 PCM
    // 從 SD 載入 NN 模型並常駐，不再關閉：播放中載入要數秒，會讓播放斷音，所以開機時先載入。
    void loadModel();
    bool modelLoaded() const;
    const char* error() const { return error_; }

private:
    enum class Phase : uint8_t { Idle, Decoding, Streaming };

    StepResult fail(const char* reason);
    StepResult decodeStep();
    StepResult streamStep(char genre[MUSIC_GENRE_MAX]);
    bool startStreaming();
    void stopStreaming();
    void closeFile();
    bool retryFromStart();

    Phase phase_;
    File file_;
    HMP3Decoder decoder_;
    uint8_t encoded_[4096];
    int16_t decoded_[1152 * 2];
    uint8_t* cursor_;
    int bytes_;
    bool eof_;
    uint32_t sampleRate_;
    uint32_t decodeErrors_;
    int16_t* pcm_;
    uint32_t pcmSamples_;
    float step_;     // 來源取樣率 / 16000
    float phase_acc_;
    float sum_;
    uint32_t sumCount_;
    uint32_t streamStartMs_;
    uint32_t audioStart_;  // ID3 之後的第一個位元組
    bool middleStart_;     // 目前是從 1/4 處開始解碼（失敗時可退回 audioStart_）
    const char* error_;
};

#endif
