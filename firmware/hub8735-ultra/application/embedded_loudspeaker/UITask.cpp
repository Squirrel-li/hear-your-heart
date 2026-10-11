#include "UITask.h"

#include "debug.h"
#include "globalData.h"
#include "oled_sh1106.h"

namespace {

constexpr const char* TAG = "UI";
uint32_t droppedCommands = 0;

// OLED on Wire (I2C0: SDA=D3/PE4, SCL=D4/PE3).
constexpr uint8_t OLED_ADDRESS = 0x3C;

// Buttons wired between pin and GND (internal pull-up, pressed = LOW).
constexpr uint8_t PIN_UP = 8;
constexpr uint8_t PIN_DOWN = 9;
constexpr uint8_t PIN_OK = 10;

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

enum TopItem : uint8_t {
    TopMode,
    TopPlay,
    TopStop,
    TopVolume,
    TopBpmAnalysis,
    TopCount
};

// Top: mode, play/pause, stop, volume, and BPM analysis. ModeList / VolumeList: pick a value with OK, which also leaves the list.
enum class Screen : uint8_t {
    Top,
    ModeList,
    VolumeList,
    BpmAnalysisList,
    BpmStatus
};

constexpr uint8_t MODE_COUNT = 3;
constexpr uint8_t BPM_ANALYSIS_COUNT = 2;
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
uint8_t cursor = TopMode;
uint8_t menuTop = 0;

// Reported by controlTask / audioTask.
PlayMode playMode = PlayMode::Enjoy;
AudioState audioState = AudioState::Stopped;
char nowPlaying[AUDIO_FILENAME_MAX] = "";
// ponytail: Volume is mirrored from menu changes only; the serial "volume" command
// can make it differ. Have audioTask report the volume if that matters.
uint8_t volumePercent = 50;  // audioTask starts at 50%
BpmAnalysisState bpmAnalysisState = BpmAnalysisState::Idle;
BpmAnalysisMode bpmAnalysisMode = BpmAnalysisMode::NewOrUnknown;
uint8_t bpmAnalysisCurrent = 0;
uint8_t bpmAnalysisTotal = 0;
uint8_t bpmAnalysisSucceeded = 0;
uint8_t bpmAnalysisFailed = 0;
char bpmAnalysisFile[AUDIO_FILENAME_MAX] = "";

uint32_t lastRefreshMs = 0;

bool bleConnected = false;
bool havePpg = false;
uint16_t heartRate = 0;
uint8_t spo2 = 0;
uint8_t ppgStatus = 0;
bool haveAcc = false;
bool haveGyro = false;
int16_t acc[3] = {0, 0, 0};
int16_t gyro[3] = {0, 0, 0};

const char* modeName(PlayMode mode)
{
    switch (mode) {
    case PlayMode::Enjoy: return "Enjoy";
    case PlayMode::Train: return "Train";
    default: return "Manual";
    }
}

uint8_t itemCount()
{
    switch (screen) {
    case Screen::ModeList: return MODE_COUNT;
    case Screen::VolumeList: return VOLUME_COUNT;
    case Screen::BpmAnalysisList: return BPM_ANALYSIS_COUNT;
    case Screen::BpmStatus: return MENU_ROWS;
    default: return TopCount;
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
    case Screen::VolumeList: return "VolumeList";
    case Screen::BpmAnalysisList: return "BpmAnalysisList";
    case Screen::BpmStatus: return "BpmStatus";
    }
    return "?";
}

const char* topItemName(uint8_t item)
{
    switch (item) {
    case TopMode: return "Mode";
    case TopPlay: return "Play/Pause";
    case TopStop: return "Stop";
    case TopVolume: return "Volume";
    case TopBpmAnalysis: return "BPM analysis";
    }
    return "?";
}

bool sendCommand(UiCommandType type, uint8_t volume = 0, PlayMode mode = PlayMode::Enjoy)
{
    UiCommandElement command{};
    command.command = type;
    command.volumePercent = volume;
    command.mode = mode;
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
void openList(Screen list, uint8_t selected)
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
    cursor = item;
    menuTop = 0;
}

void activateItem()
{
    LOGI(TAG, "OK pressed: screen=%s cursor=%u%s%s", screenName(screen), static_cast<unsigned>(cursor),
         screen == Screen::Top ? " item=" : "", screen == Screen::Top ? topItemName(cursor) : "");
    switch (screen) {
    case Screen::ModeList:
        sendCommand(UiCommandType::SetMode, 0, static_cast<PlayMode>(cursor));
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
        backToTop(TopBpmAnalysis);
        break;
    default:
        switch (cursor) {
        case TopMode:
            openList(Screen::ModeList, static_cast<uint8_t>(playMode));
            break;
        case TopPlay:
            if (audioState == AudioState::Playing) sendCommand(UiCommandType::Pause);
            else if (audioState == AudioState::Paused) sendCommand(UiCommandType::Resume);
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
        }
        break;
    }
}

void applyUpdate(const UiUpdateElement& update)
{
    switch (update.type) {
    case UiUpdateType::Ble:
        if (bleConnected != (update.bleConnected != 0)) {
            LOGI(TAG, "BLE link shown as %s", update.bleConnected ? "connected" : "scanning");
        }
        bleConnected = update.bleConnected != 0;
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
    case UiUpdateType::Mode:
        LOGI(TAG, "mode shown: %s", modeName(static_cast<PlayMode>(update.mode)));
        playMode = static_cast<PlayMode>(update.mode);
        break;
    case UiUpdateType::Audio:
        LOGD(TAG, "audio state=%u file=%s", static_cast<unsigned>(update.audioState), update.filename);
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
void itemLabel(uint8_t item, char* out, size_t size)
{
    switch (screen) {
    case Screen::ModeList:
        snprintf(out, size, "%s mode%s", modeName(static_cast<PlayMode>(item)),
                 item == static_cast<uint8_t>(playMode) ? " *" : "");
        return;
    case Screen::VolumeList:
        snprintf(out, size, "Volume %u%%%s", item * VOLUME_STEP,
                 item * VOLUME_STEP == volumePercent ? " *" : "");
        return;
    case Screen::BpmAnalysisList:
        snprintf(out, size, "%s", item == 0 ? "New / unknown BPM" : "Reanalyze all BPM");
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
                snprintf(out, size, "Stop audio first");
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
            snprintf(out, size, "OK: Back");
            break;
        }
        return;
    default:
        break;
    }

    switch (item) {
    case TopMode:
        snprintf(out, size, "Mode: %s", modeName(playMode));
        break;
    case TopPlay:
        snprintf(out, size, "%s",
                 audioState == AudioState::Playing ? "Pause"
                 : audioState == AudioState::Paused ? "Resume" : "Stopped");
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

void draw()
{
    char line[48];
    const uint8_t count = itemCount();

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
        snprintf(line, sizeof(line), "%s %.18s",
                 audioState == AudioState::Playing ? ">" : "||", nowPlaying);
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
        const uint8_t item = menuTop + row;
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
    snprintf(line, sizeof(line), "BLE:%s %02u:%02u", bleConnected ? "Conn" : "Scan",
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

    if ((uint32_t)(millis() - lastRefreshMs) >= REFRESH_MS) {
        oled.refresh();
        lastRefreshMs = millis();
    } else {
        oled.display();
    }
}

} // namespace

void UITask(void* parameter) {
    (void)parameter;

    LOGI(TAG, "started; buttons up=D%u down=D%u ok=D%u (INPUT_PULLUP)", static_cast<unsigned>(PIN_UP),
         static_cast<unsigned>(PIN_DOWN), static_cast<unsigned>(PIN_OK));
    for (Button& b : buttons) pinMode(b.pin, INPUT_PULLUP);

    if (!oled.begin()) {
        LOGE(TAG, "OLED (SH1106 @0x%02X) not found on Wire (D3/D4); UI disabled, task exits", OLED_ADDRESS);
        vTaskDelete(nullptr);
    }
    LOGI(TAG, "OLED initialized (128x64, refresh %lu ms)", static_cast<unsigned long>(REFRESH_MS));

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
