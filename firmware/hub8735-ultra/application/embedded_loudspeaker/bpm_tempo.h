#ifndef BPM_TEMPO_H
#define BPM_TEMPO_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// HUB8735（ARM_MATH_ARMV8MML）上逐 frame 的 FFT 改用 SDK 的 CMSIS-DSP（liboutsrc.a），只差浮點捨入；
// PC 與長度 > 4096 的 FFT 用下方自寫版本。
#if defined(ARM_MATH_ARMV8MML) && !defined(BPM_TEMPO_NO_CMSIS)
// Arduino 的類比腳位巨集 A0..A2 與 arm_math.h 的 PID 結構成員同名，include 期間先移除。
#pragma push_macro("A0")
#pragma push_macro("A1")
#pragma push_macro("A2")
#undef A0
#undef A1
#undef A2
#include <arm_math.h>
#pragma pop_macro("A2")
#pragma pop_macro("A1")
#pragma pop_macro("A0")
#define BPM_TEMPO_CMSIS 1
#endif

// 歌曲 BPM 估計（取代 MiniBPM）：log-mel 頻譜差 onset 曲線（全頻 + 4 頻帶）
// -> 自相關與傅立葉兩種週期分布 -> 約 25 個候選 BPM -> 梯度提升樹替每個候選打分，取最高分。
// 樹由 tools/bpm_tempo/train.py 以本檔在 PC 上算出的特徵訓練，輸出成 bpm_tempo_model.h；
// PC 與 HUB 用同一份程式算特徵，訓練與實機一致。倍數（x2、/2）難免判錯，由使用者在介面修正。
// 純 C++11、不依賴 Arduino，可在 PC 編譯（tests/bpm_tempo_check.cpp、tools/bpm_tempo/dump.cpp）。

namespace bpm_tempo {

constexpr int MEL_BANDS = 64;
constexpr int SIGNALS = 5;                       // 0 = 全頻，1..4 = 頻帶
constexpr int BAND_EDGES[5] = {0, 8, 24, 44, 64};
constexpr int GRID_SIZE = 400;                   // 30..300 BPM 對數間隔
constexpr int CONTEXT_POINTS = 32;               // 40..240 BPM 整首週期分布輪廓
constexpr int MAX_CANDIDATES = 64;
constexpr int PEAKS = 8;                         // 每個週期分布取前幾個峰
constexpr int MULT_PEAKS = 2;                    // 前幾個峰再加上 x0.5、x2、x0.75、x1.5、x4/3、x2/3
constexpr float FLOOR_DB = 80.0f;                // 低於目前最大值 80 dB 的頻帶視為靜音，避免雜訊造成假 onset
constexpr int CAND_FEATURES = 34;
constexpr int FEATURES = CAND_FEATURES + 2 * CONTEXT_POINTS;
constexpr int TEMPO_FFT = 32768;                 // onset 曲線補零後的實數 FFT 長度
constexpr float MIN_SECONDS = 8.0f;
constexpr double kPi = 3.14159265358979323846;

// 複數 radix-2 FFT（長度 n），旋轉因子表同時供實數 FFT（長度 2n）後處理使用。
struct Fft {
    int n = 0;
    float* c = nullptr;  // cos(pi k / n), k < n
    float* s = nullptr;
#ifdef BPM_TEMPO_CMSIS
    arm_rfft_fast_instance_f32 rfft;
    bool useCmsis = false;
#endif
    bool init(int size)
    {
        n = size;
        c = static_cast<float*>(malloc(sizeof(float) * n));
        s = static_cast<float*>(malloc(sizeof(float) * n));
        if (c == nullptr || s == nullptr) return false;
        for (int k = 0; k < n; ++k) {
            c[k] = static_cast<float>(cos(kPi * k / n));
            s[k] = static_cast<float>(sin(kPi * k / n));
        }
#ifdef BPM_TEMPO_CMSIS
        useCmsis = 2 * n <= 4096 && arm_rfft_fast_init_f32(&rfft, static_cast<uint16_t>(2 * n)) == ARM_MATH_SUCCESS;
#endif
        return true;
    }
    void release()
    {
        free(c);
        free(s);
        c = s = nullptr;
    }
    void complex(float* re, float* im) const
    {
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                float t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i]; im[i] = im[j]; im[j] = t;
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = 2 * (n / len);
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const float wr = c[j * step], wi = -s[j * step];
                    float* ar = re + i + j; float* ai = im + i + j;
                    const float vr = ar[half] * wr - ai[half] * wi, vi = ar[half] * wi + ai[half] * wr;
                    ar[half] = *ar - vr; ai[half] = *ai - vi;
                    *ar += vr; *ai += vi;
                }
            }
        }
    }
    // 實數輸入 x（長度 2n，會被當工作區改寫）-> |X[k]|^2，k = 0..n。work 為長度 2n 的工作區。
    void realPower(float* x, float* work, float* power)
    {
#ifdef BPM_TEMPO_CMSIS
        if (useCmsis) {  // 輸出：work[0] = X[0]、work[1] = X[n]（皆為實數），之後每對為 X[k] 的實部、虛部
            arm_rfft_fast_f32(&rfft, x, work, 0);
            power[0] = work[0] * work[0];
            power[n] = work[1] * work[1];
            for (int k = 1; k < n; ++k) power[k] = work[2 * k] * work[2 * k] + work[2 * k + 1] * work[2 * k + 1];
            return;
        }
#endif
        float* re = work;
        float* im = work + n;
        for (int k = 0; k < n; ++k) { re[k] = x[2 * k]; im[k] = x[2 * k + 1]; }
        complex(re, im);
        power[0] = (re[0] + im[0]) * (re[0] + im[0]);
        power[n] = (re[0] - im[0]) * (re[0] - im[0]);
        for (int k = 1; k < n; ++k) {
            const float a = re[k], b = im[k], cc = re[n - k], d = im[n - k];
            const float er = 0.5f * (a + cc), ei = 0.5f * (b - d), orr = 0.5f * (b + d), oi = 0.5f * (cc - a);
            const float xr = er + c[k] * orr + s[k] * oi, xi = ei + c[k] * oi - s[k] * orr;
            power[k] = xr * xr + xi * xi;
        }
    }
};

