/*
 * ==========================================
 *   NeoPixel ShiftLight - 8 LED RPM Display
 *   ช่วง RPM: 0 - 7000 RPM
 * ==========================================
 *
 * การเชื่อมต่อ:
 *   - DATA_PIN → Arduino Pin 3
 *   - NeoPixel VCC → 5V
 *   - NeoPixel GND → GND
 *   - PULSE_PIN → Pin D2 (Interrupt, ต่อกับ Optocoupler)
 *
 * การแสดงผล LED:
 *   LED 0-2  → สีเขียว      ( 500 - 2000 RPM)
 *   LED 3-5  → สีเหลือง/ส้ม (3000 - 5000 RPM)
 *   LED 6-7  → สีแดง        (6000 - 7000 RPM)
 *   7000+ RPM → กะพริบทั้งหมดสีแดง (Shift Now!)
 * ==========================================
 */

#include <Adafruit_NeoPixel.h>

// ==================== การตั้งค่า ====================
#define DATA_PIN              3
#define NUM_LEDS              8
#define BRIGHTNESS            255

#define RPM_SHIFT             4500    
#define BLINK_INTERVAL        100     // ms
#define PULSE_PIN             2
#define PULSES_PER_REV        2
#define RPM_TIMEOUT_US        1000000
#define RPM_DEBOUNCE_US       2000

// --- ตั้งค่าสำหรับกรองสัญญาณ ---
#define MAX_RPM_JUMP          1000    // (ใหม่) ป้องกันรอบกระโดดเกิน 1000 RPM ภายใน 30ms
const float SMOOTHING_FACTOR = 0.2;

const int rpmSteps[NUM_LEDS] = {
  1000,  // LED 1 → เขียว
  1500,  // LED 2 → เขียว
  2000,  // LED 3 → เขียว
  2500,  // LED 4 → เหลือง
  3000,  // LED 4 → ส้มเหลือง
  3500,  // LED 5 → ส้ม
  4000,  // LED 6 → แดง
  5000   // LED 7 → แดง (Redline)
};

const uint8_t ledColors[NUM_LEDS][3] = {
  {0,   255, 0  },  // เขียว
  {0,   255, 0  },  // เขียว
  {0,   255, 0  },  // เขียว
  {255, 200, 0  },  // เหลือง
  {255, 150, 0  },  // ส้มเหลือง
  {255, 80,  0  },  // ส้ม
  {255, 0,   0  },  // แดง
  {255, 0,   0  },  // แดง
};

// ==================== ตัวแปร ====================
Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulseInterval = 0;

float         smoothedRpm   = 0;
bool          blinkState    = false;
unsigned long lastBlinkTime = 0;
unsigned long lastDrawTime  = 0;

// ==========================================
// ISR: รับพัลส์จาก Optocoupler
// ==========================================
void pulseISR() {
  unsigned long now      = micros();
  unsigned long interval = now - lastPulseTime;

  if (interval > RPM_DEBOUNCE_US) {
    pulseInterval = interval;
    lastPulseTime = now;
  }
}

// ==========================================
// แสดงผล ShiftLight ตาม RPM
// ==========================================
void displayShiftLight(int rpm) {

  if (rpm >= RPM_SHIFT) {
    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL) {
      blinkState    = !blinkState;
      lastBlinkTime = now;
    }

    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, blinkState
        ? strip.Color(255, 0, 0)
        : strip.Color(0,   0, 0));
    }
    strip.show();
    return;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, (rpm >= rpmSteps[i])
      ? strip.Color(ledColors[i][0], ledColors[i][1], ledColors[i][2])
      : strip.Color(0, 0, 0));
  }
  strip.show();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(9600);

  pinMode(PULSE_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PULSE_PIN), pulseISR, FALLING);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
}

// ==================== Loop ====================
void loop() {
  unsigned long now = millis();

  // อัปเดตการแสดงผลไฟประมาณ 33 FPS
  if (now - lastDrawTime >= 30) {  
    lastDrawTime = now;

    noInterrupts();
    unsigned long interval           = pulseInterval;
    unsigned long timeSinceLastPulse = micros() - lastPulseTime;
    interrupts();

    // 1. คำนวณค่า RPM จากระยะเวลาของพัลส์
    int rawRpm = (timeSinceLastPulse < RPM_TIMEOUT_US && interval > 0)
      ? (60000000UL / interval) / PULSES_PER_REV
      : 0;

    // 2. เช็ค RPM Error Up (Spike Rejection)
    // หากรอบเครื่องทำงานอยู่ (> 300 RPM) และมีการกระโดดของรอบเกินค่าที่กำหนด
    if (smoothedRpm > 300 && (rawRpm - smoothedRpm) > MAX_RPM_JUMP) {
      // จำกัดค่า rawRpm ไว้ไม่ให้เกินค่าที่ยอมรับได้ (เพิกเฉย Noise ที่พุ่งสูงปรี๊ด)
      rawRpm = smoothedRpm + MAX_RPM_JUMP; 
    }

    // 3. ทำ Data Smoothing กรองค่าไม่ให้ไฟแกว่ง
    smoothedRpm = (rawRpm < 300)
      ? 0
      : (SMOOTHING_FACTOR * rawRpm) + ((1.0 - SMOOTHING_FACTOR) * smoothedRpm);

    // ปริ้นต์ค่าที่คำนวณได้ออก Serial Monitor เพื่อดูการทำงาน
    Serial.println(smoothedRpm);

    // ส่งค่าไปควบคุมการแสดงผลไฟ LED
    displayShiftLight((int)smoothedRpm);
  }
}
