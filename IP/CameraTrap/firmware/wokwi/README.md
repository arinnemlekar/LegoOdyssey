# Test the trap in the Wokwi simulator

[Wokwi](https://wokwi.com) is a free ESP32 simulator that runs in your web browser. You can
test the trap program without the real board: click the motion sensor, and watch the
"photo" get saved, logged, uploaded, and named by the AI on the team website.

**What's simulated:** XIAO ESP32-S3, PIR motion sensor, DHT22, microSD card, LED, Wi-Fi and
real uploads to the website.
**What's not:** the camera. Wokwi has no camera, so in simulator mode each wake-up uses the next of
4 built-in test photos (bobcat, mule deer, California quail, and a coyote that's hard to
spot). They're real camera-trap photos, public domain or CC0.

Simulator photos go to the **`test-trap`** trap, never `mission-trail`, so they don't mix
with real data. On the website, choose `test-trap` in the trap filter to see them, and delete
them with 🗑 when you're done.

## Wiring in the simulator

Same as the real trap (PIR → D4, DHT22 → D1, LED → D6), except the SD card's CS wire goes to
**D3**. Wokwi's XIAO doesn't have the Sense camera add-on with its built-in SD slot, so the
simulated card is a separate part.

## Run it on wokwi.com

1. Go to https://wokwi.com, sign in (free), and start a new project: **ESP32 →
   XIAO ESP32-S3** (Arduino).
2. Click the **diagram.json** tab and replace everything in it with this folder's
   `diagram.json`.
3. Replace everything in **sketch.ino** with `../camera_trap/camera_trap.ino`.
4. Use the **▾** menu next to the file tabs → **Upload file(s)…** to add these files from
   `../camera_trap/`: `google_roots.h`, `sim_photos.h`, and your `config.h`.
   (The coach has a `config.h` that already contains the test-trap key.)
5. In Wokwi's `config.h`, find `// #define SIMULATOR` and remove the `//`.
6. Use **▾ → New file…** to add `libraries.txt`, and paste in this folder's `libraries.txt`.
7. Press the green **▶** button. The first build takes a minute or two.

Then:
- The Serial Monitor shows "Powered on", the clock being set, and "Trap armed".
- Click the **PIR sensor** and press its **Simulate motion** button. The trap wakes up, takes 3 photos,
  blinks the LED after each one, and uploads them. The Serial Monitor shows the AI's answer,
  for example `server says: bobcat 91%`.
- Click the **DHT22** and move the sliders to change the temperature and humidity that get sent.
- The first upload can take 30–40 seconds while the cloud AI wakes up.

**Catch-up uploads** are hard to test here: Wokwi's SD card starts empty every time you press
▶, and Wi-Fi is always on. Test them on the real trap instead: take it somewhere without
Wi-Fi, trigger it a few times, then bring it back in range and trigger it again.

⚠️ Don't make a Wokwi project with your `config.h` in it **public**: it contains the
test-trap key. Keep the project private, or delete `config.h` before sharing.

## Run it from a computer (for the coach)

`wokwi.toml` lets the Wokwi VS Code extension or `wokwi-cli` run a firmware file built on
your computer. Build it with SIMULATOR turned on:

```
arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 --build-property "compiler.cpp.extra_flags=-DSIMULATOR" --output-dir firmware/wokwi/build firmware/camera_trap
```

Then open this folder in VS Code with the Wokwi extension (**F1 → Wokwi: Start Simulator**),
or run `wokwi-cli firmware/wokwi` with `WOKWI_CLI_TOKEN` set (get a free token at
https://wokwi.com/dashboard/ci).
