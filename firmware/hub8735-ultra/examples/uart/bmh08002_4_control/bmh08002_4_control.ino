// HUB8735 USB Serial <-> UART3 bridge for BMH08002-4 (38400 8N1).
// Wire: HUB8735 UART3 TX -> module RX, UART3 RX -> module TX, common GND.
// Drive it from the PC with bmh08002_control.py.
//
// Module -> PC goes through a ring buffer so Serial3 (only 128 bytes of RX buffer) is always
// drained quickly, even when the USB side is slow. PC -> module is short commands, sent directly.

#define HOST_BAUD 115200
#define MODULE_BAUD 38400
#define RING_SIZE 2048    // ~2.7 s of continuous upload (750 B/s)
#define CHUNK 64

static uint8_t ring[RING_SIZE];
static volatile uint16_t head = 0;   // next write
static volatile uint16_t tail = 0;   // next read
static uint32_t dropped = 0;         // bytes lost because the ring was full

void setup()
{
    Serial.begin(HOST_BAUD);
    Serial3.begin(MODULE_BAUD);
}

void loop()
{
    while (Serial.available()) {
        Serial3.write(Serial.read());
    }

    while (Serial3.available()) {
        uint16_t next = (head + 1) % RING_SIZE;
        uint8_t b = Serial3.read();
        if (next == tail) {
            dropped++;               // ponytail: full ring drops newest bytes; add a status report if this ever shows up
        } else {
            ring[head] = b;
            head = next;
        }
    }

    uint8_t chunk[CHUNK];
    uint16_t n = 0;
    while (tail != head && n < CHUNK) {
        chunk[n++] = ring[tail];
        tail = (tail + 1) % RING_SIZE;
    }
    if (n) {
        Serial.write(chunk, n);
    }
}
