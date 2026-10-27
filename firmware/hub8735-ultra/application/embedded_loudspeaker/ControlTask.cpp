#include "controlTask.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "flash_api.h"
}

#include "motion_level.h"
#include "song_catalog.h"
#include "train_plan.h"

namespace {

bool automaticMode = true;
PlayMode playMode = PlayMode::Enjoy;
bool haveTargetBpm = false;
uint16_t targetBpm = 0;
uint8_t activeZone = 0;
int currentSong = -1;
bool automaticSongActive = false;
uint32_t failedSongMask[SONG_MASK_WORDS] = {};
bool initialSelectionAttempted = false;
bool pendingAutoPlay = false;
bool pendingAfterFinish = false;
uint8_t motionLevel = MOTION_LEVEL_NONE;
bool demoPending = false;
// Pause in an automatic mode keeps the mode; selection, DEMO switches and Train time wait until Resume.
bool userPaused = false;
// The automatic song was picked at random because no song had a BPM yet; replaced once one has.
bool randomFill = false;
uint8_t preferredGenre = 0;  // GENRE_CHOICES index, 0 = Any; not saved, kept across standby
uint32_t seenLibraryRevision = 0;
bool settingsSaveAge(uint8_t age);
// DEMO playback uses songIndex DEMO_SONG_BASE - level so its events never match a library index.
constexpr int16_t DEMO_SONG_BASE = -10;
// Train progress: frozen while not in Train (manual, standby), resumed when the same plan is chosen again.
uint8_t trainPlan = 0;
uint32_t trainElapsedMs = 0;
uint32_t trainLastTickMs = 0;
bool trainRunning = false;  // false = not started or finished; the next Train starts over
uint8_t trainStage = TRAIN_STAGE_DONE;
uint16_t trainRemainingS = 0;

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
    case AudioCommandType::CancelBpmAnalysis: return "CancelBpmAnalysis";
    case AudioCommandType::SetSongBpm: return "SetSongBpm";
    case AudioCommandType::ClearAnalysisResults: return "ClearAnalysisResults";
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
    case UiCommandType::CancelBpmAnalysis: return "CancelBpmAnalysis";
    case UiCommandType::PlaySong: return "PlaySong";
    case UiCommandType::SetPpgSource: return "SetPpgSource";
    case UiCommandType::SetImuSource: return "SetImuSource";
    case UiCommandType::PowerOff: return "PowerOff";
    case UiCommandType::PowerOn: return "PowerOn";
    case UiCommandType::RestartTrain: return "RestartTrain";
    case UiCommandType::SetSongBpm: return "SetSongBpm";
    case UiCommandType::ClearAnalysisResults: return "ClearAnalysisResults";
    case UiCommandType::SetAge: return "SetAge";
    case UiCommandType::SetGenre: return "SetGenre";
    }
    return "?";
}

void sendAnalysisSource(AnalysisCmdType type, SensorSource source)
{
    AnalysisCmdElement cmd{};
    cmd.command = type;
    cmd.source = source;
    if (analysisCmdQueue == nullptr || xQueueSend(analysisCmdQueue, &cmd, pdMS_TO_TICKS(20)) != pdPASS) {
        LOGW(TAG, "analysis command was not sent");
    }
}

