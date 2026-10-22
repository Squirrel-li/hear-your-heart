/*
 * ZJY180-TV2（ST7735S 160x128橫向 + XPT2046觸控）智慧手錶式操作UI原型。
 * 只測畫面與操作手感：心率、歌曲、Demo強度都是假資料，不碰音訊、BLE、SD卡。
 *
 * 接線（見 firmware/hub8735-ultra/pinset.md）：
 *   CLK=D19  MOS=D20  MIS=D18  CS1=D21（SPI1硬體CS）  DC=D13  CS2=D3（觸控）
 *   RES=D6、PEN=D15、BLK/VCC接3.3V、NC不接；按鈕 UP=D8 DOWN=D9 OK=D10（接GND）。
 *
 * 畫面結構（參考Wear OS Tiles／Apple Watch）：
 *   主頁三張資訊頁：Now（心率、Zone、歌曲）→ Workout（依模式顯示）→ Music（播放鍵、音量）
 *   下拉：快捷面板（音量、關機）；Mode清單：Enjoy／Train 11m／Train 5m／Demo／Stop
 *
 * 操作：
 *   觸控  左右滑=換頁  往下滑=開快捷面板  往上滑=關面板  往右滑=子頁返回  點=按下該處  按住=返回
 *   按鈕  上／下=換頁、移動選項、面板中調音量  短按OK=該頁主要動作  長按OK=返回（主頁Now上=開快捷面板）
 *   待機時只有長按OK可喚醒。
 *
 * 使用SDK內附的Adafruit_GFX（不需另裝）。序列埠115200會印出事件、觸控原始值（校正用）與畫面更新耗時。
 */

#include <SPI.h>
#include <Wire.h>  // only so the builder adds the Wire library, which ships Adafruit_GFX
#include <Adafruit_OLED_libraries/Adafruit_GFX.h>  // SDK copy; the external one pulls BusIO, which does not build here
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

extern spi_t spi_obj1;  // SPI1's HAL object (SPI.cpp); spi_master_write() returns the byte read back

constexpr uint8_t PIN_UP = 8;
constexpr uint8_t PIN_DOWN = 9;
constexpr uint8_t PIN_OK = 10;
constexpr uint8_t PIN_TFT_DC = 13;     // shares the on-board camera LED, so the LED flickers with SPI traffic
constexpr uint8_t PIN_TOUCH_CS = 3;    // LCD CS (D21) is driven by the SPI1 hardware
constexpr uint8_t PIN_TFT_RST = 6;     // low-active hardware reset
constexpr uint8_t PIN_TOUCH_IRQ = 15;  // XPT2046 PENIRQ, low while touched

constexpr int16_t W = 160;  // landscape
constexpr int16_t H = 128;
constexpr uint32_t TFT_HZ = 40000000;  // ST7735S is rated 15 MHz; lower this if the image tears or speckles
constexpr uint32_t TOUCH_HZ = 2000000;  // XPT2046 max ~2.5 MHz
// Landscape: 0xA8 = MY | MV | BGR; if the image is upside down use 0x68 (MX | MV | BGR).
// If red/blue swap toggle 0x08 (BGR). The panel RAM is 132x162 with the glass at column 2 / row 1,
// which in landscape becomes x offset 1 / y offset 2; without it the right and bottom edges show garbage.
constexpr uint8_t TFT_MADCTL = 0xA8;
constexpr uint8_t TFT_COL_OFFSET = 1;
constexpr uint8_t TFT_ROW_OFFSET = 2;

// Touch calibration: raw value at screen edge 0 / edge max, from strokes along all four edges on the
// TOUCH_TEST screen with the protective film removed; mapped linearly between them. Y is reversed.
// Redo with TOUCH_TEST = true if taps drift.
constexpr int16_t TOUCH_X_MIN = 400, TOUCH_X_MAX = 3650;
constexpr int16_t TOUCH_Y_MIN = 3500, TOUCH_Y_MAX = 560;
constexpr bool TOUCH_TEST = false;  // true: boot into the calibration screen instead of the UI
constexpr bool TOUCH_SWAP_XY = true;  // XPT2046 X runs along the panel's short side; swap for landscape
constexpr uint16_t TOUCH_Z_MIN = 40;  // below this the position is not trusted (the press itself comes from PENIRQ)

constexpr uint32_t LONG_PRESS_MS = 600;
constexpr uint32_t TOUCH_RELEASE_MS = 15;  // PENIRQ must stay high this long to end a press (light presses flicker)
constexpr int16_t TOUCH_JITTER_RAW = 300;  // max disagreement between two reads of one axis (~14 px); light
                                           // touches are noisy, the longer swipe distances absorb it
constexpr int16_t SWIPE_MIN_PX = 40;
constexpr int16_t FLICK_MIN_PX = 24;  // used when the whole touch lasted under FLICK_MS
constexpr uint32_t FLICK_MS = 250;
constexpr int16_t TAP_SLOP_PX = 25;  // movement allowed before a hold stops counting as a long press

// RGB565
constexpr uint16_t C_BG = 0x0000, C_TEXT = 0xFFFF, C_DIM = 0x8410, C_PANEL = 0x2124, C_PANEL2 = 0x39C7;
constexpr uint16_t C_RED = 0xE8E4, C_ORANGE = 0xFC40, C_GREEN = 0x2E68, C_BLUE = 0x3B9F, C_GRAY = 0x6B4D;
constexpr uint16_t ZONE_COLOR[5] = {0x7BEF, C_BLUE, C_GREEN, C_ORANGE, C_RED};
const char* const ZONE_NAME[5] = {"Easy", "Fat burn", "Aerobic", "Hard", "Max"};

enum Mode : uint8_t { MODE_NONE, MODE_ENJOY, MODE_TRAIN, MODE_DEMO };
const char* const MODE_NAME[] = {"Idle", "Enjoy", "Train", "Demo"};
const uint16_t MODE_COLOR[] = {C_GRAY, C_RED, C_ORANGE, C_GREEN};

enum Tile : uint8_t { TILE_NOW, TILE_WORKOUT, TILE_MUSIC, TILE_COUNT };
enum class Screen : uint8_t { Tiles, Quick, ModeList, Power, Standby };
enum class Ev : uint8_t { None, Up, Down, Ok, Back, Tap, SwipeLeft, SwipeRight, SwipeUp, SwipeDown, Press };
const char* const EV_NAME[] = {"", "Up", "Down", "Ok", "Back", "Tap", "SwipeLeft", "SwipeRight", "SwipeUp", "SwipeDown", "Press"};

