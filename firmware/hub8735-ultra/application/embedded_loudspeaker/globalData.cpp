#include "globalData.h"

bool appQueuesInit()
{
    bleRxFrameQueue = xQueueCreate(8, sizeof(BleFrameElement));
    ppgSensorQueue  = xQueueCreate(16, sizeof(PpgSensorElement));
    gSensorQueue    = xQueueCreate(16, sizeof(GSensorElement));
    audioCmdQueue   = xQueueCreate(8, sizeof(AudioCmdElement));
    uiUpdateQueue   = xQueueCreate(8, sizeof(UiUpdateElement));
    bleTxQueue      = xQueueCreate(8, sizeof(ControlCmdElement));

    return bleRxFrameQueue &&
           ppgSensorQueue &&
           gSensorQueue &&
           audioCmdQueue &&
           uiUpdateQueue &&
           bleTxQueue;
}