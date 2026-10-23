#ifndef OLED_SH1106_H
#define OLED_SH1106_H

#include <Wire.h>
#include <Adafruit_OLED_libraries/Adafruit_GFX.h>

#include "globalData.h"

// Minimal 128x64 SH1106 I2C driver on top of the SDK's Adafruit_GFX.
// SH1106 only supports page addressing and its 132-column RAM puts the
// visible area at column 2, so SDK's Adafruit_SSD1306 cannot drive it.
class OledSh1106 : public Adafruit_GFX {
public:
    static constexpr int16_t WIDTH = 128;
    static constexpr int16_t HEIGHT = 64;

    OledSh1106(TwoWire* wire, uint8_t address)
        : Adafruit_GFX(WIDTH, HEIGHT), wire_(wire), address_(address) {}

    // Returns false when the display does not acknowledge on the bus.
    bool begin()
    {
        {
            I2cLock lock;
            wire_->begin();
            wire_->setClock(400000);
        }
        static const uint8_t init[] = {
            0xAE,             // display off
            0xD5, 0x80,       // clock divide
            0xA8, 0x3F,       // multiplex 1/64
            0xD3, 0x00,       // display offset
            0x40,             // start line
            0xAD, 0x8B,       // internal DC-DC on
            0xA1,             // segment remap
            0xC8,             // COM scan direction
            0xDA, 0x12,       // COM pins
            0x81, 0xCF,       // contrast
            0xD9, 0xF1,       // pre-charge
            0xDB, 0x40,       // VCOM deselect
            0x32,             // pump voltage 8.0V
            0xA4,             // follow RAM
            0xA6,             // normal (not inverted)
        };
        for (uint8_t c : init) {
            if (!command(c)) return false;
        }
        clearDisplay();
        display(true);
        return command(0xAF);
    }

    void clearDisplay() { memset(buffer_, 0, sizeof(buffer_)); }

    // Panel sleep (0xAE) / wake (0xAF); RAM content is kept.
    bool setPower(bool on) { return command(on ? 0xAF : 0xAE); }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override
    {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
        uint8_t& cell = buffer_[x + (y >> 3) * WIDTH];
        if (color) cell |= (1 << (y & 7));
        else cell &= ~(1 << (y & 7));
    }

    // Sends only the 8-pixel-high pages that changed since the last call
    // (all pages when force is set), so a small update costs a few ms, not ~26 ms.
    // A page whose transfer failed is sent again on the next call.
    // Returns false if any page transfer failed.
    bool display(bool force = false)
    {
        bool allOk = true;
        for (uint8_t page = 0; page < HEIGHT / 8; page++) {
            uint8_t* now = &buffer_[page * WIDTH];
            uint8_t* sent = &sent_[page * WIDTH];
            if (!force && memcmp(now, sent, WIDTH) == 0) continue;
            // column low nibble 2: skip the 2 unused columns
            bool ok = command(0xB0 | page) && command(0x02) && command(0x10);
            // Wire's buffer is 128 bytes; keep each transfer small.
            for (int16_t x = 0; ok && x < WIDTH; x += CHUNK) {
                I2cLock lock;
                wire_->beginTransmission(address_);
                wire_->write((uint8_t)0x40);
                wire_->write(&now[x], (size_t)CHUNK);
                ok = wire_->endTransmission() == 0;
            }
            if (ok) memcpy(sent, now, WIDTH);
            else {
                sent[0] = static_cast<uint8_t>(~now[0]);  // guarantees a resend
                allOk = false;
            }
        }
        return allOk;
    }

    // Re-sends the registers that decide vertical placement, then every page.
    // A glitched I2C transfer can corrupt them (or a page) and nothing else would
    // ever repair it, so call this periodically.
    bool refresh()
    {
        command(0x40);              // display start line 0
        command(0xD3);
        command(0x00);              // display offset 0
        command(0xA1);              // segment remap
        command(0xC8);              // COM scan direction
        return display(true);
    }

private:
    static constexpr int16_t CHUNK = 16;

    bool command(uint8_t c)
    {
        I2cLock lock;
        wire_->beginTransmission(address_);
        wire_->write((uint8_t)0x00);
        wire_->write(c);
        return wire_->endTransmission() == 0;
    }

    TwoWire* wire_;
    uint8_t address_;
    uint8_t buffer_[WIDTH * HEIGHT / 8];
    uint8_t sent_[WIDTH * HEIGHT / 8];  // what the panel currently shows
};

#endif // OLED_SH1106_H
