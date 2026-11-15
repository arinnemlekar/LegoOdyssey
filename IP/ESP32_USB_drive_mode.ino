#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0
#error "Select Tools > USB Mode > USB-OTG (TinyUSB)"
#endif

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error "Select Tools > USB CDC On Boot > Enabled"
#endif

#if ARDUINO_USB_MSC_ON_BOOT
#error "Select Tools > USB Firmware MSC On Boot > Disabled"
#endif

#include "USB.h"
#include "USBMSC.h"

// XIAO ESP32S3 Sense onboard microSD connections.
// These are GPIO numbers, not D-pin numbers.
constexpr int SD_CS_PIN   = 21;
constexpr int SD_SCK_PIN  = 7;
constexpr int SD_MISO_PIN = 8;
constexpr int SD_MOSI_PIN = 9;

constexpr uint32_t SECTOR_BYTES = 512;

USBMSC MSC;

uint32_t cardSectorCount = 0;
bool readerReady = false;
const char *statusMessage = "Starting...";

// Read the requested bytes from the SD card.
// Handles requests spanning multiple sectors and partial sectors.
int32_t readFromCard(uint32_t lba, uint32_t offset,
                     void *buffer, uint32_t bufsize) {
  uint64_t position =
      static_cast<uint64_t>(lba) * SECTOR_BYTES + offset;

  const uint64_t capacity =
      static_cast<uint64_t>(cardSectorCount) * SECTOR_BYTES;

  if (position > capacity || bufsize > capacity - position) {
    return -1;
  }

  uint8_t sectorBuffer[SECTOR_BYTES];
  uint8_t *destination = static_cast<uint8_t *>(buffer);
  uint32_t remaining = bufsize;

  while (remaining > 0) {
    uint32_t sector =
        static_cast<uint32_t>(position / SECTOR_BYTES);

    uint32_t withinSector =
        static_cast<uint32_t>(position % SECTOR_BYTES);

    uint32_t count = SECTOR_BYTES - withinSector;
    if (count > remaining) {
      count = remaining;
    }

    if (!SD.readRAW(sectorBuffer, sector)) {
      return -1;
    }

    memcpy(destination, sectorBuffer + withinSector, count);

    destination += count;
    position += count;
    remaining -= count;
  }

  return static_cast<int32_t>(bufsize);
}

// Reject all writes, even if the laptop attempts one.
// A write callback is required by the USBMSC library.
int32_t rejectWrite(uint32_t lba, uint32_t offset,
                    uint8_t *buffer, uint32_t bufsize) {
  return -1;
}

// Handle the laptop's eject/load request.
bool handleStartStop(uint8_t powerCondition,
                     bool start, bool loadEject) {
  if (loadEject) {
    MSC.mediaPresent(start);
  }

  return true;
}

void setup() {
  Serial.begin(115200);

  // Configure the USB storage interface before starting USB.
  MSC.vendorID("Seeed");
  MSC.productID("XIAO SD Reader");
  MSC.productRevision("1.0");

  MSC.onRead(readFromCard);
  MSC.onWrite(rejectWrite);
  MSC.onStartStop(handleStartStop);

  MSC.isWritable(false);
  MSC.mediaPresent(false);

  USB.begin();

  // Allow time for USB serial to become available.
  // Do not wait indefinitely for Serial Monitor.
  delay(2000);

  Serial.println();
  Serial.println("XIAO ESP32S3 Sense: read-only USB SD reader");
  Serial.println("Camera and PIR capture are disabled.");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  // 4 MHz SPI; automatic formatting is explicitly disabled.
  if (!SD.begin(SD_CS_PIN, SPI, 4000000, "/sd", 5, false)) {
    statusMessage =
        "ERROR: SD initialization failed. Check card and expansion board.";
    Serial.println(statusMessage);
    return;
  }

  if (SD.cardType() == CARD_NONE) {
    statusMessage = "ERROR: No SD card detected.";
    Serial.println(statusMessage);
    return;
  }

  if (SD.sectorSize() != SECTOR_BYTES) {
    statusMessage = "ERROR: Expected an SD card with 512-byte sectors.";
    Serial.println(statusMessage);
    return;
  }

  cardSectorCount = static_cast<uint32_t>(SD.numSectors());

  if (cardSectorCount == 0) {
    statusMessage = "ERROR: SD card reported zero sectors.";
    Serial.println(statusMessage);
    return;
  }

  Serial.printf(
      "Card capacity: %llu MB\n",
      static_cast<unsigned long long>(
          static_cast<uint64_t>(cardSectorCount) *
          SECTOR_BYTES / (1024ULL * 1024ULL))
  );

  if (!MSC.begin(cardSectorCount, SECTOR_BYTES)) {
    statusMessage = "ERROR: USB mass storage initialization failed.";
    Serial.println(statusMessage);
    return;
  }

  // The laptop can now access the card.
  // Do not use SD.open(), save photos, or change files afterward.
  MSC.mediaPresent(true);
  readerReady = true;

  statusMessage =
      "Ready. Open Windows File Explorer > This PC to find the SD card.";

  Serial.println(statusMessage);
  Serial.println("Read-only: viewing and copying are allowed.");
  Serial.println("Eject the drive in Windows before disconnecting.");
}

void loop() {
  // Repeat errors so they remain visible if Serial Monitor
  // is opened after setup has finished.
  static unsigned long lastReport = 0;

  if (!readerReady && millis() - lastReport >= 5000) {
    lastReport = millis();
    Serial.println(statusMessage);
  }

  // USB storage requests are handled by the USB task.
  delay(20);
}