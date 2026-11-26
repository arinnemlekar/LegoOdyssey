// ===== EXIF: writing the time and weather inside each photo =====
//
// A JPEG file can carry extra information called EXIF: the date a photo was
// taken, the camera's name, and much more. Phones and photo apps read it.
// The ESP32 camera saves photos WITHOUT any EXIF, so this file adds it:
//
//   Date taken         (DateTimeOriginal, in UTC, marked "+00:00")
//   Temperature        (AmbientTemperature, in degrees C, from the DHT22)
//   Humidity           (Humidity, in %, from the DHT22)
//   Description        "Lego Odyssey camera trap <trap ID>"
//   Camera make/model  Seeed Studio / XIAO ESP32S3 Sense
//
// Now the time and weather stay with the photo even if it is copied off the
// SD card without log.csv. The cloud server reads them back.
//
// How EXIF is stored: right after the JPEG's first two bytes (FF D8) comes an
// "APP1" block: FF E1, its length, "Exif", then a tiny TIFF file. The TIFF file
// holds lists of tags (called IFDs). Each tag is 12 bytes:
//   tag number (2), type (2), count (4), value or where the value is (4)
#pragma once
#include <Arduino.h>
#include <time.h>
#include <math.h>
#include <string.h>

// The cloud server only trusts EXIF that starts with this name in the Software tag.
#define EXIF_SOFTWARE  "Lego Odyssey camera trap"

#define EXIF_MAX_BYTES 512   // our EXIF block is about 300 bytes

// Tag types (how many bytes one value takes)
enum {
  EXIF_ASCII     = 2,    // text, 1 byte per letter, ends with a 0 byte
  EXIF_LONG      = 4,    // whole number, 4 bytes
  EXIF_RATIONAL  = 5,    // fraction (top / bottom), 8 bytes
  EXIF_UNDEFINED = 7,    // raw bytes
  EXIF_SRATIONAL = 10,   // fraction that can be negative, 8 bytes
};

struct ExifTag {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  const void *data;
};

// EXIF numbers here are "little-endian": the smallest byte comes first.
static void exifPut16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void exifPut32(uint8_t *p, uint32_t v) { exifPut16(p, v & 0xFFFF); exifPut16(p + 2, v >> 16); }

static size_t exifValueBytes(const ExifTag &t) {
  size_t each = (t.type == EXIF_RATIONAL || t.type == EXIF_SRATIONAL) ? 8 : (t.type == EXIF_LONG ? 4 : 1);
  return each * t.count;
}

// Values bigger than 4 bytes don't fit inside the tag, so they go after the
// list. Rounded up to an even number of bytes, as the rules ask.
static size_t exifExtraBytes(const ExifTag &t) {
  size_t n = exifValueBytes(t);
  return n <= 4 ? 0 : (n + 1) & ~(size_t)1;
}

static size_t exifListBytes(const ExifTag *tags, int count) {
  size_t size = 2 + 12 * count + 4;   // how many tags, the tags, "no next list"
  for (int i = 0; i < count; i++) size += exifExtraBytes(tags[i]);
  return size;
}

// Writes one list of tags at tiff + at. Positions are counted from the start of the TIFF part.
static void exifWriteList(uint8_t *tiff, size_t at, const ExifTag *tags, int count) {
  exifPut16(tiff + at, count);
  size_t extraAt = at + 2 + 12 * count + 4;
  for (int i = 0; i < count; i++) {
    uint8_t *entry = tiff + at + 2 + 12 * i;
    size_t n = exifValueBytes(tags[i]);
    exifPut16(entry, tags[i].tag);
    exifPut16(entry + 2, tags[i].type);
    exifPut32(entry + 4, tags[i].count);
    if (n <= 4) {
      memset(entry + 8, 0, 4);
      memcpy(entry + 8, tags[i].data, n);
    } else {
      exifPut32(entry + 8, extraAt);
      memcpy(tiff + extraAt, tags[i].data, n);
      if (n & 1) tiff[extraAt + n] = 0;
      extraAt += exifExtraBytes(tags[i]);
    }
  }
  exifPut32(tiff + at + 2 + 12 * count, 0);   // no next list
}

// A fraction stored as (value x 10) / 10, so 23.4 becomes 234/10.
static void exifFraction(uint8_t *out, float value) {
  exifPut32(out, (uint32_t)(int32_t)lroundf(value * 10));
  exifPut32(out + 4, 10);
}

