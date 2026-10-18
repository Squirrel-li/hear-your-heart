#include "globalData.h"

#include "debug.h"

namespace {

QueueHandle_t createQueue(const char* name, UBaseType_t length, UBaseType_t itemSize)
{
    QueueHandle_t queue = xQueueCreate(length, itemSize);
    if (queue == nullptr) {
        LOGE("MAIN", "queue '%s' create failed (len=%u, item=%u bytes)", name,
             static_cast<unsigned>(length), static_cast<unsigned>(itemSize));
    } else {
        LOGD("MAIN", "queue '%s' created (len=%u, item=%u bytes)", name,
             static_cast<unsigned>(length), static_cast<unsigned>(itemSize));
    }
    return queue;
}

} // namespace

bool appQueuesInit()
{
    bleRxFrameQueue = createQueue("bleRxFrame", 8, sizeof(BleFrameElement));
    ppgSensorQueue  = createQueue("ppgSensor", 16, sizeof(PpgSensorElement));
    accSensorQueue  = createQueue("accSensor", 16, sizeof(AccElement));
    gyroSensorQueue = createQueue("gyroSensor", 16, sizeof(GyroElement));
    targetBpmResultQueue = createQueue("targetBpmResult", 1, sizeof(TargetBpmResult));
    motionLevelQueue = createQueue("motionLevel", 1, sizeof(uint8_t));
    audioCmdQueue   = createQueue("audioCmd", 8, sizeof(AudioCmdElement));
    audioPlaybackEventQueue = createQueue("audioPlaybackEvent", 8, sizeof(AudioPlaybackEvent));
    uiUpdateQueue   = createQueue("uiUpdate", 16, sizeof(UiUpdateElement));
    bleTxQueue      = createQueue("bleTx", 8, sizeof(ControlCmdElement));
    uiCmdQueue      = createQueue("uiCmd", 4, sizeof(UiCommandElement));
    analysisCmdQueue = createQueue("analysisCmd", 4, sizeof(AnalysisCmdElement));
    bleCmdQueue     = createQueue("bleCmd", 4, sizeof(BleCmdElement));

    return bleRxFrameQueue &&
           ppgSensorQueue &&
           accSensorQueue &&
           gyroSensorQueue &&
           targetBpmResultQueue &&
           motionLevelQueue &&
           audioCmdQueue &&
           audioPlaybackEventQueue &&
           uiUpdateQueue &&
           bleTxQueue &&
           uiCmdQueue &&
           analysisCmdQueue &&
           bleCmdQueue;
}
