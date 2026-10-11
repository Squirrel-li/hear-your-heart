#include "globalData.h"

bool appQueuesInit()
{
    bleRxFrameQueue = xQueueCreate(8, sizeof(BleFrameElement));
    ppgSensorQueue  = xQueueCreate(16, sizeof(PpgSensorElement));
    accSensorQueue  = xQueueCreate(16, sizeof(AccElement));
    gyroSensorQueue = xQueueCreate(16, sizeof(GyroElement));
    targetBpmResultQueue = xQueueCreate(1, sizeof(TargetBpmResult));
    audioCmdQueue   = xQueueCreate(8, sizeof(AudioCmdElement));
    audioPlaybackEventQueue = xQueueCreate(8, sizeof(AudioPlaybackEvent));
    uiUpdateQueue   = xQueueCreate(16, sizeof(UiUpdateElement));
    bleTxQueue      = xQueueCreate(8, sizeof(ControlCmdElement));
    uiCmdQueue      = xQueueCreate(4, sizeof(UiCommandElement));

    return bleRxFrameQueue &&
           ppgSensorQueue &&
           accSensorQueue &&
           gyroSensorQueue &&
           targetBpmResultQueue &&
           audioCmdQueue &&
           audioPlaybackEventQueue &&
           uiUpdateQueue &&
           bleTxQueue &&
           uiCmdQueue;
}