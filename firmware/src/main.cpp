#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPSPlus.h>

namespace {

constexpr uint8_t kSdChipSelectPin = D1;
constexpr uint32_t kUsbBaud = 115200;
constexpr uint32_t kGpsBaud = 9600;
constexpr uint32_t kImuSamplePeriodMs = 20;
constexpr uint32_t kSdRetryPeriodMs = 2000;
constexpr uint32_t kLogFlushPeriodMs = 1000;
constexpr char kLogFileName[] = "/flight.csv";

TinyGPSPlus gps;
File logFile;

bool imuReady = false;
bool sdReady = false;

uint32_t lastSampleMs = 0;
uint32_t lastSdRetryMs = 0;
uint32_t lastFlushMs = 0;
uint32_t lastHeartbeatMs = 0;

class Mpu6886 {
 public:
  bool begin(TwoWire& wire, uint8_t address = 0x68) {
    wire_ = &wire;
    address_ = address;

    if (!probe()) {
      return false;
    }

    if (!writeRegister(0x6B, 0x01)) {
      return false;
    }

    delay(100);

    const bool configured =
        writeRegister(0x6C, 0x00) &&  // power management 2
        writeRegister(0x1A, 0x03) &&  // low-pass filter
        writeRegister(0x19, 0x04) &&  // sample rate divider
        writeRegister(0x1B, 0x00) &&  // gyro +/- 250 dps
        writeRegister(0x1C, 0x00) &&  // accel +/- 2 g
        writeRegister(0x1D, 0x03);    // accel low-pass filter

    return configured;
  }

  bool readSample(float& ax_g,
                  float& ay_g,
                  float& az_g,
                  float& gx_dps,
                  float& gy_dps,
                  float& gz_dps,
                  float& temp_c) {
    uint8_t buffer[14];
    if (!readRegisters(0x3B, buffer, sizeof(buffer))) {
      return false;
    }

    const int16_t rawAx = static_cast<int16_t>((buffer[0] << 8) | buffer[1]);
    const int16_t rawAy = static_cast<int16_t>((buffer[2] << 8) | buffer[3]);
    const int16_t rawAz = static_cast<int16_t>((buffer[4] << 8) | buffer[5]);
    const int16_t rawTemp = static_cast<int16_t>((buffer[6] << 8) | buffer[7]);
    const int16_t rawGx = static_cast<int16_t>((buffer[8] << 8) | buffer[9]);
    const int16_t rawGy = static_cast<int16_t>((buffer[10] << 8) | buffer[11]);
    const int16_t rawGz = static_cast<int16_t>((buffer[12] << 8) | buffer[13]);

    ax_g = rawAx / 16384.0f;
    ay_g = rawAy / 16384.0f;
    az_g = rawAz / 16384.0f;
    gx_dps = rawGx / 131.0f;
    gy_dps = rawGy / 131.0f;
    gz_dps = rawGz / 131.0f;
    temp_c = (rawTemp / 333.87f) + 21.0f;

    return true;
  }

 private:
  bool probe() {
    const uint8_t whoAmI = readRegister(0x75);
    return whoAmI != 0x00 && whoAmI != 0xFF;
  }