struct Input {
    Ev ev;
    int16_t x, y;  // Tap / Press position
};

// Mode list rows: Enjoy, Train full, Train short, Demo, Stop.
constexpr uint8_t ROW_COUNT = 5;
const char* const ROW_LABEL[ROW_COUNT] = {"Enjoy", "Train 11m", "Train 5m", "Demo", "Stop"};
constexpr int16_t ROW_TOP = 20, ROW_H = 21;

struct Song {
    const char* title;
    uint8_t bpm;
};
const Song SONGS[] = {{"Cool Down", 80},  {"Morning Walk", 96}, {"City Jog", 118},
                      {"Run Run Run", 134}, {"Fast Lane", 152}, {"Sprint Star", 172}};
constexpr uint8_t SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);
constexpr uint32_t SONG_MS = 180000;

// Copy of the main app's train_plan.h data (a sketch cannot include files outside its folder).
struct Stage {
    uint8_t zone;
    uint16_t seconds;
};
struct Plan {
    const char* name;
    Stage stages[5];
};
const Plan PLANS[] = {
    {"Train 11m", {{2, 180}, {4, 120}, {2, 120}, {5, 60}, {1, 180}}},
    {"Train 5m", {{2, 60}, {4, 60}, {2, 60}, {5, 60}, {1, 60}}},
};
constexpr uint8_t STAGE_COUNT = 5;
const char* const STAGE_LABEL[STAGE_COUNT] = {"Warm up", "Fast", "Recover", "Faster", "Cool down"};
constexpr uint8_t HR_MAX = 190;  // demo user, age 30
const char* const DEMO_LABEL[5] = {"Rest", "Walk", "Jog", "Run", "Sprint"};

struct Button {
    uint8_t pin;
    uint8_t history;  // last 8 samples, 1 = pressed
    bool down;
    uint32_t since;
    bool longSent;
};

GFXcanvas16 canvas(W, H);
Button buttons[3] = {{PIN_UP}, {PIN_DOWN}, {PIN_OK}};

Screen screen = Screen::Tiles;
uint8_t tile = TILE_NOW;
uint8_t rowCursor = 0;
uint8_t powerCursor = 0;  // 0 = Cancel, 1 = Off
uint8_t mode = MODE_NONE;
uint8_t volume = 60;
bool playing = false;
uint8_t song = 0;
uint32_t songMs = 0;
uint8_t trainPlan = 0xFF;  // 0xFF = never started
uint32_t trainMs = 0;
bool trainDone = false;
uint8_t heartRate = 92;
uint8_t demoLevel = 0;
bool dirty = true;
bool redrawNow = false;  // redraw even with the finger down (touch feedback)
bool touchActive = false;  // finger down: hold off redraws so a swipe is not cut by a frame push
uint8_t pendingRow = 0xFF;  // Mode list row picked, applied at pendingAt so the highlight is seen first
uint32_t pendingAt = 0;
// uiTask owns input and state changes; displayTask draws and pushes frames. stateLock covers the UI state
// while either side uses it, spiLock covers SPI1, which the panel and the touch chip share.
SemaphoreHandle_t stateLock = nullptr, spiLock = nullptr;
TaskHandle_t displayTaskHandle = nullptr;
// temporary freeze diagnosis, printed once a second by loop(): progress counters and the last step reached
volatile uint32_t uiLoops = 0, dispFrames = 0;
volatile uint8_t uiStage = 0, dispStage = 0;
int16_t touchAx = 0, touchAy = 0;  // last raw sample after the axis swap (touch test)

// ---------- ST7735S ----------

void tftCommand(uint8_t cmd, const uint8_t* data = nullptr, uint8_t len = 0)
{
    digitalWrite(PIN_TFT_DC, LOW);
    SPI1.transfer(cmd);
    digitalWrite(PIN_TFT_DC, HIGH);
    for (uint8_t i = 0; i < len; ++i) SPI1.transfer(data[i]);
}

void tftInit()
{
    SPI1.setDefaultFrequency(TFT_HZ);
    SPI1.begin();
    pinMode(PIN_TFT_DC, OUTPUT);
    pinMode(PIN_TFT_RST, OUTPUT);
    digitalWrite(PIN_TFT_RST, LOW);
    delay(20);
    digitalWrite(PIN_TFT_RST, HIGH);
    delay(150);
    tftCommand(0x11);  // SLPOUT
    delay(120);
    const uint8_t colmod = 0x05;  // 16-bit pixels
    tftCommand(0x3A, &colmod, 1);
    tftCommand(0x36, &TFT_MADCTL, 1);
    tftCommand(0x20);  // INVOFF
    tftCommand(0x13);  // NORON
    tftCommand(0x29);  // DISPON
}

void tftPush()
{
    const uint8_t caset[] = {0, TFT_COL_OFFSET, 0, static_cast<uint8_t>(TFT_COL_OFFSET + W - 1)};
    const uint8_t raset[] = {0, TFT_ROW_OFFSET, 0, static_cast<uint8_t>(TFT_ROW_OFFSET + H - 1)};
    dispStage = 4;  // waiting for spiLock in tftPush
    xSemaphoreTake(spiLock, portMAX_DELAY);
    dispStage = 5;  // pushing the frame
    tftCommand(0x2A, caset, 4);
    tftCommand(0x2B, raset, 4);
    tftCommand(0x2C);  // RAMWR, DC stays high for the pixels
    // One interrupt-driven stream for the whole frame instead of a call per byte (~65 ms -> about the bus
    // time). The stream returns early, so wait for the driver to finish feeding the FIFO, then for the
    // shifter to go idle before DC may change again.
    static uint8_t frame[W * H * 2];
    const uint16_t* px = canvas.getBuffer();
    for (int32_t i = 0; i < W * H; ++i) {  // panel wants big-endian RGB565
        frame[2 * i] = static_cast<uint8_t>(px[i] >> 8);
        frame[2 * i + 1] = static_cast<uint8_t>(px[i]);
    }
    if (spi_master_write_stream(&spi_obj1, reinterpret_cast<char*>(frame), sizeof(frame)) != 0) {
        for (uint32_t i = 0; i < sizeof(frame); ++i) SPI1.transfer(frame[i]);  // driver refused: slow path
    } else {
        const uint32_t start = millis();
        while ((spi_obj1.state & SPI_STATE_TX_BUSY) && millis() - start < 200) {
        }
        while (spi_busy(&spi_obj1) && millis() - start < 200) {
        }
    }
    dispStage = 6;  // frame pushed
    xSemaphoreGive(spiLock);
}

