#include "helix_state.h"

#include <string.h>

#include "debug.h"
#include "mp3dec.h"
extern "C" {
#include "mp3common.h"
#include "coder.h"
}

namespace {

constexpr const char* TAG = "HELIX";

// 與 libhmp3.a 內 xmp3_AllocateBuffers 清空的大小一致（arm-none-eabi-nm -S 與反組譯確認，SDK 4.1.1）。
// shortcut: 依賴預編譯函式庫的內部配置；SDK 更新後這裡編譯失敗時，重新用 nm 核對。
static_assert(sizeof(MP3DecInfo) == 2032, "libhmp3 MP3DecInfo size changed");
static_assert(sizeof(FrameHeader) == 56, "libhmp3 FrameHeader size changed");
static_assert(sizeof(SideInfo) == 328, "libhmp3 SideInfo size changed");
static_assert(sizeof(ScaleFactorInfo) == 284, "libhmp3 ScaleFactorInfo size changed");
static_assert(sizeof(HuffmanInfo) == 4624, "libhmp3 HuffmanInfo size changed");
static_assert(sizeof(DequantInfo) == 840, "libhmp3 DequantInfo size changed");
static_assert(sizeof(IMDCTInfo) == 6944, "libhmp3 IMDCTInfo size changed");
static_assert(sizeof(SubbandInfo) == 8708, "libhmp3 SubbandInfo size changed");

constexpr int PART_COUNT = 8;  // MP3DecInfo 本身 + 它開頭 7 個指標所指的結構
const size_t PART_SIZE[PART_COUNT] = {
    sizeof(MP3DecInfo), sizeof(FrameHeader), sizeof(SideInfo), sizeof(ScaleFactorInfo),
    sizeof(HuffmanInfo), sizeof(DequantInfo), sizeof(IMDCTInfo), sizeof(SubbandInfo)};
constexpr size_t STATE_BYTES = sizeof(MP3DecInfo) + sizeof(FrameHeader) + sizeof(SideInfo) +
                               sizeof(ScaleFactorInfo) + sizeof(HuffmanInfo) + sizeof(DequantInfo) +
                               sizeof(IMDCTInfo) + sizeof(SubbandInfo);

uint8_t saved[2][STATE_BYTES];  // index = HelixUser；放在 DDR（.bss）
MP3DecInfo* info = nullptr;
HelixUser current = HelixUser::Playback;

// 0 = MP3DecInfo；1..7 = FrameHeaderPS..SubbandInfoPS。還原時 MP3DecInfo 先寫回，指標值不變。
uint8_t* part(int i)
{
    return i == 0 ? reinterpret_cast<uint8_t*>(info)
                  : static_cast<uint8_t*>((&info->FrameHeaderPS)[i - 1]);
}

void copyOut(uint8_t* dst)
{
    for (int i = 0; i < PART_COUNT; ++i) {
        memcpy(dst, part(i), PART_SIZE[i]);
        dst += PART_SIZE[i];
    }
}

void copyIn(const uint8_t* src)
{
    for (int i = 0; i < PART_COUNT; ++i) {
        memcpy(part(i), src, PART_SIZE[i]);
        src += PART_SIZE[i];
    }
}

}  // namespace

bool helixStateInit()
{
    MP3DecInfo* const first = static_cast<MP3DecInfo*>(MP3InitDecoder());
    MP3DecInfo* const second = static_cast<MP3DecInfo*>(MP3InitDecoder());
    if (first == nullptr || first != second) {
        LOGE(TAG, "MP3InitDecoder does not return one static state; playback and analysis will not overlap");
        return false;
    }
    void* const* pointers = &first->FrameHeaderPS;
    for (int i = 0; i < PART_COUNT - 1; ++i) {
        if (pointers[i] == nullptr) {
            LOGE(TAG, "decoder part %d is null; playback and analysis will not overlap", i + 1);
            return false;
        }
    }
    info = first;
    copyOut(saved[0]);
    memcpy(saved[1], saved[0], STATE_BYTES);
    current = HelixUser::Playback;
    LOGI(TAG, "shared decoder state %u bytes; switched between playback and analysis",
         static_cast<unsigned>(STATE_BYTES));
    return true;
}

void helixUse(HelixUser user)
{
    if (info == nullptr || user == current) return;
    copyOut(saved[static_cast<uint8_t>(current)]);
    copyIn(saved[static_cast<uint8_t>(user)]);
    current = user;
}
