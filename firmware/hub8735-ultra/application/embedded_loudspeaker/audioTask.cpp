#include "audioTask.h"
#include "music_library.h"
#include "music_bpm_analyzer.h"
#include "music_genre_classifier.h"

#include <cstring>

namespace {

constexpr const char* TAG = "AUDIO";
constexpr const char* BPM_TAG = "BPMSCAN";

AmebaFatFS fs;
MAX98357 amp;
MusicBpmAnalyzer bpmAnalyzer;
MusicGenreClassifier genreClassifier;
bool currentPlaybackAutomatic = false;
bool playbackPaused = false;
int8_t currentAutomaticSongIndex = -1;

struct BpmScanTrack {
    char filename[MUSIC_FILE_MAX];
    uint32_t fileSize;
    uint32_t fileModified;
};

// 每首歌依序：BPM（需要時）-> 曲風（需要時）。兩者互不阻擋，任一失敗該曲即計為失敗。
enum class ScanPhase : uint8_t { Start, Bpm, GenreStart, Genre };

struct BpmScanJob {
    bool requested;
    bool running;
    ScanPhase phase;
    bool fileFailed;
    BpmAnalysisMode mode;
    BpmScanTrack tracks[MUSIC_LIBRARY_MAX];
    uint8_t count;
    uint8_t index;
    uint8_t succeeded;
    uint8_t failed;
};

BpmScanJob bpmScan{};
uint32_t bpmFileStartMs = 0;
uint32_t bpmScanStartMs = 0;

const char* bpmStateName(BpmAnalysisState state)
{
    switch (state) {
    case BpmAnalysisState::Idle: return "Idle";
    case BpmAnalysisState::Queued: return "Queued";
    case BpmAnalysisState::Running: return "Running";
    case BpmAnalysisState::Complete: return "Complete";
    case BpmAnalysisState::Failed: return "Failed";
    case BpmAnalysisState::Cancelled: return "Cancelled";
    }
    return "?";
}

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

char asciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool isAllowedMusicFile(const char* filename)
{
    if (filename == nullptr) return false;
    const size_t len = strlen(filename);
    if (len < 4) return false;
    const char* extension = filename + len - 4;
    return asciiLower(extension[0]) == '.' &&
           asciiLower(extension[1]) == 'm' &&
           asciiLower(extension[2]) == 'p' &&
           asciiLower(extension[3]) == '3';
}

bool normalizeMusicFileName(const char* source, char* destination, size_t cap)
{
    if (source == nullptr) return false;
    const size_t len = strlen(source);
    if (len == 0 || len >= cap) return false;
    for (size_t i = 0; i < len; ++i) destination[i] = asciiLower(source[i]);
    destination[len] = '\0';
    return true;
}

int findLibraryEntry(const char* filename)
{
    for (size_t i = 0; i < musicLibrary.count; ++i) {
        if (strcmp(musicLibrary.entries[i].file, filename) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void countFailure()
{
    if (bpmScan.failed < 0xFF) ++bpmScan.failed;
    LOGD(BPM_TAG, "failure counted (now %u)", static_cast<unsigned>(bpmScan.failed));
}

void reportBpmAnalysis(BpmAnalysisState state, uint8_t current = 0,
                       const char* filename = nullptr)
{
    if (uiUpdateQueue == nullptr) return;
    UiUpdateElement update{};
    update.type = UiUpdateType::BpmAnalysis;
    update.timestampMs = millis();
    update.bpmAnalysisState = state;
    update.bpmAnalysisMode = bpmScan.mode;
    update.bpmAnalysisCurrent = current;
    const unsigned total = bpmScan.count + bpmScan.failed;
    update.bpmAnalysisTotal = total > 0xFF ? 0xFF : static_cast<uint8_t>(total);
    update.bpmAnalysisSucceeded = bpmScan.succeeded;
    update.bpmAnalysisFailed = bpmScan.failed;
    if (filename != nullptr) {
        strncpy(update.filename, filename, sizeof(update.filename) - 1);
    }
    LOGD(BPM_TAG, "-> UI: state=%s current=%u/%u ok=%u fail=%u %s", bpmStateName(state),
         static_cast<unsigned>(current), static_cast<unsigned>(update.bpmAnalysisTotal),
         static_cast<unsigned>(bpmScan.succeeded), static_cast<unsigned>(bpmScan.failed),
         filename != nullptr ? filename : "");
    if (xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20)) != pdPASS) {
        LOGW(BPM_TAG, "UI queue full; BPM status update dropped");
    }
}

void finishBpmScan(BpmAnalysisState state)
{
    bpmAnalyzer.cancel();
    genreClassifier.shutdown();
    bpmScan.phase = ScanPhase::Start;
    bpmScan.running = false;
    bpmScan.requested = false;
    musicBpmAnalysisBusy = false;
    reportBpmAnalysis(state, bpmScan.count);
    LOGI(BPM_TAG, "scan %s after %lu ms: analyzed %u of %u, success=%u failed=%u",
         state == BpmAnalysisState::Cancelled ? "CANCELLED" : bpmStateName(state),
         static_cast<unsigned long>(millis() - bpmScanStartMs), static_cast<unsigned>(bpmScan.index),
         static_cast<unsigned>(bpmScan.count), static_cast<unsigned>(bpmScan.succeeded),
         static_cast<unsigned>(bpmScan.failed));
}

bool addBpmScanTrack(const char* filename, uint32_t fileSize, uint32_t fileModified)
{
    if (bpmScan.count >= MUSIC_LIBRARY_MAX) {
        countFailure();
        return false;
    }
    BpmScanTrack& track = bpmScan.tracks[bpmScan.count++];
    memset(&track, 0, sizeof(track));
    strncpy(track.filename, filename, sizeof(track.filename) - 1);
    track.fileSize = fileSize;
    track.fileModified = fileModified;
    return true;
}

bool beginBpmScan()
{
    bpmScan.running = true;
    bpmScan.requested = false;
    bpmScan.phase = ScanPhase::Start;
    bpmScan.fileFailed = false;
    bpmScan.count = 0;
    bpmScan.index = 0;
    bpmScan.succeeded = 0;
    bpmScan.failed = 0;
    bpmScanStartMs = millis();
    LOGI(BPM_TAG, "scan started: mode=%s, catalog songs=%u",
         bpmScan.mode == BpmAnalysisMode::All ? "all" : "new/unknown",
         static_cast<unsigned>(musicLibrary.count));

    const char* root = fs.getRootPath();
    if (root == nullptr) {
        LOGE(BPM_TAG, "SD root path unavailable (card not mounted)");
        countFailure();
        finishBpmScan(BpmAnalysisState::Failed);
        return false;
    }

    DIR dir{};
    FRESULT result = f_opendir(&dir, root);
    if (result != FR_OK) {
        LOGE(BPM_TAG, "cannot open SD root directory (FRESULT=%d)", static_cast<int>(result));
        countFailure();
        finishBpmScan(BpmAnalysisState::Failed);
        return false;
    }

    unsigned filesSeen = 0, upToDate = 0;
    for (;;) {
        FILINFO info{};
        result = f_readdir(&dir, &info);
        if (result != FR_OK) break;
        if (info.fname[0] == '\0') break;
        if ((info.fattrib & AM_DIR) != 0 || !isAllowedMusicFile(info.fname)) {
            LOGT(BPM_TAG, "skip non-MP3 entry: %s", info.fname);
            continue;
        }
        ++filesSeen;

        char filename[MUSIC_FILE_MAX];
        if (!normalizeMusicFileName(info.fname, filename, sizeof(filename)) ||
            info.fsize == 0 || info.fsize > 0xFFFFFFFFu) {
            LOGW(BPM_TAG, "skip %s: invalid name or size (size=%lu)", info.fname,
                 static_cast<unsigned long>(info.fsize));
            countFailure();
            continue;
        }

        const uint32_t fileSize = static_cast<uint32_t>(info.fsize);
        const uint32_t fileModified =
            (static_cast<uint32_t>(info.fdate) << 16) | info.ftime;
        const int index = findLibraryEntry(filename);
        bool needsAnalysis = bpmScan.mode == BpmAnalysisMode::All || index < 0;
        if (index >= 0) {
            const MusicEntry& entry = musicLibrary.entries[index];
            // ponytail: Size plus FAT time can miss same-size replacements; use a content fingerprint if strict detection is required.
            const bool identityKnown = entry.fileSize != 0 && entry.fileModified != 0;
            const bool replaced = identityKnown &&
                (entry.fileSize != fileSize || entry.fileModified != fileModified);
            needsAnalysis = needsAnalysis || entry.bpm == 0 || entry.genre[0] == '\0' ||
                            !identityKnown || replaced;
        }
        if (needsAnalysis) {
            if (addBpmScanTrack(filename, fileSize, fileModified)) {
                const char* why = index < 0 ? "new file"
                    : bpmScan.mode == BpmAnalysisMode::All ? "reanalyze all"
                    : musicLibrary.entries[index].bpm == 0 ? "BPM unknown"
                    : musicLibrary.entries[index].genre[0] == '\0' ? "genre unknown"
                    : "file replaced or identity unknown";
                LOGD(BPM_TAG, "queue %s (%lu bytes): %s", filename, static_cast<unsigned long>(fileSize), why);
            } else {
                LOGW(BPM_TAG, "queue full; %s not scheduled", filename);
            }
        } else {
            ++upToDate;
            LOGT(BPM_TAG, "up to date: %s (%u BPM)", filename, static_cast<unsigned>(musicLibrary.entries[index].bpm));
        }
    }
    f_closedir(&dir);
    LOGI(BPM_TAG, "directory scanned: %u MP3 files, %u need analysis, %u up to date", filesSeen,
         static_cast<unsigned>(bpmScan.count), upToDate);

    if (result != FR_OK) {
        LOGE(BPM_TAG, "directory read failed (FRESULT=%d)", static_cast<int>(result));
        countFailure();
        finishBpmScan(BpmAnalysisState::Failed);
        return false;
    }

    bool catalogChanged = false;
    uint8_t writeIndex = 0;
    for (uint8_t i = 0; i < bpmScan.count; ++i) {
        const BpmScanTrack track = bpmScan.tracks[i];
        int index = findLibraryEntry(track.filename);
        const bool isNew = index < 0;
        if (isNew && musicLibrary.count >= MUSIC_LIBRARY_MAX) {
            LOGW(BPM_TAG, "catalog full (%u); %s not added", static_cast<unsigned>(MUSIC_LIBRARY_MAX), track.filename);
            countFailure();
            continue;
        }
        if (isNew) {
            index = static_cast<int>(musicLibrary.count++);
            memset(&musicLibrary.entries[index], 0, sizeof(MusicEntry));
            strncpy(musicLibrary.entries[index].file, track.filename,
                    sizeof(musicLibrary.entries[index].file) - 1);
            catalogChanged = true;
        }

        MusicEntry& entry = musicLibrary.entries[index];
        const bool identityKnown = entry.fileSize != 0 && entry.fileModified != 0;
        const bool replaced = identityKnown &&
            (entry.fileSize != track.fileSize || entry.fileModified != track.fileModified);
        if (isNew || replaced || !identityKnown) {
            entry.bpm = 0;  // Never retain an old result for a changed or unidentified file.
            entry.genre[0] = '\0';
            entry.fileSize = track.fileSize;
            entry.fileModified = track.fileModified;
            catalogChanged = true;
        } else if (entry.fileSize != track.fileSize ||
                   entry.fileModified != track.fileModified) {
            entry.fileSize = track.fileSize;
            entry.fileModified = track.fileModified;
            catalogChanged = true;
        }

        bpmScan.tracks[writeIndex++] = track;
    }
    bpmScan.count = writeIndex;

    if (catalogChanged) {
        if (musicLibrarySave(fs, musicLibrary)) {
            LOGD(BPM_TAG, "catalog placeholders saved (%u songs)", static_cast<unsigned>(musicLibrary.count));
        } else {
            LOGE(BPM_TAG, "could not persist catalog placeholders; continuing in memory");
        }
    }

    if (bpmScan.count == 0) {
        finishBpmScan(BpmAnalysisState::Complete);
        return true;
    }

    reportBpmAnalysis(BpmAnalysisState::Running, 1,
                      bpmScan.tracks[0].filename);
    return true;
}

void finishScanFile()
{
    if (bpmScan.fileFailed) countFailure();
    else ++bpmScan.succeeded;
    ++bpmScan.index;  // the next file is reported when its analysis starts
    bpmScan.phase = ScanPhase::Start;
    bpmScan.fileFailed = false;
}

void advanceBpmScan()
{
    if (!bpmScan.running) return;
    if (bpmScan.index >= bpmScan.count) {
        finishBpmScan(BpmAnalysisState::Complete);
        return;
    }

    BpmScanTrack& track = bpmScan.tracks[bpmScan.index];
    const uint8_t current = static_cast<uint8_t>(bpmScan.index + 1);
    const int index = findLibraryEntry(track.filename);
    if (index < 0) {
        bpmScan.fileFailed = true;
        finishScanFile();
        return;
    }
    MusicEntry& entry = musicLibrary.entries[index];
    const bool all = bpmScan.mode == BpmAnalysisMode::All;

    switch (bpmScan.phase) {
    case ScanPhase::Start:
        reportBpmAnalysis(BpmAnalysisState::Running, current, track.filename);
        LOGI(BPM_TAG, "[%u/%u] analyzing %s", static_cast<unsigned>(current), static_cast<unsigned>(bpmScan.count),
             track.filename);
        bpmFileStartMs = millis();
        bpmScan.fileFailed = false;
        bpmScan.phase = ScanPhase::GenreStart;
        if (all || entry.bpm == 0) {
            if (bpmAnalyzer.begin(fs, track.filename)) {
                bpmScan.phase = ScanPhase::Bpm;
            } else {
                LOGE(BPM_TAG, "[%u/%u] %s: open failed: %s", static_cast<unsigned>(current),
                     static_cast<unsigned>(bpmScan.count), track.filename, bpmAnalyzer.error());
                bpmScan.fileFailed = true;
            }
        }
        return;

    case ScanPhase::Bpm: {
        uint16_t bpm = 0;
        const MusicBpmAnalyzer::StepResult result = bpmAnalyzer.step(bpm);
        if (result == MusicBpmAnalyzer::StepResult::InProgress) return;
        if (result == MusicBpmAnalyzer::StepResult::Complete) {
            LOGI(BPM_TAG, "[%u/%u] %s: %u BPM (was %u) in %lu ms", static_cast<unsigned>(current),
                 static_cast<unsigned>(bpmScan.count), track.filename, static_cast<unsigned>(bpm),
                 static_cast<unsigned>(entry.bpm), static_cast<unsigned long>(millis() - bpmFileStartMs));
            entry.bpm = bpm;
            entry.fileSize = track.fileSize;
            entry.fileModified = track.fileModified;
            if (!musicLibrarySave(fs, musicLibrary)) {
                bpmScan.fileFailed = true;
                LOGE(BPM_TAG, "%s: BPM estimated but catalog save failed", track.filename);
            }
        } else {
            LOGE(BPM_TAG, "[%u/%u] %s: analysis failed: %s (after %lu ms)", static_cast<unsigned>(current),
                 static_cast<unsigned>(bpmScan.count), track.filename, bpmAnalyzer.error(),
                 static_cast<unsigned long>(millis() - bpmFileStartMs));
            bpmScan.fileFailed = true;
        }
        bpmAnalyzer.cancel();
        bpmScan.phase = ScanPhase::GenreStart;
        return;
    }

    case ScanPhase::GenreStart:
        if (!(all || entry.genre[0] == '\0')) {
            finishScanFile();
        } else if (genreClassifier.begin(fs, track.filename)) {
            bpmScan.phase = ScanPhase::Genre;
        } else {
            LOGE(BPM_TAG, "[%u/%u] %s: genre start failed: %s", static_cast<unsigned>(current),
                 static_cast<unsigned>(bpmScan.count), track.filename, genreClassifier.error());
            bpmScan.fileFailed = true;
            finishScanFile();
        }
        return;

    case ScanPhase::Genre: {
        char genre[MUSIC_GENRE_MAX] = {};
        const MusicGenreClassifier::StepResult result = genreClassifier.step(genre);
        if (result == MusicGenreClassifier::StepResult::InProgress) return;
        if (result == MusicGenreClassifier::StepResult::Complete) {
            LOGI(BPM_TAG, "[%u/%u] %s: genre %s (was '%s')", static_cast<unsigned>(current),
                 static_cast<unsigned>(bpmScan.count), track.filename, genre, entry.genre);
            memcpy(entry.genre, genre, sizeof(entry.genre));
            entry.fileSize = track.fileSize;
            entry.fileModified = track.fileModified;
            if (!musicLibrarySave(fs, musicLibrary)) {
                bpmScan.fileFailed = true;
                LOGE(BPM_TAG, "%s: genre classified but catalog save failed", track.filename);
            }
        } else {
            LOGE(BPM_TAG, "[%u/%u] %s: genre failed: %s", static_cast<unsigned>(current),
                 static_cast<unsigned>(bpmScan.count), track.filename, genreClassifier.error());
            bpmScan.fileFailed = true;
        }
        genreClassifier.cancel();
        finishScanFile();
        return;
    }
    }
}

// Analysis never overlaps playback: a user-requested scan that is interrupted is discarded,
// not resumed. Results already saved stay; unfinished files keep bpm 0 and are retried on request.
void discardBpmScanForPlayback()
{
    if (bpmScan.requested || bpmScan.running) {
        LOGW(BPM_TAG, "playback started; discarding BPM scan (done %u of %u, unfinished files keep BPM 0)",
             static_cast<unsigned>(bpmScan.index), static_cast<unsigned>(bpmScan.count));
        finishBpmScan(BpmAnalysisState::Cancelled);
    }
}

void sendPlaybackEvent(AudioPlaybackEventType type, int8_t songIndex)
{
    if (audioPlaybackEventQueue == nullptr) return;
    AudioPlaybackEvent event{};
    event.type = type;
    event.songIndex = songIndex;
    if (xQueueSend(audioPlaybackEventQueue, &event, pdMS_TO_TICKS(20)) == pdPASS) {
        LOGD(TAG, "-> control: playback event %s songIndex=%d",
             type == AudioPlaybackEventType::Failed ? "Failed" : "Finished", static_cast<int>(songIndex));
    } else {
        LOGE(TAG, "playback event queue full; event for song #%d lost", static_cast<int>(songIndex));
    }
}

// filename is only given when a new file starts; otherwise the UI keeps the last one.
void reportAudioState(AudioState state, const char* filename = nullptr)
{
    if (uiUpdateQueue == nullptr) return;
    UiUpdateElement update{};
    update.type = UiUpdateType::Audio;
    update.timestampMs = millis();
    update.audioState = static_cast<uint8_t>(state);
    if (filename != nullptr) {
        strncpy(update.filename, filename, sizeof(update.filename) - 1);
    }
    xQueueSend(uiUpdateQueue, &update, pdMS_TO_TICKS(20));
}

void handleAudioCommand(AudioCmdElement& command)
{
    command.filename[sizeof(command.filename) - 1] = '\0';

    switch (command.command) {
    case AudioCommandType::Play: {
        const bool automatic = command.isAutomatic != 0;
        if (!isAllowedMusicFile(command.filename)) {
            LOGW(TAG, "rejected audio file (not .mp3): '%s'", command.filename);
            if (automatic) {
                sendPlaybackEvent(AudioPlaybackEventType::Failed, command.songIndex);
            }
            return;
        }

        discardBpmScanForPlayback();
        amp.stopMp3();
        playbackPaused = false;
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        if (amp.beginMp3(fs, command.filename)) {
            currentPlaybackAutomatic = automatic;
            currentAutomaticSongIndex = automatic ? command.songIndex : -1;
            LOGI(TAG, "playing %s (%s%s)", command.filename, automatic ? "automatic" : "manual",
                 automatic ? "" : ", manual control");
            reportAudioState(AudioState::Playing, command.filename);
        } else {
            LOGE(TAG, "playback start failed for %s: %s", command.filename, amp.lastError());
            reportAudioState(AudioState::Stopped);
            if (automatic) {
                sendPlaybackEvent(AudioPlaybackEventType::Failed, command.songIndex);
            }
        }
        break;
    }

    case AudioCommandType::Stop:
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        playbackPaused = false;
        amp.stopMp3();
        LOGI(TAG, "stopped");
        reportAudioState(AudioState::Stopped);
        break;

    case AudioCommandType::SetVolume: {
        const uint8_t percent =
            command.volumePercent > 100 ? 100 : command.volumePercent;
        amp.setVolume(percent / 100.0f);
        LOGI(TAG, "volume %u%%", static_cast<unsigned>(percent));
        break;
    }

    case AudioCommandType::Pause:
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        if (amp.pauseMp3()) {
            playbackPaused = true;
            LOGI(TAG, "paused");
            reportAudioState(AudioState::Paused);
        } else {
            LOGW(TAG, "pause ignored: no active playback");
        }
        break;

    case AudioCommandType::Resume:
        currentPlaybackAutomatic = false;
        currentAutomaticSongIndex = -1;
        if (amp.resumeMp3()) {
            playbackPaused = false;
            LOGI(TAG, "resumed");
            reportAudioState(AudioState::Playing);
        } else {
            LOGW(TAG, "resume ignored: audio is not paused");
        }
        break;

    case AudioCommandType::CancelBpmAnalysis:
        // Finished files stay saved; the file in progress is dropped (bpm 0 / genre empty, retried next scan).
        if (bpmScan.requested || bpmScan.running) {
            LOGW(BPM_TAG, "scan cancelled by user (done %u of %u)", static_cast<unsigned>(bpmScan.index),
                 static_cast<unsigned>(bpmScan.count));
            finishBpmScan(BpmAnalysisState::Cancelled);
        }
        break;

    case AudioCommandType::AnalyzeNewOrUnknownBpm:
    case AudioCommandType::ReanalyzeAllBpm:
        if (amp.isMp3Playing() || playbackPaused) {
            LOGW(BPM_TAG, "analysis request rejected: audio is %s", playbackPaused ? "paused" : "playing");
            bpmScan.mode = command.command == AudioCommandType::AnalyzeNewOrUnknownBpm
                ? BpmAnalysisMode::NewOrUnknown
                : BpmAnalysisMode::All;
            reportBpmAnalysis(BpmAnalysisState::Cancelled);  // not allowed during playback
        } else if (!bpmScan.requested && !bpmScan.running) {
            bpmScan.mode = command.command == AudioCommandType::AnalyzeNewOrUnknownBpm
                ? BpmAnalysisMode::NewOrUnknown
                : BpmAnalysisMode::All;
            bpmScan.requested = true;
            bpmScan.count = bpmScan.index = bpmScan.succeeded = bpmScan.failed = 0;
            bpmScanStartMs = millis();
            LOGI(BPM_TAG, "analysis request accepted: %s",
                 bpmScan.mode == BpmAnalysisMode::All ? "reanalyze all" : "new/unknown only");
            reportBpmAnalysis(BpmAnalysisState::Queued);
        } else {
            LOGW(BPM_TAG, "analysis request ignored: a scan is already %s", bpmScan.running ? "running" : "queued");
        }
        break;
    }
}

} // namespace

void audioTask(void* parameter)
{
    (void)parameter;

    musicStorageReady = false;
    LOGI(TAG, "started; mounting SD card (free heap %u)", static_cast<unsigned>(xPortGetFreeHeapSize()));
    const uint32_t sdStartMs = millis();
    if (!fs.begin()) {
        LOGE(TAG, "SD card mount FAILED after %lu ms; music disabled, library empty (check card/contacts/format)",
             static_cast<unsigned long>(millis() - sdStartMs));
        musicLibraryReady = true;  // An empty catalog must not block controlTask.
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    LOGI(TAG, "SD card mounted in %lu ms, root='%s'", static_cast<unsigned long>(millis() - sdStartMs),
         fs.getRootPath() != nullptr ? fs.getRootPath() : "?");
    const bool libraryLoaded = musicLibraryLoad(fs);
    LOGI(TAG, "music library %s: %u songs", libraryLoaded ? "loaded" : "NOT loaded (empty)",
         static_cast<unsigned>(musicLibrary.count));
    musicLibraryReady = true;

    if (!amp.begin()) {
        LOGE(TAG, "MAX98357 amplifier init FAILED; playback disabled");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    amp.setShutdownPin(AMP_SD_PIN_RIGHT, AMP_SD_PIN_LEFT);
    amp.setVolume(0.5f);
    musicStorageReady = true;
    LOGI(TAG, "MAX98357 ready (volume 50%%, shutdown pins D%u/D%u); audio storage ready",
         static_cast<unsigned>(AMP_SD_PIN_RIGHT), static_cast<unsigned>(AMP_SD_PIN_LEFT));

    for (;;) {
        AudioCmdElement command{};
        const bool hasBpmWork = bpmScan.requested || bpmScan.running;
        const TickType_t waitTicks =
            (amp.isMp3Playing() || hasBpmWork) ? 0 : portMAX_DELAY;

        if (xQueueReceive(audioCmdQueue, &command, waitTicks) == pdPASS) {
            if (command.command == AudioCommandType::Play) {
                LOGD(TAG, "<- control: Play %s (automatic=%d, songIndex=%d)", command.filename,
                     command.isAutomatic ? 1 : 0, static_cast<int>(command.songIndex));
            } else if (command.command == AudioCommandType::SetVolume) {
                LOGD(TAG, "<- control: SetVolume %u%%", static_cast<unsigned>(command.volumePercent));
            } else {
                LOGD(TAG, "<- control: %s", audioCommandName(command.command));
            }
            handleAudioCommand(command);
        }

        const bool audioActive = amp.isMp3Playing() || playbackPaused;
        if (amp.isMp3Playing()) {
            const Mp3ProcessResult result = amp.processMp3();
            if (result == Mp3ProcessResult::Finished) {
                LOGI(TAG, "playback finished%s", currentPlaybackAutomatic ? " (automatic)" : "");
                if (currentPlaybackAutomatic) {
                    sendPlaybackEvent(
                        AudioPlaybackEventType::Finished,
                        currentAutomaticSongIndex);
                }
                currentPlaybackAutomatic = false;
                currentAutomaticSongIndex = -1;
                playbackPaused = false;
                reportAudioState(AudioState::Stopped);
            } else if (result == Mp3ProcessResult::Stopped) {
                LOGD(TAG, "playback stopped");
            } else if (result == Mp3ProcessResult::Paused) {
                playbackPaused = true;
                LOGD(TAG, "playback paused");
            } else if (result == Mp3ProcessResult::Error) {
                LOGE(TAG, "playback error: %s%s", amp.lastError(), currentPlaybackAutomatic ? " (automatic; skipping song)" : "");
                if (currentPlaybackAutomatic) {
                    sendPlaybackEvent(
                        AudioPlaybackEventType::Failed,
                        currentAutomaticSongIndex);
                }
                amp.stopMp3();
                currentPlaybackAutomatic = false;
                currentAutomaticSongIndex = -1;
                playbackPaused = false;
                reportAudioState(AudioState::Stopped);
            }
        }

        if (!audioActive && !amp.isMp3Playing() && !playbackPaused) {
            if (bpmScan.requested || bpmScan.running) {
                musicBpmAnalysisBusy = true;  // gate automatic selection while the catalog changes
                if (bpmScan.requested) beginBpmScan();
                if (bpmScan.running) advanceBpmScan();
            }
        }

        if (amp.isMp3Playing() || playbackPaused || bpmScan.requested || bpmScan.running) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}