void tftSleep(bool sleep)
{
    xSemaphoreTake(spiLock, portMAX_DELAY);
    if (sleep) {
        tftCommand(0x28);  // DISPOFF; backlight is hard-wired to 3.3V, so the panel stays lit black
        tftCommand(0x10);  // SLPIN
    } else {
        tftCommand(0x11);
        delay(120);
        tftCommand(0x29);
    }
    xSemaphoreGive(spiLock);
}

// ---------- XPT2046 ----------

uint16_t touchChannel(uint8_t cmd)
{
    spi_master_write(&spi_obj1, cmd);
    const uint8_t hi = spi_master_write(&spi_obj1, 0);
    const uint8_t lo = spi_master_write(&spi_obj1, 0);
    return static_cast<uint16_t>(((hi << 8) | lo) >> 3);
}

// The board core's map() returns x unchanged when in_max < in_min, so the reversed Y axis needs its own math.
int16_t touchToScreen(int16_t raw, int16_t rawAt0, int16_t rawAtMax, int16_t maxPx)
{
    const int32_t px = static_cast<int32_t>(raw - rawAt0) * maxPx / (rawAtMax - rawAt0);
    return constrain(px, 0, maxPx);
}

// Returns false when the pressure is too light to trust the position (x/y left unchanged).
bool touchRead(int16_t& x, int16_t& y)
{
    // SPI1 hardware CS also selects the LCD: a NOP command makes it ignore the touch bytes as parameters.
    uiStage = 10;  // waiting for spiLock in touchRead
    xSemaphoreTake(spiLock, portMAX_DELAY);
    uiStage = 11;  // touch SPI transfer
    tftCommand(0x00);
    SPI1.beginTransaction(SPISettings(TOUCH_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_TOUCH_CS, LOW);
    const uint16_t z1 = touchChannel(0xB1);
    // Two samples per axis: a finger landing or lifting gives readings that disagree (they showed up as
    // taps on the bottom edge during fast swipes), so such a sample is dropped instead of averaged in.
    const uint16_t y1 = touchChannel(0x91), y2 = touchChannel(0x91);
    const uint16_t x1 = touchChannel(0xD1), x2 = touchChannel(0xD0);  // last one PD=00: pen IRQ back on
    digitalWrite(PIN_TOUCH_CS, HIGH);
    SPI1.beginTransaction(SPISettings(TFT_HZ, MSBFIRST, SPI_MODE0));
    xSemaphoreGive(spiLock);
    uiStage = 12;  // touch read done
    if (z1 < TOUCH_Z_MIN || abs(x1 - x2) > TOUCH_JITTER_RAW || abs(y1 - y2) > TOUCH_JITTER_RAW) return false;
    const uint16_t rawX = (x1 + x2) / 2, rawY = (y1 + y2) / 2;

    const int16_t ax = TOUCH_SWAP_XY ? rawY : rawX;
    const int16_t ay = TOUCH_SWAP_XY ? rawX : rawY;
    touchAx = ax;
    touchAy = ay;
    x = touchToScreen(ax, TOUCH_X_MIN, TOUCH_X_MAX, W - 1);
    y = touchToScreen(ay, TOUCH_Y_MIN, TOUCH_Y_MAX, H - 1);
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 300) {
        lastLog = millis();
        Serial.print("touch raw x=");
        Serial.print(rawX);
        Serial.print(" y=");
        Serial.print(rawY);
        Serial.print(" z1=");
        Serial.print(z1);
        Serial.print(" -> ");
        Serial.print(x);
        Serial.print(",");
        Serial.println(y);
    }
    return true;
}

// Gestures are decided on release: a long enough move is a swipe, otherwise a tap. Holding still for
// LONG_PRESS_MS sends Back right away, without waiting for the release.
Input pollTouch(uint32_t now)
{
    static bool down = false;
    static uint32_t since = 0;
    static int16_t firstX, firstY, tapX, tapY, endX, endY, smoothX, smoothY;
    static bool longSent = false;
    static uint8_t samples = 0;  // trusted position samples in this press
    static int32_t sumX = 0, sumY = 0;  // samples from the 3rd on, for the tap position
    static uint8_t nSum = 0;
    static uint32_t highSince = 0;
    const Input none = {Ev::None, 0, 0};

    // PENIRQ decides "touched" and is read before any SPI: each conversion pulls it low for a while, so the
    // chip is only read while it is already low (reading it every poll kept it low forever). A very light
    // touch does not pull PENIRQ low through its ~50 kOhm pull-up; that is a limit of this panel.
    const bool pen = digitalRead(PIN_TOUCH_IRQ) == LOW;
    int16_t x, y;
    if (pen && touchRead(x, y)) {
        // Swipes use the raw first/last samples so a quick flick is not shortened by filtering. The first
        // readings of a press read low on Y while the finger is still pressing in (every tap landed on the
        // bottom edge), so the tap position is the average from the 3rd sample on, the same way the
        // calibration was measured. The hold check uses a smoothed position.
        if (samples == 0) firstX = tapX = smoothX = x, firstY = tapY = smoothY = y;
        if (samples >= 2) {
            sumX += x;
            sumY += y;
            ++nSum;
            tapX = sumX / nSum;
            tapY = sumY / nSum;
        }
        smoothX = (smoothX * 3 + x) / 4;
        smoothY = (smoothY * 3 + y) / 4;
        endX = x;
        endY = y;
        if (samples < 255) ++samples;
        if (samples == 4) return {Ev::Press, tapX, tapY};  // finger settled: lets lists highlight right away
    }
    if (pen || !down) highSince = now;
    // A press counts at once; the release waits TOUCH_RELEASE_MS (time, not poll count, so the loop speed
    // does not matter).
    if (pen != down && (pen || now - highSince >= TOUCH_RELEASE_MS)) {
        down = pen;
        touchActive = down;
        if (down) {
            since = now;
            longSent = false;
        } else {
            const uint8_t had = samples;
            samples = 0;
            sumX = sumY = nSum = 0;
            if (longSent || had == 0) return none;  // a light tap may give only 1-2 samples; still a tap
            // A quick flick travels less before the finger lifts, so it needs a shorter distance.
            const int16_t minPx = now - since < FLICK_MS ? FLICK_MIN_PX : SWIPE_MIN_PX;
            const int16_t dx = endX - firstX, dy = endY - firstY;
            if (abs(dx) >= minPx && abs(dx) >= abs(dy)) return {dx < 0 ? Ev::SwipeLeft : Ev::SwipeRight, 0, 0};
            if (abs(dy) >= minPx) return {dy < 0 ? Ev::SwipeUp : Ev::SwipeDown, 0, 0};
            return {Ev::Tap, tapX, tapY};
        }
    }
    if (down && samples > 0 && !longSent && now - since >= LONG_PRESS_MS && abs(smoothX - tapX) < TAP_SLOP_PX &&
        abs(smoothY - tapY) < TAP_SLOP_PX) {
        longSent = true;
        return {Ev::Back, 0, 0};
    }
    return none;
}

