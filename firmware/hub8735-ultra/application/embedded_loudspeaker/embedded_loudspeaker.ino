#include "AmebaFatFS.h"
#include <MAX98357.h>

AmebaFatFS fs;
MAX98357 amp;

void setup() 
{
  fs.begin();                  // SD 卡
  amp.begin();                 // I2S 初始化(佔用 D11/D12/D22/D23/D24)
  amp.setShutdownPin(10);      // 選用:SD_MODE 接 D10,不播放時硬體關斷
  amp.setVolume(0.8);          // 軟體音量 0.0 ~ 1.0

  if (!amp.playWav(fs, "test001.wav")) {   // 阻塞式播放 SD 卡根目錄的檔案
      Serial.println(amp.lastError());
  }
}

void loop() 
{
  int test_num = 0x2A;
}
