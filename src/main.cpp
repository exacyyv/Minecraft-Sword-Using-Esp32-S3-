#include <Arduino.h>
#include <Wire.h>
#include <BleCombo.h>

// ---- Tuning ----
const float SWING_THRESHOLD_DPS = 380.0f;
const float DEADZONE_DPS        = 8.0f;
const float CAM_SENSITIVITY     = 3.5f;   // mouse counts per degree rotated
const int   YAW_SIGN            = -1;     // flip to 1 if camera turns the wrong way left/right
const int   PITCH_SIGN          = -1;     // flip to 1 if camera looks the wrong way up/down
const unsigned long SWING_COOLDOWN_MS = 350;
const unsigned long LOOP_MS = 10;
const unsigned long DEBOUNCE_MS = 30;  // a button level must hold this long to count

const bool TEST_ALWAYS_AIM = false;  // true: gyro always moves the cursor (swing clicks disabled)

// ---- Pins ----
const int PIN_SDA = 8, PIN_SCL = 9;
const int PIN_AIM = 4, PIN_JUMP = 5, PIN_W = 7, PIN_USE = 11, PIN_HOTBAR = 10, PIN_BOOT = 0;  // BOOT button on the board also works as aim

const uint8_t MPU_ADDR = 0x68;
const float GYRO_LSB_PER_DPS = 65.5f;  // +-500 dps

BleComboKeyboard keyboard("Minecraft Sword", "Espressif", 100);
BleComboMouse mouse(&keyboard);

// Active-LOW button with debounce. A button that already reads pressed at boot (legs shorted,
// bad wiring) stays ignored until it has been seen released once, so it can't spam keys.
struct Button { const char *name; int pin; char key; uint8_t mouseBtn; bool raw, stable, armed, held, down; unsigned long changedAt; };
// Mouse buttons follow the user's game: right = attack/mine, left = use/place.
const uint8_t SCROLL_STEP_BTN = 0xFF;  // pseudo mouse button: one scroll step on press (next hotbar slot)
Button buttons[] = {{"WALK", PIN_W, 'w', 0}, {"JUMP", PIN_JUMP, ' ', 0}, {"USE", PIN_USE, 0, MOUSE_LEFT},
                    {"HOTBAR", PIN_HOTBAR, 0, SCROLL_STEP_BTN}, {"AIM", PIN_AIM, 0, 0}};
Button &aimButton = buttons[4];

bool imuOk = false, wasConnected = false;
float gx_off = 0, gy_off = 0, gz_off = 0;
float accX = 0, accY = 0;
unsigned long lastSwing = 0, lastLoop = 0;

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool mpuReadGyroRaw(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 6) != 6) return false;
  x = (Wire.read() << 8) | Wire.read();
  y = (Wire.read() << 8) | Wire.read();
  z = (Wire.read() << 8) | Wire.read();
  return true;
}

bool readGyro(float &x, float &y, float &z) {
  int16_t rx, ry, rz;
  if (!mpuReadGyroRaw(rx, ry, rz)) return false;
  x = rx / GYRO_LSB_PER_DPS - gx_off;
  y = ry / GYRO_LSB_PER_DPS - gy_off;
  z = rz / GYRO_LSB_PER_DPS - gz_off;
  return true;
}

void initButtons() {
  for (Button &b : buttons) pinMode(b.pin, INPUT_PULLUP);
  delay(20);  // let the pull-ups settle before the first read
  unsigned long now = millis();
  for (Button &b : buttons) {
    b.raw = b.stable = digitalRead(b.pin) == LOW;
    b.armed = !b.stable;
    b.changedAt = now;
    if (b.stable) Serial.printf("WARNING: %s (GPIO%d) reads PRESSED at boot - ignored until released. Check its legs and wiring.\n", b.name, b.pin);
  }
}

void updateButtons(unsigned long now) {
  for (Button &b : buttons) {
    bool rawNow = digitalRead(b.pin) == LOW;
    if (rawNow != b.raw) { b.raw = rawNow; b.changedAt = now; }
    if (now - b.changedAt >= DEBOUNCE_MS) b.stable = b.raw;
    if (!b.stable) b.armed = true;
    b.held = b.stable && b.armed;
  }
}