// ---------- buttons ----------

// Up/Down fire on press; OK fires Ok on a short release or Back once held for LONG_PRESS_MS.
Ev pollButtons(uint32_t now)
{
    Ev ev = Ev::None;
    for (uint8_t i = 0; i < 3; ++i) {
        Button& b = buttons[i];
        b.history = static_cast<uint8_t>((b.history << 1) | (digitalRead(b.pin) == LOW));
        if ((b.history & 0x0F) == 0x0F && !b.down) {
            b.down = true;
            b.since = now;
            b.longSent = false;
            if (i == 0) ev = Ev::Up;
            if (i == 1) ev = Ev::Down;
        } else if ((b.history & 0x0F) == 0 && b.down) {
            b.down = false;
            if (i == 2 && !b.longSent) ev = Ev::Ok;
        } else if (i == 2 && b.down && !b.longSent && now - b.since >= LONG_PRESS_MS) {
            b.longSent = true;
            ev = Ev::Back;
        }
    }
    return ev;
}

// ---------- fake playback ----------

uint8_t closestSong(uint8_t bpm)
{
    uint8_t best = 0;
    for (uint8_t i = 1; i < SONG_COUNT; ++i)
        if (abs(SONGS[i].bpm - bpm) < abs(SONGS[best].bpm - bpm)) best = i;
    return best;
}

uint8_t zoneBpm(uint8_t zone)
{
    static const uint8_t MID_PERCENT[5] = {55, 65, 75, 85, 95};
    return static_cast<uint8_t>((HR_MAX * MID_PERCENT[zone - 1] + 50) / 100);
}

// 1..5, same edges as analysisTask: <60 / 60-70 / 70-80 / 80-90 / >=90 %HRmax.
uint8_t heartZone()
{
    const uint16_t pct = heartRate * 100u / HR_MAX;
    return pct < 60 ? 1 : pct < 70 ? 2 : pct < 80 ? 3 : pct < 90 ? 4 : 5;
}

// Stage index at trainMs (STAGE_COUNT once finished); remainingS = seconds left in it.
uint8_t trainStage(uint16_t* remainingS)
{
    uint32_t endMs = 0;
    for (uint8_t i = 0; i < STAGE_COUNT; ++i) {
        endMs += PLANS[trainPlan].stages[i].seconds * 1000u;
        if (trainMs < endMs) {
            if (remainingS) *remainingS = (endMs - trainMs + 999) / 1000;
            return i;
        }
    }
    if (remainingS) *remainingS = 0;
    return STAGE_COUNT;
}

uint8_t targetBpm()
{
    if (mode == MODE_TRAIN) {
        const uint8_t s = trainStage(nullptr);
        return s < STAGE_COUNT ? zoneBpm(PLANS[trainPlan].stages[s].zone) : 0;
    }
    return constrain(heartRate, 80, 175);
}

void tick(uint32_t dt, uint32_t now)
{
    static uint32_t lastSecond = 0, lastDemo = 0;
    if (now - lastSecond >= 1000) {
        lastSecond = now;
        heartRate = constrain(heartRate + random(-3, 5) - (heartRate > 150 ? 4 : 0), 70, 185);
        if (screen == Screen::Tiles) dirty = true;
    }
    if (now - lastDemo >= 4000) {  // fake intensity change (real one needs ~3 s hold)
        lastDemo = now;
        demoLevel = random(0, 5);
    }
    if (!playing) return;

    if (mode == MODE_TRAIN) {
        const uint8_t before = trainStage(nullptr);
        trainMs += dt;
        const uint8_t after = trainStage(nullptr);
        if (after == STAGE_COUNT) {  // plan finished counts as a manual stop
            playing = false;
            trainDone = true;
            dirty = true;
            Serial.println("train done");
            return;
        }
        if (after != before) {  // new stage -> new song immediately
            song = closestSong(targetBpm());
            songMs = 0;
        }
    }
    songMs += dt;
    if (songMs >= SONG_MS) {
        song = mode == MODE_DEMO ? song : closestSong(targetBpm());
        songMs = 0;
    }
}

// ---------- events ----------

void startMode(uint8_t m)
{
    mode = m;
    playing = true;
    song = closestSong(targetBpm());
    songMs = 0;
    screen = Screen::Tiles;
    tile = TILE_NOW;
}

void openModeList()
{
    rowCursor = mode == MODE_TRAIN ? 1 + trainPlan : mode == MODE_DEMO ? 3 : 0;
    screen = Screen::ModeList;
}

// Play/pause, or pick a mode when there is nothing to resume.
void togglePlay()
{
    if (mode == MODE_NONE || (mode == MODE_TRAIN && trainDone)) openModeList();
    else playing = !playing;
}

void changeVolume(int8_t delta)
{
    volume = constrain(volume + delta, 0, 100);
}

