/*
 * ============================================================
 *  NeoPixel ShiftLight — 8 LED RPM Display
 *  Target: Arduino Uno/Nano (ATmega328P)
 *  Refactored for production embedded use
 * ============================================================
 *
 * Wiring:
 *   DATA_PIN (D3)  → NeoPixel DIN
 *   PULSE_PIN (D2) → Optocoupler output  (INT0, FALLING edge)
 *   5V / GND       → NeoPixel VCC / GND
 *
 * RPM → LED mapping:
 *   LED 0-2  Green        1000–2000 RPM
 *   LED 3-5  Yellow→Orange 2500–3500 RPM
 *   LED 6-7  Red          4000–5000 RPM
 *   ≥ SHIFT_RPM → all LEDs blink red ("Shift Now")
 * ============================================================
 */

#include <Adafruit_NeoPixel.h>

// ── Hardware ──────────────────────────────────────────────
constexpr uint8_t  DATA_PIN        = 3;
constexpr uint8_t  PULSE_PIN       = 2;
constexpr uint8_t  NUM_LEDS        = 8;
constexpr uint8_t  LED_BRIGHTNESS  = 50;

// ── RPM Signal ────────────────────────────────────────────
constexpr uint8_t  PULSES_PER_REV  = 2;
constexpr uint32_t RPM_TIMEOUT_US  = 1'000'000UL;  // engine stopped if no pulse > 1 s
constexpr uint32_t DEBOUNCE_US     = 2'000UL;       // ignore pulses closer than 2 ms

// ── Display Thresholds ────────────────────────────────────
constexpr uint16_t SHIFT_RPM       = 4500;
constexpr uint32_t BLINK_PERIOD_MS = 100;
constexpr uint32_t DRAW_PERIOD_MS  = 30;     // ~33 FPS

// ── Signal Filter ─────────────────────────────────────────
constexpr float    SMOOTH_ALPHA    = 0.2f;   // EMA weight for new sample
constexpr uint16_t SPIKE_LIMIT_RPM = 1000;   // max RPM rise per update frame
constexpr uint16_t IDLE_THRESHOLD  = 300;    // RPM below this → treat as 0

// ── Per-LED configuration (PROGMEM saves ~24 bytes RAM) ───
struct LedConfig {
    uint16_t thresholdRpm;
    uint8_t  r, g, b;
};

static const LedConfig LED_MAP[NUM_LEDS] PROGMEM = {
    { 1000, 0,   255, 0   },  // 0 Green
    { 1500, 0,   255, 0   },  // 1 Green
    { 2000, 0,   255, 0   },  // 2 Green
    { 2500, 255, 200, 0   },  // 3 Yellow
    { 3000, 255, 150, 0   },  // 4 Amber
    { 3500, 255, 80,  0   },  // 5 Orange
    { 4000, 255, 0,   0   },  // 6 Red
    { 5000, 255, 0,   0   },  // 7 Red
};

// ── Display State ─────────────────────────────────────────
enum class DisplayMode : uint8_t { NORMAL, SHIFT_BLINK };

struct DisplayState {
    DisplayMode   mode          = DisplayMode::NORMAL;
    bool          blinkOn       = false;
    uint32_t      lastBlinkMs   = 0;
    uint32_t      lastDrawMs    = 0;
};

// ── RPM Measurement (ISR-shared, volatile) ────────────────
struct RpmSignal {
    volatile uint32_t intervalUs   = 0;  // period between last two pulses
    volatile uint32_t lastPulseUs  = 0;  // timestamp of last accepted pulse
};

// ── Smoothing State ───────────────────────────────────────
struct RpmFilter {
    float smoothed = 0.0f;
};

// ── Module Instances ──────────────────────────────────────
static Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);
static RpmSignal  rpmSignal;
static RpmFilter  rpmFilter;
static DisplayState display;


