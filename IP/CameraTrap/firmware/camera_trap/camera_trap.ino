/*
 * FLL Camera Trap — Seeed Studio XIAO ESP32S3 Sense firmware
 *
 * What it does, every time an animal walks by:
 *   1. The PIR motion sensor on D4 wakes the ESP32 from deep sleep.
 *   2. Wi-Fi starts connecting in the background.
 *   3. The camera takes BURST_COUNT photos.
 *   4. The DHT22 sensor (on D1) measures temperature and humidity.
 *   5. Each photo is saved to the microSD card, a line is added to log.csv on
 *      the card, and the photo is uploaded to the cloud along with the time,
 *      temperature and humidity. The LED on D6 blinks after each saved photo.
 *   6. Catch-up: if Wi-Fi works, also sends up to CATCHUP_PER_WAKE older photos
 *      that log.csv says were never uploaded (taken while out of Wi-Fi range).
 *   7. Waits for the PIR sensor to go quiet, then goes back to deep sleep.
 *
 * Same pins as the team's PIR_sensor_ESP32_LED sketch (IP folder on GitHub):
 *   PIR OUT -> D4,  LED -> D6 (through 330 ohm),  microSD = onboard slot (CS 21)
 *   DHT22 data -> D1 (new)
 *
 * Arduino IDE settings:
 *   Board:  "XIAO_ESP32S3"   (install "esp32 by Espressif" in Boards Manager)
 *   PSRAM:  "OPI PSRAM"      <- required for the camera
 * Library needed (Tools -> Manage Libraries):
 *   "DHT sensor library" by Adafruit (also install "Adafruit Unified Sensor" if asked)
 *
 * Note: the Serial Monitor disconnects while the board sleeps (the USB port
 * turns off). It reconnects each time motion wakes it up.
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <DHT.h>
#include "driver/rtc_io.h"
#include "config.h"
#include "google_roots.h"

// ---------- Pins ----------
#define PIR_PIN        GPIO_NUM_5    // D4: PIR sensor output (must be an RTC pin to wake from sleep)
#define LED_PIN        43            // D6: LED that blinks after each saved photo (HIGH = on)
#define DHT_PIN        2             // D1: DHT22 data
#define SD_CS_PIN      21            // onboard microSD slot (uses SPI pins 7, 8, 9 = D8, D9, D10)
#define LED_BLINK_MS   150           // keep it short so the light doesn't scare animals

// XIAO ESP32S3 Sense camera pins
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

Preferences prefs;   // Stores the photo counter in flash so it survives power loss
DHT dht(DHT_PIN, DHT22);
bool sdOk = false;

// Weather readings for this wake-up (NAN = no reading)
float temperatureC = NAN;
float humidityPct  = NAN;

// ---------- Camera ----------
bool startCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;  // 800x600: sharp enough, uploads quickly
    config.jpeg_quality = 10;              // 0-63, lower = better quality
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  // The first frames after waking are often too dark/green while the sensor
  // adjusts exposure. Throw a few away.
  for (int i = 0; i < 3; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }
  return true;
}

// ---------- Temperature & humidity ----------
void readClimate() {
  temperatureC = dht.readTemperature();
  humidityPct  = dht.readHumidity();
  if (isnan(temperatureC) || isnan(humidityPct)) {
    // DHT sensors sometimes miss a reading. They need 2 seconds between tries.
    delay(2100);
    temperatureC = dht.readTemperature(false, true);
    humidityPct  = dht.readHumidity(true);
  }
  if (isnan(temperatureC) || isnan(humidityPct)) {
    Serial.println("DHT22 reading failed (check wiring)");
  } else {
    Serial.printf("Temperature %.1f C, humidity %.1f%%\n", temperatureC, humidityPct);
  }
}

// ---------- Clock ----------
// The ESP32's clock keeps running in deep sleep, but forgets the time if the
// battery is removed. We set it from the internet whenever Wi-Fi works.
// Times are in UTC as "Unix time" (seconds since 1 Jan 1970); the server
// converts them to local time.
bool clockIsSet() {
  return time(nullptr) > 1704067200;  // later than 1 Jan 2024 = clock has been set
}

void startClockSync() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void waitForClock(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (!clockIsSet() && millis() - start < timeoutMs) delay(100);
}

// ---------- SD card ----------
bool startSD() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card mount failed");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card inserted");
    return false;
  }
  return true;
}

bool savePhoto(const String &path, camera_fb_t *fb) {
  File file = SD.open(path, FILE_WRITE);
  if (!file) return false;
  size_t written = file.write(fb->buf, fb->len);
  file.close();
  return written == fb->len;
}

// One line per photo in /log.csv, so photos that never got uploaded still
// have their time and weather recorded. "no" is written as "no " (3 letters) so
// catchUpUploads() can later change it to "yes" without rewriting the file.
void logPhoto(const char *name, time_t capturedAt, bool uploaded) {
  bool isNew = !SD.exists("/log.csv");
  File log = SD.open("/log.csv", FILE_APPEND);
  if (!log) return;
  if (isNew) log.println("photo,unix_time_utc,temperature_c,humidity_pct,uploaded");
  log.printf("%s,%ld,", name, (long)capturedAt);
  if (!isnan(temperatureC)) log.printf("%.1f", temperatureC);
  log.print(",");
  if (!isnan(humidityPct)) log.printf("%.1f", humidityPct);
  log.printf(",%s\n", uploaded ? "yes" : "no ");
  log.close();
}

// ---------- Wi-Fi + upload ----------
bool waitForWiFi() {
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) return false;
    delay(100);
  }
  return true;
}

// Sends one photo. Returns the HTTP code: 200 = the server has it (it also says
// 200 if it already had this photo), below 0 = couldn't reach the server.
int sendPhoto(const char *name, const uint8_t *buf, size_t len, time_t capturedAt,
              float tempC, float humidity) {
  HTTPClient http;
  WiFiClientSecure secureClient;   // for https:// (the cloud)
  WiFiClient plainClient;          // for http:// (the laptop server)
  if (String(SERVER_URL).startsWith("https://")) {
    secureClient.setCACert(GOOGLE_ROOT_CERTS);   // only trust Google's real servers
    http.begin(secureClient, SERVER_URL);
  } else {
    http.begin(plainClient, SERVER_URL);
  }
  http.setConnectTimeout(15000);
  http.setTimeout(UPLOAD_TIMEOUT_MS);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Trap-Key", TRAP_KEY);
  http.addHeader("X-Photo-Name", name);
  if (capturedAt > 0)   http.addHeader("X-Captured-At", String((long)capturedAt));
  if (!isnan(tempC))    http.addHeader("X-Temperature-C", String(tempC, 1));
  if (!isnan(humidity)) http.addHeader("X-Humidity", String(humidity, 1));
  int code = http.POST((uint8_t *)buf, len);
  if (code == 200) {
    Serial.printf("%s -> server says: %s\n", name, http.getString().c_str());
  } else {
    Serial.printf("%s -> upload failed, HTTP code %d\n", name, code);
  }
  http.end();
  return code;
}

bool uploadPhoto(const char *name, camera_fb_t *fb, time_t capturedAt) {
  return sendPhoto(name, fb->buf, fb->len, capturedAt, temperatureC, humidityPct) == 200;
}

// ---------- Catch-up uploads ----------
// Photos taken while the trap was out of Wi-Fi range are marked "no" in log.csv.
// Whenever Wi-Fi works, send up to CATCHUP_PER_WAKE of them (oldest first) and
// change their "no " to "yes". The server recognises photos it already has, so
// a photo that was also uploaded from the website is never counted twice.
// "logDone" (saved in flash) remembers how far into log.csv everything is
// finished, so we don't re-read the whole file on every wake-up.
float cellToFloat(const String &cell) {
  return cell.length() ? cell.toFloat() : NAN;
}

void catchUpUploads() {
  if (!sdOk || CATCHUP_PER_WAKE <= 0 || WiFi.status() != WL_CONNECTED) return;
  File log = SD.open("/log.csv", "r+");
  if (!log) return;

  prefs.begin("trap", false);
  uint32_t done = prefs.getUInt("logDone", 0);
  if (done > log.size()) done = 0;   // a different card or a new log file
  log.seek(done);

  int sent = 0;
  bool stop = false;
  while (!stop && sent < CATCHUP_PER_WAKE && log.available()) {
    uint32_t lineStart = log.position();
    String line = log.readStringUntil('\n');   // the '\n' itself is not included
    uint32_t lineEnd = log.position();

    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    String flag = c4 > 0 ? line.substring(c4 + 1) : "";
    flag.trim();

    if (flag == "no" && !line.startsWith("photo,")) {
      String name = line.substring(0, c1);
      time_t stamp = (time_t)line.substring(c1 + 1, c2).toInt();
      float tempC = cellToFloat(line.substring(c2 + 1, c3));
      float humidity = cellToFloat(line.substring(c3 + 1, c4));

      File photo = SD.open("/" + name, FILE_READ);
      size_t len = photo ? photo.size() : 0;
      uint8_t *buf = len ? (uint8_t *)ps_malloc(len) : nullptr;
      bool readOk = buf && photo.read(buf, len) == len;
      if (photo) photo.close();

      if (readOk) {
        sent++;
        int code = sendPhoto(name.c_str(), buf, len, stamp, tempC, humidity);
        if (code == 200) {
          // Overwrite "no " with "yes". (Older logs wrote just "no", with no room
          // for "yes"; leave those alone — the server ignores repeats anyway.)
          if (line.length() - (c4 + 1) >= 3) {
            log.seek(lineStart + c4 + 1);
            log.print("yes");
            log.seek(lineEnd);
          }
        } else if (code < 0 || code >= 500 || code == 401 || code == 403 || code == 429) {
          stop = true;   // no connection, wrong trap key, or server trouble: try again next time
        }
        // Any other answer (e.g. 400 = not a real photo) will never work, so move on.
        // A team member can still upload it from the website's Upload page.
      }
      // A missing or unreadable photo file can't be sent either, so move on.
      free(buf);
    }
    if (!stop) done = lineEnd;
  }
  log.close();
  prefs.putUInt("logDone", done);
  prefs.end();
  if (sent) Serial.printf("Catch-up: tried %d older photo(s)\n", sent);
}

// ---------- Sleep ----------
void goToSleep() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (sdOk) SD.end();
  esp_camera_deinit();
  digitalWrite(LED_PIN, LOW);

  // Wait until the PIR sensor stops reporting motion, otherwise we'd
  // wake up again instantly.
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  unsigned long start = millis();
  while (digitalRead(PIR_PIN) == HIGH && millis() - start < PIR_SETTLE_MAX_MS) {
    delay(100);
  }

  // Wake up when the PIR pin goes HIGH (motion!).
  rtc_gpio_pullup_dis(PIR_PIN);
  rtc_gpio_pulldown_en(PIR_PIN);
  esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);

  Serial.println("Going to sleep. Zzz...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------- Main ----------
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  bool wokeFromMotion = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
  Serial.println(wokeFromMotion ? "\nMotion detected!" : "\nPowered on.");

  // Start Wi-Fi now; it connects in the background while we take photos.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (!startCamera()) goToSleep();
  sdOk = startSD();

  // On first power-up: set the clock, test the sensor, and arm the trap.
  if (!wokeFromMotion) {
    if (waitForWiFi()) {
      startClockSync();
      waitForClock(10000);
    }
    Serial.println(clockIsSet() ? "Clock set from internet." : "WARNING: clock not set (no Wi-Fi?)");
    readClimate();
    catchUpUploads();
    Serial.println("Trap armed. Waiting for motion...");
    goToSleep();
  }

  prefs.begin("trap", false);
  uint32_t counter = prefs.getUInt("count", 0);
  bool wifiOk = true;   // becomes false if Wi-Fi times out, so we stop waiting for it
  bool climateRead = false;
  bool clockSyncStarted = false;

  for (int i = 0; i < BURST_COUNT; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Photo capture failed");
      continue;
    }
    time_t capturedAt = clockIsSet() ? time(nullptr) : 0;

    // Read the weather once, right after the first photo (so the photo isn't delayed).
    if (!climateRead) {
      readClimate();
      climateRead = true;
    }

    counter++;
    char name[32];
    snprintf(name, sizeof(name), "%s_%06u.jpg", DEVICE_ID, counter);
    Serial.printf("Took %s (%u bytes)\n", name, fb->len);

    if (sdOk && savePhoto(String("/") + name, fb)) {
      digitalWrite(LED_PIN, HIGH);   // quick blink = photo saved
      delay(LED_BLINK_MS);
      digitalWrite(LED_PIN, LOW);
    } else if (sdOk) {
      Serial.println("Could not save to SD card");
    }

    bool uploaded = false;
    if (wifiOk) {
      wifiOk = waitForWiFi();
      if (wifiOk) {
        if (!clockSyncStarted) {   // keep the clock accurate (finishes in the background)
          startClockSync();
          clockSyncStarted = true;
        }
        uploaded = uploadPhoto(name, fb, capturedAt);
      } else {
        Serial.println("No Wi-Fi — photo kept on SD card only");
      }
    }
    if (sdOk) logPhoto(name, capturedAt, uploaded);

    esp_camera_fb_return(fb);
    if (i < BURST_COUNT - 1) delay(BURST_DELAY_MS);
  }

  prefs.putUInt("count", counter);
  prefs.end();
  catchUpUploads();   // only runs if Wi-Fi is connected
  goToSleep();
}

void loop() {
  // Never reached — the ESP32 restarts from setup() every time it wakes up.
}
