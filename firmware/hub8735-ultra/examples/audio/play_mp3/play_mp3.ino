/*
 * PlayMp3 - play an MP3 file from the SD card through MAX98357.
 *
 * Wiring (I2S0 fixed pin group):
 *   MAX98357 BCLK -> D24, LRC -> D12, DIN -> D11, VIN -> 5V, GND -> GND
 *
 * Put an MP3 named test001.mp3 in the SD card root.
 */

#include "AmebaFatFS.h"
#include <MAX98357.h>

AmebaFatFS fs;
MAX98357 amp;

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    delay(1000);

    fs.begin();
    amp.begin();
    amp.setVolume(0.8);

    Serial.println("Playing music2.mp3 ...");
    if (amp.playMp3(fs, "music2.mp3")) {
        Serial.println("Done.");
    } else {
        Serial.print("Playback failed: ");
        Serial.println(amp.lastError());
    }
}

void loop()
{
    digitalWrite(LED_BUILTIN, LOW);
    delayMicroseconds(500);
}
