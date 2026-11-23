#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include <DHT.h>
#include <math.h>

const char *WIFI_NAME = "FLL-Camera";
// Change this password if desired; use 8-63 characters.
const char *WIFI_PASSWORD = "FLLphotos2026";
// DHT22 module: VCC -> 3V3, DATA -> D0, GND -> shared ground.
// Use a 3.3 V-compatible module with a built-in pull-up resistor.
DHT dht(D0, DHT22);
String lastDHTReading = "No measurement yet (read after a photo saves)";
bool lastDHTLogSaved = false;
WebServer server(80);
const unsigned long CAPTURE_COOLDOWN_MS = 5000;
unsigned long warmupStartedAt = 0;
unsigned long cooldownStartedAt = 0;
bool warmingUp = true;
bool coolingDown = false;
String lastSavedPhoto = "None yet";
String lastCaptureStatus = "No capture attempted";


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
const unsigned long PHOTO_LED_DURATION_MS = 5000;

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

// Turn off the LED after the success interval.
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
  config.frame_size = FRAMESIZE_HD;  // 1280 x 720
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


// One CSV per photo; same filename stem as its JPEG.
String measurementPath(const String &photoPath) {
  return photoPath.substring(0, photoPath.length() - 4) + ".csv";
}

bool saveDHTMeasurement(const String &photoPath) {
  // Read after saving, never from webpage handlers. The existing 5-second
  // cooldown spaces measurements. Both calls share one cached transaction.
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature(); // Celsius
  bool valid = isfinite(temperature) && isfinite(humidity) &&
               temperature >= -40 && temperature <= 80 &&
               humidity >= 0 && humidity <= 100;
  String csv = "temperature_c,humidity_percent,sensor_status\n";
  if (valid) {
    lastDHTReading = String(temperature, 1) + " C, " + String(humidity, 1) + " % RH";
    csv += String(temperature, 1) + "," + String(humidity, 1) + ",OK\n";
  } else {
    lastDHTReading = "Unavailable: DHT22 read failed";
    csv += ",,READ_FAILED\n";
  }
  Serial.print("DHT22: ");
  Serial.println(lastDHTReading);
  String path = measurementPath(photoPath);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Photo saved, but could not create measurement CSV.");
    return false;
  }
  size_t written = file.print(csv);
  file.flush();
  file.close();
  if (written != csv.length()) {
    SD.remove(path);
    Serial.println("Photo saved, but measurement CSV write failed.");
    return false;
  }
  Serial.print("Saved measurements: ");
  Serial.println(path);
  return true;
}

// Bounded parsing: never insert untrusted raw file contents into HTML.
String photoMeasurementSummary(const String &name) {
  File file = SD.open(measurementPath("/" + name), FILE_READ);
  if (!file) return "No recorded measurements for this photo.";
  if (file.isDirectory() || file.size() > 160) {
    file.close();
    return "Measurement file could not be read.";
  }
  char header[80] = {};
  char row[80] = {};
  size_t n = file.readBytesUntil('\n', header, sizeof(header) - 1);
  header[n] = '\0';
  n = file.readBytesUntil('\n', row, sizeof(row) - 1);
  row[n] = '\0';
  file.close();
  if (strcmp(header, "temperature_c,humidity_percent,sensor_status") != 0)
    return "Unrecognized measurement file.";
  if (strcmp(row, ",,READ_FAILED") == 0)
    return "Temperature and humidity unavailable: sensor read failed.";
  float t = NAN, h = NAN;
  char status[20] = {};
  if (sscanf(row, "%f,%f,%19s", &t, &h, status) != 3 ||
      strcmp(status, "OK") != 0 || !isfinite(t) || !isfinite(h) ||
      t < -40 || t > 80 || h < 0 || h > 100)
    return "Measurement file is incomplete or invalid.";
  return "Temperature: " + String(t, 1) + " &deg;C &nbsp; Humidity: " +
         String(h, 1) + "% RH";
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
  } while (SD.exists(filename) || SD.exists(measurementPath(String(filename))));

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

  lastSavedPhoto = filename;
  lastDHTLogSaved = saveDHTMeasurement(lastSavedPhoto);
  // Light the LED only after capture and writing succeed.
  signalPhotoSaved();

  return true;
}


