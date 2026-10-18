#include "music_library.h"

#include "debug.h"
#include "AmebaFatFS.h"

MusicLibrary musicLibrary{};

namespace {

constexpr const char* TAG = "LIBRARY";
constexpr const char* MUSIC_LIBRARY_BACKUP_FILE = "music_library.bak";
constexpr const char* MUSIC_LIBRARY_CORRUPT_FILE = "music_library.corrupt";
char jsonBuf[MUSIC_LIBRARY_JSON_MAX];  // ~160 KB; static so it stays off the 4 KB task stack.
MusicLibrary loaded;                   // ~60 KB load scratch copy, static for the same reason.

bool libraryPath(AmebaFatFS& fs, const char* name, char* path, size_t cap)
{
    const char* root = fs.getRootPath();
    if (root == nullptr) return false;
    const int n = snprintf(path, cap, "%s%s", root, name);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool readLibraryFile(AmebaFatFS& fs, const char* name, MusicLibrary& lib)
{
    char path[64];
    if (!libraryPath(fs, name, path, sizeof(path))) {
        LOGE(TAG, "read %s: path too long or SD not mounted", name);
        return false;
    }
    if (!fs.exists(path)) {
        LOGD(TAG, "read %s: file does not exist", name);
        return false;
    }

    File file = fs.open(path);
    if (!file) {
        LOGE(TAG, "read %s: open failed", name);
        return false;
    }
    const uint32_t size = file.size();
    if (size == 0 || size >= sizeof(jsonBuf)) {
        LOGE(TAG, "read %s: bad size %lu (buffer %u)", name, static_cast<unsigned long>(size),
             static_cast<unsigned>(sizeof(jsonBuf)));
        file.close();
        return false;
    }
    const int got = file.read(jsonBuf, size);
    file.close();
    if (got != static_cast<int>(size)) {
        LOGE(TAG, "read %s: short read %d of %lu bytes", name, got, static_cast<unsigned long>(size));
        return false;
    }
    jsonBuf[size] = '\0';
    const bool parsed = music_json::parse(jsonBuf, lib);
    LOGD(TAG, "read %s: %lu bytes, parse %s", name, static_cast<unsigned long>(size), parsed ? "ok" : "FAILED");
    return parsed;
}

bool writeLibraryFile(AmebaFatFS& fs, const char* name,
                      const char* data, size_t len)
{
    char path[64];
    if (!libraryPath(fs, name, path, sizeof(path))) {
        LOGE(TAG, "write %s: path too long or SD not mounted", name);
        return false;
    }
    if (fs.exists(path) && !fs.remove(path)) {
        LOGE(TAG, "write %s: could not remove old file", name);
        return false;
    }

    File file = fs.open(path);
    if (!file) {
        LOGE(TAG, "write %s: open failed", name);
        return false;
    }
    const size_t written = file.write(data, len);
    file.close();
    if (written != len) {
        LOGE(TAG, "write %s: wrote %u of %u bytes (SD full or write error)", name,
             static_cast<unsigned>(written), static_cast<unsigned>(len));
    }
    return written == len;
}

} // namespace

bool musicLibraryLoad(AmebaFatFS& fs)
{
    loaded.count = 0;  // no MusicLibrary{} temporary: it would put ~60 KB on the stack
    char path[64], backup[64], corrupt[64];
    if (!libraryPath(fs, MUSIC_LIBRARY_FILE, path, sizeof(path)) ||
        !libraryPath(fs, MUSIC_LIBRARY_BACKUP_FILE, backup, sizeof(backup)) ||
        !libraryPath(fs, MUSIC_LIBRARY_CORRUPT_FILE, corrupt, sizeof(corrupt))) {
        LOGE(TAG, "load: cannot build library paths (SD not mounted?)");
        musicLibrary.count = 0;
        return false;
    }

    LOGI(TAG, "loading %s", MUSIC_LIBRARY_FILE);
    if (readLibraryFile(fs, MUSIC_LIBRARY_FILE, loaded)) {
        musicLibrary = loaded;
        // A valid main file means any remaining backup is from an interrupted cleanup.
        if (fs.exists(backup)) fs.remove(backup);
        LOGI(TAG, "loaded %u songs from %s", static_cast<unsigned>(musicLibrary.count), MUSIC_LIBRARY_FILE);
        for (size_t i = 0; i < musicLibrary.count; ++i) {
            LOGD(TAG, "  #%u %s bpm=%u genre=%s size=%lu", static_cast<unsigned>(i), musicLibrary.entries[i].file,
                 static_cast<unsigned>(musicLibrary.entries[i].bpm), musicLibrary.entries[i].genre,
                 static_cast<unsigned long>(musicLibrary.entries[i].fileSize));
        }
        return true;
    }

    LOGW(TAG, "%s missing or invalid; trying backup %s", MUSIC_LIBRARY_FILE, MUSIC_LIBRARY_BACKUP_FILE);
    loaded.count = 0;  // no MusicLibrary{} temporary: it would put ~60 KB on the stack
    if (readLibraryFile(fs, MUSIC_LIBRARY_BACKUP_FILE, loaded)) {
        musicLibrary = loaded;
        // Recover the previous valid catalog without overwriting a corrupt main file.
        if (!fs.exists(path)) {
            fs.rename(backup, path);
        } else if (!fs.exists(corrupt) && fs.rename(path, corrupt)) {
            fs.rename(backup, path);
        }
        LOGW(TAG, "recovered %u songs from backup %s", static_cast<unsigned>(musicLibrary.count),
             MUSIC_LIBRARY_BACKUP_FILE);
        return true;
    }

    musicLibrary.count = 0;
    if (fs.exists(path) && !fs.exists(corrupt) && fs.rename(path, corrupt)) {
        LOGW(TAG, "invalid catalog preserved as %s", MUSIC_LIBRARY_CORRUPT_FILE);
    }
    LOGE(TAG, "%s missing or invalid and no usable backup; music library is empty", MUSIC_LIBRARY_FILE);
    return false;
}

bool musicLibrarySave(AmebaFatFS& fs, const MusicLibrary& lib)
{
    if (lib.count > MUSIC_LIBRARY_MAX) {
        LOGE(TAG, "save refused: %u songs exceeds limit %u", static_cast<unsigned>(lib.count),
             static_cast<unsigned>(MUSIC_LIBRARY_MAX));
        return false;
    }
    for (size_t i = 0; i < lib.count; ++i) {
        if (!music_json::validEntry(lib.entries[i])) {
            LOGE(TAG, "save refused: entry #%u '%s' is invalid (bpm=%u)", static_cast<unsigned>(i),
                 lib.entries[i].file, static_cast<unsigned>(lib.entries[i].bpm));
            return false;
        }
    }
    const size_t len = music_json::format(lib, jsonBuf, sizeof(jsonBuf));
    if (len == 0) {
        LOGE(TAG, "save refused: catalog does not fit the %u-byte JSON buffer", static_cast<unsigned>(sizeof(jsonBuf)));
        return false;
    }

    char path[64], tmp[64], backup[64];
    if (!libraryPath(fs, MUSIC_LIBRARY_FILE, path, sizeof(path)) ||
        !libraryPath(fs, "music_library.tmp", tmp, sizeof(tmp)) ||
        !libraryPath(fs, MUSIC_LIBRARY_BACKUP_FILE, backup, sizeof(backup))) {
        LOGE(TAG, "save: cannot build paths (SD not mounted?)");
        return false;
    }

    LOGD(TAG, "saving %u songs (%u bytes)", static_cast<unsigned>(lib.count), static_cast<unsigned>(len));
    if (!writeLibraryFile(fs, "music_library.tmp", jsonBuf, len)) {
        LOGE(TAG, "save failed: could not write music_library.tmp");
        if (fs.exists(tmp)) fs.remove(tmp);
        return false;
    }

    bool hasMain = fs.exists(path);
    if (fs.exists(backup)) {
        if (hasMain) {
            // The main catalog was validated during load; this is a stale backup.
            if (!fs.remove(backup)) {
                LOGE(TAG, "save failed: could not remove stale backup");
                fs.remove(tmp);
                return false;
            }
        } else {
            if (!fs.rename(backup, path)) {
                LOGE(TAG, "save failed: could not restore backup as main file");
                fs.remove(tmp);
                return false;
            }
            LOGW(TAG, "main catalog was missing; restored from backup before saving");
            hasMain = true;
        }
    }

    if (hasMain && !fs.rename(path, backup)) {
        LOGE(TAG, "save failed: could not move main catalog to backup");
        fs.remove(tmp);
        return false;
    }
    if (!fs.rename(tmp, path)) {
        LOGE(TAG, "save failed: could not move new catalog into place");
        if (hasMain && !fs.rename(backup, path)) {
            LOGE(TAG, "new save failed; previous catalog remains in %s", MUSIC_LIBRARY_BACKUP_FILE);
        }
        return false;
    }

    if (hasMain && fs.exists(backup)) fs.remove(backup);
    if (&lib != &musicLibrary) musicLibrary = lib;  // callers usually save musicLibrary itself
    LOGD(TAG, "saved %u songs", static_cast<unsigned>(lib.count));
    return true;
}