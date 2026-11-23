// ===== Camera trap settings (XIAO ESP32S3 Sense) — edit these before uploading =====
// Copy this file to config.h and fill it in. config.h is kept out of GitHub
// because it holds the Wi-Fi password and the trap key.
#pragma once

// Wi-Fi network the trap connects to (home Wi-Fi, or a phone hotspot in the field).
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// Where to send photos.
//   Cloud (Firebase):  the Cloud Run address from `gcloud run deploy`, plus /upload
//                      e.g. "https://camera-trap-abc123xyz-uc.a.run.app/upload"
//   Laptop (offline):  "http://192.168.1.50:5000/upload"  (laptop IP from `ipconfig`)
#define SERVER_URL     "https://YOUR-CLOUD-RUN-ADDRESS.run.app/upload"

// This trap's ID and secret key. Get both from the coach, who runs:
//   python admin_tool.py add-trap trap-1
// Every trap needs a different ID and key. (The laptop server ignores the key.)
#define DEVICE_ID      "trap-1"
#define TRAP_KEY       "YOUR_TRAP_KEY"

// How many photos to take each time motion is detected, and the gap between them.
#define BURST_COUNT        3
#define BURST_DELAY_MS     700

// Give up on Wi-Fi after this long and just keep the photos on the SD card.
#define WIFI_TIMEOUT_MS    10000

// How long to wait for the server to answer. The cloud AI "sleeps" when nobody
// is using it and can take ~30-40 seconds to wake up for the first photo.
#define UPLOAD_TIMEOUT_MS  60000

// Each time Wi-Fi works, also send up to this many older photos that never got
// uploaded (log.csv says "no"). More = catches up faster but uses more battery.
// 0 turns catch-up off.
#define CATCHUP_PER_WAKE   5

// Max time to wait for the PIR sensor to calm down before sleeping again.
#define PIR_SETTLE_MAX_MS  30000

// ----- Simulator (Wokwi) -----
// Uncomment the next line ONLY to build for the Wokwi simulator (see ../wokwi/README.md).
// In simulator mode the trap uses Wokwi's Wi-Fi, built-in test photos, and the
// test trap below instead of the real one, so test photos never mix with real data.
// #define SIMULATOR
#define SIM_DEVICE_ID  "test-trap"
#define SIM_TRAP_KEY   "YOUR_TEST_TRAP_KEY"
