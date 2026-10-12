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
uint32_t failedSongMask = 0;
bool initialSelectionAttempted = false;
bool pendingAutoPlay = false;
bool pendingAfterFinish = false;

constexpr const char* TAG = "CONTROL";

const char* audioCommandName(AudioCommandType type)
{
    switch (type) {
    case AudioCommandType::Play: return "Play";
    case AudioCommandType::Stop: return "Stop";
    case AudioCommandType::SetVolume: return "SetVolume";
    case AudioCommandType::Pause: return "Pause";
    case AudioCommandType::Resume: return "Resume";
    case AudioCommandType::AnalyzeNewOrUnknownBpm: return "AnalyzeNewOrUnknownBpm";
    case AudioCommandType::ReanalyzeAllBpm: return "ReanalyzeAllBpm";
    }
    return "?";
}

const char* uiCommandName(UiCommandType type)
{
    switch (type) {
    case UiCommandType::SetMode: return "SetMode";
    case UiCommandType::Pause: return "Pause";
    case UiCommandType::Resume: return "Resume";
    case UiCommandType::Stop: return "Stop";
    case UiCommandType::SetVolume: return "SetVolume";
    case UiCommandType::AnalyzeNewOrUnknownBpm: return "AnalyzeNewOrUnknownBpm";
    case UiCommandType::ReanalyzeAllBpm: return "ReanalyzeAllBpm";
    case UiCommandType::PlaySong: return "PlaySong";
    }
    return "?";
}

const char* modeLabel(PlayMode mode)
{
    switch (mode) {
    case PlayMode::Enjoy: return "Enjoy";
    case PlayMode::Train: return "Train";
    default: return "Manual";
    }
}

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
    LOGI(TAG, "play mode %s -> %s (automatic=%d)", modeLabel(playMode), modeLabel(mode),
         mode != PlayMode::Manual ? 1 : 0);
    playMode = mode;
    automaticMode = mode != PlayMode::Manual;
    reportMode();
}

bool sendAudioCommand(const AudioCmdElement& command)
{
    if (audioCmdQueue == nullptr ||
        xQueueSend(audioCmdQueue, &command, pdMS_TO_TICKS(20)) != pdPASS) {
        LOGW(TAG, "audio command queue full; %s not sent", audioCommandName(command.command));
        return false;
    }

    if (command.command == AudioCommandType::Play) {
        LOGD(TAG, "-> audio: Play file=%s automatic=%d songIndex=%d", command.filename,
             command.isAutomatic ? 1 : 0, static_cast<int>(command.songIndex));
    } else if (command.command == AudioCommandType::SetVolume) {
        LOGD(TAG, "-> audio: SetVolume %u%%", static_cast<unsigned>(command.volumePercent));
    } else {
        LOGD(TAG, "-> audio: %s", audioCommandName(command.command));
    }
    return true;
}

void selectSongForTarget(bool afterFinish)
{
    if (!automaticMode || !haveTargetBpm || targetBpm == 0) {
        LOGD(TAG, "select skipped: automatic=%d haveTarget=%d target=%u", automaticMode ? 1 : 0,
             haveTargetBpm ? 1 : 0, static_cast<unsigned>(targetBpm));
        return;
    }
    if (musicBpmAnalysisBusy) {
        if (!pendingAutoPlay) LOGI(TAG, "BPM analysis running; automatic selection deferred");
        pendingAutoPlay = true;
        pendingAfterFinish = afterFinish;
        return;
    }

    const int selected = afterFinish
        ? songAfterFinish(targetBpm, currentSong, failedSongMask)
        : songForStart(targetBpm, currentSong, failedSongMask);
    {
        unsigned withBpm = 0, excluded = 0, matching = 0;
        for (size_t i = 0; i < musicLibrary.count; ++i) {
            if (musicLibrary.entries[i].bpm == 0) continue;
            ++withBpm;
            if (songUnavailable(static_cast<int>(i), failedSongMask)) { ++excluded; continue; }
            if (bpmDistance(musicLibrary.entries[i].bpm, targetBpm) * 100u <= static_cast<uint32_t>(targetBpm) * 5u) ++matching;
        }
        LOGD(TAG, "select (%s): target=%u current=%d songs=%u withBpm=%u failed-excluded=%u within5%%=%u -> %d",
             afterFinish ? "after finish" : "start/zone change", static_cast<unsigned>(targetBpm), currentSong,
             static_cast<unsigned>(musicLibrary.count), withBpm, excluded, matching, selected);
    }
    if (selected < 0) {
        pendingAutoPlay = false;
        LOGW(TAG, "no playable song (library=%u, failed mask=0x%08lX); nothing to play",
             static_cast<unsigned>(musicLibrary.count), static_cast<unsigned long>(failedSongMask));
        return;
    }
    if (!afterFinish && automaticSongActive && selected == currentSong) {
        pendingAutoPlay = false;
        LOGD(TAG, "selected song %d is already playing; keep", selected);
        return;
    }

    AudioCmdElement command{};
    command.command = AudioCommandType::Play;
    command.isAutomatic = 1;
    command.songIndex = static_cast<int8_t>(selected);
    strncpy(command.filename, musicLibrary.entries[selected].file,
            sizeof(command.filename) - 1);

    if (sendAudioCommand(command)) {
        currentSong = selected;
        automaticSongActive = true;
        pendingAutoPlay = false;
        LOGI(TAG, "auto selected #%d %s (%u BPM) for target %u BPM", selected,
             musicLibrary.entries[selected].file, static_cast<unsigned>(musicLibrary.entries[selected].bpm),
             static_cast<unsigned>(targetBpm));
    } else {
        pendingAutoPlay = true;
        pendingAfterFinish = afterFinish;
        LOGW(TAG, "will retry automatic selection");
    }
}