void selectRow(uint8_t row)
{
    if (row == 0) startMode(MODE_ENJOY);
    if (row == 1 || row == 2) {
        const uint8_t plan = row - 1;
        // Same plan resumes where it stopped; another plan or a finished one starts over.
        if (plan != trainPlan || trainDone) {
            trainMs = 0;
            trainDone = false;
        }
        trainPlan = plan;
        startMode(MODE_TRAIN);
    }
    if (row == 3) startMode(MODE_DEMO);
    if (row == 4) {  // manual stop
        playing = false;
        screen = Screen::Tiles;
    }
}

uint8_t rowAt(int16_t y)
{
    return y < ROW_TOP ? 0 : min((y - ROW_TOP) / ROW_H, ROW_COUNT - 1);
}

bool onBack(const Input& in)
{
    return in.x < 50 && in.y < 14;
}

// Show the chosen row for a moment before leaving the list, so the user sees what was picked.
void confirmRow(uint8_t row)
{
    rowCursor = row;
    redrawNow = true;
    pendingRow = row;
    pendingAt = millis() + 200;
}

void handle(const Input& in)
{
    const Ev ev = in.ev;
    switch (screen) {
    case Screen::Tiles:
        if ((ev == Ev::Up || ev == Ev::SwipeRight) && tile > 0) --tile;
        if ((ev == Ev::Down || ev == Ev::SwipeLeft) && tile < TILE_COUNT - 1) ++tile;
        if (ev == Ev::SwipeDown || (ev == Ev::Back && tile == TILE_NOW)) screen = Screen::Quick;
        if (ev == Ev::Back && tile != TILE_NOW) tile = TILE_NOW;
        if (ev == Ev::Ok || ev == Ev::Tap) {
            if (tile == TILE_WORKOUT) openModeList();
            else if (tile == TILE_MUSIC && ev == Ev::Tap && in.x < 55) changeVolume(-5);
            else if (tile == TILE_MUSIC && ev == Ev::Tap && in.x > 105) changeVolume(5);
            else togglePlay();
        }
        break;
    case Screen::Quick:
        if (ev == Ev::Up) changeVolume(5);
        if (ev == Ev::Down) changeVolume(-5);
        if (ev == Ev::Back || ev == Ev::SwipeUp) screen = Screen::Tiles;
        if (ev == Ev::Ok || (ev == Ev::Tap && in.y >= 66)) {
            powerCursor = 0;  // default to Cancel against a stray press
            screen = Screen::Power;
        }
        if (ev == Ev::Tap && in.y < 66) {  // volume row: -/+ at the ends, slider in between
            if (in.x < 36) changeVolume(-5);
            else if (in.x > W - 36) changeVolume(5);
            else volume = constrain(map(in.x, 40, W - 40, 0, 100), 0, 100);
        }
        break;
    case Screen::ModeList:
        if (pendingRow != 0xFF) break;  // a choice is already on its way
        // All rows fit, so there is no scrolling: touch picks a row directly, swipe right / "< back" leave.
        if (ev == Ev::Up) rowCursor = (rowCursor + ROW_COUNT - 1) % ROW_COUNT;
        if (ev == Ev::Down) rowCursor = (rowCursor + 1) % ROW_COUNT;
        // Only the "< back" corner leaves; the rest of the header counts as the first row, since taps
        // measured a few px above where people aim (Enjoy taps landed at y 14-18).
        if (ev == Ev::Back || ev == Ev::SwipeRight || (ev == Ev::Tap && onBack(in))) screen = Screen::Tiles;
        if (ev == Ev::Press && !onBack(in)) {
            rowCursor = rowAt(in.y);
            redrawNow = true;
        }
        if (ev == Ev::Ok) confirmRow(rowCursor);
        if (ev == Ev::Tap && !onBack(in)) confirmRow(rowAt(in.y));
        break;
    case Screen::Power:
        if (ev == Ev::Up || ev == Ev::Down || ev == Ev::SwipeLeft || ev == Ev::SwipeRight) powerCursor ^= 1;
        if (ev == Ev::Tap) powerCursor = in.x >= W / 2;
        if (ev == Ev::Back || ev == Ev::SwipeUp || ((ev == Ev::Ok || ev == Ev::Tap) && powerCursor == 0))
            screen = Screen::Tiles;
        if ((ev == Ev::Ok || ev == Ev::Tap) && powerCursor == 1) {
            playing = false;
            screen = Screen::Standby;
            tftSleep(true);
        }
        break;
    case Screen::Standby:
        if (ev == Ev::Back) {  // wake keeps settings but playback stays stopped
            tftSleep(false);
            screen = Screen::Tiles;
            tile = TILE_NOW;
        }
        break;
    }
    dirty = true;
}

// ---------- drawing ----------

void centerText(const char* s, int16_t y, uint8_t size, uint16_t color, int16_t cx = W / 2)
{
    int16_t x1, y1;
    uint16_t w, h;
    canvas.setTextSize(size);
    canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    canvas.setTextColor(color);
    canvas.setCursor(cx - w / 2, y);
    canvas.print(s);
}

void drawHeart(int16_t cx, int16_t cy, uint8_t r, uint16_t c)
{
    canvas.fillCircle(cx - r, cy - r / 2, r, c);
    canvas.fillCircle(cx + r, cy - r / 2, r, c);
    canvas.fillTriangle(cx - 2 * r, cy, cx + 2 * r, cy, cx, cy + 2 * r, c);
}

void drawPlayGlyph(int16_t cx, int16_t cy, uint8_t s, bool pause, uint16_t c)
{
    if (pause) {
        canvas.fillRect(cx - s, cy - s, s * 2 / 3, s * 2, c);
        canvas.fillRect(cx + s / 3, cy - s, s * 2 / 3, s * 2, c);
    } else {
        canvas.fillTriangle(cx - s * 2 / 3, cy - s, cx - s * 2 / 3, cy + s, cx + s, cy, c);
    }
}

void drawProgress(int16_t x, int16_t y, int16_t w, uint32_t pos, uint32_t total, uint16_t c)
{
    canvas.fillRoundRect(x, y, w, 4, 2, C_PANEL2);
    canvas.fillRoundRect(x, y, static_cast<int16_t>(w * min(pos, total) / total), 4, 2, c);
}

