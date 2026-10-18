#include "UITask.h"

#include "debug.h"
#include "globalData.h"
#include "music_library.h"
#include "oled_sh1106.h"
#include "motion_level.h"
#include "train_plan.h"

namespace {

constexpr const char* TAG = "UI";
uint32_t droppedCommands = 0;

// OLED on Wire (I2C0: SDA=D3/PE4, SCL=D4/PE3).
constexpr uint8_t OLED_ADDRESS = 0x3C;

// Buttons wired between pin and GND (internal pull-up, pressed = LOW).
constexpr uint8_t PIN_UP = 8;
constexpr uint8_t PIN_DOWN = 9;
constexpr uint8_t PIN_OK = 10;
constexpr uint8_t PIN_BATTERY = 2;  // ADC0, battery through a /2 divider

constexpr uint32_t POLL_MS = 10;
constexpr uint32_t REDRAW_MS = 50;
// Full resend (and placement registers) so a glitched I2C transfer heals by itself.
constexpr uint32_t REFRESH_MS = 2000;

// Layout (6x8 font, 21 columns, 8 rows aligned to the panel's 8-pixel pages so an
// update only resends the pages that changed): now playing, 4 menu rows, 3 status
// rows. The font leaves the bottom pixel row of each cell empty, which holds the lines.
constexpr int16_t HEADER_Y = 0;
constexpr int16_t HEADER_LINE_Y = 7;
constexpr int16_t MENU_Y = 8;
constexpr uint8_t MENU_ROWS = 4;
constexpr int16_t DIVIDER_Y = 39;
constexpr int16_t STATUS_Y[3] = {40, 48, 56};

// Rows depend on the mode: TopStatus (Train progress / Enjoy target / Demo level) outside Manual,
// TopSongs only in Manual. The Top cursor is a row; topItemAt() maps it to an item.
enum TopItem : uint8_t {
    TopMode,
    TopStatus,
    TopPlay,
    TopSongs,
    TopStop,
    TopVolume,
    TopBpmAnalysis,
    TopSource,
    TopBle,
    TopPowerOff,
    TopCount
};

// Top: mode, play/pause, stop, volume, and BPM analysis. ModeList / VolumeList: pick a value with OK, which also leaves the list.
enum class Screen : uint8_t {
    Top,
    ModeList,
    PlanList,      // Train plans, opened when Train is picked in ModeList
    VolumeList,
    BpmAnalysisList,
    BpmStatus,
    SongList,
    SourceList,    // PPG / IMU (OK cycles Chest/Watch/Local) / Back
    BleList,       // per device: link (OK connects / disconnects), mode (OK toggles Auto/Manual); Back
    PowerConfirm   // Cancel / Power off
};

constexpr uint8_t MODE_COUNT = 4;  // PlayMode values in order: Enjoy, Train, Manual, Demo
constexpr uint8_t SOURCE_LIST_COUNT = 3;
constexpr uint8_t BLE_LIST_COUNT = 5;  // Chest link, Chest mode, Watch link, Watch mode, Back
constexpr uint8_t SOURCE_COUNT = 3;  // Chest, Watch, Local
constexpr uint8_t BPM_ANALYSIS_COUNT = 2;
constexpr uint8_t POWER_CONFIRM_COUNT = 2;
constexpr uint8_t VOLUME_STEP = 10;
constexpr uint8_t VOLUME_COUNT = 100 / VOLUME_STEP + 1;

struct Button {
    uint8_t pin;
    uint8_t history;  // last 8 samples, 1 = pressed
    bool down;
};

OledSh1106 oled(&Wire, OLED_ADDRESS);

Button buttons[3] = {{PIN_UP, 0, false}, {PIN_DOWN, 0, false}, {PIN_OK, 0, false}};

Screen screen = Screen::Top;
uint16_t cursor = TopMode;
uint16_t menuTop = 0;

// Mirrors the choice sent to analysisTask (both start on Chest).
SensorSource ppgSource = SensorSource::Chest;
SensorSource imuSource = SensorSource::Chest;

// Reported by controlTask / audioTask.
PlayMode playMode = PlayMode::Enjoy;
uint8_t trainPlan = 0;
uint8_t trainStage = TRAIN_STAGE_DONE;
uint16_t trainRemainingS = 0;
bool trainDone = false;  // shown until the mode changes again
uint16_t targetBpm = 0;  // Enjoy: 0 until a valid heart rate gives a target
uint8_t targetZone = 0;
uint8_t motionLevel = MOTION_LEVEL_NONE;  // Demo
AudioState audioState = AudioState::Stopped;
char nowPlaying[AUDIO_FILENAME_MAX] = "";
// ponytail: Volume is mirrored from menu changes only; the serial "volume" command
// can make it differ. Have audioTask report the volume if that matters.
uint8_t volumePercent = 50;  // audioTask starts at 50%
BpmAnalysisState bpmAnalysisState = BpmAnalysisState::Idle;
BpmAnalysisMode bpmAnalysisMode = BpmAnalysisMode::NewOrUnknown;
uint16_t bpmAnalysisCurrent = 0;
uint16_t bpmAnalysisTotal = 0;
uint16_t bpmAnalysisSucceeded = 0;
uint16_t bpmAnalysisFailed = 0;
char bpmAnalysisFile[AUDIO_FILENAME_MAX] = "";

uint32_t lastRefreshMs = 0;
bool powerOffRequested = false;  // set by the confirm screen, handled in the UITask loop

// Reported by BLETask, index = SensorSource (Chest, Watch).
BleLinkState bleLink[2] = {BleLinkState::Down, BleLinkState::Down};
bool bleAuto[2] = {true, true};
bool havePpg = false;
uint16_t heartRate = 0;
uint8_t spo2 = 0;
uint8_t ppgStatus = 0;
bool haveAcc = false;
bool haveGyro = false;
int16_t acc[3] = {0, 0, 0};
int16_t gyro[3] = {0, 0, 0};

const char* sourceLabel(SensorSource source)
{
    return source == SensorSource::Chest ? "Chest" : source == SensorSource::Watch ? "Watch" : "Local";
}

const char* modeName(PlayMode mode)
{
    switch (mode) {
    case PlayMode::Enjoy: return "Enjoy";
    case PlayMode::Train: return "Train";
    case PlayMode::Demo: return "Demo";
    default: return "Manual";
    }
}

bool topItemVisible(uint8_t item)
{
    if (item == TopStatus) return playMode != PlayMode::Manual;
    if (item == TopSongs) return playMode == PlayMode::Manual;
    return true;
}

uint16_t topRowCount()
{
    uint16_t n = 0;
    for (uint8_t i = 0; i < TopCount; ++i) n += topItemVisible(i) ? 1 : 0;
    return n;
}

uint8_t topItemAt(uint16_t row)
{
    for (uint8_t i = 0; i < TopCount; ++i) {
        if (!topItemVisible(i)) continue;
        if (row-- == 0) return i;
    }
    return TopMode;
}

uint16_t topRowOf(uint8_t item)
{
    uint16_t row = 0;
    for (uint8_t i = 0; i < item && i < TopCount; ++i) row += topItemVisible(i) ? 1 : 0;
    return topItemVisible(item) ? row : 0;
}

// The library is rewritten only by audioTask while it scans with playback stopped; the
// list is not offered then. Never returns 0 while the list is open (cursor math needs >= 1).
uint16_t songCount()
{
    const size_t n = musicLibrary.count > MUSIC_LIBRARY_MAX ? MUSIC_LIBRARY_MAX : musicLibrary.count;
    return n == 0 ? 1 : static_cast<uint16_t>(n);
}

uint16_t itemCount()
{
    switch (screen) {
    case Screen::ModeList: return MODE_COUNT;
    case Screen::PlanList: return TRAIN_PLAN_COUNT;
    case Screen::VolumeList: return VOLUME_COUNT;
    case Screen::BpmAnalysisList: return BPM_ANALYSIS_COUNT;
    case Screen::BpmStatus: return MENU_ROWS;
    case Screen::SongList: return songCount();
    case Screen::SourceList: return SOURCE_LIST_COUNT;
    case Screen::BleList: return BLE_LIST_COUNT;
    case Screen::PowerConfirm: return POWER_CONFIRM_COUNT;
    default: return topRowCount();
    }
}
// True once on the sample that completes a stable press (40 ms debounce).
bool buttonPressed(Button& b)
{
    b.history = static_cast<uint8_t>((b.history << 1) | (digitalRead(b.pin) == LOW));
    if ((b.history & 0x0F) == 0x0F && !b.down) {
        b.down = true;
        return true;
    }
    if ((b.history & 0x0F) == 0) b.down = false;
    return false;
}

const char* screenName(Screen value)
{
    switch (value) {
    case Screen::Top: return "Top";
    case Screen::ModeList: return "ModeList";
    case Screen::PlanList: return "PlanList";
    case Screen::VolumeList: return "VolumeList";
    case Screen::BpmAnalysisList: return "BpmAnalysisList";
    case Screen::BpmStatus: return "BpmStatus";
    case Screen::SongList: return "SongList";
    case Screen::SourceList: return "SourceList";
    case Screen::BleList: return "BleList";
    case Screen::PowerConfirm: return "PowerConfirm";
    }
    return "?";
}

const char* topItemName(uint8_t item)
{
    switch (item) {
    case TopMode: return "Mode";
    case TopStatus: return "Status";
    case TopPlay: return "Play/Pause";
    case TopSongs: return "Songs";
    case TopStop: return "Stop";
    case TopVolume: return "Volume";
    case TopBpmAnalysis: return "BPM analysis";
    case TopSource: return "Source";
    case TopBle: return "BLE";
    case TopPowerOff: return "Power off";
    }
    return "?";
}

const char* linkLabel(BleLinkState state)
{
    return state == BleLinkState::Up ? "Conn" : state == BleLinkState::Connecting ? "Scan" : "--";
}

// BLETask reports the result; the menu shows it on the next Ble update.
void sendBleCommand(BleCommandType type, uint8_t device)
{
    const BleCmdElement command{type, static_cast<SensorSource>(device)};
    if (bleCmdQueue == nullptr || xQueueSend(bleCmdQueue, &command, 0) != pdPASS) {
        ++droppedCommands;
        LOGW(TAG, "BLE command queue full; command %d dropped", static_cast<int>(type));
        return;
    }
    LOGI(TAG, "-> BLE: command %d device %u", static_cast<int>(type), static_cast<unsigned>(device));
}

bool sendCommand(UiCommandType type, uint8_t volume = 0, PlayMode mode = PlayMode::Enjoy,
                 int16_t songIndex = -1, SensorSource source = SensorSource::Chest, uint8_t plan = 0)
{
    UiCommandElement command{};
    command.command = type;
    command.trainPlan = plan;
    command.source = source;
    command.volumePercent = volume;
    command.mode = mode;
    command.songIndex = songIndex;
    if (uiCmdQueue == nullptr || xQueueSend(uiCmdQueue, &command, 0) != pdPASS) {
        ++droppedCommands;
        LOGW(TAG, "control command queue full; command %d dropped (total dropped %lu)", static_cast<int>(type),
             static_cast<unsigned long>(droppedCommands));
        return false;
    }
    LOGD(TAG, "-> control: command %d (volume=%u mode=%s)", static_cast<int>(type), static_cast<unsigned>(volume),
         modeName(mode));
    return true;
}
void openList(Screen list, uint16_t selected)
{
    LOGD(TAG, "screen %s -> %s (cursor %u)", screenName(screen), screenName(list), static_cast<unsigned>(selected));
    screen = list;
    cursor = selected;
    menuTop = 0;
}

void backToTop(TopItem item)
{
    LOGD(TAG, "screen %s -> Top (cursor %s)", screenName(screen), topItemName(item));
    screen = Screen::Top;
    cursor = topRowOf(item);
    menuTop = 0;
}

void activateItem()
{
    LOGI(TAG, "OK pressed: screen=%s cursor=%u%s%s", screenName(screen), static_cast<unsigned>(cursor),
         screen == Screen::Top ? " item=" : "", screen == Screen::Top ? topItemName(topItemAt(cursor)) : "");
    switch (screen) {
    case Screen::ModeList:
        if (static_cast<PlayMode>(cursor) == PlayMode::Train) {
            openList(Screen::PlanList, trainPlan);
            break;
        }
        sendCommand(UiCommandType::SetMode, 0, static_cast<PlayMode>(cursor));
        backToTop(TopMode);
        break;
    case Screen::PlanList:
        sendCommand(UiCommandType::SetMode, 0, PlayMode::Train, -1, SensorSource::Chest, static_cast<uint8_t>(cursor));
        backToTop(TopMode);
        break;
    case Screen::VolumeList:
        backToTop(TopVolume);
        break;
    case Screen::BpmAnalysisList: {
        const bool newOrUnknown = cursor == 0;
        const UiCommandType command = newOrUnknown
            ? UiCommandType::AnalyzeNewOrUnknownBpm
            : UiCommandType::ReanalyzeAllBpm;
        bpmAnalysisMode = newOrUnknown
            ? BpmAnalysisMode::NewOrUnknown
            : BpmAnalysisMode::All;
        LOGI(TAG, "BPM analysis requested from menu: %s", newOrUnknown ? "new/unknown" : "reanalyze all");
        bpmAnalysisState = BpmAnalysisState::Queued;
        bpmAnalysisCurrent = 0;
        bpmAnalysisTotal = 0;
        bpmAnalysisSucceeded = 0;
        bpmAnalysisFailed = 0;
        bpmAnalysisFile[0] = 0;
        if (!sendCommand(command)) {
            bpmAnalysisState = BpmAnalysisState::Failed;
            bpmAnalysisFailed = 1;
        }
        screen = Screen::BpmStatus;
        cursor = 0;
        menuTop = 0;
        break;
    }
    case Screen::BpmStatus:
        if (bpmAnalysisState == BpmAnalysisState::Queued ||
            bpmAnalysisState == BpmAnalysisState::Running) {
            sendCommand(UiCommandType::CancelBpmAnalysis);
            bpmAnalysisState = BpmAnalysisState::Cancelled;
        }
        backToTop(TopBpmAnalysis);
        break;
    case Screen::SongList:
        if (cursor < musicLibrary.count) {
            sendCommand(UiCommandType::PlaySong, 0, PlayMode::Enjoy, static_cast<int16_t>(cursor));
        }
        backToTop(TopSongs);
        break;
    case Screen::SourceList:
        if (cursor >= 2) {
            backToTop(TopSource);
        } else {
            // OK cycles Chest -> Watch -> Local in place.
            const bool isPpg = cursor == 0;
            SensorSource& current = isPpg ? ppgSource : imuSource;
            const SensorSource next = static_cast<SensorSource>((static_cast<uint8_t>(current) + 1) % SOURCE_COUNT);
            if (sendCommand(isPpg ? UiCommandType::SetPpgSource : UiCommandType::SetImuSource, 0, PlayMode::Enjoy, -1,
                            next)) {
                current = next;
            }
        }
        break;
    case Screen::BleList: {
        if (cursor >= 4) {
            backToTop(TopBle);
            break;
        }
        const uint8_t device = cursor / 2;
        if (cursor % 2 == 0) {
            if (bleLink[device] == BleLinkState::Up) sendBleCommand(BleCommandType::Disconnect, device);
            else if (bleLink[device] == BleLinkState::Down) sendBleCommand(BleCommandType::Connect, device);
        } else {
            sendBleCommand(bleAuto[device] ? BleCommandType::SetManual : BleCommandType::SetAuto, device);
        }
        break;
    }
    case Screen::PowerConfirm:
        // Standby starts only once controlTask has the command.
        if (cursor == 1 && sendCommand(UiCommandType::PowerOff)) powerOffRequested = true;
        else backToTop(TopPowerOff);
        break;
    default:
        switch (topItemAt(cursor)) {
        case TopMode:
            openList(Screen::ModeList, static_cast<uint8_t>(playMode));
            break;
        case TopStatus:
            if (playMode == PlayMode::Train) sendCommand(UiCommandType::RestartTrain);  // others are read-only
            break;
        case TopPlay:
            if (audioState == AudioState::Playing) sendCommand(UiCommandType::Pause);
            else if (audioState == AudioState::Paused) sendCommand(UiCommandType::Resume);
            break;
        case TopSongs:
            if (musicLibraryReady && !musicBpmAnalysisBusy && musicLibrary.count > 0) {
                openList(Screen::SongList, 0);
            } else {
                LOGD(TAG, "song list unavailable (ready=%d busy=%d count=%u)", musicLibraryReady ? 1 : 0,
                     musicBpmAnalysisBusy ? 1 : 0, static_cast<unsigned>(musicLibrary.count));
            }
            break;
        case TopStop:
            sendCommand(UiCommandType::Stop);
            break;
        case TopVolume:
            openList(Screen::VolumeList, 0);
            break;
        case TopBpmAnalysis:
            if (bpmAnalysisState == BpmAnalysisState::Queued ||
                bpmAnalysisState == BpmAnalysisState::Running) {
                LOGD(TAG, "BPM analysis already active; showing status");
                screen = Screen::BpmStatus;
                cursor = 0;
                menuTop = 0;
            } else {
                openList(Screen::BpmAnalysisList, 0);
            }
            break;
        case TopSource:
            openList(Screen::SourceList, 0);
            break;
        case TopBle:
            openList(Screen::BleList, 0);
            break;
        case TopPowerOff:
            openList(Screen::PowerConfirm, 0);  // default to Cancel against a stray press
            break;
        }
        break;
    }
}

void applyUpdate(const UiUpdateElement& update)
{
    switch (update.type) {
    case UiUpdateType::Ble:
        for (uint8_t i = 0; i < 2; ++i) {
            const BleLinkState state = static_cast<BleLinkState>(update.bleLink[i]);
            if (state != bleLink[i]) LOGI(TAG, "BLE %s shown as %s", i == 0 ? "chest" : "watch", linkLabel(state));
            bleLink[i] = state;
            bleAuto[i] = update.bleAuto[i] != 0;
        }
        break;
    case UiUpdateType::Sensor:
        LOGT(TAG, "update: HR=%u SpO2=%u status=%u", static_cast<unsigned>(update.heartRateBpm),
             static_cast<unsigned>(update.spo2), static_cast<unsigned>(update.sensorStatus));
        havePpg = true;
        heartRate = update.heartRateBpm;
        spo2 = update.spo2;
        ppgStatus = update.sensorStatus;
        break;
    case UiUpdateType::Acc:
        LOGT(TAG, "update: Acc %d,%d,%d", update.acc[0], update.acc[1], update.acc[2]);
        haveAcc = true;
        for (uint8_t i = 0; i < 3; i++) acc[i] = update.acc[i];
        break;
    case UiUpdateType::Gyro:
        LOGT(TAG, "update: Gyro %d,%d,%d", update.gyro[0], update.gyro[1], update.gyro[2]);
        haveGyro = true;
        for (uint8_t i = 0; i < 3; i++) gyro[i] = update.gyro[i];
        break;
    case UiUpdateType::Mode: {
        LOGI(TAG, "mode shown: %s", modeName(static_cast<PlayMode>(update.mode)));
        const uint8_t item = topItemAt(cursor);  // rows change with the mode; keep the highlighted item
        playMode = static_cast<PlayMode>(update.mode);
        trainDone = false;
        if (screen == Screen::Top) cursor = topRowOf(item);
        break;
    }
    case UiUpdateType::Analysis:
        targetBpm = update.targetBpm;
        targetZone = update.zone;
        break;
    case UiUpdateType::Motion:
        motionLevel = update.activityLevel;
        break;
    case UiUpdateType::Train:
        LOGD(TAG, "train plan=%u stage=%u left=%us", static_cast<unsigned>(update.trainPlan),
             static_cast<unsigned>(update.trainStage), static_cast<unsigned>(update.trainRemainingS));
        trainPlan = update.trainPlan < TRAIN_PLAN_COUNT ? update.trainPlan : 0;
        trainStage = update.trainStage;
        trainRemainingS = update.trainRemainingS;
        trainDone = trainStage == TRAIN_STAGE_DONE;
        break;
    case UiUpdateType::Audio:
        LOGD(TAG, "audio state=%u file=%s", static_cast<unsigned>(update.audioState), update.filename);
        if (audioState != static_cast<AudioState>(update.audioState)) {
            // Starting/stopping playback loads the SD and decoder and is when the bus glitches
            // were seen; repair the panel shortly after rather than up to REFRESH_MS later.
            lastRefreshMs = millis() - REFRESH_MS + 300;
        }
        audioState = static_cast<AudioState>(update.audioState);
        if (update.filename[0] != 0) {
            memcpy(nowPlaying, update.filename, sizeof(nowPlaying));
            nowPlaying[sizeof(nowPlaying) - 1] = 0;
        }
        break;
    case UiUpdateType::BpmAnalysis:
        LOGD(TAG, "BPM analysis update: state=%u %u/%u ok=%u fail=%u %s", static_cast<unsigned>(update.bpmAnalysisState),
             static_cast<unsigned>(update.bpmAnalysisCurrent), static_cast<unsigned>(update.bpmAnalysisTotal),
             static_cast<unsigned>(update.bpmAnalysisSucceeded), static_cast<unsigned>(update.bpmAnalysisFailed),
             update.filename);
        bpmAnalysisState = update.bpmAnalysisState;
        bpmAnalysisMode = update.bpmAnalysisMode;
        bpmAnalysisCurrent = update.bpmAnalysisCurrent;
        bpmAnalysisTotal = update.bpmAnalysisTotal;
        bpmAnalysisSucceeded = update.bpmAnalysisSucceeded;
        bpmAnalysisFailed = update.bpmAnalysisFailed;
        if (update.filename[0] != 0) {
            strncpy(bpmAnalysisFile, update.filename, sizeof(bpmAnalysisFile) - 1);
            bpmAnalysisFile[sizeof(bpmAnalysisFile) - 1] = 0;
        }
        break;
    default:
        break;
    }
}
void itemLabel(uint16_t item, char* out, size_t size)
{
    switch (screen) {
    case Screen::ModeList:
        snprintf(out, size, "%s mode%s", modeName(static_cast<PlayMode>(item)),
                 item == static_cast<uint8_t>(playMode) ? " *" : "");
        return;
    case Screen::PlanList:
        snprintf(out, size, "%s%s", TRAIN_PLANS[item].name,
                 playMode == PlayMode::Train && item == trainPlan ? " *" : "");
        return;
    case Screen::VolumeList:
        snprintf(out, size, "Volume %u%%%s", item * VOLUME_STEP,
                 item * VOLUME_STEP == volumePercent ? " *" : "");
        return;
    case Screen::BpmAnalysisList:
        snprintf(out, size, "%s", item == 0 ? "New / unknown BPM" : "Reanalyze all BPM");
        return;
    case Screen::SourceList:
        if (item >= 2) snprintf(out, size, "OK: Back");
        else snprintf(out, size, "%s: %s", item == 0 ? "PPG" : "IMU",
                      sourceLabel(item == 0 ? ppgSource : imuSource));
        return;
    case Screen::BleList:
        if (item >= 4) snprintf(out, size, "OK: Back");
        else if (item % 2 == 0) snprintf(out, size, "%s: %s", sourceLabel(static_cast<SensorSource>(item / 2)),
                                         linkLabel(bleLink[item / 2]));
        else snprintf(out, size, "  Mode: %s", bleAuto[item / 2] ? "Auto" : "Manual");
        return;
    case Screen::PowerConfirm:
        snprintf(out, size, "%s", item == 0 ? "Cancel" : "Power off");
        return;
    case Screen::SongList:
        if (item >= musicLibrary.count) {
            snprintf(out, size, "(no songs)");
        } else if (musicLibrary.entries[item].bpm == 0) {
            snprintf(out, size, "%.15s --", musicLibrary.entries[item].file);
        } else {
            snprintf(out, size, "%.15s %u", musicLibrary.entries[item].file,
                     static_cast<unsigned>(musicLibrary.entries[item].bpm));
        }
        return;
    case Screen::BpmStatus:
        switch (item) {
        case 0:
            snprintf(out, size, "BPM: %s",
                     bpmAnalysisState == BpmAnalysisState::Queued ? "Queued" :
                     bpmAnalysisState == BpmAnalysisState::Running ? "Analyzing" :
                     bpmAnalysisState == BpmAnalysisState::Complete ? "Complete" :
                     bpmAnalysisState == BpmAnalysisState::Failed ? "Failed" :
                     bpmAnalysisState == BpmAnalysisState::Cancelled ? "Cancelled" : "Idle");
            break;
        case 1:
            if (bpmAnalysisState == BpmAnalysisState::Queued)
                snprintf(out, size, "Starting...");
            else if (bpmAnalysisState == BpmAnalysisState::Cancelled)
                snprintf(out, size, "Cancelled");
            else if (bpmAnalysisState == BpmAnalysisState::Complete ||
                     bpmAnalysisState == BpmAnalysisState::Failed)
                snprintf(out, size, "OK %u  Fail %u",
                         bpmAnalysisSucceeded, bpmAnalysisFailed);
            else
                snprintf(out, size, "File %u / %u",
                         bpmAnalysisCurrent, bpmAnalysisTotal);
            break;
        case 2:
            snprintf(out, size, "%.21s", bpmAnalysisFile);
            break;
        default:
            snprintf(out, size, "%s",
                     bpmAnalysisState == BpmAnalysisState::Queued ||
                     bpmAnalysisState == BpmAnalysisState::Running ? "OK: Cancel" : "OK: Back");
            break;
        }
        return;
    default:
        break;
    }

    switch (topItemAt(item)) {
    case TopMode:
        if (playMode == PlayMode::Train) snprintf(out, size, "Mode: %s", TRAIN_PLANS[trainPlan].name);
        else if (trainDone && playMode == PlayMode::Manual) snprintf(out, size, "Mode: Train done");
        else snprintf(out, size, "Mode: %s", modeName(playMode));
        break;
    case TopStatus:
        if (playMode == PlayMode::Train) {
            if (trainStage < TRAIN_PLANS[trainPlan].stageCount)
                snprintf(out, size, " Stage %u/%u %u:%02u", static_cast<unsigned>(trainStage + 1),
                         static_cast<unsigned>(TRAIN_PLANS[trainPlan].stageCount), trainRemainingS / 60u,
                         trainRemainingS % 60u);
            else snprintf(out, size, " Stage --");
        } else if (playMode == PlayMode::Demo) {
            if (motionLevel == MOTION_LEVEL_NONE) snprintf(out, size, " Level: --");
            else snprintf(out, size, " Level: %u", static_cast<unsigned>(motionLevel));
        } else if (targetBpm == 0) {
            snprintf(out, size, " Target: wait HR");
        } else {
            snprintf(out, size, " Z%u Target %u BPM", static_cast<unsigned>(targetZone), static_cast<unsigned>(targetBpm));
        }
        break;
    case TopPlay:
        snprintf(out, size, "%s",
                 audioState == AudioState::Playing ? "Pause"
                 : audioState == AudioState::Paused ? "Resume" : "Stopped");
        break;
    case TopSongs:
        snprintf(out, size, "Songs");
        break;
    case TopStop:
        snprintf(out, size, "Stop");
        break;
    case TopVolume:
        snprintf(out, size, "Volume: %u%%", volumePercent);
        break;
    case TopBpmAnalysis:
        if (bpmAnalysisState == BpmAnalysisState::Queued)
            snprintf(out, size, "BPM: queued");
        else if (bpmAnalysisState == BpmAnalysisState::Running)
            snprintf(out, size, "BPM: running");
        else
            snprintf(out, size, "BPM Analysis");
        break;
    case TopSource:
        snprintf(out, size, "Source");
        break;
    case TopBle:
        snprintf(out, size, "BLE");
        break;
    case TopPowerOff:
        snprintf(out, size, "Power off");
        break;
    default:
        out[0] = 0;
        break;
    }
}
// The board has no RTC/NTP, so the time shown is sys_ms since boot (millis()),
// same "mmMssS" format as the BM7701 sensor OLED. Minutes wrap at 100.
uint32_t clockSeconds()
{
    return millis() / 1000;
}

// Single-cell Li-ion, rough discharge curve (battery mV -> %); 0% at 3.4 V leaves headroom for amp load sag; 100% at 4.1 V, where the charger stops. Below BATTERY_MIN_MV the
// ADC is taken as "no battery connected" and "--" is shown.
constexpr uint32_t BATTERY_MIN_MV = 2500;
constexpr uint16_t BATTERY_CURVE[][2] = {{3400, 0},  {3500, 10}, {3600, 25}, {3700, 45},
                                         {3800, 65}, {3900, 82}, {4000, 93}, {4100, 100}};

// Returns 0..100, or -1 when no battery is detected. Averages a few reads to tame ADC noise.
int batteryPercent()
{
    uint32_t sum = 0;
    constexpr uint8_t SAMPLES = 8;
    for (uint8_t i = 0; i < SAMPLES; ++i) sum += analogRead(PIN_BATTERY);
    const uint32_t mv = (sum / SAMPLES) * 3300UL / 4095UL * 2;  // 12-bit reading, undo the /2 divider
    if (mv < BATTERY_MIN_MV) return -1;
    constexpr size_t N = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
    if (mv <= BATTERY_CURVE[0][0]) return 0;
    for (size_t i = 1; i < N; ++i) {
        if (mv > BATTERY_CURVE[i][0]) continue;
        const uint32_t lo = BATTERY_CURVE[i - 1][0], hi = BATTERY_CURVE[i][0];
        const uint32_t pLo = BATTERY_CURVE[i - 1][1], pHi = BATTERY_CURVE[i][1];
        return static_cast<int>(pLo + (mv - lo) * (pHi - pLo) / (hi - lo));
    }
    return 100;
}

void draw()
{
    char line[48];
    const uint16_t count = itemCount();

    if (cursor < menuTop) menuTop = cursor;
    if (cursor >= menuTop + MENU_ROWS) menuTop = cursor - MENU_ROWS + 1;

    oled.clearDisplay();
    oled.setTextSize(1);
    const bool volumeBar = screen == Screen::VolumeList;
    oled.setTextColor(1);
    oled.setCursor(0, HEADER_Y);
    if (audioState == AudioState::Stopped) {
        oled.print("Stopped");
    } else {
        // "001.mp3(136)": file name plus its library BPM, "--" when unknown.
        char bpmText[8] = "--";
        for (size_t i = 0; i < musicLibrary.count && i < MUSIC_LIBRARY_MAX; ++i) {
            if (strcmp(musicLibrary.entries[i].file, nowPlaying) != 0) continue;
            if (musicLibrary.entries[i].bpm != 0) snprintf(bpmText, sizeof(bpmText), "%u", musicLibrary.entries[i].bpm);
            break;
        }
        snprintf(line, sizeof(line), "%s %.9s(%s)",
                 audioState == AudioState::Playing ? ">" : "||", nowPlaying, bpmText);
        oled.print(line);
    }
    {
        // Battery at the far right of the header; refreshed every REFRESH_MS to keep ADC reads cheap.
        static int percent = -1;
        static uint32_t lastBatteryMs = 0;
        static bool batteryRead = false;
        const uint32_t nowMs = millis();
        if (!batteryRead || (uint32_t)(nowMs - lastBatteryMs) >= REFRESH_MS) {
            percent = batteryPercent();
            lastBatteryMs = nowMs;
            batteryRead = true;
        }
        if (percent < 0) snprintf(line, sizeof(line), "--%%");
        else snprintf(line, sizeof(line), "%d%%", percent);
        oled.setCursor(OledSh1106::WIDTH - 6 * static_cast<int16_t>(strlen(line)), HEADER_Y);
        oled.print(line);
    }
    oled.drawFastHLine(0, HEADER_LINE_Y, OledSh1106::WIDTH, 1);

    if (volumeBar) {
        // Horizontal bar filling from the left; Up/Down adjust, OK leaves.
        snprintf(line, sizeof(line), "Volume %u%%", volumePercent);
        oled.setCursor(2, MENU_Y + 2);
        oled.print(line);
        constexpr int16_t BAR_W = OledSh1106::WIDTH - 4;
        oled.drawRect(2, MENU_Y + 14, BAR_W, 12, 1);
        oled.fillRect(4, MENU_Y + 16, (BAR_W - 4) * volumePercent / 100, 8, 1);
    }

    for (uint8_t row = 0; row < MENU_ROWS && !volumeBar; row++) {
        const uint16_t item = menuTop + row;
        if (item >= count) break;
        const int16_t y = MENU_Y + row * 8;
        const bool selected = item == cursor;
        if (selected) oled.fillRect(0, y, OledSh1106::WIDTH, 8, 1);
        oled.setTextColor(selected ? 0 : 1);
        itemLabel(item, line, sizeof(line));
        oled.setCursor(2, y);
        oled.print(line);
        // Scroll hints on the right edge.
        if (row == 0 && menuTop > 0) {
            oled.setCursor(120, y);
            oled.print('^');
        } else if (row == MENU_ROWS - 1 && item + 1 < count) {
            oled.setCursor(120, y);
            oled.print('v');
        }
    }

    oled.setTextColor(1);
    oled.drawFastHLine(0, DIVIDER_Y, OledSh1106::WIDTH, 1);

    oled.setCursor(0, STATUS_Y[0]);
    const uint32_t sec = clockSeconds();
    snprintf(line, sizeof(line), "C:%s W:%s %02u:%02u", linkLabel(bleLink[0]), linkLabel(bleLink[1]),
             (unsigned)(sec / 60 % 100), (unsigned)(sec % 60));
    oled.print(line);

    if (havePpg) snprintf(line, sizeof(line), "HR:%u SpO2:%u St:%u", heartRate, spo2, ppgStatus);
    else snprintf(line, sizeof(line), "HR:-- SpO2:-- St:-");
    oled.setCursor(0, STATUS_Y[1]);
    oled.print(line);

    // 128 px / 6 px = 21 columns: A:--,--,-- G:--,--,-- fits, real values may clip.
    char accText[24] = "--,--,--";
    char gyroText[24] = "--,--,--";
    if (haveAcc) snprintf(accText, sizeof(accText), "%d,%d,%d", acc[0], acc[1], acc[2]);
    if (haveGyro) snprintf(gyroText, sizeof(gyroText), "%d,%d,%d", gyro[0], gyro[1], gyro[2]);
    snprintf(line, sizeof(line), "A:%s G:%s", accText, gyroText);
    oled.setCursor(0, STATUS_Y[2]);
    oled.print(line);

    const bool full = (uint32_t)(millis() - lastRefreshMs) >= REFRESH_MS;
    const bool ok = full ? oled.refresh() : oled.display();
    if (full) lastRefreshMs = millis();
    if (!ok) {
        // A failed I2C transfer can leave the panel's address registers off (shifted
        // or misplaced rows); repair on the next draw instead of waiting for REFRESH_MS.
        LOGW(TAG, "OLED I2C transfer failed; full refresh scheduled");
        lastRefreshMs = millis() - REFRESH_MS;
    }
}

// ---- Boot / power-off animation and standby ----

constexpr uint32_t ANIM_FRAME_MS = 40;
constexpr uint32_t BOOT_MIN_MS = 3000;
// The bar waits at 80% for the music library; a missing SD card must not hold the menu forever.
constexpr uint32_t BOOT_TIMEOUT_MS = 15000;
constexpr uint32_t POWER_OFF_MS = 1100;
constexpr uint32_t WAKE_HOLD_MS = 2000;

// Keep UI state current while an animation or standby owns the loop.
void drainUpdates()
{
    UiUpdateElement update{};
    while (uiUpdateQueue != nullptr && xQueueReceive(uiUpdateQueue, &update, 0) == pdPASS) applyUpdate(update);
}

// Two lobes plus a point: 4r wide, 3r tall, cy is the lobe centre line.
void drawHeart(int16_t cx, int16_t cy, int16_t r)
{
    if (r < 1) {
        oled.drawPixel(cx, cy, 1);
        return;
    }
    oled.fillCircle(cx - r, cy, r, 1);
    oled.fillCircle(cx + r, cy, r, 1);
    oled.fillTriangle(cx - 2 * r, cy, cx + 2 * r, cy, cx, cy + 2 * r, 1);
}

void drawNote(int16_t x, int16_t y)
{
    oled.fillCircle(x, y, 2, 1);
    oled.drawFastVLine(x + 2, y - 8, 9, 1);
    oled.drawLine(x + 2, y - 8, x + 5, y - 5, 1);
}

// One heartbeat trace (P wave, QRS, flat) across x 4..124, drawn up to xEnd.
void drawEcg(int16_t baseY, int16_t xEnd)
{
    static const int8_t POINTS[][2] = {{0, 0},  {36, 0}, {43, -3}, {50, 0},  {60, 0},
                                       {65, 4}, {72, -14}, {79, 8}, {84, 0}, {120, 0}};
    constexpr int16_t X0 = 4;
    for (size_t i = 1; i < sizeof(POINTS) / sizeof(POINTS[0]); ++i) {
        const int16_t x0 = X0 + POINTS[i - 1][0], y0 = baseY + POINTS[i - 1][1];
        int16_t x1 = X0 + POINTS[i][0], y1 = baseY + POINTS[i][1];
        if (x0 >= xEnd) break;
        if (x1 > xEnd) {
            y1 = y0 + (y1 - y0) * (xEnd - x0) / (x1 - x0);
            x1 = xEnd;
        }
        oled.drawLine(x0, y0, x1, y1, 1);
    }
}

// "Lub-dub" every 800 ms: extra heart radius 0..2.
int16_t heartBeat(uint32_t ms)
{
    const uint32_t phase = ms % 800;
    return phase < 120 ? 2 : (phase >= 200 && phase < 300) ? 1 : 0;
}

void printCentered(const char* text, int16_t y)
{
    oled.setCursor((OledSh1106::WIDTH - 6 * static_cast<int16_t>(strlen(text))) / 2, y);
    oled.print(text);
}

// Trace sweeps in, turns into a heart, the heart beats with notes; bar shows boot progress.
void drawBootFrame(uint32_t elapsed, uint8_t progress)
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(1);
    if (elapsed < 1000) {
        drawEcg(26, static_cast<int16_t>(4 + 120 * elapsed / 1000));
    } else if (elapsed < 1600) {
        drawHeart(64, 18, static_cast<int16_t>(1 + 6 * (elapsed - 1000) / 600));
    } else {
        drawHeart(64, 18, 6 + heartBeat(elapsed - 1600));
        drawNote(92, 26);
        drawNote(102, 20);
        printCentered("Hear your heart", 38);
    }
    oled.drawRect(14, 52, 100, 8, 1);
    oled.fillRect(16, 54, 96 * progress / 100, 4, 1);
    oled.display();
}