void updateTarget(const TargetBpmResult& result)
{
    if (result.targetBpm == 0 || result.zone < 1 || result.zone > 5) {
        LOGW(TAG, "ignored invalid target result: bpm=%u zone=%u", static_cast<unsigned>(result.targetBpm),
             static_cast<unsigned>(result.zone));
        return;
    }

    const bool zoneChanged = activeZone != 0 && activeZone != result.zone;
    if (result.targetBpm != targetBpm || result.zone != activeZone) {
        LOGI(TAG, "target BPM %u -> %u, zone %u -> %u%s", static_cast<unsigned>(targetBpm),
             static_cast<unsigned>(result.targetBpm), static_cast<unsigned>(activeZone),
             static_cast<unsigned>(result.zone), zoneChanged ? " (zone changed)" : "");
    }
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
        event.songIndex >= static_cast<int>(MUSIC_LIBRARY_MAX)) {
        LOGD(TAG, "playback event ignored (automatic=%d songIndex=%d)", automaticMode ? 1 : 0,
             static_cast<int>(event.songIndex));
        return;
    }
    LOGI(TAG, "playback %s: song #%d%s", event.type == AudioPlaybackEventType::Failed ? "FAILED" : "finished",
         static_cast<int>(event.songIndex), event.songIndex == currentSong ? "" : " (not the current song)");

    if (event.type == AudioPlaybackEventType::Failed) {
        failedSongMask |= (1u << event.songIndex);
        LOGW(TAG, "song #%d marked unplayable (failed mask=0x%08lX)", static_cast<int>(event.songIndex),
             static_cast<unsigned long>(failedSongMask));
    }
    if (musicBpmAnalysisBusy) {
        if (event.songIndex == currentSong) {
            automaticSongActive = false;
            pendingAutoPlay = true;
            pendingAfterFinish = event.type == AudioPlaybackEventType::Finished;
        }
        return;
    }
    if (event.songIndex >= static_cast<int>(musicLibrary.count)) return;

    if (event.type == AudioPlaybackEventType::Failed) {
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
    LOGI(TAG, "manual control takes priority; automatic selection stopped");
    setPlayMode(PlayMode::Manual);
    automaticSongActive = false;
    pendingAutoPlay = false;
}

void sendManualCommand(AudioCmdElement& command)
{
    enterManualMode();
    sendAudioCommand(command);  // failure is logged there
}

void enableAutomaticMode(PlayMode mode)
{
    setPlayMode(mode);
    LOGI(TAG, "automatic mode enabled (haveTarget=%d target=%u); failed-song list cleared", haveTargetBpm ? 1 : 0,
         static_cast<unsigned>(targetBpm));
    failedSongMask = 0;
    initialSelectionAttempted = false;
    if (haveTargetBpm) {
        initialSelectionAttempted = true;
        selectSongForTarget(false);
    } else {
        LOGI(TAG, "automatic mode enabled; waiting for a valid heart rate");
    }
}

void reportBpmAnalysis(BpmAnalysisState state, BpmAnalysisMode mode,
                       uint8_t current = 0, uint8_t total = 0,
                       uint8_t succeeded = 0, uint8_t failed = 0)
{
    if (uiUpdateQueue == nullptr) return;
    UiUpdateElement update{};
    update.type = UiUpdateType::BpmAnalysis;
    update.timestampMs = millis();
    update.bpmAnalysisState = state;
    update.bpmAnalysisMode = mode;
    update.bpmAnalysisCurrent = current;
    update.bpmAnalysisTotal = total;
    update.bpmAnalysisSucceeded = succeeded;
    update.bpmAnalysisFailed = failed;
    xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
}

