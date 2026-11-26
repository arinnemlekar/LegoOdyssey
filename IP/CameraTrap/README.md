# Lego Odyssey Camera Trap — FLL Team #75368

A battery-powered wildlife camera for the San Diego chaparral. When an animal walks by, a
motion sensor wakes a XIAO ESP32S3 Sense camera board. It takes a few photos, measures temperature and humidity,
and sends everything to the cloud. There, AI works out which animal it is, and the team
website shows the photos, charts, and a map. Only team members can log in.

```
 [PIR sensor] --wakes--> [XIAO ESP32S3 + DHT22] ==HTTPS + trap key==> Cloud Run (cloud/)
                              |                                     AI names the animal,
                          microSD card                              adds moon phase
                       (photos + log.csv)                              |          |
                                                              Cloud Storage   Firestore
                                                                (photos)     (sightings)
                                                                          |
                                Team website (web/) on Firebase Hosting --+  <-- team logs in
                                photos · stats · map · traps · animal list
```

| Folder | What's in it |
|--------|------|
| `firmware/` | Code for the trap (XIAO ESP32S3 Sense), plus `wokwi/` to test it in a simulator |
| `cloud/` | The upload service with the AI (runs on Google Cloud Run), plus `admin_tool.py` for the coach |
| `web/` | The team website (runs on Firebase Hosting) |
| `firestore.rules`, `storage.rules` | Who can see and change what — these keep the data private |
| `server/` | Offline version: everything on one laptop, no internet or logins (see the end of this file) |
| `tools/send_test_photo.py` | Pretends to be a trap, so you can test without the hardware |

---

## Part 1 — Build the trap

### Parts list

| Part | Notes | Approx. cost |
|------|-------|------|
| Seeed Studio **XIAO ESP32S3 Sense** | Camera + microSD slot on the add-on board; programs over USB-C | $15–20 |
| AM312 mini PIR sensor | Runs on 3.3 V and uses very little power. (HC-SR501 also works on 5 V.) | $2 |
| DHT22 temperature/humidity sensor | Reads −40 to 80 °C | $5 |
| microSD card, 32 GB or smaller | Format as **FAT32** | $5 |
| 3.7 V LiPo battery (soldered to the BAT pads under the board), or a 5 V source on the 5V pin | See power notes below | $8 |
| LED + 330 Ω resistor | Blinks after each saved photo | $1 |
| Weatherproof box, with a window for the lens and PIR | A clear lid or a hole covered with clear plastic | $5–10 |

### Wiring

These are the same pins as the team's `PIR_sensor_ESP32_LED` sketch (GitHub, `IP` folder),
plus the DHT22.

| Part | XIAO pin |
|-----|-----|
| PIR VCC | 3V3 (AM312) or 5V (HC-SR501) |
| PIR GND | GND |
| PIR OUT | **D4** |
| LED (+) through 330 Ω | **D6** |
| LED (−) | GND |
| DHT22 + (VCC) | 3V3 |
| DHT22 − (GND) | GND |
| DHT22 OUT (data) | **D1** |

- The microSD card goes in the slot on the camera add-on board. It uses D8, D9 and D10, so
  don't connect anything else to those pins.
- A bare 4-pin DHT22 (not on a small board) needs a 10 kΩ resistor between data and 3.3 V.
- Mount the DHT22 outside the box, or behind a vent, in the shade. Chaparral sun will heat a
  closed box well above the air temperature.

### Programming the trap

**No board yet, or want to test first?** Run the trap program in the free Wokwi simulator in
your browser. See [`firmware/wokwi/README.md`](firmware/wokwi/README.md).

