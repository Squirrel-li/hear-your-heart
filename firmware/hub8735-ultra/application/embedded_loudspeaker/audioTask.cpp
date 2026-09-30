#include "audioTask.h"

#include <cstring>

namespace {

AmebaFatFS fs;
MAX98357 amp;
bool currentPlaybackAutomatic = false;
int8_t currentAutomaticSongIndex = -1;

bool isAllowedMusicFile(const char* filename)
{
    if (filename == nullptr) return false;
    const size_t len = strlen(filename);
    constexpr size_t ext_len = 4; // strlen(".mp3")
    return len >= ext_len && strcmp(filename + len - ext_len, ".mp3") == 0;
}

void sendPlaybackEvent(AudioPlaybackEventType type, int8_t songIndex)
{
    if (audioPlaybackEventQueue == nullptr) return;
    AudioPlaybackEvent event{};
    event.type = type;
    event.songIndex = songIndex;
    if (xQueueSend(audioPlaybackEventQueue, &event, pdMS_TO_TICKS(20)) == pdPASS) {
        DEBUG_PRINT("audioTask -> controlTask: playback event=");
        DEBUG_PRINT(static_cast<int>(event.type));
        DEBUG_PRINT(" songIndex=");
        DEBUG_PRINTLN(static_cast<int>(event.songIndex));
    } else {
        DEBUG_PRINTLN("Could not report automatic playback event");
    }
}

void handleAudioCommand(AudioCmdElement& command)
{
    command.filename[sizeof(command.filename) - 1] = '\0';

    switch (command.command) {
    case AudioCommandType::Play: {
        const bool automatic = command.isAutomatic != 0;
        if (!isAllowedMusicFile(command.filename)) {
            DEBUG_PRINT("Rejected audio file: ");
            DEBUG_PRINTLN(command.filename);
            if (automatic) {
                sendPlaybackEvent(AudioPlaybackEventType::Failed, command.songIndex);
            }
            return;
        }

        amp.stopMp3();
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        if (amp.beginMp3(fs, command.filename)) {
            currentPlaybackAutomatic = automatic;
            currentAutomaticSongIndex = automatic ? command.songIndex : -1;
            DEBUG_PRINT("Audio started: ");
            DEBUG_PRINTLN(command.filename);
        } else {
            DEBUG_PRINT("Playback start failed: ");
            DEBUG_PRINTLN(amp.lastError());
            if (automatic) {
                sendPlaybackEvent(AudioPlaybackEventType::Failed, command.songIndex);
            }
        }
        break;
    }

    case AudioCommandType::Stop:
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
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
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        if (amp.pauseMp3()) {
            DEBUG_PRINTLN("Audio pause requested");
        } else {
            DEBUG_PRINTLN("Audio pause ignored: no active playback");
        }
        break;

    case AudioCommandType::Resume:
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
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
            DEBUG_PRINT("controlTask -> audioTask [recv]: command=");
            DEBUG_PRINT(static_cast<int>(command.command));
            if (command.command == AudioCommandType::Play) {
                DEBUG_PRINT(" file=");
                DEBUG_PRINTLN(command.filename);
            } else if (command.command == AudioCommandType::SetVolume) {
                DEBUG_PRINT(" volume=");
                DEBUG_PRINTLN(command.volumePercent);
            } else {
                DEBUG_PRINTLN("");
            }
            handleAudioCommand(command);
        }

        if (amp.isMp3Playing()) {
            const Mp3ProcessResult result = amp.processMp3();
            if (result == Mp3ProcessResult::Finished) {
                DEBUG_PRINTLN("Audio playback finished");
                if (currentPlaybackAutomatic) {
                    sendPlaybackEvent(
                        AudioPlaybackEventType::Finished,
                        currentAutomaticSongIndex);
                }
                currentPlaybackAutomatic = false;
                currentAutomaticSongIndex = -1;
            } else if (result == Mp3ProcessResult::Stopped) {
                DEBUG_PRINTLN("Audio playback stopped");
            } else if (result == Mp3ProcessResult::Paused) {
                DEBUG_PRINTLN("Audio playback paused");
            } else if (result == Mp3ProcessResult::Error) {
                DEBUG_PRINT("Audio playback error: ");
                DEBUG_PRINTLN(amp.lastError());
                if (currentPlaybackAutomatic) {
                    sendPlaybackEvent(
                        AudioPlaybackEventType::Failed,
                        currentAutomaticSongIndex);
                }
                amp.stopMp3();
                currentPlaybackAutomatic = false;
                currentAutomaticSongIndex = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