void playBootAnimation()
{
    LOGI(TAG, "boot animation start");
    const uint32_t start = millis();
    for (;;) {
        const uint32_t elapsed = millis() - start;
        const bool ready = musicLibraryReady || elapsed >= BOOT_TIMEOUT_MS;
        uint32_t progress = elapsed * 100 / BOOT_MIN_MS;
        if (progress > 100) progress = 100;
        if (!ready && progress > 80) progress = 80;
        drawBootFrame(elapsed, static_cast<uint8_t>(progress));
        drainUpdates();
        if (ready && elapsed >= BOOT_MIN_MS) break;
        vTaskDelay(pdMS_TO_TICKS(ANIM_FRAME_MS));
    }
    if (!musicLibraryReady) LOGW(TAG, "music library not ready after %lu ms; menu shown anyway",
             static_cast<unsigned long>(BOOT_TIMEOUT_MS));
    lastRefreshMs = millis() - REFRESH_MS;  // the menu's first draw resends every page
}

// Heart shrinks with "Goodbye", then the picture collapses to a line like an old CRT.
void playPowerOffAnimation()
{
    const uint32_t start = millis();
    for (uint32_t elapsed = 0; elapsed < POWER_OFF_MS; elapsed = millis() - start) {
        oled.clearDisplay();
        oled.setTextColor(1);
        if (elapsed < 800) {
            drawHeart(64, 18, static_cast<int16_t>(7 * (800 - elapsed) / 800));
            printCentered("Goodbye", 40);
        } else {
            const int16_t w = static_cast<int16_t>(OledSh1106::WIDTH * (POWER_OFF_MS - elapsed) / (POWER_OFF_MS - 800));
            oled.drawFastHLine((OledSh1106::WIDTH - w) / 2, 31, w, 1);
        }
        oled.display();
        vTaskDelay(pdMS_TO_TICKS(ANIM_FRAME_MS));
    }
    oled.clearDisplay();
    oled.display();
}