1. Install the [Arduino IDE](https://www.arduino.cc/en/software).
2. Go to **File → Preferences → Additional boards manager URLs** and add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
   Then open **Tools → Board → Boards Manager** and install **esp32 by Espressif**.
3. In **Tools → Manage Libraries**, install **DHT sensor library** by Adafruit (say yes to
   installing *Adafruit Unified Sensor* too).
4. In `firmware/camera_trap/`, copy `config.example.h` to a new file named `config.h` (the coach
   may give you a filled-in copy). Open `camera_trap.ino`, then edit `config.h`:
   - Wi-Fi name and password
   - `SERVER_URL`: the Cloud Run address from Part 2, step 8, followed by `/upload`
   - `DEVICE_ID` and `TRAP_KEY`: from the coach (Part 2, step 9)
5. Under **Tools**, choose Board **XIAO_ESP32S3** and PSRAM **OPI PSRAM** (the camera needs
   it).
6. Connect USB-C, select the COM port, and click Upload. If the port doesn't show up, hold
   the **B** (boot) button while plugging in the cable.
7. Open **Serial Monitor** at 115200 baud and press the **R** (reset) button. When the board first
   powers on, it sets its clock from the internet, tests the DHT22, and arms the trap. Then
   wave your hand in front of the PIR sensor. The Serial Monitor disconnects while the board
   sleeps and comes back when motion wakes it.

The first photo after a quiet period can take **30–40 seconds** to upload. The cloud AI shuts
down when nobody is using it (that's what keeps it free), and it has to start up again first.
Photos are always saved to the SD card, so nothing is lost if an upload fails. The trap
never deletes photos from the card.

**Catching up:** if a photo couldn't be sent (no Wi-Fi), `log.csv` marks it `no`. The next
time the trap wakes up with Wi-Fi, it also sends up to 5 of those older photos (oldest first,
set by `CATCHUP_PER_WAKE` in `config.h`) and changes their `no` to `yes`. So a trap that was
out of range catches up on its own, a few photos per animal visit. If you don't want to wait,
take out the card and use the website's **Upload** page. The two methods never double-count
a photo.

---

## Part 2 — Set up the cloud (coach, about 1 hour, one time)

You'll need a Google account and a credit card. Firebase's free **Spark** plan can't run the
AI or store photos, so the project needs the **Blaze** (pay-as-you-go) plan. Blaze still
includes monthly free amounts, and a team's camera traps should stay within them (see
*Costs* below). Step 2 sets up a budget alert so there are no surprises.

**Use the region `us-west1` (Oregon) for everything.** Cloud Storage's free tier only covers
a few US regions, and this is the closest one to San Diego.

1. **Create the project.** Go to https://console.firebase.google.com → *Create a project*.
   Name it something like `lego-odyssey-trap`. Google Analytics isn't needed. Note the
   **project ID** it shows (for example `lego-odyssey-trap-1a2b3`).
2. **Turn on Blaze and set a budget alert.** In the Firebase console, click *Upgrade* (bottom
   left) and choose Blaze. Then, in https://console.cloud.google.com/billing → *Budgets &
   alerts*, create a **$5/month** budget with email alerts.
3. **Logins.** Go to *Build → Authentication → Get started* and enable **Email/Password**.
   Then, under *Settings → User actions*, **uncheck "Enable create (sign-up)"**, so that only
   the coach can create accounts.
4. **Database.** Go to *Build → Firestore Database → Create database*, choose location
   **us-west1**, and start in **production mode**.
5. **Photo storage.** Go to *Build → Storage → Get started*, choose location **us-west1**,
   and start in **production mode**.
6. **Website, database rules, and storage rules.** First, in the Firebase console, go to
   *Project settings → General → Your apps → Add app → Web (`</>`)*. Give it any nickname and
   check *Also set up Firebase Hosting*. (The website gets its settings from this app.)
   Then, on the coach's computer, install [Node.js](https://nodejs.org) and run:
   ```
   npm install -g firebase-tools
   firebase login
   ```
   Put your project ID in `.firebaserc` (replacing `YOUR_FIREBASE_PROJECT_ID`). Then, in this
   folder, run:
   ```
   firebase deploy
   ```
   The website is now live at `https://YOUR-PROJECT-ID.web.app`. Nobody can log in yet.
7. **Install the Google Cloud CLI** from https://cloud.google.com/sdk/docs/install, then:
   ```
   gcloud auth login
   gcloud config set project YOUR-PROJECT-ID
   ```
8. **Give the project's build/run robot account its permissions.** New Google Cloud
   projects no longer grant these automatically. Without them, the next command fails with
   `PERMISSION_DENIED ... default service account is missing required IAM permissions`.
   Run these once (PowerShell), then wait 2–3 minutes:
   ```
   $PROJECT = "YOUR-PROJECT-ID"
   $NUMBER  = gcloud projects describe $PROJECT --format="value(projectNumber)"
   $SA      = "serviceAccount:$NUMBER-compute@developer.gserviceaccount.com"
   gcloud projects add-iam-policy-binding $PROJECT --member=$SA --role="roles/run.builder"
   gcloud projects add-iam-policy-binding $PROJECT --member=$SA --role="roles/serviceusage.serviceUsageConsumer"
   gcloud projects add-iam-policy-binding $PROJECT --member=$SA --role="roles/datastore.user"
   gcloud projects add-iam-policy-binding $PROJECT --member=$SA --role="roles/storage.objectAdmin"
   ```
   **Upload the AI service.** This takes about 10–15 minutes the first time. Say **yes** if
   it asks to enable APIs.
   ```
   cd cloud
   gcloud run deploy camera-trap --source . --region us-west1 --memory 2Gi --cpu 1 --cpu-boost --min-instances 0 --max-instances 2 --concurrency 1 --timeout 120 --allow-unauthenticated --set-env-vars STORAGE_BUCKET=YOUR-PROJECT-ID.firebasestorage.app
   ```
   - `--allow-unauthenticated` is needed because traps can't log in like people do. Each trap
     proves who it is with its own secret key instead.
   - Check the exact bucket name under *Storage* in the Firebase console. Older projects use
     `YOUR-PROJECT-ID.appspot.com`.
   - When the command finishes, it prints a **Service URL**. The trap's `SERVER_URL` is that
     address followed by `/upload`.
9. **Create accounts and trap keys.** Still in the `cloud` folder:
   ```
   gcloud auth application-default login
   gcloud auth application-default set-quota-project YOUR-PROJECT-ID
   pip install firebase-admin
   python admin_tool.py add-user coach --admin
   python admin_tool.py add-user arin
   python admin_tool.py add-user anya
   python admin_tool.py add-trap trap-1
   ```
   `add-user` prints the new password; give each kid their username and password.
   `add-trap` prints the two lines to paste into that trap's `config.h`. Also available:
   `reset-password`, `remove-user`, `make-admin`, `list-users`, and `upgrade-photos`, which
   you run once after updating to the version with the Trash. It gives older photos the
   Trash fields so they still show up.
10. **Test it.** Send any animal photo as if a trap took it, then check the website:
    ```
    python ../tools/send_test_photo.py coyote.jpg --server https://YOUR-SERVICE-URL/upload --trap trap-1 --key THE-TRAP-KEY
    ```

**If the deploy or uploads fail with a permission error:** check that step 8's roles were
granted. In the Google Cloud console, go to *IAM* and find the **Compute Engine default
service account** (ending in `-compute@developer.gserviceaccount.com`). It should have
**Cloud Run Builder**, **Service Usage Consumer**, **Cloud Datastore User**, and
**Storage Object Admin**.

**After changing the code:** if `cloud/` changed, update the AI service **first** (repeat the
`gcloud run deploy` command from step 8), then run `firebase deploy` to update the website and
rules. The website's `/api/...` addresses are forwarded to the AI service, so the service needs
to be up to date first. Changes to the animal list don't need a redeploy, because the
team edits that list on the website.

### Costs

At camera-trap volume (a few hundred photos a day), everything stays within Google's free
monthly amounts, except about **$0.10–0.20/month** for storing the AI service's container
image. The budget alert from step 2 emails you if anything changes.

| Service | Free each month | This project |
|---|---|---|
| Cloud Run (the AI) | 180,000 vCPU-seconds, 360,000 GiB-seconds | ~3 seconds per photo |
| Firestore | 50,000 reads/day, 20,000 writes/day, 1 GB | small text records |
| Cloud Storage (us-west1) | 5 GB | ~150 KB per photo, so ~30,000 photos |
| Firebase Hosting | 10 GB storage, 360 MB/day transfer | the website |

---

## Part 3 — Using the website

Go to `https://YOUR-PROJECT-ID.web.app` and log in with your username and password.

| Page | What it's for |
|------|------|
| **Photos** | Every photo, newest first, which updates live. Shows the animal, time, temperature, humidity, and moon phase. **✓ Right** / **✎ Fix** checks the AI's answer. |
| **Stats** | Animal visits by time of day, moon phase, temperature, humidity, and trap. Also how accurate the AI is, and a **CSV download** for Excel or Google Sheets. |
| **Map** | Where the traps are, and what each one has seen |
| **Traps** | Each trap's GPS location, habitat, and date placed. On a phone at the trap, use *Use this device's location*. |
| **Animals** | The list of animals the AI chooses from. Edit it as you learn what lives near your traps. |
| **Upload** | When a trap couldn't send its photos, upload them from its SD card. The page reads `log.csv` for each photo's time and weather, and skips photos that are already on the site. |
| **Trash** | Blank photos and photos with no animal (or a person) land here automatically, along with anything a member moves here with 🗑. **Restore** puts a photo back; **Empty Trash** deletes everything in it for good. |
| **Guide** | One-page guide for the team (also in `docs/kids_guide.html` for printing) |

The coach's account can also delete traps.

### How the pieces work

- **Identifying animals:** The AI is called CLIP. It learned to match pictures with written
  descriptions. It compares each photo with every animal on the **Animals** page, plus some
  descriptions of empty scenes. The starting list covers San Diego chaparral and coastal sage
  scrub wildlife. If the AI keeps mixing two animals up (rabbits are the hardest), add a
  better description after a `|`.
- **Checking the AI:** Each **✓ Right** or **✎ Fix** is recorded. The Stats page shows the
  percentage the AI got right and its most common mistakes. That's real data for the judges.
- **Empty triggers and the Trash:** Wind, sun, and moving branches can also trigger a PIR
  sensor. Photos that are blank (almost all one color, so the AI isn't needed) or that the AI
  labels "no animal" or "person" go straight to the **Trash**. So does a photo a team member
  fixes to one of those. Emptying the Trash is done by the cloud service (`/api/empty-trash`).
  It deletes the photo files, and it remembers each deleted photo, so uploading the same SD
  card again won't bring it back.
- **Visits vs. photos:** The trap takes 3 photos each time. On the Stats page, photos from the
  same trap less than 60 seconds apart count as **one visit**, so one coyote doesn't count as
  three.
- **Time:** The ESP32 sets its clock from the internet and keeps time while it sleeps. If the
  clock wasn't set, the photo gets the time it arrived, marked with `*`.
- **Moon phase:** Calculated from the date in `cloud/moon.py`, by counting 29.53-day cycles
  from a known new moon.
- **Photos from the SD card:** Every photo is saved to the trap's SD card, and `log.csv` records
  its time, temperature, humidity, and whether it reached the cloud. Each photo gets an ID made
  from the trap, its file name, and when it was taken, so it can't be added twice, whether it
  arrives from the trap, from the SD card, or both. SD card uploads go through
  `/api/upload-sd`, which Firebase Hosting forwards to Cloud Run, and only work for logged-in
  team members.
- **EXIF (time and weather inside each photo):** The trap also writes the time (UTC), temperature,
  humidity and trap ID into each JPEG's EXIF data (`firmware/camera_trap/exif.h`). Photo apps
  show it as the date taken and description. The cloud server reads it back, so photos copied
  off the SD card without `log.csv` still get their real time and weather.
- **Security:** Website users must log in, and the rules in `firestore.rules` and
  `storage.rules` are enforced by Google's servers, not by the web pages. Each trap has its
  own secret key, and the cloud stores only a scrambled (hashed) copy of it. Traps upload over
  HTTPS and check Google's certificates (`google_roots.h`).

### Privacy

Kids log in with a username only; no email addresses or personal information are stored. A
trap's GPS location may be a team member's home, so round the coordinates (for example, to 2
decimal places, about 1 km) before sharing data or maps outside the team. The AI labels people
as "person", and the coach can delete those photos.

### Power and battery life

- The XIAO draws well under 1 mA in deep sleep, but the PIR sensor and DHT22 stay powered.
  Measuring this with a multimeter in series with the battery is a good experiment for the team.
- **USB power banks often switch off** when the current draw is this small. An 18650 battery
  with a boost module, or AA batteries, are more reliable.
- Each wake-up with Wi-Fi uses about 150–250 mA for a few seconds. HTTPS uploads take a bit
  longer than plain ones.

---

## Testing on your own computer (no Google account needed)

Firebase's emulators run the whole system locally. In this folder:

```
firebase emulators:start --project demo-camera-trap
```

In a second window (PowerShell), start the upload service against the emulators:

```
cd cloud
$env:GOOGLE_CLOUD_PROJECT="demo-camera-trap"; $env:FIRESTORE_EMULATOR_HOST="127.0.0.1:8080"
$env:FIREBASE_AUTH_EMULATOR_HOST="127.0.0.1:9099"; $env:STORAGE_EMULATOR_HOST="http://127.0.0.1:9199"
$env:STORAGE_BUCKET="demo-camera-trap.appspot.com"; $env:PORT="8081"
python admin_tool.py --project demo-camera-trap add-user coach --admin
python admin_tool.py --project demo-camera-trap add-trap trap-1
python main.py
```

Send photos with `tools/send_test_photo.py --server http://127.0.0.1:8081/upload --key ...`
and open http://localhost:5000. The emulator dashboard is at http://localhost:4000. When the
website runs on `localhost`, it uses the emulators automatically.

## Offline version (laptop only)

`server/` runs everything on one laptop, with no internet and no logins. It's handy at a
competition venue with bad Wi-Fi. Set the trap's `SERVER_URL` to `http://LAPTOP-IP:5000/upload`,
then:

```
cd server
pip install -r requirements.txt
python server.py
```

and open http://localhost:5000. It uses `server/animals.txt` for its animal list.

## Ideas to improve it

- Add IR LEDs and a no-IR-filter camera for night photos. Most chaparral mammals are active
  at night.
- Add sunrise/sunset times, so the stats can show visits at dawn, during the day, at dusk, and at night.
- Compare traps in chaparral, coastal sage scrub, and near water. Which animals visit each one?