inline float gridBpm(int i) { return 30.0f * powf(10.0f, static_cast<float>(i) / (GRID_SIZE - 1)); }
inline float contextBpm(int i) { return 40.0f * powf(6.0f, static_cast<float>(i) / (CONTEXT_POINTS - 1)); }

// 一條 onset 曲線的自相關（ACF）與傅立葉週期分布，以 BPM 查詢；搜尋 ±2% 內的最大值。
struct Tempo {
    float fps = 0;
    float* acf = nullptr; int acfLen = 0;
    float* spec = nullptr; int specLen = 0; float df = 0;
    float acfAt(float bpm) const
    {
        const float l = 60.0f * fps / bpm;
        const int lo = static_cast<int>(l * 0.98f), hi = static_cast<int>(ceilf(l * 1.02f)) + 1;
        if (lo < 1 || hi > acfLen) return 0.0f;
        float m = acf[lo];
        for (int i = lo + 1; i < hi; ++i) if (acf[i] > m) m = acf[i];
        return m;
    }
    float fourier(float bpm) const
    {
        const float c = bpm / 60.0f / df;
        const int lo = static_cast<int>(c * 0.98f), hi = static_cast<int>(ceilf(c * 1.02f)) + 1;
        if (hi > specLen) return 0.0f;
        float m = spec[lo];
        for (int i = lo + 1; i < hi; ++i) if (spec[i] > m) m = spec[i];
        return m;
    }
    float combA(float t) const { return 0.25f * (acfAt(t) + acfAt(t / 2) + acfAt(t / 3) + acfAt(t / 4)); }
    float combF(float t) const { return 0.25f * (fourier(t) + fourier(2 * t) + fourier(3 * t) + fourier(4 * t)); }
};

struct Candidate {
    float bpm;
    float features[FEATURES];
};

