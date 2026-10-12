/*
 Classify an MP3 file on the SD card with the audio classification NN
 (the NN model itself is configured separately on the SD card).

 Put the MP3 in the SD card root and set `song` below.
 Flow: SD MP3 -> Helix decode -> 16 kHz mono PCM in RAM -> array_module -> NN.

 ponytail: array_module has no documented Arduino wrapper; the parameters in
 PcmArray::begin() are a best guess and may need tuning on the board.
*/

#include "AmebaFatFS.h"
#include "StreamIO.h"
#include "NNAudioClassification.h"
#include "AudioClassList.h"
#include "mp3dec.h"

extern "C" {
#include "mmf2_module.h"
#include "module_array.h"
#include "avcodec.h"
}

static const char *song = "001.mp3";
#define ANALYZE_SECONDS 30     // Only the first N seconds are classified
#define NN_RATE 16000          // NN audio classification requires 16 kHz mono
#define PCM_FRAME_BYTES 1920   // 60 ms per array_module frame
#define DEBUG_PRINT_ALL 1      // 1: print every class the model reports

static AmebaFatFS fs;
static uint8_t encoded[4096];
static int16_t decoded[1152 * 2];
static int16_t *pcm = nullptr;
static uint32_t pcmSamples = 0;

// Wraps the SDK array_module so a RAM buffer can feed StreamIO like a microphone.
class PcmArray : public MMFModule {
public:
    bool begin(int16_t *data, uint32_t samples)
    {
        _p_mmf_context = mm_module_open(&array_module);
        if (!_p_mmf_context) return false;
        array_params_t params = {};
        params.type = AVMEDIA_TYPE_AUDIO;
        params.codec_id = AV_CODEC_ID_PCM_RAW;
        params.mode = ARRAY_MODE_ONCE;
        params.u.a.channel = 1;
        params.u.a.samplerate = NN_RATE;
        params.u.a.sample_bit_length = 16;
        params.u.a.frame_size = PCM_FRAME_BYTES;
        array_t array = {};
        array.data_addr = (uint32_t)data;
        array.data_len = samples * 2;
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_SET_PARAMS, (int)&params);
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_SET_ARRAY, (int)&array);
        mm_module_ctrl(_p_mmf_context, MM_CMD_SET_QUEUE_LEN, 6);
        mm_module_ctrl(_p_mmf_context, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
        mm_module_ctrl(_p_mmf_context, CMD_ARRAY_APPLY, 0);
        return true;
    }
    void startStreaming() { mm_module_ctrl(_p_mmf_context, CMD_ARRAY_STREAMING, 1); }
};

// Decode the first ANALYZE_SECONDS of the MP3 into pcm[] as 16 kHz mono.
// Sample-rate conversion is nearest-neighbour; ponytail: add a low-pass filter if aliasing hurts accuracy.
static bool decodeMp3(const char *filename)
{
    File file = fs.open(String(fs.getRootPath()) + filename);
    if (!file) return false;
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) { file.close(); return false; }

    const uint32_t maxSamples = (uint32_t)NN_RATE * ANALYZE_SECONDS;
    pcm = (int16_t *)malloc(maxSamples * sizeof(int16_t));
    if (!pcm) { file.close(); return false; }
    pcmSamples = 0;

    uint8_t *cursor = encoded;
    int bytes = 0;
    bool eof = false;
    uint32_t srcRate = 0;
    double srcPos = 0;      // fractional read position carried across frames
    double step = 1.0;

    while (pcmSamples < maxSamples) {
        if (!eof && bytes < MAINBUF_SIZE) {
            memmove(encoded, cursor, bytes);
            cursor = encoded;
            const int got = file.read(encoded + bytes, sizeof(encoded) - bytes);
            if (got < 0) break;
            if (got == 0) eof = true;
            bytes += got;
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
        uint8_t *const before = cursor;
        const int beforeBytes = bytes;
        const int error = MP3Decode(decoder, &cursor, &bytes, decoded, 0);
        if (error == ERR_MP3_INDATA_UNDERFLOW || error == ERR_MP3_MAINDATA_UNDERFLOW) {
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
            info.outputSamps <= 0 || info.outputSamps > 2304 ||
            info.outputSamps % info.nChans != 0 ||
            (srcRate && srcRate != (uint32_t)info.samprate)) {
            break;
        }
        if (!srcRate) {
            srcRate = info.samprate;
            step = (double)srcRate / NN_RATE;
        }
        const int count = info.outputSamps / info.nChans;
        while (srcPos < count && pcmSamples < maxSamples) {
            const int i = (int)srcPos;
            int32_t value = decoded[i * info.nChans];
            if (info.nChans == 2) value = (value + decoded[i * 2 + 1]) / 2;
            pcm[pcmSamples++] = (int16_t)value;
            srcPos += step;
        }
        srcPos -= count;
    }
    file.close();
    return pcmSamples >= (uint32_t)NN_RATE;    // need at least ~1 s for one inference
}

