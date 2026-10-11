#ifndef MUSIC_LIBRARY_H
#define MUSIC_LIBRARY_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

constexpr const char* MUSIC_LIBRARY_FILE = "music_library.json";
constexpr size_t MUSIC_LIBRARY_MAX = 32;
constexpr size_t MUSIC_FILE_MAX = 32;
constexpr size_t MUSIC_GENRE_MAX = 16;
constexpr size_t MUSIC_LIBRARY_JSON_MAX = 3072;  // 32 筆 * 約 80 B 的上限

struct MusicEntry {
    char file[MUSIC_FILE_MAX];
    char genre[MUSIC_GENRE_MAX];
    uint16_t bpm;
};

struct MusicLibrary {
    MusicEntry entries[MUSIC_LIBRARY_MAX];
    size_t count;
};

extern MusicLibrary musicLibrary;  // 開機載入的內容；寫入請走 musicLibrarySave()

// fs 須已 begin()；SD 由 audioTask 擁有，讀寫都在該 task 內呼叫。
class AmebaFatFS;
bool musicLibraryLoad(AmebaFatFS& fs);                          // 缺檔或格式錯誤時保持空表並回傳 false
bool musicLibrarySave(AmebaFatFS& fs, const MusicLibrary& lib); // 成功才更新 musicLibrary

// 以下為純邏輯（無 I/O），可在 PC 上自我測試。
namespace music_json {

inline const char* skipWs(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

// 解析字串；超出 cap 視為格式錯誤，不截斷。
inline const char* parseString(const char* p, char* out, size_t cap)
{
    if (*p != '"') return nullptr;
    ++p;
    size_t n = 0;
    while (*p != '"') {
        if (*p == '\0') return nullptr;
        if (*p == '\\') {
            ++p;
            if (*p == '\0') return nullptr;
        }
        if (n + 1 >= cap) return nullptr;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return p + 1;
}

inline const char* parseUint(const char* p, uint32_t& value)
{
    if (*p < '0' || *p > '9') return nullptr;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > 100000) return nullptr;
        ++p;
    }
    value = v;
    return p;
}

inline bool validEntry(const MusicEntry& e)
{
    const size_t len = strlen(e.file);
    if (len < 5 || strcmp(e.file + len - 4, ".mp3") != 0) return false;
    if (strpbrk(e.file, "/\\") != nullptr) return false;
    return e.bpm >= 30 && e.bpm <= 300;
}

// 解析一個曲目物件，p 指向 '{'。
inline const char* parseEntry(const char* p, MusicEntry& e)
{
    memset(&e, 0, sizeof(e));
    bool hasFile = false, hasBpm = false;
    p = skipWs(p + 1);
    while (*p != '}') {
        char key[16];
        p = parseString(p, key, sizeof(key));
        if (p == nullptr) return nullptr;
        p = skipWs(p);
        if (*p++ != ':') return nullptr;
        p = skipWs(p);
        if (strcmp(key, "file") == 0) {
            p = parseString(p, e.file, sizeof(e.file));
            hasFile = true;
        } else if (strcmp(key, "genre") == 0) {
            p = parseString(p, e.genre, sizeof(e.genre));
        } else if (strcmp(key, "bpm") == 0) {
            uint32_t bpm = 0;
            p = parseUint(p, bpm);
            if (p == nullptr || bpm > 0xFFFF) return nullptr;
            e.bpm = (uint16_t)bpm;
            hasBpm = true;
        } else {  // 未知欄位：略過字串或整數值
            char skip[MUSIC_FILE_MAX];
            uint32_t n;
            p = *p == '"' ? parseString(p, skip, sizeof(skip)) : parseUint(p, n);
        }
        if (p == nullptr) return nullptr;
        p = skipWs(p);
        if (*p == ',') p = skipWs(p + 1);
        else if (*p != '}') return nullptr;
    }
    if (!hasFile || !hasBpm || !validEntry(e)) return nullptr;
    return p + 1;
}

// 解析整份檔案；任何錯誤都回傳 false 且 lib 為空（不使用部分資料）。
inline bool parse(const char* json, MusicLibrary& lib)
{
    lib.count = 0;
    const char* p = skipWs(json);
    if (*p++ != '{') return false;
    p = skipWs(p);
    bool hasSongs = false;
    while (*p != '}') {
        char key[16];
        p = parseString(p, key, sizeof(key));
        if (p == nullptr) return lib.count = 0, false;
        p = skipWs(p);
        if (*p++ != ':') return lib.count = 0, false;
        p = skipWs(p);
        if (strcmp(key, "songs") == 0) {
            if (*p++ != '[') return lib.count = 0, false;
            p = skipWs(p);
            while (*p != ']') {
                if (*p != '{' || lib.count >= MUSIC_LIBRARY_MAX) return lib.count = 0, false;
                p = parseEntry(p, lib.entries[lib.count]);
                if (p == nullptr) return lib.count = 0, false;
                for (size_t i = 0; i < lib.count; ++i) {  // 檔名不可重複
                    if (strcmp(lib.entries[i].file, lib.entries[lib.count].file) == 0)
                        return lib.count = 0, false;
                }
                ++lib.count;
                p = skipWs(p);
                if (*p == ',') p = skipWs(p + 1);
                else if (*p != ']') return lib.count = 0, false;
            }
            ++p;
            hasSongs = true;
        } else {  // version 等其他欄位：略過整數值
            uint32_t n;
            p = parseUint(p, n);
            if (p == nullptr) return lib.count = 0, false;
        }
        p = skipWs(p);
        if (*p == ',') p = skipWs(p + 1);
        else if (*p != '}') return lib.count = 0, false;
    }
    if (!hasSongs) return lib.count = 0, false;
    return true;
}

// 序列化到 out；放不下回傳 0。genre/file 在寫入前須先通過 validEntry 與字元檢查。
inline size_t format(const MusicLibrary& lib, char* out, size_t cap)
{
    size_t n = 0;
    auto put = [&](const char* fmt, const char* a, unsigned b) {
        const int w = snprintf(out + n, cap - n, fmt, a, b);
        if (w < 0 || (size_t)w >= cap - n) return false;
        n += (size_t)w;
        return true;
    };
    if (!put("{\"version\":1,\"songs\":[", "", 0)) return 0;
    for (size_t i = 0; i < lib.count; ++i) {
        const MusicEntry& e = lib.entries[i];
        if (strpbrk(e.file, "\"\\") || strpbrk(e.genre, "\"\\")) return 0;
        char head[MUSIC_FILE_MAX + 24];
        snprintf(head, sizeof(head), "%s{\"file\":\"%s\",", i ? "," : "", e.file);
        if (!put("%s", head, 0)) return 0;
        char tail[MUSIC_GENRE_MAX + 24];
        snprintf(tail, sizeof(tail), "\"bpm\":%u,\"genre\":\"%s\"}", (unsigned)e.bpm, e.genre);
        if (!put("%s", tail, 0)) return 0;
    }
    if (!put("%s", "]}\n", 0)) return 0;
    return n;
}

}  // namespace music_json