// 串流使用：begin -> 反覆 process（單聲道 -1..1）-> analyze。所有緩衝區在 begin 配置、release 釋放。
class Analyzer {
public:
    ~Analyzer() { release(); }

    bool begin(uint32_t sampleRate, uint32_t maxSeconds)
    {
        release();
        if (sampleRate < 8000) return false;
        frameSize_ = sampleRate > 24000 ? 2048 : (sampleRate > 12000 ? 1024 : 512);
        hop_ = frameSize_ / 4;
        fps_ = static_cast<float>(sampleRate) / hop_;
        maxFrames_ = static_cast<int>(static_cast<uint64_t>(maxSeconds) * sampleRate / hop_) + 2;
        const int bins = frameSize_ / 2 + 1;
        buf_ = alloc(frameSize_); window_ = alloc(frameSize_); work_ = alloc(2 * TEMPO_FFT);
        power_ = alloc(TEMPO_FFT / 2 + 1); prevMel_ = alloc(MEL_BANDS); melFrac_ = alloc(bins);
        melIndex_ = static_cast<int16_t*>(malloc(sizeof(int16_t) * bins));
        for (int s = 0; s < SIGNALS; ++s) flux_[s] = alloc(maxFrames_);
        if (buf_ == nullptr || window_ == nullptr || work_ == nullptr || power_ == nullptr ||
            prevMel_ == nullptr || melFrac_ == nullptr || melIndex_ == nullptr ||
            !frameFft_.init(frameSize_ / 2) || !tempoFft_.init(TEMPO_FFT / 2)) {
            release();
            return false;
        }
        for (int s = 0; s < SIGNALS; ++s) if (flux_[s] == nullptr) { release(); return false; }
        for (int i = 0; i < frameSize_; ++i) window_[i] = 0.5f - 0.5f * cosf(2.0f * static_cast<float>(kPi) * i / frameSize_);
        // mel 三角濾波器（0..min(11025, sr/2) Hz）：每個 FFT bin 最多落在相鄰兩個頻帶。
        const float fmax = sampleRate / 2.0f < 11025.0f ? sampleRate / 2.0f : 11025.0f;
        const float melMax = mel(fmax), melStep = melMax / (MEL_BANDS + 1);
        for (int k = 0; k < bins; ++k) {
            const float f = static_cast<float>(k) * sampleRate / frameSize_;
            if (f >= fmax) { melIndex_[k] = -1; continue; }
            const float m = mel(f) / melStep;
            melIndex_[k] = static_cast<int16_t>(m);
            melFrac_[k] = m - melIndex_[k];
        }
        filled_ = 0;
        frames_ = 0;
        maxDb_ = -100.0f;
        return true;
    }

    void process(const float* x, int n)
    {
        while (n > 0) {
            int take = frameSize_ - filled_;
            if (take > n) take = n;
            memcpy(buf_ + filled_, x, sizeof(float) * take);
            filled_ += take; x += take; n -= take;
            if (filled_ == frameSize_) {
                frame();
                memmove(buf_, buf_ + hop_, sizeof(float) * (frameSize_ - hop_));
                filled_ = frameSize_ - hop_;
            }
        }
    }

    float seconds() const { return frames_ / fps_; }

    // 回傳候選數；out 至少 MAX_CANDIDATES。音訊不足或配置失敗回傳 0。
    int analyze(Candidate* out)
    {
        const int n = frames_ - 1;  // onset 曲線長度（第一個 frame 沒有前一個可比較）
        if (n < MIN_SECONDS * fps_ || n > TEMPO_FFT) return 0;
        Tempo t[SIGNALS];
        float* acf[SIGNALS] = {};
        float* spec[SIGNALS] = {};
        const int acfLen = static_cast<int>(60.0f * fps_ / 30.0f * 4.5f) < n - 1 ? static_cast<int>(60.0f * fps_ / 30.0f * 4.5f) : n - 1;
        const float df = fps_ / TEMPO_FFT;
        const int specLen = static_cast<int>(1260.0f / 60.0f / df) + 2;  // 只需查到 4 x 300 BPM
        int count = 0;
        bool ok = true;
        for (int s = 0; s < SIGNALS && ok; ++s) {
            acf[s] = alloc(acfLen); spec[s] = alloc(specLen);
            ok = acf[s] != nullptr && spec[s] != nullptr;
            if (ok) tempo(flux_[s] + 1, n, acf[s], acfLen, spec[s], specLen, t[s]);
            t[s].df = df;
        }
        if (ok) count = candidates(t, out);
        for (int s = 0; s < SIGNALS; ++s) { free(acf[s]); free(spec[s]); }
        return count;
    }

