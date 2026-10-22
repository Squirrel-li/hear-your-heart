// Host check for bpm_tempo.h:
//   g++ -std=c++11 -O2 -I../application/embedded_loudspeaker bpm_tempo_check.cpp -o bpm_tempo_check && ./bpm_tempo_check
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "bpm_tempo.h"

using namespace bpm_tempo;

int main()
{
    // Real FFT power matches a direct DFT.
    const int N = 64;
    float x[N], x0[N], work[N], power[N / 2 + 1];
    for (int i = 0; i < N; ++i) x0[i] = x[i] = sinf(0.3f * i) + 0.5f * cosf(1.7f * i) + (i % 7) * 0.1f;
    Fft f;
    assert(f.init(N / 2));
    f.realPower(x, work, power);
    for (int k = 0; k <= N / 2; ++k) {
        double r = 0, q = 0;
        for (int i = 0; i < N; ++i) { r += x0[i] * cos(2 * kPi * k * i / N); q -= x0[i] * sin(2 * kPi * k * i / N); }
        assert(fabs(power[k] - (r * r + q * q)) < 1e-2 * (1 + r * r + q * q));
    }
    f.release();

    // 100 BPM click track (44.1 kHz, 20 s): 100 BPM is a candidate and the ACF peaks at its period.
    const uint32_t sr = 44100;
    Analyzer a;
    assert(a.begin(sr, 60));
    static float pcm[sr];
    uint32_t n = 0;
    for (int sec = 0; sec < 20; ++sec) {
        for (uint32_t i = 0; i < sr; ++i, ++n) {
            const uint32_t phase = n % (sr * 60 / 100);
            pcm[i] = phase < 400 ? sinf(phase * 0.3f) * (1.0f - phase / 400.0f) : 0.0f;
        }
        a.process(pcm, sr);
    }
    assert(fabsf(a.seconds() - 20.0f) < 0.1f);
    static Candidate c[MAX_CANDIDATES];
    const int count = a.analyze(c);
    assert(count > 5 && count <= MAX_CANDIDATES);
    int hit = -1;
    for (int i = 0; i < count; ++i) {
        assert(c[i].bpm >= 30.0f && c[i].bpm <= 300.0f);
        if (i > 0) assert(c[i].bpm > c[i - 1].bpm * 1.02f);
        if (fabsf(c[i].bpm - 100.0f) <= 4.0f) hit = i;
    }
    assert(hit >= 0);
    assert(c[hit].features[9] > 0.9f && c[hit].features[1] > 0.7f);  // fourier(t), acfAt(t) relative to the grid maximum
    // The trained model picks 100 BPM or one octave away (x2, /2 are left to the user).
    const float e = estimate(c, count);
    assert(fabsf(e - 100.0f) <= 4.0f || fabsf(e - 200.0f) <= 8.0f || fabsf(e - 50.0f) <= 2.0f);
    assert(estimate(c, 0) == 0.0f);
    printf("bpm_tempo_check OK: %d candidates, estimate %.2f BPM\n", count, e);

    // Too little audio -> no candidates.
    assert(a.begin(sr, 60));
    a.process(pcm, sr);
    assert(a.analyze(c) == 0);
    a.release();
    return 0;
}