// Only expose camera-generated filenames, never arbitrary SD paths.
bool validPhotoName(const String &name) {
  if (!name.startsWith("photo_") || !name.endsWith(".jpg")) return false;
  if (name.length() < 11 || name.length() > 32) return false;
  for (unsigned int i = 6; i < name.length() - 4; i++) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

void handleHome() {
  // Bound page memory: at most 30 rows, in SD directory order.
  const unsigned long PAGE_SIZE = 30;
  unsigned long page = 0;
  if (server.hasArg("page")) {
    String value = server.arg("page");
    if (value.length() > 6) {
      server.send(400, "text/plain", "Invalid page");
      return;
    }
    for (unsigned int i = 0; i < value.length(); i++) {
      if (value[i] < '0' || value[i] > '9') {
        server.send(400, "text/plain", "Invalid page");
        return;
      }
    }
    page = value.toInt();
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    server.send(500, "text/plain", "Cannot read SD directory.");
    return;
  }

  String html;
  html.reserve(12000);
  html = F("<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>FLL Camera</title><style>"
           "body{font-family:Arial,sans-serif;max-width:850px;margin:24px auto;padding:0 16px;"
           "background:#f3f6fa;color:#172435}"
           "table{width:100%;border-collapse:collapse;background:white}"
           "td,th{padding:12px;text-align:left;border-bottom:1px solid #ddd}"
           "a{color:#075ea8}nav a{display:inline-block;margin:16px 18px 16px 0}"
           "</style></head><body><h1>FLL Camera Photos</h1><p>Status: ");
  html += warmingUp ? "PIR warming up" : (coolingDown ? "Capture cooldown" : "Monitoring motion");
  html += "</p><p>Last photo saved this session: " + lastSavedPhoto;
  html += "</p><p>Last capture: " + lastCaptureStatus;
  html += "</p><p>DHT22 reading for the latest photo this session: " + lastDHTReading;
  html += F("</p><p>Refresh to see new photos. Viewing and downloading leave the originals on the SD card.</p>");
  html += "<nav><a href='/?page=" + String(page) + "'>Refresh</a></nav>";
  html += F("<table><tr><th>Photo</th><th>Size</th><th>Actions</th></tr>");

  unsigned long seen = 0;
  unsigned long shown = 0;
  bool hasNext = false;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    bool photo = !entry.isDirectory() && validPhotoName(name);
    size_t bytes = entry.size();
    entry.close();
    updatePhotoLed();
    delay(0);
    if (!photo) continue;
    if (seen++ < page * PAGE_SIZE) continue;
    if (shown == PAGE_SIZE) {
      hasNext = true;
      break;
    }
    html += "<tr><td>" + name + "</td><td>" + String((unsigned long)bytes);
    html += " bytes</td><td><a target='_blank' rel='noopener' href='/view?file=";
    html += name + "'>View</a> &nbsp; <a href='/download?file=";
    html += name + "'>Download</a></td></tr>";
    shown++;
  }
  root.close();
  if (shown == 0) html += F("<tr><td colspan='3'>No photos on this page.</td></tr>");
  html += "</table><nav>";
  if (page > 0) html += "<a href='/?page=" + String(page - 1) + "'>Previous</a>";
  if (hasNext) html += "<a href='/?page=" + String(page + 1) + "'>Next</a>";
  html += "</nav><p>Page " + String(page + 1) + "</p></body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

// Find neighbors in the same SD directory order used by the photo list.
// Scan the directory instead of keeping an unbounded filename array in RAM.
void handleViewer() {
  String name = server.arg("file");
  if (!validPhotoName(name)) {
    server.send(400, "text/plain", "Invalid photo filename.");
    return;
  }
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    server.send(500, "text/plain", "Cannot read SD directory.");
    return;
  }
  String previous;
  String next;
  bool found = false;
  unsigned long index = 0;
  unsigned long currentIndex = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String candidate = entry.name();
    int slash = candidate.lastIndexOf('/');
    if (slash >= 0) candidate = candidate.substring(slash + 1);
    bool photo = !entry.isDirectory() && validPhotoName(candidate);
    entry.close();
    updatePhotoLed();
    delay(0);
    if (!photo) continue;
    if (found) {
      next = candidate;
      break;
    }
    if (candidate == name) {
      found = true;
      currentIndex = index;
    } else {
      previous = candidate;
    }
    index++;
  }
  root.close();
  if (!found) {
    server.send(404, "text/plain", "Photo not found. Return to the photo list.");
    return;
  }

  String html;
  html.reserve(4500);
  html = F("<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>FLL Photo Viewer</title><style>"
           "body{font-family:Arial,sans-serif;margin:0;padding:16px;background:#111827;"
           "color:#f9fafb;text-align:center}h1{font-size:20px;overflow-wrap:anywhere}"
           "nav{display:flex;flex-wrap:wrap;gap:12px;justify-content:center;margin:16px 0}"
           "nav a,nav span{padding:12px 16px;border-radius:8px;background:#25364e;color:white;"
           "text-decoration:none}nav span{opacity:.4}a:focus-visible{outline:3px solid #60a5fa}"
           "img{display:block;max-width:100%;max-height:72vh;object-fit:contain;margin:auto}"
           "p{color:#cbd5e1}</style></head><body><h1>");
  html += name + "</h1><nav>";
  if (previous.length()) {
    html += "<a id='previous' href='/view?file=" + previous + "'>&larr; Previous</a>";
  } else {
    html += F("<span aria-disabled='true'>&larr; Previous</span>");
  }
  if (next.length()) {
    html += "<a id='next' href='/view?file=" + next + "'>Next &rarr;</a>";
  } else {
    html += F("<span aria-disabled='true'>Next &rarr;</span>");
  }
  html += "<a href='/download?file=" + name + "'>Download</a>";
  html += "<a href='/?page=" + String(currentIndex / 30) + "'>Back to photos</a></nav>";
  html += "<img src='/image?file=" + name + "' alt='" + name + "'>";
  html += "<p>" + photoMeasurementSummary(name) + "</p>";
  if (SD.exists(measurementPath("/" + name))) {
    html += "<p><a style='color:#93c5fd' href='/measurements?file=" + name +
            "'>Download temperature / humidity CSV</a></p>";
  }
  html += F("<p>Readings are taken just after this photo saves.</p>");
  html += F("<p>Use the left and right arrow keys, or the buttons above.</p>");
  if (!next.length()) html += F("<p>Last saved photo in this list. Refresh to check for new photos.</p>");
  html += F("<script>let navigating=false;"
            "window.addEventListener('pageshow',()=>{navigating=false;});"
            "document.addEventListener('keydown',function(e){"
            "if(e.altKey||e.ctrlKey||e.metaKey||e.shiftKey)return;"
            "if(e.target.isContentEditable||/^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName))return;"
            "let id=e.key==='ArrowRight'?'next':e.key==='ArrowLeft'?'previous':null;"
            "if(!id)return;e.preventDefault();if(e.repeat||navigating)return;"
            "let link=document.getElementById(id);"
            "if(link){navigating=true;window.location.assign(link.href);}"
            "});</script></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

void servePhoto(bool download) {
  String name = server.arg("file");
  if (!validPhotoName(name)) {
    server.send(400, "text/plain", "Invalid photo filename.");
    return;
  }
  File photo = SD.open("/" + name, FILE_READ);
  if (!photo || photo.isDirectory()) {
    if (photo) photo.close();
    server.send(404, "text/plain", "Photo not found.");
    return;
  }
  String disposition = download ? "attachment" : "inline";
  disposition += "; filename=\"" + name + "\"";
  server.sendHeader("Content-Disposition", disposition);
  server.sendHeader("Cache-Control", "no-store");
  // Synchronous transfer: no capture or other SD access runs in parallel.
  server.streamFile(photo, "image/jpeg");
  photo.close();
}


void downloadMeasurements() {
  String name = server.arg("file");
  if (!validPhotoName(name)) {
    server.send(400, "text/plain", "Invalid photo filename.");
    return;
  }
  String path = measurementPath("/" + name);
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "No measurements recorded for this photo.");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, "text/csv");
  file.close();
}