    void release()
    {
        free(buf_); free(window_); free(work_); free(power_); free(prevMel_); free(melFrac_); free(melIndex_);
        buf_ = window_ = work_ = power_ = prevMel_ = melFrac_ = nullptr;
        melIndex_ = nullptr;
        for (int s = 0; s < SIGNALS; ++s) { free(flux_[s]); flux_[s] = nullptr; }
        frameFft_.release();
        tempoFft_.release();
        frames_ = 0;
    }

private:
    static float* alloc(int n) { return static_cast<float*>(malloc(sizeof(float) * n)); }
    static float mel(float f) { return 2595.0f * log10f(1.0f + f / 700.0f); }  // HTK mel

    void frame()
    {
        if (frames_ >= maxFrames_) return;
        float* x = work_;
        for (int i = 0; i < frameSize_; ++i) x[i] = buf_[i] * window_[i];
        frameFft_.realPower(x, work_ + frameSize_, power_);  // work_ 長度 2 x TEMPO_FFT
        float melE[MEL_BANDS] = {};
        for (int k = 0; k <= frameSize_ / 2; ++k) {
            const int j = melIndex_[k];
            if (j < 0) continue;
            if (j >= 1 && j - 1 < MEL_BANDS) melE[j - 1] += (1.0f - melFrac_[k]) * power_[k];
            if (j < MEL_BANDS) melE[j] += melFrac_[k] * power_[k];
        }
        float bandSum[SIGNALS] = {};
        for (int b = 0; b < MEL_BANDS; ++b) {
            float db = 10.0f * log10f(melE[b] + 1e-10f);
            if (db > maxDb_) maxDb_ = db;
            if (db < maxDb_ - FLOOR_DB) db = maxDb_ - FLOOR_DB;
            const float d = frames_ > 0 && db > prevMel_[b] ? db - prevMel_[b] : 0.0f;
            prevMel_[b] = db;
            bandSum[0] += d;
            for (int s = 1; s < SIGNALS; ++s) if (b >= BAND_EDGES[s - 1] && b < BAND_EDGES[s]) bandSum[s] += d;
        }
        flux_[0][frames_] = bandSum[0] / MEL_BANDS;
        for (int s = 1; s < SIGNALS; ++s) flux_[s][frames_] = bandSum[s] / (BAND_EDGES[s] - BAND_EDGES[s - 1]);
        ++frames_;
    }