// Clock-like strip shared by every tile: mode on the left, play state, battery on the right.
void drawStatusBar()
{
    canvas.setTextSize(1);
    canvas.setTextColor(MODE_COLOR[mode]);
    canvas.setCursor(4, 3);
    canvas.print(mode == MODE_TRAIN ? PLANS[trainPlan].name : MODE_NAME[mode]);
    if (mode != MODE_NONE) drawPlayGlyph(W / 2, 6, 3, playing, C_TEXT);
    canvas.drawRect(W - 20, 2, 15, 8, C_DIM);  // fake battery 75%
    canvas.fillRect(W - 5, 4, 2, 4, C_DIM);
    canvas.fillRect(W - 18, 4, 8, 4, C_GREEN);
}

void drawPageDots()
{
    for (uint8_t i = 0; i < TILE_COUNT; ++i) {
        const int16_t x = W / 2 - (TILE_COUNT - 1) * 6 + i * 12;
        if (i == tile) canvas.fillRoundRect(x - 5, 122, 10, 4, 2, C_TEXT);
        else canvas.fillCircle(x, 123, 2, C_PANEL2);
    }
}

const char* songTitle()
{
    static char buf[16];
    if (mode == MODE_DEMO) {
        snprintf(buf, sizeof(buf), "DEMO_%02u.mp3", demoLevel);
        return buf;
    }
    return SONGS[song].title;
}

void drawNowTile()
{
    char buf[16];
    const uint8_t zone = heartZone();
    drawHeart(30, 44, 11, C_RED);
    snprintf(buf, sizeof(buf), "%u", heartRate);
    centerText(buf, 24, 5, C_TEXT, 106);
    centerText("BPM", 66, 1, C_DIM, 106);
    // Five-segment zone gauge like a watch's heart-rate complication.
    for (uint8_t i = 0; i < 5; ++i) {
        const bool on = i + 1 == zone;
        canvas.fillRoundRect(8 + i * 29, on ? 78 : 80, 27, on ? 7 : 3, 2, on ? ZONE_COLOR[i] : C_PANEL2);
    }
    snprintf(buf, sizeof(buf), "Z%u %s", zone, ZONE_NAME[zone - 1]);
    centerText(buf, 89, 1, ZONE_COLOR[zone - 1]);
    if (mode == MODE_NONE) {
        centerText("Tap to start", 104, 1, C_DIM);
        return;
    }
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT);
    canvas.setCursor(8, 101);
    canvas.print(mode == MODE_TRAIN && trainDone ? "Workout done" : songTitle());
    drawProgress(8, 112, W - 16, songMs, SONG_MS, MODE_COLOR[mode]);
}

void drawWorkoutTile()
{
    char buf[16];
    if (mode == MODE_NONE) {
        canvas.fillRoundRect(40, 26, 80, 50, 12, C_PANEL);
        centerText("+", 34, 4, C_TEXT);
        centerText("Pick a mode", 88, 1, C_TEXT);
        centerText("tap", 100, 1, C_DIM);
    } else if (mode == MODE_TRAIN && trainDone) {
        centerText("DONE!", 34, 3, C_ORANGE);
        centerText("Great workout", 66, 1, C_TEXT);
        centerText("tap: pick a mode", 90, 1, C_DIM);
    } else if (mode == MODE_TRAIN) {
        uint16_t remaining = 0;
        const uint8_t s = trainStage(&remaining);
        const uint8_t zone = PLANS[trainPlan].stages[s].zone;
        canvas.fillRoundRect(8, 20, 44, 40, 8, ZONE_COLOR[zone - 1]);
        snprintf(buf, sizeof(buf), "Z%u", zone);
        centerText(buf, 33, 2, C_TEXT, 30);
        centerText(STAGE_LABEL[s], 22, 1, C_TEXT, 106);
        snprintf(buf, sizeof(buf), "%u:%02u", remaining / 60, remaining % 60);
        centerText(buf, 36, 3, C_TEXT, 106);
        // Whole plan as a strip, each block as wide as its stage; past stages dimmed.
        uint32_t total = 0;
        for (const Stage& st : PLANS[trainPlan].stages) total += st.seconds;
        int16_t x = 8;
        for (uint8_t i = 0; i < STAGE_COUNT; ++i) {
            const Stage& st = PLANS[trainPlan].stages[i];
            const int16_t w = (W - 16) * st.seconds / total;
            canvas.fillRect(x, i == s ? 68 : 71, w - 1, i == s ? 10 : 4, i < s ? C_PANEL2 : ZONE_COLOR[st.zone - 1]);
            x += w;
        }
        snprintf(buf, sizeof(buf), "Song %u BPM", targetBpm());
        centerText(buf, 90, 1, C_DIM);
        centerText("tap: change mode", 104, 1, C_PANEL2);
    } else if (mode == MODE_DEMO) {
        const uint16_t lc = ZONE_COLOR[demoLevel];
        canvas.fillCircle(36, 54, 28, lc);
        snprintf(buf, sizeof(buf), "%u", demoLevel);
        centerText(buf, 40, 4, C_TEXT, 37);
        for (uint8_t i = 0; i < 5; ++i) {
            const int16_t h = 8 + i * 8;
            canvas.fillRoundRect(80 + i * 13, 64 - h, 10, h, 2, i <= demoLevel ? lc : C_PANEL2);
        }
        centerText(DEMO_LABEL[demoLevel], 74, 2, lc, 112);
        centerText("tap: change mode", 104, 1, C_PANEL2);
    } else {  // enjoy
        centerText("Target", 24, 1, C_DIM);
        snprintf(buf, sizeof(buf), "%u", targetBpm());
        centerText(buf, 38, 4, C_RED);
        centerText("song BPM follows your HR", 76, 1, C_TEXT);
        centerText("tap: change mode", 104, 1, C_PANEL2);
    }
}

void drawMusicTile()
{
    char buf[16];
    const uint16_t c = MODE_COLOR[mode];
    centerText(mode == MODE_NONE ? "No music" : songTitle(), 20, 1, C_TEXT);
    if (mode != MODE_NONE && mode != MODE_DEMO) {
        snprintf(buf, sizeof(buf), "%u BPM", SONGS[song].bpm);
        centerText(buf, 31, 1, C_DIM);
    }
    canvas.fillCircle(30, 66, 14, C_PANEL);  // volume -
    canvas.fillRect(24, 65, 12, 3, C_TEXT);
    canvas.fillCircle(W - 30, 66, 14, C_PANEL);  // volume +
    canvas.fillRect(W - 36, 65, 12, 3, C_TEXT);
    canvas.fillRect(W - 31, 60, 3, 13, C_TEXT);
    canvas.fillCircle(W / 2, 66, 22, c);
    drawPlayGlyph(W / 2 + (playing ? 0 : 2), 66, 9, playing, C_TEXT);
    snprintf(buf, sizeof(buf), "Vol %u", volume);
    centerText(buf, 94, 1, C_DIM);
    drawProgress(8, 110, W - 16, songMs, SONG_MS, c);
}

