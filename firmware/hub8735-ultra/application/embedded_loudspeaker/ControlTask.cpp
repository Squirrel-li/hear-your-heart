#include "controlTask.h"

#include <cstring>

#include "song_catalog.h"

namespace {

bool automaticMode = true;
PlayMode playMode = PlayMode::Enjoy;
bool haveTargetBpm = false;
uint16_t targetBpm = 0;
uint8_t activeZone = 0;
int currentSong = -1;
bool automaticSongActive = false;
uint16_t failedSongMask = 0;
bool initialSelectionAttempted = false;
bool pendingAutoPlay = false;
bool pendingAfterFinish = false;

void reportMode()
{
    if (uiUpdateQueue == nullptr) return;
    UiUpdateElement update{};
    update.type = UiUpdateType::Mode;
    update.timestampMs = millis();
    update.mode = static_cast<uint8_t>(playMode);
    xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
}

void setPlayMode(PlayMode mode)
{
    playMode = mode;
    automaticMode = mode != PlayMode::Manual;
    reportMode();
}

bool sendAudioCommand(const AudioCmdElement& command)
{
    if (audioCmdQueue == nullptr ||
        xQueueSend(audioCmdQueue, &command, pdMS_TO_TICKS(20)) != pdPASS) {
        return false;
    }

    DEBUG_PRINT("controlTask -> audioTask: command=");
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
    return true;
}

void selectSongForTarget(bool afterFinish)
{
    if (!automaticMode || !haveTargetBpm || targetBpm == 0) return;

    const int selected = afterFinish
        ? songAfterFinish(targetBpm, currentSong, failedSongMask)
        : songForStart(targetBpm, currentSong, failedSongMask);
    if (selected < 0) {
        pendingAutoPlay = false;
        DEBUG_PRINTLN("No playable song matches the target BPM");
        return;
    }
    if (!afterFinish && automaticSongActive && selected == currentSong) {
        pendingAutoPlay = false;
        return;
    }

    AudioCmdElement command{};
    command.command = AudioCommandType::Play;
    command.isAutomatic = 1;
    command.songIndex = static_cast<int8_t>(selected);
    strncpy(command.filename, songs[selected].filename,
            sizeof(command.filename) - 1);

    if (sendAudioCommand(command)) {
        currentSong = selected;
        automaticSongActive = true;
        pendingAutoPlay = false;
        DEBUG_PRINT("Auto selected: ");
        DEBUG_PRINT(songs[selected].filename);
        DEBUG_PRINT(" (");
        DEBUG_PRINT(songs[selected].bpm);
        DEBUG_PRINTLN(" BPM)");
    } else {
        pendingAutoPlay = true;
        pendingAfterFinish = afterFinish;
        DEBUG_PRINTLN("Audio queue full; will retry automatic selection");
    }
}

void updateTarget(const TargetBpmResult& result)
{
    if (result.targetBpm == 0 || result.zone < 1 || result.zone > 5) return;

    const bool zoneChanged = activeZone != 0 && activeZone != result.zone;
    targetBpm = result.targetBpm;
    activeZone = result.zone;
    haveTargetBpm = true;

    if (automaticMode && (!initialSelectionAttempted || zoneChanged)) {
        initialSelectionAttempted = true;
        selectSongForTarget(false);
    }
}

void handlePlaybackEvent(const AudioPlaybackEvent& event)
{
    if (!automaticMode || event.songIndex < 0 ||
        event.songIndex >= songCount) {
        return;
    }

    if (event.type == AudioPlaybackEventType::Failed) {
        failedSongMask |= static_cast<uint16_t>(1u << event.songIndex);
        if (event.songIndex == currentSong) {
            automaticSongActive = false;
            selectSongForTarget(false);
        }
    } else if (event.type == AudioPlaybackEventType::Finished &&
               event.songIndex == currentSong) {
        automaticSongActive = false;
        selectSongForTarget(true);
    }
}

void enterManualMode()
{
    setPlayMode(PlayMode::Manual);
    automaticSongActive = false;
    pendingAutoPlay = false;
}

void sendManualCommand(AudioCmdElement& command)
{
    enterManualMode();
    if (!sendAudioCommand(command)) {
        DEBUG_PRINTLN("Audio queue full; command was not sent");
    }
}

void enableAutomaticMode(PlayMode mode)
{
    setPlayMode(mode);
    failedSongMask = 0;
    initialSelectionAttempted = false;
    if (haveTargetBpm) {
        initialSelectionAttempted = true;
        selectSongForTarget(false);
    } else {
        DEBUG_PRINTLN("Automatic mode enabled; waiting for valid heart rate");
    }
}

void handleUiCommand(const UiCommandElement& cmd)
{
    AudioCmdElement command{};
    switch (cmd.command) {
    case UiCommandType::SetMode:
        if (cmd.mode == PlayMode::Manual) {
            if (automaticMode) enterManualMode();
        } else if (automaticMode) {
            setPlayMode(cmd.mode);  // Enjoy <-> Train keeps the current song
        } else {
            enableAutomaticMode(cmd.mode);
        }
        break;
    case UiCommandType::Pause:
        command.command = AudioCommandType::Pause;
        sendManualCommand(command);
        break;
    case UiCommandType::Resume:
        command.command = AudioCommandType::Resume;
        sendManualCommand(command);
        break;
    case UiCommandType::Stop:
        command.command = AudioCommandType::Stop;
        sendManualCommand(command);
        break;
    case UiCommandType::SetVolume:
        command.command = AudioCommandType::SetVolume;
        command.volumePercent = cmd.volumePercent > 100 ? 100 : cmd.volumePercent;
        if (!sendAudioCommand(command)) {
            DEBUG_PRINTLN("Audio queue full; volume command was not sent");
        }
        break;
    }
}

void handleSerialCommand(String input)
{
    input.trim();
    if (input.length() == 0) return;

    AudioCmdElement command{};
    if (input.equalsIgnoreCase("auto")) {
        enableAutomaticMode(PlayMode::Enjoy);
    } else if (input.startsWith("play ")) {
        String filename = input.substring(5);
        filename.trim();
        if (filename.length() == 0 ||
            filename.length() >= sizeof(command.filename)) {
            DEBUG_PRINTLN("Invalid audio filename");
            return;
        }
        command.command = AudioCommandType::Play;
        strncpy(command.filename, filename.c_str(), sizeof(command.filename) - 1);
        sendManualCommand(command);
    } else if (input.equalsIgnoreCase("stop")) {
        command.command = AudioCommandType::Stop;
        sendManualCommand(command);
    } else if (input.startsWith("volume ")) {
        int volume = input.substring(7).toInt();
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        command.command = AudioCommandType::SetVolume;
        command.volumePercent = static_cast<uint8_t>(volume);
        if (!sendAudioCommand(command)) {
            DEBUG_PRINTLN("Audio queue full; volume command was not sent");
        }
    } else if (input.equalsIgnoreCase("pause")) {
        command.command = AudioCommandType::Pause;
        sendManualCommand(command);
    } else if (input.equalsIgnoreCase("resume")) {
        command.command = AudioCommandType::Resume;
        sendManualCommand(command);
    } else {
        DEBUG_PRINTLN("Unknown command");
    }
}

} // namespace