// ============================================================
//  ISR  (keep as short as possible)
// ============================================================
void IRAM_ATTR pulseISR() {
    const uint32_t now      = micros();
    const uint32_t interval = now - rpmSignal.lastPulseUs;

    if (interval >= DEBOUNCE_US) {          // reject bounce
        rpmSignal.intervalUs  = interval;
        rpmSignal.lastPulseUs = now;
    }
}


// ============================================================
//  RPM Calculation
// ============================================================

/* Read ISR data atomically and derive raw RPM. */
static uint16_t readRawRpm() {
    uint32_t interval, timeSinceLast;

    // --- critical section: copy volatile data ---
    noInterrupts();
    interval      = rpmSignal.intervalUs;
    timeSinceLast = micros() - rpmSignal.lastPulseUs;
    interrupts();

    if (timeSinceLast >= RPM_TIMEOUT_US || interval == 0) {
        return 0;   // engine stopped or no signal yet
    }

    // RPM = (60 s × 10^6 µs) / interval_µs / pulses_per_rev
    return static_cast<uint16_t>((60'000'000UL / interval) / PULSES_PER_REV);
}

/* Apply spike rejection then EMA smoothing. Returns display RPM. */
static uint16_t updateRpmFilter(uint16_t rawRpm) {
    float& sm = rpmFilter.smoothed;

    // Spike rejection: limit upward jump per frame while engine is running
    if (sm > IDLE_THRESHOLD && (rawRpm - sm) > SPIKE_LIMIT_RPM) {
        rawRpm = static_cast<uint16_t>(sm + SPIKE_LIMIT_RPM);
    }

    // Below idle threshold → snap to 0 (avoids creep to false positive)
    if (rawRpm < IDLE_THRESHOLD) {
        sm = 0.0f;
    } else {
        sm = SMOOTH_ALPHA * rawRpm + (1.0f - SMOOTH_ALPHA) * sm;
    }

    return static_cast<uint16_t>(sm);
}


// ============================================================
//  LED / Display Layer
// ============================================================

static void showAllBlink(bool on) {
    const uint32_t color = on ? strip.Color(255, 0, 0) : 0;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, color);
    }
    strip.show();
}

static void showNormal(uint16_t rpm) {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        // Read threshold and colour from PROGMEM
        LedConfig cfg;
        memcpy_P(&cfg, &LED_MAP[i], sizeof(LedConfig));

        const uint32_t color = (rpm >= cfg.thresholdRpm)
            ? strip.Color(cfg.r, cfg.g, cfg.b)
            : 0;
        strip.setPixelColor(i, color);
    }
    strip.show();
}

/* Main display update — called every DRAW_PERIOD_MS. */
static void updateDisplay(uint16_t rpm) {
    if (rpm >= SHIFT_RPM) {
        display.mode = DisplayMode::SHIFT_BLINK;
    } else {
        display.mode = DisplayMode::NORMAL;
        display.blinkOn = false;  // reset so first blink starts fresh next time
    }

    if (display.mode == DisplayMode::SHIFT_BLINK) {
        const uint32_t now = millis();
        if (now - display.lastBlinkMs >= BLINK_PERIOD_MS) {
            display.blinkOn    = !display.blinkOn;
            display.lastBlinkMs = now;
        }
        showAllBlink(display.blinkOn);
    } else {
        showNormal(rpm);
    }
}


// ============================================================
//  Arduino Entry Points
// ============================================================

void setup() {
    Serial.begin(9600);

    pinMode(PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PULSE_PIN), pulseISR, FALLING);

    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    strip.clear();
    strip.show();
}

void loop() {
    const uint32_t now = millis();

    if (now - display.lastDrawMs < DRAW_PERIOD_MS) {
        return;   // nothing to do yet — avoid busy-spin with early return
    }
    display.lastDrawMs = now;

    const uint16_t rawRpm     = readRawRpm();
    const uint16_t displayRpm = updateRpmFilter(rawRpm);

    Serial.println(displayRpm);   // remove or guard with #ifdef DEBUG in production

    updateDisplay(displayRpm);
}