// Generic "Music" (132) and all music-related classes (genres, songs).
static bool isMusicClass(int id)
{
    return id == 132 || (id >= 211 && id <= 270);
}

// Genre classes only (Pop, Rock, Jazz, ...).
static bool isGenreClass(int id)
{
    return id >= 211 && id <= 260;
}

void ACPostProcess(std::vector<AudioClassificationResult> results);

AudioSetting configA(NN_RATE, 1, USE_AUDIO_LEFT_DMIC);    // Only used to describe the PCM format to the NN
NNAudioClassification audioNN;
PcmArray pcmSource;
StreamIO audioStreamerNN(1, 1);

void setup()
{
    Serial.begin(115200);
    delay(1000);
    if (!fs.begin()) {
        Serial.println("SD card initialization failed");
        return;
    }
    Serial.print("Decoding ");
    Serial.println(song);
    if (!decodeMp3(song)) {
        Serial.println("MP3 decode failed");
        return;
    }
    Serial.print("Decoded samples @16kHz: ");
    Serial.println(pcmSamples);

    audioNN.configAudio(configA);
    audioNN.setResultCallback(ACPostProcess);
    audioNN.modelSelect(AUDIO_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_YAMNET);
    audioNN.begin();

    if (!pcmSource.begin(pcm, pcmSamples)) {
        Serial.println("array_module open failed");
        return;
    }
    audioStreamerNN.registerInput(pcmSource);
    audioStreamerNN.registerOutput(audioNN);
    if (audioStreamerNN.begin() != 0) {
        Serial.println("StreamIO link start failed");
        return;
    }
    pcmSource.startStreaming();
}

void loop()
{
}

void ACPostProcess(std::vector<AudioClassificationResult> results)
{
    int count = audioNN.getResultCount();
    if (count == 0) {
        printf("No result\r\n");
        return;
    }

    bool music = false;
    int musicScore = 0;
    int genreId = -1;
    int genreScore = 0;
    for (int i = 0; i < count; i++) {
        int class_id = (int)results[i].classID();
        int score = results[i].score();

        if (isMusicClass(class_id)) {
            music = true;
            if (score > musicScore) musicScore = score;
            if (isGenreClass(class_id) && score > genreScore) {
                genreId = class_id;
                genreScore = score;
            }
        }
        if (DEBUG_PRINT_ALL || isMusicClass(class_id)) {
            printf("class %d, score: %d, %s\r\n", class_id, score, audioNames[class_id].audioName);
        }
    }

    if (!music) {
        printf("=> no music\r\n");
    } else if (genreId >= 0) {
        printf("=> MUSIC, genre: %s (class %d, score %d)\r\n", audioNames[genreId].audioName, genreId, genreScore);
    } else {
        printf("=> MUSIC, genre: unknown (score %d)\r\n", musicScore);
    }
}