bool setupWiFi() {
  if (strlen(WIFI_PASSWORD) < 8 || strlen(WIFI_PASSWORD) > 63) return false;
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, ip, subnet)) return false;
  if (!WiFi.softAP(WIFI_NAME, WIFI_PASSWORD)) return false;
  server.on("/", HTTP_GET, handleHome);
  server.on("/view", HTTP_GET, handleViewer);
  server.on("/measurements", HTTP_GET, downloadMeasurements);
  server.on("/image", HTTP_GET, []() { servePhoto(false); });
  server.on("/download", HTTP_GET, []() { servePhoto(true); });
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found. Open http://192.168.4.1/");
  });
  server.begin();
  Serial.printf("Wi-Fi network: %s\n", WIFI_NAME);
  Serial.print("Photo page: http://");
  Serial.println(WiFi.softAPIP());
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(PHOTO_LED_PIN, OUTPUT);
  turnPhotoLedOff();
  delay(2000);
  // Preserve the working direct PIR connection without a pull-down.
  pinMode(PIR_SIGNAL_PIN, INPUT);
  dht.begin(); // Existing 30-second warm-up also lets the DHT22 settle.
  Serial.println("DHT22 DATA on D0; measurements taken after each photo save.");
  Serial.println("\nXIAO ESP32S3 Sense: PIR + camera + SD + Wi-Fi");
  if (!setupCamera()) stopWithError("Stopping: camera initialization failed.");
  if (!setupSDCard()) stopWithError("Stopping: SD card initialization failed.");
  if (!setupWiFi()) stopWithError("Stopping: Wi-Fi initialization failed.");
  warmupStartedAt = millis();
  Serial.println("PIR warming up for 30 seconds. Photo webpage is available.");
}