    // 去掉約 0.5 秒的移動平均、只留正值並標準化，再算 ACF（直接計算）與補零 FFT 的幅度譜。
    void tempo(const float* raw, int n, float* acf, int acfLen, float* spec, int specLen, Tempo& t)
    {
        float* y = work_;  // 前 TEMPO_FFT 個放補零的曲線，後半給 FFT 當工作區
        const int h = static_cast<int>(0.25f * fps_ + 0.5f), w = 2 * h + 1;
        double run = 0;
        for (int i = 0; i < h && i < n; ++i) run += raw[i];
        double mean = 0, sq = 0;
        for (int i = 0; i < n; ++i) {
            if (i + h < n) run += raw[i + h];
            if (i - h - 1 >= 0) run -= raw[i - h - 1];
            const float v = raw[i] - static_cast<float>(run / w);
            y[i] = v > 0 ? v : 0;
            mean += y[i]; sq += static_cast<double>(y[i]) * y[i];
        }
        mean /= n;
        const double sd = sqrt(sq / n - mean * mean);
        for (int i = 0; i < n; ++i) y[i] = sd > 0 ? static_cast<float>((y[i] - mean) / sd) : 0.0f;
        for (int l = 0; l < acfLen; ++l) {  // 熱點（約 2000 萬次乘加）：M33 的 FPU 只有單精度，不可用 double
            float a = 0;
            for (int i = 0; i + l < n; ++i) a += y[i] * y[i + l];
            acf[l] = a / (n - l);
        }
        const float a0 = acf[0];
        if (a0 > 0) for (int l = 0; l < acfLen; ++l) acf[l] /= a0;
        for (int i = n; i < TEMPO_FFT; ++i) y[i] = 0;
        tempoFft_.realPower(y, work_ + TEMPO_FFT, power_);
        float mx = 0;
        for (int k = 0; k <= TEMPO_FFT / 2; ++k) { power_[k] = sqrtf(power_[k]); if (power_[k] > mx) mx = power_[k]; }
        for (int k = 0; k < specLen; ++k) spec[k] = power_[k] / (mx + 1e-12f);
        t.acf = acf; t.acfLen = acfLen; t.spec = spec; t.specLen = specLen; t.fps = fps_;
    }