void calibrateGyro() {
  Serial.println("Calibrating gyro: keep the sword still...");
  long sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 200; i++) {
    int16_t x, y, z;
    if (imuOk && mpuReadGyroRaw(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(10);
  }
  if (n > 0) {
    gx_off = sx / (float)n / GYRO_LSB_PER_DPS;
    gy_off = sy / (float)n / GYRO_LSB_PER_DPS;
    gz_off = sz / (float)n / GYRO_LSB_PER_DPS;
  }
  Serial.printf("Gyro offsets %.2f %.2f %.2f (IMU %s)\n", gx_off, gy_off, gz_off, imuOk ? "OK" : "MISSING");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  initButtons();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 1);
  uint8_t who = Wire.available() ? Wire.read() : 0xFF;
  Serial.printf("WHO_AM_I = 0x%02X\n", who);
  imuOk = (who == 0x70 || who == 0x71 || who == 0x73);

  mpuWrite(0x6B, 0x01);  // wake
  delay(100);
  mpuWrite(0x1A, 0x03);  // gyro DLPF ~41 Hz
  mpuWrite(0x1B, 0x08);  // +-500 dps

  calibrateGyro();

  keyboard.begin();
  mouse.begin();
  Serial.println("BLE started: pair 'Minecraft Sword' on your PC");
  lastLoop = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastLoop < LOOP_MS) return;
  float dt = (now - lastLoop) / 1000.0f;
  lastLoop = now;

  // Handle incoming serial commands (e.g. CAL, PING)
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "CAL" || cmd == "CALIBRATE") {
      calibrateGyro();
    } else if (cmd == "PING") {
      Serial.println("{\"type\":\"pong\"}");
    }
  }

  updateButtons(now);

  bool connected = keyboard.isConnected();
  if (connected != wasConnected) {
    Serial.println(connected ? "BLE connected" : "BLE disconnected");
    if (connected) {
      keyboard.releaseAll();
      mouse.release(MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    }
    wasConnected = connected;
  }

  // Handle button events (both serial log and BLE action)
  for (Button &b : buttons) {
    if (b.held == b.down) continue;
    b.down = b.held;
    Serial.printf("%s %s\n", b.name, b.held ? "down" : "up");
    if (connected) {
      if (b.mouseBtn == SCROLL_STEP_BTN) { if (b.held) mouse.move(0, 0, -1); }
      else if (b.key) { if (b.held) keyboard.press(b.key); else keyboard.release(b.key); }
      else if (b.mouseBtn) { if (b.held) mouse.press(b.mouseBtn); else mouse.release(b.mouseBtn); }
    }
  }

  // Periodic human-readable button debug line
  static unsigned long lastBtnDbg = 0;
  static uint8_t lastMask = 0;
  uint8_t mask = 0;
  for (Button &b : buttons) mask = (mask << 1) | (b.raw ? 1 : 0);
  if ((mask != lastMask && now - lastBtnDbg > 100) || now - lastBtnDbg > 2000) {
    lastMask = mask;
    lastBtnDbg = now;
    Serial.print("buttons (1=released):");
    for (Button &b : buttons) Serial.printf(" %s(GPIO%d)=%d%s", b.name, b.pin, digitalRead(b.pin), b.armed ? "" : " STUCK");
    Serial.printf(" BOOT=%d\n", digitalRead(PIN_BOOT));
  }

  // Always read gyro even if BLE is disconnected, so the HUD and diagnostic suite work
  float gx = 0, gy = 0, gz = 0;
  bool gyroValid = imuOk && readGyro(gx, gy, gz);

  bool aiming = TEST_ALWAYS_AIM || aimButton.held || digitalRead(PIN_BOOT) == LOW;
  static bool wasAiming = false;
  if (aiming != wasAiming) {
    Serial.printf("AIM %s (GPIO%d=%d BOOT=%d)\n", aiming ? "down" : "up", PIN_AIM, digitalRead(PIN_AIM), digitalRead(PIN_BOOT));
    wasAiming = aiming;
  }

  float mag = gyroValid ? sqrtf(gx * gx + gy * gy + gz * gz) : 0;
  static float peak = 0;
  peak = fmaxf(peak, mag);

  static unsigned long lastDbg = 0;
  if (now - lastDbg > 500) {
    lastDbg = now;
    Serial.printf("peak %.0f dps, aiming=%d\n", peak, aiming);
    peak = 0;
  }

  bool swingThisTick = false;
  if (mag > SWING_THRESHOLD_DPS && now - lastSwing > SWING_COOLDOWN_MS) {
    lastSwing = now;
    swingThisTick = true;
    Serial.printf("SWING (%.0f dps)\n", mag);
    if (connected) {
      mouse.press(MOUSE_RIGHT);
      delay(150);  // Minecraft polls input per tick, so brief click may be missed
      mouse.release(MOUSE_RIGHT);
    }
  }

  if (connected && aiming) {
    float yaw = fabsf(gz) > DEADZONE_DPS ? gz : 0;
    float pitch = fabsf(gx) > DEADZONE_DPS ? gx : 0;
    accX += yaw * dt * CAM_SENSITIVITY * YAW_SIGN;
    accY += pitch * dt * CAM_SENSITIVITY * PITCH_SIGN;
    int mx = (int)accX, my = (int)accY;
    if (mx || my) {
      accX -= mx;
      accY -= my;
      mouse.move(constrain(mx, -127, 127), constrain(my, -127, 127));
    }
  } else {
    accX = accY = 0;
  }

  // Periodic structured telemetry for HUD (every 50 ms = 20 Hz)
  static unsigned long lastTelem = 0;
  if (now - lastTelem >= 50) {
    lastTelem = now;
    int walkVal = buttons[0].held ? 1 : 0;
    int jumpVal = buttons[1].held ? 1 : 0;
    int useVal = buttons[2].held ? 1 : 0;
    int hotbarVal = buttons[3].held ? 1 : 0;
    int aimVal = buttons[4].held ? 1 : 0;
    int bootVal = (digitalRead(PIN_BOOT) == LOW) ? 1 : 0;
    Serial.printf("TELEMETRY:{\"walk\":%d,\"jump\":%d,\"use\":%d,\"hotbar\":%d,\"aim\":%d,\"boot\":%d,\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f,\"mag\":%.1f,\"ble\":%d,\"aiming\":%d,\"swing\":%d}\n",
      walkVal, jumpVal, useVal, hotbarVal, aimVal, bootVal, gx, gy, gz, mag, connected ? 1 : 0, aiming ? 1 : 0, swingThisTick ? 1 : 0);
  }
}
         