void drawTiles()
{
    drawStatusBar();
    if (tile == TILE_NOW) drawNowTile();
    if (tile == TILE_WORKOUT) drawWorkoutTile();
    if (tile == TILE_MUSIC) drawMusicTile();
    drawPageDots();
}

// Pull-down panel, like a watch's quick settings.
void drawQuick()
{
    char buf[8];
    canvas.fillRoundRect(0, -8, W, H - 4, 10, C_PANEL);
    canvas.fillRoundRect(W / 2 - 12, H - 9, 24, 3, 1, C_DIM);  // grab handle
    centerText("Quick settings", 6, 1, C_DIM);
    canvas.fillCircle(18, 40, 13, C_PANEL2);
    canvas.fillRect(12, 39, 12, 3, C_TEXT);
    canvas.fillCircle(W - 18, 40, 13, C_PANEL2);
    canvas.fillRect(W - 24, 39, 12, 3, C_TEXT);
    canvas.fillRect(W - 19, 34, 3, 13, C_TEXT);
    canvas.fillRoundRect(40, 36, W - 80, 8, 4, C_PANEL2);
    canvas.fillRoundRect(40, 36, (W - 80) * volume / 100, 8, 4, C_BLUE);
    snprintf(buf, sizeof(buf), "Vol %u", volume);
    centerText(buf, 50, 1, C_TEXT);
    canvas.fillRoundRect(20, 70, W - 40, 34, 17, C_RED);
    for (int16_t r = 7; r <= 8; ++r) canvas.drawCircle(46, 88, r, C_TEXT);  // power glyph
    canvas.fillRect(43, 76, 7, 6, C_RED);
    canvas.fillRect(45, 77, 3, 9, C_TEXT);
    centerText("Power off", 84, 1, C_TEXT, 92);
}

void drawModeList()
{
    centerText("Mode", 6, 1, C_DIM);
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT);
    canvas.setCursor(6, 6);
    canvas.print("< back");
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        const int16_t y = ROW_TOP + i * ROW_H;
        const uint16_t c = i == 4 ? C_GRAY : MODE_COLOR[i == 0 ? MODE_ENJOY : i == 3 ? MODE_DEMO : MODE_TRAIN];
        const bool active = (i == 0 && mode == MODE_ENJOY) || (i == 3 && mode == MODE_DEMO) ||
                            ((i == 1 || i == 2) && mode == MODE_TRAIN && trainPlan == i - 1);
        if (i == rowCursor) canvas.fillRoundRect(6, y, W - 12, ROW_H - 2, 9, C_PANEL2);
        canvas.fillCircle(18, y + 9, 5, c);
        canvas.setTextSize(1);
        canvas.setTextColor(C_TEXT);
        canvas.setCursor(30, y + 5);
        canvas.print(ROW_LABEL[i]);
        if (active) {
            canvas.setTextColor(c);
            canvas.setCursor(W - 50, y + 5);
            canvas.print(playing ? "playing" : "paused");
        }
    }
}

void drawPower()
{
    centerText("Power off?", 26, 2, C_TEXT);
    centerText("tap or OK to confirm", 50, 1, C_DIM);
    const char* const labels[2] = {"Cancel", "Off"};
    for (uint8_t i = 0; i < 2; ++i) {
        const int16_t x = 10 + i * 74;
        const uint16_t c = i == 1 ? C_RED : C_PANEL2;
        if (i == powerCursor) canvas.fillRoundRect(x, 72, 66, 34, 17, c);
        else canvas.drawRoundRect(x, 72, 66, 34, 17, c);
        centerText(labels[i], 85, 1, C_TEXT, x + 33);
    }
}

// Draws the current state into the canvas; false when nothing should be shown (standby).
bool drawFrame()
{
    if (screen == Screen::Standby) return false;
    canvas.fillScreen(C_BG);
    switch (screen) {
    case Screen::Tiles: drawTiles(); break;
    case Screen::Quick: drawQuick(); break;
    case Screen::ModeList: drawModeList(); break;
    case Screen::Power: drawPower(); break;
    case Screen::Standby: break;
    }
    return true;
}

// Waits for uiTask to ask for a frame. The state is only locked while drawing into the canvas, so input
// keeps being polled during the ~4 ms push.
void displayTask(void*)
{
    for (;;) {
        dispStage = 1;  // idle, waiting for a frame request
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t start = millis();
        dispStage = 2;  // waiting for stateLock
        xSemaphoreTake(stateLock, portMAX_DELAY);
        dispStage = 3;  // drawing into the canvas
        const bool show = drawFrame();
        xSemaphoreGive(stateLock);
        ++dispFrames;
        if (!show) continue;
        tftPush();
        static uint8_t logged = 0;
        if (logged < 3) {
            ++logged;
            Serial.print("frame ms=");
            Serial.println(millis() - start);
        }
    }
}

// ---------- touch test ----------

// Tap the five numbered targets in order; each release prints the averaged raw value (after the axis swap)
// and where it landed on screen, which is what the TOUCH_*_MIN/MAX constants are fitted from.
const int16_t TARGETS[5][2] = {{10, 10}, {W - 11, 10}, {10, H - 11}, {W - 11, H - 11}, {W / 2, H / 2}};