// Builds the APP1 (EXIF) block. takenUtc = 0 means the clock wasn't set (no date
// is written); NAN temperature or humidity is left out. Returns its size, or 0.
size_t buildExif(uint8_t *out, size_t room, time_t takenUtc, float tempC, float humidity, const char *trapId) {
  char description[64];
  snprintf(description, sizeof(description), "Lego Odyssey camera trap %s", trapId);
  static const char make[] = "Seeed Studio";
  static const char model[] = "XIAO ESP32S3 Sense";
  static const char software[] = EXIF_SOFTWARE " firmware";
  static const char exifVersion[4] = {'0', '2', '3', '1'};   // 2.31 added temperature and humidity
  static const char utc[] = "+00:00";

  bool hasTime = takenUtc > 0;
  char when[20] = "";   // "YYYY:MM:DD HH:MM:SS"
  if (hasTime) {
    struct tm t;
    gmtime_r(&takenUtc, &t);
    strftime(when, sizeof(when), "%Y:%m:%d %H:%M:%S", &t);
  }
  uint8_t tempValue[8], humidityValue[8], detailsAt[4];
  if (!isnan(tempC)) exifFraction(tempValue, tempC);
  if (!isnan(humidity)) exifFraction(humidityValue, humidity);

  // List 1 (IFD0): about the picture. Tags must be in number order.
  ExifTag picture[6];
  int nPicture = 0;
  picture[nPicture++] = {0x010E, EXIF_ASCII, (uint32_t)strlen(description) + 1, description};   // ImageDescription
  picture[nPicture++] = {0x010F, EXIF_ASCII, sizeof(make), make};                                // Make
  picture[nPicture++] = {0x0110, EXIF_ASCII, sizeof(model), model};                              // Model
  picture[nPicture++] = {0x0131, EXIF_ASCII, sizeof(software), software};                        // Software
  if (hasTime) picture[nPicture++] = {0x0132, EXIF_ASCII, sizeof(when), when};                   // DateTime
  picture[nPicture++] = {0x8769, EXIF_LONG, 1, detailsAt};                                       // where list 2 is

  // List 2 (Exif IFD): the details.
  ExifTag details[5];
  int nDetails = 0;
  details[nDetails++] = {0x9000, EXIF_UNDEFINED, 4, exifVersion};                          // ExifVersion
  if (hasTime) {
    details[nDetails++] = {0x9003, EXIF_ASCII, sizeof(when), when};                        // DateTimeOriginal
    details[nDetails++] = {0x9011, EXIF_ASCII, sizeof(utc), utc};                          // OffsetTimeOriginal
  }
  if (!isnan(tempC))    details[nDetails++] = {0x9400, EXIF_SRATIONAL, 1, tempValue};      // AmbientTemperature
  if (!isnan(humidity)) details[nDetails++] = {0x9401, EXIF_RATIONAL, 1, humidityValue};   // Humidity

  const size_t header = 2 + 2 + 6;   // FF E1, length, "Exif\0\0"
  size_t pictureAt = 8;                 // right after the 8-byte TIFF header
  size_t detailsListAt = pictureAt + exifListBytes(picture, nPicture);
  size_t total = header + detailsListAt + exifListBytes(details, nDetails);
  if (total > room || total > 65535) return 0;

  memset(out, 0, total);
  out[0] = 0xFF; out[1] = 0xE1;                         // APP1
  out[2] = (total - 2) >> 8; out[3] = (total - 2) & 0xFF;  // length (big-endian, the JPEG way)
  memcpy(out + 4, "Exif\0\0", 6);
  uint8_t *tiff = out + header;
  tiff[0] = 'I'; tiff[1] = 'I';                         // "II" = little-endian
  exifPut16(tiff + 2, 42);                              // the TIFF magic number
  exifPut32(tiff + 4, pictureAt);
  exifPut32(detailsAt, detailsListAt);
  exifWriteList(tiff, pictureAt, picture, nPicture);
  exifWriteList(tiff, detailsListAt, details, nDetails);
  return total;
}

// Returns a NEW copy of the photo with the EXIF block inside, and sets *len to
// its size. Free it with free() when done. Returns nullptr (and leaves *len
// alone) if something goes wrong; then just use the photo without EXIF.
uint8_t *addExif(const uint8_t *jpeg, size_t *len, time_t takenUtc, float tempC, float humidity, const char *trapId) {
  if (*len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return nullptr;   // not a JPEG
  uint8_t block[EXIF_MAX_BYTES];
  size_t blockLen = buildExif(block, sizeof(block), takenUtc, tempC, humidity, trapId);
  if (!blockLen) return nullptr;

  // Put EXIF right after FF D8, or after a JFIF (APP0) block if the photo starts with one.
  size_t insertAt = 2;
  if (jpeg[2] == 0xFF && jpeg[3] == 0xE0 && *len > 6) {
    size_t app0Len = ((size_t)jpeg[4] << 8) | jpeg[5];
    if (2 + 2 + app0Len <= *len) insertAt = 2 + 2 + app0Len;
  }

  size_t newLen = *len + blockLen;
  uint8_t *copy = (uint8_t *)ps_malloc(newLen);
  if (!copy) copy = (uint8_t *)malloc(newLen);   // no PSRAM (e.g. simulator)
  if (!copy) return nullptr;
  memcpy(copy, jpeg, insertAt);
  memcpy(copy + insertAt, block, blockLen);
  memcpy(copy + insertAt + blockLen, jpeg + insertAt, *len - insertAt);
  *len = newLen;
  return copy;
}