    static int topPeaks(const float* v, float* out)
    {
        int idx[GRID_SIZE]; int m = 0;
        for (int i = 1; i + 1 < GRID_SIZE; ++i) if (v[i] > v[i - 1] && v[i] >= v[i + 1]) idx[m++] = i;
        for (int i = 1; i < m; ++i)
            for (int j = i; j > 0 && v[idx[j]] > v[idx[j - 1]]; --j) { const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
        const int k = m < PEAKS ? m : PEAKS;
        for (int i = 0; i < k; ++i) out[i] = gridBpm(idx[i]);
        return k;
    }

    static float rankIn(const float* list, int n, float t)
    {
        for (int k = 0; k < n; ++k) if (fabsf(list[k] - t) <= 0.03f * t) return static_cast<float>(k);
        return 9.0f;
    }

    static int candidates(const Tempo* T, Candidate* out)
    {
        static float cA[GRID_SIZE], cF[GRID_SIZE], rA[GRID_SIZE], rF[GRID_SIZE];
        float mA = 0, mF = 0, mcA = 0, mcF = 0;
        for (int i = 0; i < GRID_SIZE; ++i) {
            const float g = gridBpm(i);
            cA[i] = T[0].combA(g); cF[i] = T[0].combF(g); rA[i] = T[0].acfAt(g); rF[i] = T[0].fourier(g);
            if (rA[i] > mA) mA = rA[i];
            if (rF[i] > mF) mF = rF[i];
            if (cA[i] > mcA) mcA = cA[i];
            if (cF[i] > mcF) mcF = cF[i];
        }
        mA += 1e-9f; mF += 1e-9f; mcA += 1e-9f; mcF += 1e-9f;
        float src[4][PEAKS]; int srcN[4];
        srcN[0] = topPeaks(cA, src[0]); srcN[1] = topPeaks(cF, src[1]);
        srcN[2] = topPeaks(rA, src[2]); srcN[3] = topPeaks(rF, src[3]);
        static const float MULTS[6] = {0.5f, 2.0f, 0.75f, 1.5f, 4.0f / 3.0f, 2.0f / 3.0f};
        float pool[4 * (PEAKS + 6 * MULT_PEAKS)]; int np = 0;
        for (int s = 0; s < 4; ++s) {
            for (int k = 0; k < srcN[s]; ++k) pool[np++] = src[s][k];
            for (int p = 0; p < srcN[s] && p < MULT_PEAKS; ++p)
                for (int k = 0; k < 6; ++k) pool[np++] = src[s][p] * MULTS[k];
        }
        for (int i = 1; i < np; ++i)
            for (int j = i; j > 0 && pool[j] < pool[j - 1]; --j) { const float t = pool[j]; pool[j] = pool[j - 1]; pool[j - 1] = t; }
        float context[2 * CONTEXT_POINTS]; float ma = 0, mf = 0;
        for (int i = 0; i < CONTEXT_POINTS; ++i) {
            context[i] = T[0].combA(contextBpm(i)); context[CONTEXT_POINTS + i] = T[0].combF(contextBpm(i));
            if (context[i] > ma) ma = context[i];
            if (context[CONTEXT_POINTS + i] > mf) mf = context[CONTEXT_POINTS + i];
        }
        for (int i = 0; i < CONTEXT_POINTS; ++i) { context[i] /= ma + 1e-9f; context[CONTEXT_POINTS + i] /= mf + 1e-9f; }
        int count = 0;
        float last = 0;
        for (int i = 0; i < np && count < MAX_CANDIDATES; ++i) {
            const float t = pool[i];
            if (t < 30.0f || t > 300.0f || (count > 0 && t <= last * 1.02f)) continue;
            last = t;
            Candidate& c = out[count++];
            float* f = c.features; int j = 0;
            c.bpm = t;
            f[j++] = log2f(t / 120.0f);
            const float am[8] = {1, 0.5f, 1.0f / 3, 0.25f, 2, 3, 1.5f, 1 / 1.5f};
            for (int k = 0; k < 8; ++k) f[j++] = T[0].acfAt(t * am[k]) / mA;
            const float fm[7] = {1, 2, 3, 4, 0.5f, 1.0f / 3, 1.5f};
            for (int k = 0; k < 7; ++k) f[j++] = T[0].fourier(t * fm[k]) / mF;
            f[j++] = T[0].combA(t) / mcA;
            f[j++] = T[0].combF(t) / mcF;
            for (int s = 1; s < SIGNALS; ++s) f[j++] = T[s].acfAt(t);
            for (int s = 1; s < SIGNALS; ++s) {
                float m = T[s].fourier(t / 2);
                if (T[s].fourier(t) > m) m = T[s].fourier(t);
                if (T[s].fourier(2 * t) > m) m = T[s].fourier(2 * t);
                f[j++] = T[s].fourier(t) / (m + 1e-9f);
            }
            for (int s = 1; s < SIGNALS; ++s) f[j++] = T[s].acfAt(t / 2);
            for (int s = 0; s < 4; ++s) f[j++] = rankIn(src[s], srcN[s], t);
            memcpy(f + j, context, sizeof(context));
        }
        return count;
    }

    Fft frameFft_, tempoFft_;
    int frameSize_ = 0, hop_ = 0, maxFrames_ = 0, filled_ = 0, frames_ = 0;
    float fps_ = 0, maxDb_ = -100.0f;
    float* buf_ = nullptr; float* window_ = nullptr; float* work_ = nullptr; float* power_ = nullptr;
    float* prevMel_ = nullptr; float* melFrac_ = nullptr; int16_t* melIndex_ = nullptr;
    float* flux_[SIGNALS] = {};
};

}  // namespace bpm_tempo

#ifndef BPM_TEMPO_NO_MODEL
#include "bpm_tempo_model.h"

namespace bpm_tempo {

static_assert(MODEL_FEATURES == FEATURES, "bpm_tempo_model.h was trained on a different feature set");

inline float score(const float* f)
{
    float s = MODEL_BASE;
    for (int t = 0; t < MODEL_TREES; ++t) {
        int n = MODEL_ROOTS[t];
        while (MODEL_NODES[n].feature >= 0)
            n = f[MODEL_NODES[n].feature] <= MODEL_NODES[n].value ? MODEL_NODES[n].left : MODEL_NODES[n].right;
        s += MODEL_NODES[n].value;
    }
    return s;
}

// 分數最高的候選 BPM；沒有候選時回傳 0。
inline float estimate(const Candidate* c, int n)
{
    float best = 0, bestScore = -1e30f;
    for (int i = 0; i < n; ++i) {
        const float s = score(c[i].features);
        if (s > bestScore) { bestScore = s; best = c[i].bpm; }
    }
    return best;
}

}  // namespace bpm_tempo
#endif

#endif