void startBpmAnalysis(BpmAnalysisMode mode)
{
    LOGI(TAG, "BPM analysis requested: %s", mode == BpmAnalysisMode::NewOrUnknown ? "new/unknown" : "all");
    if (!musicStorageReady) {
        LOGW(TAG, "BPM analysis rejected: SD storage not ready");
        reportBpmAnalysis(BpmAnalysisState::Failed, mode, 0, 0, 0, 1);
        return;
    }

    AudioCmdElement command{};
    command.command = mode == BpmAnalysisMode::NewOrUnknown
        ? AudioCommandType::AnalyzeNewOrUnknownBpm
        : AudioCommandType::ReanalyzeAllBpm;

    // audioTask raises musicBpmAnalysisBusy only while a scan really runs (never during playback).
    if (!sendAudioCommand(command)) {
        LOGE(TAG, "BPM analysis request could not be sent to audioTask");
        reportBpmAnalysis(BpmAnalysisState::Failed, mode, 0, 0, 0, 1);
        return;
    }
    reportBpmAnalysis(BpmAnalysisState::Queued, mode);
}

void handleUiCommand(const UiCommandElement& cmd)
{
    AudioCmdElement command{};
    if (cmd.command == UiCommandType::SetMode) {
        LOGI(TAG, "UI command: SetMode %s", modeLabel(cmd.mode));
    } else if (cmd.command == UiCommandType::SetVolume) {
        LOGI(TAG, "UI command: SetVolume %u%%", static_cast<unsigned>(cmd.volumePercent));
    } else {
        LOGI(TAG, "UI command: %s", uiCommandName(cmd.command));
    }
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
            LOGW(TAG, "volume command was not sent");
        }
        break;
    case UiCommandType::PlaySong:
        if (!musicLibraryReady || cmd.songIndex < 0 ||
            cmd.songIndex >= static_cast<int>(musicLibrary.count)) {
            LOGW(TAG, "PlaySong ignored: invalid index %d", static_cast<int>(cmd.songIndex));
            break;
        }
        command.command = AudioCommandType::Play;
        command.songIndex = cmd.songIndex;
        strncpy(command.filename, musicLibrary.entries[cmd.songIndex].file, sizeof(command.filename) - 1);
        currentSong = cmd.songIndex;
        sendManualCommand(command);
        break;
    case UiCommandType::AnalyzeNewOrUnknownBpm:
        startBpmAnalysis(BpmAnalysisMode::NewOrUnknown);
        break;
    case UiCommandType::ReanalyzeAllBpm:
        startBpmAnalysis(BpmAnalysisMode::All);
        break;
    }
}
void handleSerialCommand(String input)
{
    input.trim();
    if (input.length() == 0) return;
    LOGI(TAG, "serial command: '%s'", input.c_str());

    AudioCmdElement command{};
    if (input.equalsIgnoreCase("auto")) {
        enableAutomaticMode(PlayMode::Enjoy);
    } else if (input.startsWith("play ")) {
        String filename = input.substring(5);
        filename.trim();
        if (filename.length() == 0 ||
            filename.length() >= sizeof(command.filename)) {
            LOGW(TAG, "serial: invalid audio filename (length %u)", static_cast<unsigned>(filename.length()));
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
            LOGW(TAG, "volume command was not sent");
        }
    } else if (input.equalsIgnoreCase("pause")) {
        command.command = AudioCommandType::Pause;
        sendManualCommand(command);
    } else if (input.equalsIgnoreCase("resume")) {
        command.command = AudioCommandType::Resume;
        sendManualCommand(command);
    } else {
        LOGW(TAG, "serial: unknown command '%s'", input.c_str());
    }
}

} // namespace

void controlTask(void* parameter)
{
    (void)parameter;

    LOGI(TAG, "started; waiting for music library");
    reportMode();
    while (!musicLibraryReady) {  // 佇列會先緩衝，載入完成後再處理
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    LOGI(TAG, "music library ready: %u songs, mode=%s", static_cast<unsigned>(musicLibrary.count),
         modeLabel(playMode));
    for (;;) {
        TargetBpmResult result{};
        while (targetBpmResultQueue != nullptr &&
               xQueueReceive(targetBpmResultQueue, &result, 0) == pdPASS) {
            LOGD(TAG, "<- analysis: target BPM=%u zone=%u", static_cast<unsigned>(result.targetBpm),
                 static_cast<unsigned>(result.zone));
            updateTarget(result);
        }

        AudioPlaybackEvent event{};
        while (audioPlaybackEventQueue != nullptr &&
               xQueueReceive(audioPlaybackEventQueue, &event, 0) == pdPASS) {
            handlePlaybackEvent(event);
        }

        UiCommandElement uiCommand{};
        while (uiCmdQueue != nullptr &&
               xQueueReceive(uiCmdQueue, &uiCommand, 0) == pdPASS) {
            handleUiCommand(uiCommand);
        }

        if (Serial.available()) {
            handleSerialCommand(Serial.readStringUntil('\n'));
        }

        if (pendingAutoPlay && automaticMode) {
            if (!musicBpmAnalysisBusy) LOGD(TAG, "retrying deferred automatic selection");
            selectSongForTarget(pendingAfterFinish);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