// Called after controlTask accepted PowerOff. Returns on the main menu after the wake animation.
void runStandby()
{
    playPowerOffAnimation();
    oled.setPower(false);
    LOGI(TAG, "standby: hold OK for %lu ms to wake", static_cast<unsigned long>(WAKE_HOLD_MS));

    bool holding = false;
    uint32_t holdStart = 0;
    for (;;) {
        drainUpdates();
        if (digitalRead(PIN_OK) != LOW) {
            holding = false;
        } else if (!holding) {
            holding = true;
            holdStart = millis();
        } else if ((uint32_t)(millis() - holdStart) >= WAKE_HOLD_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    LOGI(TAG, "OK held: waking");
    oled.setPower(true);
    // controlTask must leave standby (BLE and sensors follow it); retry until the queue takes it.
    while (!sendCommand(UiCommandType::PowerOn)) vTaskDelay(pdMS_TO_TICKS(50));
    playBootAnimation();
    // OK is probably still held: treat every button as down so nothing fires until released.
    for (Button& b : buttons) {
        b.history = 0xFF;
        b.down = true;
    }
    backToTop(TopMode);
}

} // namespace

void UITask(void* parameter) {
    (void)parameter;

    LOGI(TAG, "started; buttons up=D%u down=D%u ok=D%u (INPUT_PULLUP)", static_cast<unsigned>(PIN_UP),
         static_cast<unsigned>(PIN_DOWN), static_cast<unsigned>(PIN_OK));
    for (Button& b : buttons) pinMode(b.pin, INPUT_PULLUP);
    analogReadResolution(12);

    if (!oled.begin()) {
        LOGE(TAG, "OLED (SH1106 @0x%02X) not found on Wire (D3/D4); UI disabled, task exits", OLED_ADDRESS);
        vTaskDelete(nullptr);
    }
    LOGI(TAG, "OLED initialized (128x64, refresh %lu ms)", static_cast<unsigned long>(REFRESH_MS));
    playBootAnimation();

    bool dirty = true;
    uint32_t lastDrawMs = 0;
    uint32_t lastClockSec = clockSeconds();
    for (;;) {
        const bool upPressed = buttonPressed(buttons[0]);
        const bool downPressed = buttonPressed(buttons[1]);
        if (upPressed || downPressed) {
            LOGD(TAG, "button %s pressed (screen=%s cursor=%u)", upPressed ? "UP" : "DOWN", screenName(screen),
                 static_cast<unsigned>(cursor));
        }
        if (screen == Screen::VolumeList) {
            if (upPressed && volumePercent < 100) {
                volumePercent += VOLUME_STEP;
                LOGI(TAG, "volume -> %u%%", static_cast<unsigned>(volumePercent));
                sendCommand(UiCommandType::SetVolume, volumePercent);
                dirty = true;
            }
            if (downPressed && volumePercent > 0) {
                volumePercent -= VOLUME_STEP;
                LOGI(TAG, "volume -> %u%%", static_cast<unsigned>(volumePercent));
                sendCommand(UiCommandType::SetVolume, volumePercent);
                dirty = true;
            }
        } else if (screen != Screen::BpmStatus) {
            if (upPressed) {
                cursor = cursor == 0 ? itemCount() - 1 : cursor - 1;
                dirty = true;
            }
            if (downPressed) {
                cursor = cursor + 1 >= itemCount() ? 0 : cursor + 1;
                dirty = true;
            }
        }        if (buttonPressed(buttons[2])) {
            activateItem();
            dirty = true;
        }
        if (powerOffRequested) {
            powerOffRequested = false;
            runStandby();
            dirty = true;
        }

        UiUpdateElement update{};
        while (uiUpdateQueue != nullptr &&
               xQueueReceive(uiUpdateQueue, &update, 0) == pdPASS) {
            applyUpdate(update);
            dirty = true;
        }

        const uint32_t now = millis();
        if ((uint32_t)(now - lastRefreshMs) >= REFRESH_MS) dirty = true;
        const uint32_t sec = clockSeconds();
        if (sec != lastClockSec) {
            lastClockSec = sec;
            dirty = true;
        }
        if (dirty &&(uint32_t)(now - lastDrawMs) >= REDRAW_MS) {
            LOGT(TAG, "redraw (screen=%s cursor=%u)", screenName(screen), static_cast<unsigned>(cursor));
            draw();
            dirty = false;
            lastDrawMs = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
