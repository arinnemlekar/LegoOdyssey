#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// Direct PIR connection: no transistor.
const int PIR_SIGNAL_PIN = D4;

// External LED: D6 -> 330 ohm -> LED (+), LED (-) -> GND.
const int PHOTO_LED_PIN = D6;

// Onboard microSD chip-select pin.
const int SD_CS_PIN = 21;

const unsigned long PIR_WARMUP_MS = 30000;
const unsigned long DEBUG_INTERVAL_MS = 500;
const unsigned long PHOTO_LED_DURATION_MS = 1000;

bool previousMotion = false;
unsigned long photoNumber = 1;
unsigned long lastDebugTime = 0;

bool photoLedOn = false;
unsigned long photoLedStartedAt = 0;

// XIAO ESP32S3 Sense camera pins.
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40
#define SIOC_GPIO_NUM    39

#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15
#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

void turnPhotoLedOff() {
  digitalWrite(PHOTO_LED_PIN, LOW);
  photoLedOn = false;
}

void signalPhotoSaved() {
  digitalWrite(PHOTO_LED_PIN, HIGH);
  photoLedStartedAt = millis();
  photoLedOn = true;
}

// Turn off the LED without pausing PIR monitoring.
void updatePhotoLed() {
  if (photoLedOn &&
      millis() - photoLedStartedAt >= PHOTO_LED_DURATION_MS) {
    turnPhotoLedOff();
  }
}

void stopWithError(const char *message) {
  Serial.println(message);
  turnPhotoLedOff();

  while (true) {
    delay(1000);
  }
}

bool setupCamera() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_HD;  // 320 x 240
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t result = esp_camera_init(&config);

  if (result != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", result);
    return false;
  }

  Serial.println("Camera ready.");
  return true;
}

bool setupSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card mount failed.");
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card detected.");
    return false;
  }

  Serial.println("SD card ready.");
  return true;
}

bool takeAndSavePhoto() {
  Serial.println("Taking photo...");

  // Clear any previous success indication.
  turnPhotoLedOff();

  // Discard buffered frames so the saved photo is fresh.
  for (int i = 0; i < 2; i++) {
    camera_fb_t *oldFrame = esp_camera_fb_get();

    if (!oldFrame) {
      Serial.println("Camera refresh failed.");
      return false;
    }

    esp_camera_fb_return(oldFrame);
  }

  camera_fb_t *frame = esp_camera_fb_get();

  if (!frame) {
    Serial.println("Camera capture failed.");
    return false;
  }

  // Find an unused filename, including after restarting.
  char filename[40];

  do {
    snprintf(filename, sizeof(filename),
             "/photo_%06lu.jpg", photoNumber++);
  } while (SD.exists(filename));

  File file = SD.open(filename, FILE_WRITE);

  if (!file) {
    Serial.printf("Could not create %s\n", filename);
    esp_camera_fb_return(frame);
    return false;
  }

  size_t expectedBytes = frame->len;
  size_t writtenBytes = file.write(frame->buf, expectedBytes);

  file.flush();
  file.close();
  esp_camera_fb_return(frame);

  if (expectedBytes == 0 || writtenBytes != expectedBytes) {
    Serial.printf("Save failed: wrote %u of %u bytes.\n",
                  (unsigned int)writtenBytes,
                  (unsigned int)expectedBytes);

    SD.remove(filename);
    return false;
  }

  Serial.printf("Saved %s (%u bytes)\n",
                filename, (unsigned int)writtenBytes);

  // Light the LED only after capture and writing succeed.
  signalPhotoSaved();

  return true;
}

void setup() {
  Serial.begin(115200);

  pinMode(PHOTO_LED_PIN, OUTPUT);
  turnPhotoLedOff();

  delay(2000);

  // No internal pull-down: avoid loading the PIR output.
  pinMode(PIR_SIGNAL_PIN, INPUT);

  Serial.println();
  Serial.println("XIAO ESP32S3 Sense: PIR + camera + microSD");
  Serial.println("PIR on D4. Photo confirmation LED on D6.");

  if (!setupCamera()) {
    stopWithError("Stopping: camera initialization failed.");
  }

  if (!setupSDCard()) {
    stopWithError("Stopping: SD card initialization failed.");
  }

  Serial.println("Waiting 30 seconds for PIR warm-up...");
  delay(PIR_WARMUP_MS);

  // Capture once if the PIR is already HIGH after warm-up.
  previousMotion = false;

  Serial.println("Ready. LED pulses after each successful photo save.");
}

void loop() {
updatePhotoLed();

  int pinReading = digitalRead(PIR_SIGNAL_PIN);
  bool motion = (pinReading == HIGH);

  // Show the input state every half-second.
  if (millis() - lastDebugTime >= DEBUG_INTERVAL_MS) {
    lastDebugTime = millis();

    Serial.print("D4 reads: ");
    Serial.print(pinReading);
    Serial.println(motion ? " (HIGH - motion)" : " (LOW - no motion)");
  }

  // Take one photo when the signal changes LOW -> HIGH.
  if (motion && !previousMotion) {
    Serial.println("Motion detected: D4 changed to HIGH.");

    if (takeAndSavePhoto()) {
      Serial.println("Photo saved. Pausing D4 checks for 5 seconds...");

      // The success LED stays ON throughout this wait.
      delay(5000);

      turnPhotoLedOff();
      Serial.println("Resuming D4 checks.");
    } else {
      Serial.println("Photo failed. Will retry on the next motion event.");
    }

    Serial.println("Waiting for PIR to return LOW before another photo.");
  }

  if (!motion && previousMotion) {
    Serial.println("PIR returned LOW. Ready for the next motion event.");
  }

  previousMotion = motion;
  delay(20);
}