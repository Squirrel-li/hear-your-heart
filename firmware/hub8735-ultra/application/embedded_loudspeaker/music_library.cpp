#include "music_library.h"

#include "debug.h"
#include "AmebaFatFS.h"

MusicLibrary musicLibrary{};

namespace {

char jsonBuf[MUSIC_LIBRARY_JSON_MAX];  // 靜態緩衝：task stack 只有 4 KB

bool libraryPath(AmebaFatFS& fs, const char* name, char* path, size_t cap)
{
    const int n = snprintf(path, cap, "%s%s", fs.getRootPath(), name);
    return n > 0 && (size_t)n < cap;
}

} // namespace

bool musicLibraryLoad(AmebaFatFS& fs)
{
    musicLibrary.count = 0;
    char path[64];
    if (!libraryPath(fs, MUSIC_LIBRARY_FILE, path, sizeof(path))) return false;
    if (!fs.exists(path)) {
        DEBUG_PRINTLN("Error: music_library.json not found on SD card, music library is empty");
        return false;
    }

    File file = fs.open(path);
    const uint32_t size = file.size();
    if (!file || size == 0 || size >= sizeof(jsonBuf)) {
        file.close();
        DEBUG_PRINTLN("Error: music_library.json is unreadable or too large, music library is empty");
        return false;
    }
    const int got = file.read(jsonBuf, size);
    file.close();
    if (got != (int)size) {
        DEBUG_PRINTLN("Error: music_library.json read failed, music library is empty");
        return false;
    }
    jsonBuf[size] = '\0';

    if (!music_json::parse(jsonBuf, musicLibrary)) {
        musicLibrary.count = 0;
        DEBUG_PRINTLN("Error: music_library.json parse failed, music library is empty");
        return false;
    }
    DEBUG_PRINT("Music library: loaded songs=");
    DEBUG_PRINTLN(static_cast<int>(musicLibrary.count));
    return true;
}

// 先寫暫存檔再改名，寫到一半斷電時不會毀掉舊檔。
bool musicLibrarySave(AmebaFatFS& fs, const MusicLibrary& lib)
{
    for (size_t i = 0; i < lib.count; ++i) {
        if (!music_json::validEntry(lib.entries[i])) return false;
    }
    const size_t len = music_json::format(lib, jsonBuf, sizeof(jsonBuf));
    if (len == 0) return false;

    char path[64], tmp[64];
    if (!libraryPath(fs, MUSIC_LIBRARY_FILE, path, sizeof(path)) ||
        !libraryPath(fs, "music_library.tmp", tmp, sizeof(tmp))) return false;

    if (fs.exists(tmp)) fs.remove(tmp);
    File file = fs.open(tmp);
    if (!file) return false;
    const size_t written = file.write(jsonBuf, len);
    file.close();
    if (written != len) {
        fs.remove(tmp);
        return false;
    }

    if (fs.exists(path) && !fs.remove(path)) {
        fs.remove(tmp);
        return false;
    }
    if (!fs.rename(tmp, path)) {
        DEBUG_PRINTLN("Music library: rename failed, data kept in music_library.tmp");
        return false;
    }
    musicLibrary = lib;
    return true;
}