void controlTask(void* parameter)
{
    (void)parameter;

    reportMode();
    for (;;) {
        TargetBpmResult result{};
        while (targetBpmResultQueue != nullptr &&
               xQueueReceive(targetBpmResultQueue, &result, 0) == pdPASS) {
            DEBUG_PRINT("analysisTask -> controlTask [recv]: target BPM=");
            DEBUG_PRINT(result.targetBpm);
            DEBUG_PRINT(" zone=");
            DEBUG_PRINTLN(result.zone);
            updateTarget(result);
        }

        AudioPlaybackEvent event{};
        while (audioPlaybackEventQueue != nullptr &&
               xQueueReceive(audioPlaybackEventQueue, &event, 0) == pdPASS) {
            DEBUG_PRINT("audioTask -> controlTask [recv]: playback event=");
            DEBUG_PRINT(static_cast<int>(event.type));
            DEBUG_PRINT(" songIndex=");
            DEBUG_PRINTLN(static_cast<int>(event.songIndex));
            handlePlaybackEvent(event);
        }

        UiCommandElement uiCommand{};
        while (uiCmdQueue != nullptr &&
               xQueueReceive(uiCmdQueue, &uiCommand, 0) == pdPASS) {
            DEBUG_PRINT("UITask -> controlTask [recv]: menu command=");
            DEBUG_PRINTLN(static_cast<int>(uiCommand.command));
            handleUiCommand(uiCommand);
        }

        if (Serial.available()) {
            handleSerialCommand(Serial.readStringUntil('\n'));
        }

        if (pendingAutoPlay && automaticMode) {
            selectSongForTarget(pendingAfterFinish);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