void loop() {
  updatePhotoLed();
  server.handleClient();
  updatePhotoLed();

  // Keep the website available without probing D4 during these intervals.
  if (warmingUp) {
    if (millis() - warmupStartedAt < PIR_WARMUP_MS) {
      delay(2);
      return;
    } 
    warmingUp = false;
    previousMotion = false;
    Serial.println("PIR ready.");
  }
  if (coolingDown) {
    if (millis() - cooldownStartedAt < CAPTURE_COOLDOWN_MS) {
      delay(2);
      return;
    }
    coolingDown = false;
    turnPhotoLedOff();
    Serial.println("Resuming D4 checks; waiting for LOW to rearm.");
  }

  int reading = digitalRead(PIR_SIGNAL_PIN);
  bool motion = reading == HIGH;
  if (millis() - lastDebugTime >= DEBUG_INTERVAL_MS) {
    lastDebugTime = millis();
    Serial.printf("D4 reads: %d (%s)\n", reading,
                  motion ? "HIGH - motion" : "LOW - no motion");
  }
  if (motion && !previousMotion) {
    Serial.println("Motion detected.");
    if (takeAndSavePhoto()) {
      lastCaptureStatus = lastDHTLogSaved
        ? "Photo and measurement CSV saved (see DHT22 status below)"
        : "Photo saved, but measurement CSV failed to save";
      cooldownStartedAt = millis();
      coolingDown = true;
      Serial.println("Photo saved. D4 checks paused for 5 seconds; Wi-Fi remains available.");
    } else {
      lastCaptureStatus = "Failed - check Serial Monitor";
      Serial.println("Photo failed. Waiting for the next LOW-to-HIGH event.");
    }
  }
  if (!motion && previousMotion) Serial.println("PIR returned LOW. Ready for next motion.");
  previousMotion = motion;
  delay(2);
}