const char* modeLabel(PlayMode mode)
{
    switch (mode) {
    case PlayMode::Enjoy: return "Enjoy";
    case PlayMode::Train: return "Train";
    case PlayMode::Demo: return "Demo";
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

// DEMO: each motion level plays SD /demo/DEMO_nn.mp3 (outside the library); a failed file stays
// silent until the level changes, a finished one repeats.
void playDemoForLevel()
{
    if (!automaticMode || playMode != PlayMode::Demo) return;
    if (userPaused) {
        demoPending = true;  // the level that holds at Resume plays then
        return;
    }
    if (motionLevel == MOTION_LEVEL_NONE) {
        LOGI(TAG, "DEMO: waiting for motion data");
        return;
    }
    if (musicBpmAnalysisBusy) {
        if (!demoPending) LOGI(TAG, "BPM analysis running; DEMO playback deferred");
        demoPending = true;
        return;
    }
    AudioCmdElement command{};
    command.command = AudioCommandType::Play;
    command.isAutomatic = 1;
    command.songIndex = static_cast<int16_t>(DEMO_SONG_BASE - motionLevel);
    snprintf(command.filename, sizeof(command.filename), "demo/DEMO_%02u.mp3", static_cast<unsigned>(motionLevel));
    demoPending = !sendAudioCommand(command);
    if (!demoPending) LOGI(TAG, "DEMO: level %u -> %s", static_cast<unsigned>(motionLevel), command.filename);
}

void updateMotionLevel(uint8_t level)
{
    if (level >= MOTION_LEVELS && level != MOTION_LEVEL_NONE) {
        LOGW(TAG, "ignored invalid motion level %u", static_cast<unsigned>(level));
        return;
    }
    if (level == motionLevel) return;
    LOGI(TAG, "motion level %d -> %u", motionLevel == MOTION_LEVEL_NONE ? -1 : static_cast<int>(motionLevel),
         static_cast<unsigned>(level));
    motionLevel = level;
    if (uiUpdateQueue != nullptr) {
        UiUpdateElement update{};
        update.type = UiUpdateType::Motion;
        update.timestampMs = millis();
        update.activityLevel = level;
        xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
    }
    playDemoForLevel();
}

// Train songs follow the plan stage (HRmax from age); Enjoy follows the heart-rate target.
uint16_t selectionBpm()
{
    if (playMode != PlayMode::Train) return haveTargetBpm ? targetBpm : 0;
    if (!trainRunning || trainStage == TRAIN_STAGE_DONE) return 0;
    return train_stage_bpm(userInfo.hrmaxBpm(), TRAIN_PLANS[trainPlan].stages[trainStage].zone);
}

void selectSongForTarget(bool afterFinish)
{
    if (playMode == PlayMode::Demo) return;
    const uint16_t wantBpm = selectionBpm();
    if (!automaticMode || wantBpm == 0) {
        LOGD(TAG, "select skipped: automatic=%d mode=%s target=%u", automaticMode ? 1 : 0, modeLabel(playMode),
             static_cast<unsigned>(wantBpm));
        return;
    }
    if (userPaused || musicBpmAnalysisBusy) {
        if (!pendingAutoPlay) LOGI(TAG, "%s; automatic selection deferred", userPaused ? "paused" : "BPM analysis running");
        pendingAutoPlay = true;
        pendingAfterFinish = afterFinish;
        return;
    }

    const char* genre = preferredGenre == 0 ? nullptr : GENRE_CHOICES[preferredGenre];
    int selected = afterFinish
        ? songAfterFinish(wantBpm, currentSong, failedSongMask, genre)
        : songForStart(wantBpm, currentSong, failedSongMask, genre);
    {
        unsigned withBpm = 0, excluded = 0, matching = 0;
        for (size_t i = 0; i < musicLibrary.count; ++i) {
            if (musicLibrary.entries[i].bpm == 0) continue;
            ++withBpm;
            if (songUnavailable(static_cast<int>(i), failedSongMask)) { ++excluded; continue; }
            if (bpmDistance(musicLibrary.entries[i].bpm, wantBpm) * 100u <= static_cast<uint32_t>(wantBpm) * 5u) ++matching;
        }
        LOGD(TAG, "select (%s): target=%u genre=%s current=%d songs=%u withBpm=%u failed-excluded=%u within5%%=%u -> %d",
             afterFinish ? "after finish" : "start/zone change", static_cast<unsigned>(wantBpm),
             GENRE_CHOICES[preferredGenre], currentSong,
             static_cast<unsigned>(musicLibrary.count), withBpm, excluded, matching, selected);
    }
    // No song has a BPM yet: play a random one now; a song that gets a BPM replaces it (controlTask loop).
    const bool random = selected < 0;
    if (random) selected = randomSong(static_cast<uint32_t>(rand()), currentSong, failedSongMask);
    if (selected < 0) {
        pendingAutoPlay = false;
        LOGW(TAG, "no playable song (library=%u); nothing to play",
             static_cast<unsigned>(musicLibrary.count));
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
    command.songIndex = static_cast<int16_t>(selected);
    strncpy(command.filename, musicLibrary.entries[selected].file,
            sizeof(command.filename) - 1);

    if (sendAudioCommand(command)) {
        currentSong = selected;
        automaticSongActive = true;
        pendingAutoPlay = false;
        randomFill = random;
        LOGI(TAG, "auto selected #%d %s (%u BPM) for target %u BPM%s", selected,
             musicLibrary.entries[selected].file, static_cast<unsigned>(musicLibrary.entries[selected].bpm),
             static_cast<unsigned>(wantBpm), random ? " (random: no song has a BPM yet)" : "");
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
    if (uiUpdateQueue != nullptr) {
        UiUpdateElement update{};
        update.type = UiUpdateType::Analysis;
        update.timestampMs = millis();
        update.targetBpm = targetBpm;
        update.zone = activeZone;
        xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
    }

    if (automaticMode && playMode == PlayMode::Enjoy && (!initialSelectionAttempted || zoneChanged)) {
        initialSelectionAttempted = true;
        selectSongForTarget(false);
    }
}

void handlePlaybackEvent(const AudioPlaybackEvent& event)
{
    if (event.songIndex <= DEMO_SONG_BASE) {
        const int level = DEMO_SONG_BASE - event.songIndex;
        if (!automaticMode || playMode != PlayMode::Demo || level != motionLevel) return;  // stale event
        if (event.type == AudioPlaybackEventType::Finished) {
            playDemoForLevel();
        } else {
            LOGW(TAG, "DEMO_%02d.mp3 failed; silent until the motion level changes", level);
        }
        return;
    }
    if (!automaticMode || event.songIndex < 0 ||
        event.songIndex >= static_cast<int>(MUSIC_LIBRARY_MAX)) {
        LOGD(TAG, "playback event ignored (automatic=%d songIndex=%d)", automaticMode ? 1 : 0,
             static_cast<int>(event.songIndex));
        return;
    }
    LOGI(TAG, "playback %s: song #%d%s", event.type == AudioPlaybackEventType::Failed ? "FAILED" : "finished",
         static_cast<int>(event.songIndex), event.songIndex == currentSong ? "" : " (not the current song)");

    if (event.type == AudioPlaybackEventType::Failed) {
        failedSongMask[event.songIndex / 32] |= 1u << (event.songIndex % 32);
        LOGW(TAG, "song #%d marked unplayable", static_cast<int>(event.songIndex));
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
void reportTrain()
{
    if (uiUpdateQueue == nullptr) return;
    UiUpdateElement update{};
    update.type = UiUpdateType::Train;
    update.timestampMs = millis();
    update.trainPlan = trainPlan;
    update.trainStage = trainStage;
    update.trainRemainingS = trainRemainingS;
    xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
}

void enterManualMode();

// Advances the Train plan; a new stage switches the song at once, the end stops playback (= manual stop).
void updateTraining()
{
    if (!automaticMode || playMode != PlayMode::Train || !trainRunning) return;
    const uint32_t now = millis();
    if (userPaused) {
        trainLastTickMs = now;  // the plan is frozen while paused
        return;
    }
    trainElapsedMs += now - trainLastTickMs;
    trainLastTickMs = now;
    uint16_t remaining = 0;
    const uint8_t stage = train_stage_at(TRAIN_PLANS[trainPlan], trainElapsedMs, &remaining);
    if (stage == TRAIN_STAGE_DONE) {
        LOGI(TAG, "Train %s finished; playback stopped", TRAIN_PLANS[trainPlan].name);
        trainRunning = false;
        trainStage = TRAIN_STAGE_DONE;
        trainRemainingS = 0;
        enterManualMode();
        AudioCmdElement stop{};
        stop.command = AudioCommandType::Stop;
        sendAudioCommand(stop);
        reportTrain();
        return;
    }
    const bool stageChanged = stage != trainStage;
    if (!stageChanged && remaining == trainRemainingS) return;
    trainStage = stage;
    trainRemainingS = remaining;
    reportTrain();
    if (stageChanged) {
        LOGI(TAG, "Train stage %u/%u: zone %u, %u s", static_cast<unsigned>(stage + 1),
             static_cast<unsigned>(TRAIN_PLANS[trainPlan].stageCount),
             static_cast<unsigned>(TRAIN_PLANS[trainPlan].stages[stage].zone), static_cast<unsigned>(remaining));
        selectSongForTarget(false);
    }
}

void enterManualMode()
{
    LOGI(TAG, "manual control takes priority; automatic selection stopped");
    setPlayMode(PlayMode::Manual);
    automaticSongActive = false;
    pendingAutoPlay = false;
    demoPending = false;
    userPaused = false;
}

void sendManualCommand(AudioCmdElement& command)
{
    enterManualMode();
    sendAudioCommand(command);  // failure is logged there
}

// Pause / Resume keep an automatic mode (user decision 2026-10-27); only Stop or a manual song leaves it.
// Sensor updates never resume a pause: selection waits in pendingAutoPlay / demoPending until Resume.
void pausePlayback()
{
    AudioCmdElement command{};
    command.command = AudioCommandType::Pause;
    if (!automaticMode) {
        sendManualCommand(command);
        return;
    }
    if (audioPlaybackState != AudioState::Playing) {
        LOGW(TAG, "pause ignored: nothing is playing");
        return;
    }
    if (sendAudioCommand(command)) {
        userPaused = true;
        LOGI(TAG, "paused; %s stays on until Resume", modeLabel(playMode));
    }
}

void resumePlayback()
{
    AudioCmdElement command{};
    command.command = AudioCommandType::Resume;
    if (!automaticMode) {
        sendManualCommand(command);
        return;
    }
    if (!userPaused) {
        LOGW(TAG, "resume ignored: not paused");
        return;
    }
    if (sendAudioCommand(command)) {
        userPaused = false;
        trainLastTickMs = millis();
        LOGI(TAG, "resumed %s", modeLabel(playMode));
    }
}

void enableAutomaticMode(PlayMode mode, uint8_t plan = 0)
{
    const bool leavingDemo = playMode == PlayMode::Demo && mode != PlayMode::Demo;
    setPlayMode(mode);
    userPaused = false;  // a mode chosen while paused starts playing
    memset(failedSongMask, 0, sizeof(failedSongMask));
    initialSelectionAttempted = false;
    if (mode == PlayMode::Train) {
        if (plan >= TRAIN_PLAN_COUNT) plan = 0;
        demoPending = false;
        trainLastTickMs = millis();
        if (!trainRunning || plan != trainPlan) {
            trainPlan = plan;
            trainElapsedMs = 0;
            trainRunning = true;
            trainStage = TRAIN_STAGE_DONE;  // first tick reports stage 0 as new and selects its song
            LOGI(TAG, "Train %s started", TRAIN_PLANS[plan].name);
            updateTraining();
        } else {
            LOGI(TAG, "Train %s resumed at %lu s", TRAIN_PLANS[plan].name,
                 static_cast<unsigned long>(trainElapsedMs / 1000));
            reportTrain();
            selectSongForTarget(false);
        }
        if (leavingDemo && !automaticSongActive) {
            AudioCmdElement stop{};
            stop.command = AudioCommandType::Stop;
            sendAudioCommand(stop);  // nothing selectable: the DEMO track must not keep playing
        }
        return;
    }
    if (mode == PlayMode::Demo) {
        currentSong = -1;
        automaticSongActive = false;
        pendingAutoPlay = false;
        playDemoForLevel();
        return;
    }
    demoPending = false;
    LOGI(TAG, "automatic mode enabled (haveTarget=%d target=%u); failed-song list cleared", haveTargetBpm ? 1 : 0,
         static_cast<unsigned>(targetBpm));
    if (haveTargetBpm) {
        initialSelectionAttempted = true;
        selectSongForTarget(false);
    } else {
        if (leavingDemo) {
            AudioCmdElement stop{};
            stop.command = AudioCommandType::Stop;
            sendAudioCommand(stop);  // the DEMO track must not keep playing in a heart-rate mode
        }
        LOGI(TAG, "automatic mode enabled; waiting for a valid heart rate");
    }
}

void reportBpmAnalysis(BpmAnalysisState state, BpmAnalysisMode mode,
                       uint16_t current = 0, uint16_t total = 0,
                       uint16_t succeeded = 0, uint16_t failed = 0)
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

    // Accepted during playback too: audioTask analyzes between decodes and keeps playback fed first.
    if (!sendAudioCommand(command)) {
        LOGE(TAG, "BPM analysis request could not be sent to audioTask");
        reportBpmAnalysis(BpmAnalysisState::Failed, mode, 0, 0, 0, 1);
        return;
    }
    reportBpmAnalysis(BpmAnalysisState::Queued, mode);
}

// User correction of one song's BPM (OLED x2 / /2 or serial "bpm"). Not a playback command:
// manual/automatic mode stays as it is. audioTask validates 30..300 and saves the library.
void setSongBpm(int index, uint16_t bpm)
{
    if (!musicLibraryReady || index < 0 || index >= static_cast<int>(musicLibrary.count)) {
        LOGW(TAG, "SetSongBpm ignored: invalid index %d", index);
        return;
    }
    AudioCmdElement command{};
    command.command = AudioCommandType::SetSongBpm;
    command.songIndex = static_cast<int16_t>(index);
    command.bpm = bpm;
    strncpy(command.filename, musicLibrary.entries[index].file, sizeof(command.filename) - 1);
    if (!sendAudioCommand(command)) LOGW(TAG, "SetSongBpm was not sent");
}

// "bpm ..." serial commands: list / new / all / cancel / x2 <song> / /2 <song> / set <song> <bpm>.
// <song> is the index printed by "bpm list" or the file name.
void handleBpmSerial(const String& input)
{
    static const char* const USAGE = "serial: usage: bpm list|new|all|cancel|clear|x2 <song>|/2 <song>|set <song> <bpm>";
    String rest = input.substring(3);
    rest.trim();
    const int space = rest.indexOf(' ');
    String verb = space < 0 ? rest : rest.substring(0, space);
    String arg = space < 0 ? String("") : rest.substring(space + 1);
    verb.toLowerCase();
    arg.trim();
    if (verb == "list") {
        LOGI(TAG, "library: %u songs (index file bpm genre)", static_cast<unsigned>(musicLibrary.count));
        for (size_t i = 0; i < musicLibrary.count && i < MUSIC_LIBRARY_MAX; ++i) {
            const MusicEntry& e = musicLibrary.entries[i];
            LOGI(TAG, "  #%u %s %u %s", static_cast<unsigned>(i), e.file, static_cast<unsigned>(e.bpm),
                 e.genre[0] != 0 ? e.genre : "-");
        }
        return;
    }
    if (verb == "new" || verb == "all") {
        startBpmAnalysis(verb == "new" ? BpmAnalysisMode::NewOrUnknown : BpmAnalysisMode::All);
        return;
    }
    if (verb == "cancel" || verb == "clear") {
        AudioCmdElement command{};
        command.command = verb == "cancel" ? AudioCommandType::CancelBpmAnalysis
                                           : AudioCommandType::ClearAnalysisResults;
        if (!sendAudioCommand(command)) LOGW(TAG, "%s analysis command was not sent", verb.c_str());
        return;
    }
    if (verb != "x2" && verb != "/2" && verb != "set") {
        LOGW(TAG, "%s", USAGE);
        return;
    }
    String target = arg;
    long value = 0;
    if (verb == "set") {
        const int last = arg.lastIndexOf(' ');
        if (last < 0) {
            LOGW(TAG, "%s", USAGE);
            return;
        }
        target = arg.substring(0, last);
        target.trim();
        value = arg.substring(last + 1).toInt();
    }
    int index = -1;
    bool digits = target.length() > 0;
    for (unsigned i = 0; i < target.length(); ++i) digits = digits && isdigit(static_cast<unsigned char>(target[i]));
    if (digits) {
        index = target.toInt();
    } else {
        target.toLowerCase();  // library names are stored in lower case
        for (size_t i = 0; i < musicLibrary.count && i < MUSIC_LIBRARY_MAX; ++i)
            if (target == musicLibrary.entries[i].file) index = static_cast<int>(i);
    }
    if (index < 0 || index >= static_cast<int>(musicLibrary.count)) {
        LOGW(TAG, "serial: no song '%s' (see bpm list)", target.c_str());
        return;
    }
    const uint16_t current = musicLibrary.entries[index].bpm;
    if (verb == "x2") value = current * 2;
    else if (verb == "/2") value = (current + 1) / 2;
    if (verb != "set" && current == 0) {
        LOGW(TAG, "serial: #%d has no BPM yet; use bpm set", index);
        return;
    }
    if (value < 30 || value > 300) {
        LOGW(TAG, "serial: BPM %ld is outside 30..300", value);
        return;
    }
    LOGI(TAG, "serial: #%d %s BPM %u -> %ld", index, musicLibrary.entries[index].file, static_cast<unsigned>(current), value);
    setSongBpm(index, static_cast<uint16_t>(value));
}

// Software standby: stop everything that runs; BLETask / analysisTask follow systemStandby.
// playMode is kept for display, but automatic selection stays off until the user picks a mode again.
void enterStandby()
{
    LOGI(TAG, "power off: entering standby (mode %s kept, automatic selection off)", modeLabel(playMode));
    automaticMode = false;
    automaticSongActive = false;
    pendingAutoPlay = false;
    demoPending = false;
    userPaused = false;
    // Another person may wear the sensor after standby: wait for a fresh heart rate (analysisTask resets too).
    haveTargetBpm = false;
    targetBpm = 0;
    activeZone = 0;
    if (uiUpdateQueue != nullptr) {
        UiUpdateElement update{};
        update.type = UiUpdateType::Analysis;
        update.timestampMs = millis();
        xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
    }
    AudioCmdElement command{};
    command.command = AudioCommandType::CancelBpmAnalysis;
    sendAudioCommand(command);
    command.command = AudioCommandType::Stop;
    sendAudioCommand(command);  // the amps go back to shutdown once playback stops
    systemStandby = true;
}

void handleUiCommand(const UiCommandElement& cmd)
{
    AudioCmdElement command{};
    if (systemStandby && cmd.command != UiCommandType::PowerOn) {
        LOGW(TAG, "UI command %s ignored in standby", uiCommandName(cmd.command));
        return;
    }
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
            // After standby playMode is kept with automaticMode off, so test the mode itself.
            if (playMode != PlayMode::Manual) enterManualMode();
        } else if (automaticMode && cmd.mode == PlayMode::Enjoy && playMode != PlayMode::Demo) {
            setPlayMode(cmd.mode);  // Train -> Enjoy keeps the current song
        } else if (automaticMode && cmd.mode == PlayMode::Train && playMode == PlayMode::Train &&
                   cmd.trainPlan == trainPlan) {
            LOGD(TAG, "Train %s already running", TRAIN_PLANS[trainPlan].name);
        } else {
            enableAutomaticMode(cmd.mode, cmd.trainPlan);
        }
        break;
    case UiCommandType::Pause:
        pausePlayback();
        break;
    case UiCommandType::Resume:
        resumePlayback();
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
    case UiCommandType::CancelBpmAnalysis:
        command.command = AudioCommandType::CancelBpmAnalysis;
        if (!sendAudioCommand(command)) LOGW(TAG, "cancel analysis command was not sent");
        break;
    case UiCommandType::ClearAnalysisResults:
        command.command = AudioCommandType::ClearAnalysisResults;
        if (!sendAudioCommand(command)) LOGW(TAG, "clear analysis results command was not sent");
        break;
    case UiCommandType::SetPpgSource:
        sendAnalysisSource(AnalysisCmdType::SetPpgSource, cmd.source);
        break;
    case UiCommandType::SetImuSource:
        sendAnalysisSource(AnalysisCmdType::SetMotionSource, cmd.source);
        break;
    case UiCommandType::PowerOff:
        enterStandby();
        break;
    case UiCommandType::RestartTrain:
        if (playMode != PlayMode::Train) break;  // also after standby: an explicit user restart
        trainRunning = false;  // enableAutomaticMode starts the plan over
        enableAutomaticMode(PlayMode::Train, trainPlan);
        break;
    case UiCommandType::SetSongBpm:
        setSongBpm(cmd.songIndex, cmd.bpm);
        break;
    case UiCommandType::PowerOn:
        if (!systemStandby) break;
        systemStandby = false;
        // Wake = boot: Enjoy, playing once a fresh valid heart rate arrives (user decision 2026-10-27).
        LOGI(TAG, "power on: Enjoy, waiting for a valid heart rate");
        enableAutomaticMode(PlayMode::Enjoy);
        break;
    case UiCommandType::SetAge:
        if (cmd.age < USER_AGE_MIN || cmd.age > USER_AGE_MAX) {
            LOGW(TAG, "SetAge ignored: %u outside %u..%u", static_cast<unsigned>(cmd.age),
                 static_cast<unsigned>(USER_AGE_MIN), static_cast<unsigned>(USER_AGE_MAX));
            break;
        }
        if (cmd.age == userInfo.age) break;
        userInfo.age = cmd.age;  // analysisTask and Train pick up the new HRmax on their next update
        if (settingsSaveAge(cmd.age)) {
            LOGI(TAG, "age %u saved (HRmax %u)", static_cast<unsigned>(cmd.age), static_cast<unsigned>(userInfo.hrmaxBpm()));
        } else {
            LOGE(TAG, "age %u applied but not saved to flash", static_cast<unsigned>(cmd.age));
        }
        break;
    case UiCommandType::SetGenre:
        if (cmd.genre >= GENRE_CHOICE_COUNT) {
            LOGW(TAG, "SetGenre ignored: %u", static_cast<unsigned>(cmd.genre));
            break;
        }
        preferredGenre = cmd.genre;  // the current song keeps playing; the next selection uses it
        LOGI(TAG, "preferred genre: %s", GENRE_CHOICES[preferredGenre]);
        break;
    }
}
const char* sourceLabel(SensorSource source)
{
    return source == SensorSource::Chest ? "chest" : source == SensorSource::Watch ? "watch" : "local";
}

// "status": snapshot of controlTask state for the PC console (apps/hub_serial_console).
void reportStatus()
{
    LOGI(TAG, "status: mode=%s automatic=%d paused=%d random=%d age=%d target=%u zone=%u motion=%u ppg=%s imu=%s "
         "songs=%u analysis=%d heap=%u",
         modeLabel(playMode), automaticMode ? 1 : 0, userPaused ? 1 : 0, randomFill ? 1 : 0, userInfo.age,
         haveTargetBpm ? static_cast<unsigned>(targetBpm) : 0u,
         static_cast<unsigned>(activeZone), static_cast<unsigned>(motionLevel), sourceLabel(activePpgSource),
         sourceLabel(activeImuSource), static_cast<unsigned>(musicLibrary.count), musicBpmAnalysisBusy ? 1 : 0,
         static_cast<unsigned>(xPortGetFreeHeapSize()));
    if (playMode == PlayMode::Train) {
        LOGI(TAG, "status: train plan=%u stage=%u remaining=%us running=%d", static_cast<unsigned>(trainPlan + 1),
             static_cast<unsigned>(trainStage), static_cast<unsigned>(trainRemainingS), trainRunning ? 1 : 0);
    }
}

// "ble connect|disconnect|auto|manual chest|watch": same commands as the OLED BLE menu.
void handleBleSerial(const String& input)
{
    BleCmdElement cmd{};
    if (input.startsWith("ble connect ")) cmd.command = BleCommandType::Connect;
    else if (input.startsWith("ble disconnect ")) cmd.command = BleCommandType::Disconnect;
    else if (input.startsWith("ble auto ")) cmd.command = BleCommandType::SetAuto;
    else if (input.startsWith("ble manual ")) cmd.command = BleCommandType::SetManual;
    else {
        LOGW(TAG, "serial: usage: ble connect|disconnect|auto|manual chest|watch");
        return;
    }
    if (input.endsWith(" chest")) cmd.device = SensorSource::Chest;
    else if (input.endsWith(" watch")) cmd.device = SensorSource::Watch;
    else {
        LOGW(TAG, "serial: usage: ble connect|disconnect|auto|manual chest|watch");
        return;
    }
    if (bleCmdQueue == nullptr || xQueueSend(bleCmdQueue, &cmd, pdMS_TO_TICKS(20)) != pdPASS) {
        LOGW(TAG, "BLE command was not sent");
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
    } else if (input.equalsIgnoreCase("demo")) {
        enableAutomaticMode(PlayMode::Demo);
    } else if (input.equalsIgnoreCase("train") || input.startsWith("train ")) {
        // train [1|2]: built-in plan number, default 1
        const long plan = input.length() > 6 ? input.substring(6).toInt() : 1;
        if (plan < 1 || plan > TRAIN_PLAN_COUNT) {
            LOGW(TAG, "serial: usage: train [1..%u]", static_cast<unsigned>(TRAIN_PLAN_COUNT));
            return;
        }
        enableAutomaticMode(PlayMode::Train, static_cast<uint8_t>(plan - 1));
    } else if (input.startsWith("src ")) {
        // src ppg|motion chest|watch|local
        const bool ppg = input.startsWith("src ppg ");
        const bool motion = input.startsWith("src motion ");
        SensorSource source = SensorSource::Chest;
        bool valid = true;
        if (input.endsWith(" chest")) source = SensorSource::Chest;
        else if (input.endsWith(" watch")) source = SensorSource::Watch;
        else if (input.endsWith(" local")) source = SensorSource::Local;
        else valid = false;
        if ((!ppg && !motion) || !valid) {
            LOGW(TAG, "serial: usage: src ppg|motion chest|watch|local");
            return;
        }
        sendAnalysisSource(ppg ? AnalysisCmdType::SetPpgSource : AnalysisCmdType::SetMotionSource, source);
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
        pausePlayback();
    } else if (input.equalsIgnoreCase("resume")) {
        resumePlayback();
    } else if (input.equalsIgnoreCase("bpm") || input.startsWith("bpm ")) {
        handleBpmSerial(input);
    } else if (input.equalsIgnoreCase("status")) {
        reportStatus();
    } else if (input.startsWith("ble ")) {
        handleBleSerial(input);
    } else if (input.equalsIgnoreCase("help")) {
        LOGI(TAG, "commands: status | help | auto | demo | train [1|2] | play <file> | pause | resume | stop | volume <0-100>");
        LOGI(TAG, "  src ppg|motion chest|watch|local | ble connect|disconnect|auto|manual chest|watch");
        LOGI(TAG, "  bpm list|new|all|cancel|clear|x2 <song>|/2 <song>|set <song> <bpm>");
    } else {
        LOGW(TAG, "serial: unknown command '%s'", input.c_str());
    }
}

// Age word in the FlashMemory library's app area (last 192 KB of the 16 MB NOR): magic in the top
// 24 bits, age in the low byte. Not FlashMemory itself: its global object mallocs the whole 192 KB.
constexpr uint32_t SETTINGS_FLASH_ADDR = 0xFD0000;
constexpr uint32_t SETTINGS_MAGIC = 0x49544100;  // "ITA"
flash_t settingsFlash;

// shortcut: the sector erase can stall flash-resident code for tens of ms and may click the audio;
// only on an age change, move to a RAM-buffered write if it is heard.
bool settingsSaveAge(uint8_t age)
{
    const uint32_t word = SETTINGS_MAGIC | age;
    flash_erase_sector(&settingsFlash, SETTINGS_FLASH_ADDR);
    flash_write_word(&settingsFlash, SETTINGS_FLASH_ADDR, word);
    uint32_t check = 0;
    flash_read_word(&settingsFlash, SETTINGS_FLASH_ADDR, &check);
    return check == word;
}

} // namespace

int settingsLoadAge()
{
    uint32_t word = 0;
    flash_read_word(&settingsFlash, SETTINGS_FLASH_ADDR, &word);
    const uint8_t age = static_cast<uint8_t>(word & 0xFF);
    if ((word & 0xFFFFFF00u) == SETTINGS_MAGIC && age >= USER_AGE_MIN && age <= USER_AGE_MAX) return age;
    return USER_AGE_DEFAULT;  // erased flash (0xFFFFFFFF) or never saved
}

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
    srand(micros());  // SD and boot-scan timing vary, so the random fallback song differs per boot
    for (;;) {
        TargetBpmResult result{};
        while (targetBpmResultQueue != nullptr &&
               xQueueReceive(targetBpmResultQueue, &result, 0) == pdPASS) {
            LOGD(TAG, "<- analysis: target BPM=%u zone=%u", static_cast<unsigned>(result.targetBpm),
                 static_cast<unsigned>(result.zone));
            updateTarget(result);
        }

        uint8_t level = 0;
        if (motionLevelQueue != nullptr && xQueueReceive(motionLevelQueue, &level, 0) == pdPASS) {
            updateMotionLevel(level);
        }

        AudioPlaybackEvent event{};
        while (audioPlaybackEventQueue != nullptr &&
               xQueueReceive(audioPlaybackEventQueue, &event, 0) == pdPASS) {
            handlePlaybackEvent(event);
        }

        UiCommandElement uiCommand{};
        while (uiCmdQueue != nullptr &&
               xQueueReceive(uiCmdQueue, &uiCommand, 0) == pdPASS) {
            LOGI(TAG, "OKT 2 control got %s", uiCommandName(uiCommand.command));
            handleUiCommand(uiCommand);
        }

        if (Serial.available()) {
            const String input = Serial.readStringUntil('\n');
            if (systemStandby) LOGW(TAG, "serial command ignored in standby");
            else handleSerialCommand(input);
        }

        // A song got a BPM while a random one plays: switch to a BPM match now (user decision 2026-10-27).
        if (musicLibraryRevision != seenLibraryRevision) {
            seenLibraryRevision = musicLibraryRevision;
            const uint16_t want = selectionBpm();
            if (automaticMode && randomFill && automaticSongActive && !userPaused && want != 0 &&
                songForStart(want, currentSong, failedSongMask) >= 0) {
                LOGI(TAG, "a song has a BPM now; replacing the random song");
                automaticSongActive = false;
                selectSongForTarget(false);
            }
        }
        if (pendingAutoPlay && automaticMode && !userPaused) {
            if (!musicBpmAnalysisBusy) LOGD(TAG, "retrying deferred automatic selection");
            selectSongForTarget(pendingAfterFinish);
        }
        if (demoPending && !musicBpmAnalysisBusy) playDemoForLevel();
        updateTraining();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
