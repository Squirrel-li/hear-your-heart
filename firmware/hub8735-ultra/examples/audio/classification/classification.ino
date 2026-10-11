/*
 Example guide:
 https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Example_Guides/Neural%20Network/Audio%20Classification.html
*/

#include "WiFi.h"
#include "StreamIO.h"
#include "NNAudioClassification.h"
#include "AudioClassList.h"

// Mic type: USE_AUDIO_AMIC / USE_AUDIO_LEFT_DMIC / USE_AUDIO_RIGHT_DMIC
// If everything is classified as Silence, try another mic type.
#define MIC_TYPE USE_AUDIO_LEFT_DMIC
#define MUSIC_MIN_SCORE 0    // Raise to ignore low-confidence music results
#define DEBUG_PRINT_ALL 1    // 1: print every class the model reports, 0: music/silence only
#define SILENCE_CLASS_ID 494

// NN audio classification requires 16KHz
AudioSetting configA(16000, 1, MIC_TYPE);    // Sample rate, Channel count, Mic type
Audio audio;
NNAudioClassification audioNN;
StreamIO audioStreamerNN(1, 1);    // 1 Input Audio -> 1 Output Audio Classification

void setup()
{
    // Configure audio peripheral for audio data format
    audio.configAudio(configA);
    audio.begin();
    audio.printInfo();    // Check mic type, gain and mute state

    audioNN.configAudio(configA);
    audioNN.setResultCallback(ACPostProcess);
    audioNN.modelSelect(AUDIO_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_YAMNET);
    audioNN.begin();

    // Configure StreamIO object to stream data from audio to audio classification
    audioStreamerNN.registerInput(audio);
    audioStreamerNN.registerOutput(audioNN);
    if (audioStreamerNN.begin() != 0) {
        Serial.println("StreamIO link start failed");
    }
}

void loop()
{
    // Do nothing
}

// Generic "Music" (132) and all music-related classes (genres, songs). Instruments (133-206) are excluded on purpose.
static bool isMusicClass(int id)
{
    return id == 132 || (id >= 211 && id <= 270);
}

// Genre classes only (Pop, Rock, Jazz, ...). Generic labels such as Song / Background music are not genres.
static bool isGenreClass(int id)
{
    return id >= 211 && id <= 260;
}

// User callback function
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

        if (isMusicClass(class_id) && score >= MUSIC_MIN_SCORE) {
            music = true;
            if (score > musicScore) {
                musicScore = score;
            }
            if (isGenreClass(class_id) && score > genreScore) {
                genreId = class_id;
                genreScore = score;
            }
        }
        if (DEBUG_PRINT_ALL || isMusicClass(class_id) || class_id == SILENCE_CLASS_ID) {
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