void touchTestLoop()
{
    static bool started = false, down = false;
    static uint32_t sumAx = 0, sumAy = 0, lastPush = 0;
    static uint16_t n = 0;
    static int16_t lastX = 0, lastY = 0;
    static int16_t minAx = 4095, maxAx = 0, minAy = 4095, maxAy = 0;  // whole-screen extent of all strokes
    if (!started) {
        started = true;
        canvas.fillScreen(C_BG);
        centerText("Touch test: tap 1-5", 56, 1, C_DIM);
        for (uint8_t i = 0; i < 5; ++i) {
            const int16_t tx = TARGETS[i][0], ty = TARGETS[i][1];
            canvas.drawFastHLine(tx - 8, ty, 17, C_TEXT);
            canvas.drawFastVLine(tx, ty - 8, 17, C_TEXT);
            canvas.setTextColor(C_ORANGE);
            canvas.setCursor(tx + (tx < W / 2 ? 5 : -10), ty + (ty < H / 2 ? 4 : -11));
            canvas.print(i + 1);
        }
        tftPush();
    }
    const bool pen = digitalRead(PIN_TOUCH_IRQ) == LOW;
    int16_t x, y;
    if (pen && touchRead(x, y)) {
        sumAx += touchAx;
        sumAy += touchAy;
        minAx = min(minAx, touchAx);
        maxAx = max(maxAx, touchAx);
        minAy = min(minAy, touchAy);
        maxAy = max(maxAy, touchAy);
        ++n;
        lastX = x;
        lastY = y;
        down = true;
        canvas.fillCircle(x, y, 1, C_GREEN);  // live trail of where the touch maps to
        if (millis() - lastPush > 80) {
            lastPush = millis();
            tftPush();
        }
    } else if (!pen && down) {
        down = false;
        char line[80];
        snprintf(line, sizeof(line), "cal ax=%lu ay=%lu n=%u -> %d,%d", static_cast<unsigned long>(sumAx / n),
                 static_cast<unsigned long>(sumAy / n), n, lastX, lastY);
        Serial.println(line);
        snprintf(line, sizeof(line), "range ax=%d..%d ay=%d..%d", minAx, maxAx, minAy, maxAy);
        Serial.println(line);
        canvas.fillCircle(lastX, lastY, 3, C_RED);
        tftPush();
        sumAx = sumAy = n = 0;
    }
    delay(5);
}

// ---------- main ----------

void setup()
{
    Serial.begin(115200);
    stateLock = xSemaphoreCreateMutex();
    spiLock = xSemaphoreCreateMutex();
    for (Button& b : buttons) pinMode(b.pin, INPUT_PULLUP);
    pinMode(PIN_TOUCH_IRQ, INPUT_PULLUP);
    pinMode(PIN_TOUCH_CS, OUTPUT);
    digitalWrite(PIN_TOUCH_CS, HIGH);
    tftInit();
    // PENIRQ only works after a conversion that ends with PD=00; the chip may come up (or be clocked by
    // boot-time SPI noise while CS2 floats) with it off, and then no touch is ever seen.
    int16_t tx, ty;
    touchRead(tx, ty);
    randomSeed(analogRead(2));
    Serial.println("tft_card_ui ready: swipe/tap or Up/Down/OK, hold = back");
    Serial.print("PEN (D15) idle level = ");  // must be 1 without touching, else the pin is not working as GPIO
    Serial.println(digitalRead(PIN_TOUCH_IRQ));
    // Input runs above the display so a frame push never delays a touch poll.
    if (stateLock == nullptr || spiLock == nullptr ||
        xTaskCreate(displayTask, "display", 2048, nullptr, 2, &displayTaskHandle) != pdPASS ||
        xTaskCreate(uiTask, "ui", 2048, nullptr, 3, nullptr) != pdPASS) {
        Serial.println("task create failed; halting");
        while (1) {
        }
    }
}

// Input, state changes and the fake data clock; polls about every 1 ms (light taps hold PENIRQ ~40 ms).
void uiTask(void*)
{
    if (TOUCH_TEST) {
        for (;;) touchTestLoop();
    }
    uint32_t lastTick = millis(), lastButtons = 0;
    xTaskNotifyGive(displayTaskHandle);  // first frame
    for (;;) {
        const uint32_t now = millis();
        ++uiLoops;
        uiStage = 1;  // buttons
        Input in = {Ev::None, 0, 0};
        if (now - lastButtons >= 5) {  // 4-sample debounce stays ~20 ms however fast the poll runs
            lastButtons = now;
            in.ev = pollButtons(now);
        }
        uiStage = 2;  // touch
        if (in.ev == Ev::None && screen != Screen::Standby) in = pollTouch(now);  // standby: only long OK wakes
        uiStage = 3;  // event log
        if (in.ev != Ev::None && in.ev != Ev::Press) {  // Press precedes every tap; logging it is just noise
            Serial.print("event ");
            Serial.print(EV_NAME[static_cast<uint8_t>(in.ev)]);
            if (in.ev == Ev::Tap) {
                Serial.print(" at ");
                Serial.print(in.x);
                Serial.print(",");
                Serial.print(in.y);
            }
            Serial.println();
        }
        uiStage = 4;  // waiting for stateLock
        xSemaphoreTake(stateLock, portMAX_DELAY);
        uiStage = 5;  // handle / tick
        if (in.ev != Ev::None) handle(in);
        if (pendingRow != 0xFF && static_cast<int32_t>(now - pendingAt) >= 0) {
            Serial.print("select ");
            Serial.println(ROW_LABEL[pendingRow]);
            selectRow(pendingRow);
            pendingRow = 0xFF;
            dirty = true;
        }
        if (now - lastTick >= 100) {
            tick(now - lastTick, now);
            lastTick = now;
        }
        const bool redraw = dirty && (!touchActive || redrawNow);
        if (redraw) dirty = redrawNow = false;
        xSemaphoreGive(stateLock);
        if (redraw) xTaskNotifyGive(displayTaskHandle);
        uiStage = 6;  // sleeping
        vTaskDelay(1);
    }
}

const char* holderName(SemaphoreHandle_t lock)
{
    TaskHandle_t holder = xSemaphoreGetMutexHolder(lock);
    return holder ? pcTaskGetName(holder) : "-";
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));  // all work runs in uiTask and displayTask
    // temporary freeze diagnosis: if ui/disp stop counting, the stage and lock holders show where they stopped
    char line[112];
    snprintf(line, sizeof(line), "hb ui=%lu stage=%u disp=%lu stage=%u stateLock=%s spiLock=%s spiState=%lu",
             static_cast<unsigned long>(uiLoops), uiStage, static_cast<unsigned long>(dispFrames), dispStage,
             holderName(stateLock), holderName(spiLock), static_cast<unsigned long>(spi_obj1.state));
    Serial.println(line);
}
