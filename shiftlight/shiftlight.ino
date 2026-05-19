#include <Adafruit_NeoPixel.h>

// ── Config ────────────────────────────────────────────────
constexpr uint8_t  DATA_PIN         = 3;
constexpr uint8_t  PULSE_PIN        = 2;
constexpr uint8_t  NUM_LEDS         = 8;
constexpr uint8_t  BRIGHTNESS       = 10;

constexpr uint8_t  PULSES_PER_REV   = 2;
constexpr uint32_t RPM_TIMEOUT_US   = 1000000UL;
constexpr uint32_t DEBOUNCE_US      = 2000UL;

constexpr uint16_t SHIFT_RPM        = 4500;
constexpr uint16_t IDLE_THRESHOLD   = 300;
constexpr uint16_t SPIKE_LIMIT      = 1000;
constexpr uint32_t BLINK_MS         = 100;
constexpr uint32_t DRAW_MS          = 30;

constexpr float    ALPHA_FAST       = 0.5f;
constexpr float    ALPHA_SLOW       = 0.15f;
constexpr float    ALPHA_THRESHOLD  = 200.0f;

// ── LED Map ───────────────────────────────────────────────
struct LedConfig { uint16_t rpm; uint8_t r, g, b; };

static const LedConfig LED_MAP[NUM_LEDS] PROGMEM = {
    { 1000,   0, 255,   0 },
    { 1500,   0, 255,   0 },
    { 2000,   0, 255,   0 },
    { 2500, 255, 200,   0 },
    { 3000, 255, 150,   0 },
    { 3500, 255,  80,   0 },
    { 4000, 255,   0,   0 },
    { 5000, 255,   0,   0 },
};

// ── State ─────────────────────────────────────────────────
static Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

static volatile uint32_t isrInterval  = 0;
static volatile uint32_t isrLastPulse = 0;

static float    smoothed    = 0.0f;
static bool     blinkOn     = false;
static uint32_t lastBlinkMs = 0;
static uint32_t lastDrawMs  = 0;

// ── ISR ───────────────────────────────────────────────────
void pulseISR() {
    const uint32_t now = micros();
    const uint32_t dt  = now - isrLastPulse;
    if (dt < DEBOUNCE_US) return;
    isrInterval  = dt;
    isrLastPulse = now;
}

// ── RPM ───────────────────────────────────────────────────
static uint16_t readRpm() {
    noInterrupts();
    const uint32_t interval     = isrInterval;
    const uint32_t timeSinceLast = micros() - isrLastPulse;
    interrupts();

    if (timeSinceLast >= RPM_TIMEOUT_US || interval == 0) return 0;
    return static_cast<uint16_t>((60000000UL / interval) / PULSES_PER_REV);
}

static uint16_t filterRpm(uint16_t raw) {
    // Clamp upward spike
    if (smoothed > IDLE_THRESHOLD && (raw - smoothed) > SPIKE_LIMIT)
        raw = static_cast<uint16_t>(smoothed + SPIKE_LIMIT);

    if (raw < IDLE_THRESHOLD) { smoothed = 0.0f; return 0; }

    const float alpha = (fabsf(raw - smoothed) > ALPHA_THRESHOLD) ? ALPHA_FAST : ALPHA_SLOW;
    smoothed = alpha * raw + (1.0f - alpha) * smoothed;
    return static_cast<uint16_t>(smoothed);
}

// ── Display ───────────────────────────────────────────────
static void renderNormal(uint16_t rpm) {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        LedConfig cfg;
        memcpy_P(&cfg, &LED_MAP[i], sizeof(LedConfig));
        strip.setPixelColor(i, rpm >= cfg.rpm ? strip.Color(cfg.r, cfg.g, cfg.b) : 0);
    }
    strip.show();
}

static void renderShift() {
    const uint32_t now = millis();
    if (now - lastBlinkMs >= BLINK_MS) {
        blinkOn     = !blinkOn;
        lastBlinkMs = now;
    }
    const uint32_t color = blinkOn ? strip.Color(255, 0, 0) : 0;
    for (uint8_t i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
}

// ── Arduino ───────────────────────────────────────────────
void setup() {
    pinMode(PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PULSE_PIN), pulseISR, FALLING);
    strip.begin();
    strip.setBrightness(BRIGHTNESS);
    strip.clear();
    strip.show();
}

void loop() {
    const uint32_t now = millis();
    if (now - lastDrawMs < DRAW_MS) return;
    lastDrawMs = now;

    const uint16_t rpm = filterRpm(readRpm());
    (rpm >= SHIFT_RPM) ? renderShift() : renderNormal(rpm);
}