  bool writeRegister(uint8_t reg, uint8_t value) {
    wire_->beginTransmission(address_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }

  uint8_t readRegister(uint8_t reg) {
    uint8_t value = 0xFF;
    if (!readRegisters(reg, &value, 1)) {
      return 0xFF;
    }
    return value;
  }

  bool readRegisters(uint8_t reg, uint8_t* data, size_t length) {
    wire_->beginTransmission(address_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) {
      return false;
    }

    const size_t received = wire_->requestFrom(address_, length);
    if (received != length) {
      return false;
    }

    for (size_t index = 0; index < length; ++index) {
      data[index] = wire_->read();
    }

    return true;
  }

  TwoWire* wire_ = &Wire;
  uint8_t address_ = 0x68;
};

Mpu6886 imu;

void printTwoDigits(Print& out, int value) {
  if (value < 10) {
    out.print('0');
  }
  out.print(value);
}

void printGpsTimestamp(Print& out) {
  if (!gps.date.isValid() || !gps.time.isValid()) {
    return;
  }

  out.print(gps.date.year());
  out.print('-');
  printTwoDigits(out, gps.date.month());
  out.print('-');
  printTwoDigits(out, gps.date.day());
  out.print('T');
  printTwoDigits(out, gps.time.hour());
  out.print(':');
  printTwoDigits(out, gps.time.minute());
  out.print(':');
  printTwoDigits(out, gps.time.second());
  out.print('.');
  printTwoDigits(out, gps.time.centisecond());
  out.print('Z');
}

void writeCsvHeader(Print& out) {
  out.println(F("ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,temp_c,gps_fix,gps_lat_deg,gps_lon_deg,gps_alt_m,gps_speed_kmph,gps_course_deg,gps_sats,gps_hdop_x100,gps_utc"));
}

bool openLogFile() {
  if (logFile) {
    logFile.close();
  }

  logFile = SD.open(kLogFileName, FILE_WRITE);
  if (!logFile) {
    return false;
  }

  if (logFile.size() == 0) {
    writeCsvHeader(logFile);
    logFile.flush();
  }

  return true;
}

bool ensureStorage() {
  const uint32_t now = millis();
  if (logFile) {
    return true;
  }

  if (now - lastSdRetryMs < kSdRetryPeriodMs) {
    return false;
  }

  lastSdRetryMs = now;

  if (!sdReady) {
    sdReady = SD.begin(kSdChipSelectPin);
    if (!sdReady) {
      Serial.println(F("SD init failed"));
      return false;
    }
  }

  if (!openLogFile()) {
    Serial.println(F("SD open failed"));
    return false;
  }

  Serial.println(F("SD logging ready"));
  return true;
}

void serviceGps() {
  while (Serial1.available() > 0) {
    gps.encode(static_cast<char>(Serial1.read()));
  }
}

void heartbeat() {
  const uint32_t now = millis();
  if (now - lastHeartbeatMs < 500) {
    return;
  }

  lastHeartbeatMs = now;
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void logRow(uint32_t timestampMs,
            float ax_g,
            float ay_g,
            float az_g,
            float gx_dps,
            float gy_dps,
            float gz_dps,
            float temp_c) {
  if (!logFile) {
    return;
  }

  logFile.print(timestampMs);
  logFile.print(',');
  logFile.print(ax_g, 4);
  logFile.print(',');
  logFile.print(ay_g, 4);
  logFile.print(',');
  logFile.print(az_g, 4);
  logFile.print(',');
  logFile.print(gx_dps, 2);
  logFile.print(',');
  logFile.print(gy_dps, 2);
  logFile.print(',');
  logFile.print(gz_dps, 2);
  logFile.print(',');
  logFile.print(temp_c, 2);
  logFile.print(',');
  logFile.print(gps.location.isValid() ? 1 : 0);
  logFile.print(',');

  if (gps.location.isValid()) {
    logFile.print(gps.location.lat(), 6);
  }
  logFile.print(',');

  if (gps.location.isValid()) {
    logFile.print(gps.location.lng(), 6);
  }
  logFile.print(',');

  if (gps.altitude.isValid()) {
    logFile.print(gps.altitude.meters(), 2);
  }
  logFile.print(',');

  if (gps.speed.isValid()) {
    logFile.print(gps.speed.kmph(), 2);
  }
  logFile.print(',');

  if (gps.course.isValid()) {
    logFile.print(gps.course.deg(), 2);
  }
  logFile.print(',');

  if (gps.satellites.isValid()) {
    logFile.print(gps.satellites.value());
  }
  logFile.print(',');

  if (gps.hdop.isValid()) {
    logFile.print(gps.hdop.value());
  }
  logFile.print(',');

  printGpsTimestamp(logFile);
  logFile.println();
}

void retryMpuInit() {
  static uint32_t lastAttemptMs = 0;
  const uint32_t now = millis();
  if (imuReady || now - lastAttemptMs < kSdRetryPeriodMs) {
    return;
  }

  lastAttemptMs = now;
  imuReady = imu.begin(Wire);
  if (imuReady) {
    Serial.println(F("MPU6886 ready"));
  } else {
    Serial.println(F("MPU6886 init failed"));
  }
}

}  // namespace

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(kUsbBaud);
  while (!Serial && millis() < 4000) {
  }

  Serial.println(F("Blackbox booting"));

  Wire.begin();
  Wire.setClock(400000);

  SPI.begin();

  Serial1.begin(kGpsBaud);

  imuReady = imu.begin(Wire);
  if (imuReady) {
    Serial.println(F("MPU6886 ready"));
  } else {
    Serial.println(F("MPU6886 init failed"));
  }

  sdReady = SD.begin(kSdChipSelectPin);
  if (sdReady) {
    if (openLogFile()) {
      Serial.println(F("SD logging ready"));
    } else {
      Serial.println(F("SD open failed"));
    }
  } else {
    Serial.println(F("SD init failed"));
  }
}

void loop() {
  serviceGps();
  retryMpuInit();

  if (ensureStorage()) {
    const uint32_t now = millis();
    if (imuReady && now - lastSampleMs >= kImuSamplePeriodMs) {
      lastSampleMs = now;

      float ax_g = 0.0f;
      float ay_g = 0.0f;
      float az_g = 0.0f;
      float gx_dps = 0.0f;
      float gy_dps = 0.0f;
      float gz_dps = 0.0f;
      float temp_c = 0.0f;

      if (imu.readSample(ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps, temp_c)) {
        logRow(now, ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps, temp_c);
      } else {
        imuReady = false;
        Serial.println(F("MPU6886 read failed"));
      }
    }

    if (logFile && now - lastFlushMs >= kLogFlushPeriodMs) {
      lastFlushMs = now;
      logFile.flush();
    }
  }

  heartbeat();
}
