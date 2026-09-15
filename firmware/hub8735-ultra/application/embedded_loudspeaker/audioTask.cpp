#include "audioTask.h"

#include <cstdlib>
#include <cstring>

namespace {

AmebaFatFS fs;
MAX98357 amp;

#include <cstring>

bool isAllowedMusicFile(const char* filename)
{
    if (filename == nullptr) {
        return false;
    }

    const size_t len = strlen(filename);
    constexpr size_t ext_len = 4; // strlen(".mp3")

    if (len < ext_len) {
        return false;
    }

    return strcmp(filename + len - ext_len, ".mp3") == 0;
}

void handleAudioCommand(AudioCmdElement& command)
{
    command.filename[sizeof(command.filename) - 1] = '\0';

    switch (command.command) {
    case AudioCommandType::Play:
        if (!isAllowedMusicFile(command.filename)) {
            DEBUG_PRINT("Rejected audio file: ");
            DEBUG_PRINTLN(command.filename);
            return;
        }

        amp.stopMp3();
        if (amp.beginMp3(fs, command.filename)) {
            DEBUG_PRINT("Audio started: ");
            DEBUG_PRINTLN(command.filename);
        } else {
            DEBUG_PRINT("Playback start failed: ");
            DEBUG_PRINTLN(amp.lastError());
        }
        break;

    case AudioCommandType::Stop:
        amp.stopMp3();
        DEBUG_PRINTLN("Audio stopped");
        break;

    case AudioCommandType::SetVolume: {
        const uint8_t percent =
            command.volumePercent > 100 ? 100 : command.volumePercent;
        amp.setVolume(percent / 100.0f);
        DEBUG_PRINT("Audio volume: ");
        DEBUG_PRINTLN(percent);
        break;
    }

    case AudioCommandType::Pause:
        if (amp.pauseMp3()) {
            DEBUG_PRINTLN("Audio pause requested");
        } else {
            DEBUG_PRINTLN("Audio pause ignored: no active playback");
        }
        break;

    case AudioCommandType::Resume:
        if (amp.resumeMp3()) {
            DEBUG_PRINTLN("Audio resume requested");
        } else {
            DEBUG_PRINTLN("Audio resume ignored: audio is not paused");
        }
        break;
    }
}

} // namespace

void audioTask(void* parameter)
{
    (void)parameter;

    if (!fs.begin()) {
        DEBUG_PRINTLN("SD card initialization failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    DEBUG_PRINTLN("SD card initialized successfully");

    if (!amp.begin()) {
        DEBUG_PRINTLN("MAX98357 initialization failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    amp.setShutdownPin(10);
    amp.setVolume(0.5f);
    DEBUG_PRINTLN("MAX98357 initialized successfully");

    for (;;) {
        AudioCmdElement command{};
        const TickType_t waitTicks =
            amp.isMp3Playing() ? 0 : portMAX_DELAY;

        if (xQueueReceive(audioCmdQueue, &command, waitTicks) == pdPASS) {
            handleAudioCommand(command);
        }

        if (amp.isMp3Playing()) {
            const Mp3ProcessResult result = amp.processMp3();
            if (result == Mp3ProcessResult::Finished) {
                DEBUG_PRINTLN("Audio playback finished");
            } else if (result == Mp3ProcessResult::Stopped) {
                DEBUG_PRINTLN("Audio playback stopped");
            } else if (result == Mp3ProcessResult::Paused) {
                DEBUG_PRINTLN("Audio playback paused");
            } else if (result == Mp3ProcessResult::Error) {
                DEBUG_PRINT("Audio playback error: ");
                DEBUG_PRINTLN(amp.lastError());
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}