#ifdef MUSIC_LIBRARY_SELF_TEST
#include <assert.h>
int main(void)
{
    static MusicLibrary lib;
    const char* ok = "{\"version\":1,\"songs\":[{\"file\":\"bpm060.mp3\",\"bpm\":60,\"genre\":\"Folk\"},"
                     " {\"genre\":\"Pop\", \"bpm\": 117, \"file\":\"bpm117.mp3\"}]}";
    assert(music_json::parse(ok, lib) && lib.count == 2);
    assert(strcmp(lib.entries[1].file, "bpm117.mp3") == 0 && lib.entries[1].bpm == 117);
    assert(strcmp(lib.entries[1].genre, "Pop") == 0);

    char buf[MUSIC_LIBRARY_JSON_MAX];
    const size_t n = music_json::format(lib, buf, sizeof(buf));
    assert(n > 0);
    static MusicLibrary back;
    assert(music_json::parse(buf, back) && back.count == 2);
    assert(strcmp(back.entries[0].genre, "Folk") == 0 && back.entries[0].bpm == 60);

    assert(music_json::parse("{\"songs\":[]}", lib) && lib.count == 0);
    assert(!music_json::parse("", lib));
    assert(!music_json::parse("{\"songs\":[{\"file\":\"a.mp3\"}]}", lib));            // 缺 bpm
    assert(!music_json::parse("{\"songs\":[{\"file\":\"a.wav\",\"bpm\":90}]}", lib)); // 非 mp3
    assert(!music_json::parse("{\"songs\":[{\"file\":\"../a.mp3\",\"bpm\":90}]}", lib));
    assert(!music_json::parse("{\"songs\":[{\"file\":\"a.mp3\",\"bpm\":9}]}", lib));  // BPM 超出範圍
    assert(!music_json::parse("{\"songs\":[{\"file\":\"a.mp3\",\"bpm\":90},"
                              "{\"file\":\"a.mp3\",\"bpm\":91}]}", lib));             // 重複
    assert(!music_json::parse("{\"songs\":[{\"file\":\"a.mp3\",\"bpm\":90}", lib));   // 截斷
    assert(lib.count == 0);
    return 0;
}
#endif

#endif
