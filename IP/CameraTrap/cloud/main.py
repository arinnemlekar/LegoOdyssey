"""Lego Odyssey (FLL Team #75368) — camera trap upload service.

Runs on Google Cloud Run. Each trap POSTs its photos to /upload with:
    X-Device-Id       the trap's ID (DEVICE_ID in config.h)
    X-Trap-Key        the trap's secret key (from `python admin_tool.py add-trap`)
    X-Captured-At     when the photo was taken (Unix time, UTC) — optional
    X-Temperature-C   from the DHT22 — optional
    X-Humidity        from the DHT22 — optional
Newer trap firmware also writes the time and weather inside each photo (EXIF).
This service reads them from there when the headers are missing, e.g. for SD-card
photos uploaded without log.csv.

Team members can also upload photos from a trap's SD card on the website's
Upload page; those arrive at /api/upload-sd with the member's login token.

For each photo this service:
    1. checks the trap's secret key
    2. asks the AI which animal it is
    3. works out the moon phase
    4. saves the photo + a small thumbnail to Cloud Storage
    5. saves a "sighting" record to Firestore, which the website reads
"""

import hashlib
import hmac
import io
import os
import re
import time
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

import firebase_admin
from firebase_admin import auth as fb_auth, firestore, storage
from flask import Flask, abort, jsonify, request
from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore_v1.base_query import FieldFilter
from PIL import Image, ImageOps, ImageStat

from classifier import classify
from moon import moon_phase

# ---------- Firebase setup ----------
PROJECT = os.environ.get("GOOGLE_CLOUD_PROJECT") or os.environ.get("GCLOUD_PROJECT")
BUCKET = os.environ.get("STORAGE_BUCKET") or f"{PROJECT}.firebasestorage.app"

if os.environ.get("FIRESTORE_EMULATOR_HOST"):
    # Local testing with the Firebase emulators: no real Google login needed.
    from google.auth.credentials import AnonymousCredentials

    class _EmulatorCredential(firebase_admin.credentials.Base):
        def get_credential(self):
            return AnonymousCredentials()

    firebase_admin.initialize_app(_EmulatorCredential(), {"projectId": PROJECT, "storageBucket": BUCKET})
else:
    firebase_admin.initialize_app(options={"projectId": PROJECT, "storageBucket": BUCKET})

db = firestore.client()
bucket = storage.bucket()
app = Flask(__name__)

DEFAULT_ANIMALS = (Path(__file__).parent / "animals.txt").read_text(encoding="utf-8")
THUMB_SIZE = (400, 300)


# ---------- Helpers ----------

def key_hash(key):
    return hashlib.sha256(key.encode()).hexdigest()


def trap_key_ok(device, key):
    """Each trap has a secret key. We store only a hash of it (like a password)."""
    if not key:
        return False
    doc = db.collection("trapKeys").document(device).get()
    return doc.exists and hmac.compare_digest(doc.get("keyHash"), key_hash(key))


_animal_list = {"text": None, "checked": 0.0}


def animal_list():
    """The team's animal list from the website (re-checked at most once a minute)."""
    if time.time() - _animal_list["checked"] > 60:
        doc = db.collection("settings").document("animals").get()
        text = doc.get("text") if doc.exists else None
        _animal_list.update(text=text or DEFAULT_ANIMALS, checked=time.time())
    return _animal_list["text"]


def header_float(name, low, high):
    """Read a number from a request header, or None if it's missing or impossible."""
    try:
        value = float(request.headers.get(name, ""))
    except ValueError:
        return None
    return value if low <= value <= high else None


EARLIEST = 1704067200   # 1 Jan 2024: anything earlier means the trap's clock wasn't set
TRAP_SOFTWARE = "Lego Odyssey camera trap"   # EXIF_SOFTWARE in the firmware's exif.h


def exif_number(value, low, high):
    try:
        value = float(value)
    except (TypeError, ValueError, ZeroDivisionError):
        return None
    return value if low <= value <= high else None


def read_exif(image):
    """The time and weather our trap wrote inside the photo (EXIF, see firmware/exif.h).
    Returns None for photos from other cameras, or from older trap firmware."""
    try:
        exif = image.getexif()
        if not str(exif.get(0x0131, "")).startswith(TRAP_SOFTWARE):   # Software tag
            return None
        details = exif.get_ifd(0x8769)   # the Exif details list
    except Exception:
        return None
    stamp = 0   # 0 = the trap's clock wasn't set (same as the trap and log.csv)
    when = str(details.get(0x9003, "")).strip("\0 ")            # DateTimeOriginal
    offset = str(details.get(0x9011, "+00:00")).strip("\0 ")    # OffsetTimeOriginal
    if when:
        try:
            stamp = int(datetime.strptime(when + offset, "%Y:%m:%d %H:%M:%S%z").timestamp())
        except ValueError:
            pass
    return {
        "stamp": stamp,
        "temperature": exif_number(details.get(0x9400), -40, 80),   # AmbientTemperature
        "humidity": exif_number(details.get(0x9401), 0, 100),       # Humidity
    }


def photo_time(exif=None):
    """When was the photo taken? Trust the trap's clock if it looks sensible."""
    now = datetime.now(timezone.utc)
    if exif and EARLIEST <= exif["stamp"] <= (now + timedelta(days=1)).timestamp():
        return datetime.fromtimestamp(exif["stamp"], timezone.utc), "trap"
    stamp = header_float("X-Captured-At", EARLIEST, (now + timedelta(days=1)).timestamp())
    if stamp:
        # "file" = the date on the SD card file (used when the trap's clock wasn't set)
        source = "file" if request.headers.get("X-Time-Source") == "file" else "trap"
        return datetime.fromtimestamp(stamp, timezone.utc), source
    return now, "server"


def save_to_storage(path, data):
    blob = bucket.blob(path)
    # A download token lets signed-in website users load the photo.
    blob.metadata = {"firebaseStorageDownloadTokens": str(uuid.uuid4())}
    blob.upload_from_string(data, content_type="image/jpeg")


# A photo this plain (almost no difference between its pixels) is blank:
# all black, all white, or the lens is covered. Measured as brightness spread, 0-255.
BLANK_SPREAD = 5


def is_blank(image):
    return ImageStat.Stat(image.convert("L")).stddev[0] < BLANK_SPREAD


def trash_info(result, blank):
    """Photos with no animal go straight to the Trash. People can restore them from the Trash page."""
    if blank:
        return {"trashed": True, "trashReason": "blank photo"}
    if not result["is_animal"]:
        return {"trashed": True, "trashReason": f"AI: {result['label']}"}
    return {"trashed": False, "trashReason": None}


def sighting_id(device, photo_name, exif=None):
    """The same photo always gets the same ID, whether it arrives from the trap
    or later from the SD card. So it can never be counted twice."""
    if not photo_name:
        return None
    if exif:
        # Newer trap photos carry their own time inside, so it's the same however they arrive.
        return hashlib.sha256(f"{device}/{photo_name}/{exif['stamp']}".encode()).hexdigest()[:24]
    # The trap sends its clock time (or nothing if its clock wasn't set). The website
    # sends the time from the SD card's log.csv as X-Photo-Stamp (0 = clock wasn't set),
    # because it may fill in X-Captured-At from the file's date instead.
    stamp_header = "X-Photo-Stamp" if request.headers.get("X-Photo-Stamp") is not None else "X-Captured-At"
    stamp = int(header_float(stamp_header, 0, 4e9) or 0)
    return hashlib.sha256(f"{device}/{photo_name}/{stamp}".encode()).hexdigest()[:24]


def process_photo(device, source, uploaded_by=None):
    """Check, identify, and save one photo. Returns (status, result)."""
    data = request.get_data()
    if not data.startswith(b"\xff\xd8"):  # every JPEG starts with these bytes
        abort(400, "Expected a JPEG image")

    try:
        original = Image.open(io.BytesIO(data))   # only reads the header for now
    except Exception:
        abort(400, "Could not read the image")
    exif = read_exif(original)

    photo_name = re.sub(r"[^\w.-]", "", request.headers.get("X-Photo-Name", ""))[:64]
    doc_id = sighting_id(device, photo_name, exif)
    doc = db.collection("sightings").document(doc_id) if doc_id else db.collection("sightings").document()
    if doc_id and doc.get().exists:
        return "duplicate", None
    if doc_id and db.collection("deletedSightings").document(doc_id).get().exists:
        return "deleted", None   # was emptied from the Trash; don't bring it back

    try:
        image = ImageOps.exif_transpose(original)
        image.load()
    except Exception:
        abort(400, "Could not read the image")

    # Time: the EXIF the trap wrote inside the photo comes first, then the request headers.
    # Weather: the headers (from the trap, or log.csv on the website), else the EXIF.
    taken, time_source = photo_time(exif)
    temperature = header_float("X-Temperature-C", -40, 80)   # DHT22 range
    humidity = header_float("X-Humidity", 0, 100)
    if exif:
        temperature = temperature if temperature is not None else exif["temperature"]
        humidity = humidity if humidity is not None else exif["humidity"]
    phase, phase_name, illumination = moon_phase(taken)
    blank = is_blank(image)
    if blank:
        result = {"is_animal": False, "label": "no animal", "confidence": 1.0,
                  "top_guesses": [{"label": "no animal", "confidence": 1.0}]}
    else:
        result = classify(image, animal_list())
    trash = trash_info(result, blank)

    # Save the photo and a small thumbnail for the gallery.
    name = f"{taken:%Y%m%d_%H%M%S}_{device}_{uuid.uuid4().hex[:6]}.jpg"
    thumb = image.convert("RGB")
    thumb.thumbnail(THUMB_SIZE)
    thumb_bytes = io.BytesIO()
    thumb.save(thumb_bytes, "JPEG", quality=80)
    save_to_storage(f"photos/{name}", data)
    save_to_storage(f"thumbs/{name}", thumb_bytes.getvalue())

    trap = db.collection("traps").document(device).get()
    trap = trap.to_dict() if trap.exists else {}

    try:
        doc.create({
            "device": device,
            "photo": f"photos/{name}",
            "thumb": f"thumbs/{name}",
            "photoName": photo_name or None,   # name on the trap's SD card
            "source": source,                  # "trap" (sent live) or "sd-card" (uploaded later)
            "uploadedBy": uploaded_by,
            "capturedAt": taken,
            "timeSource": time_source,
            "receivedAt": firestore.SERVER_TIMESTAMP,
            # What the AI said:
            "aiLabel": result["label"],
            "aiIsAnimal": result["is_animal"],
            "aiConfidence": result["confidence"],
            "aiTopGuesses": result["top_guesses"],
            # What the website shows (starts as the AI's answer; team members can fix it):
            "label": result["label"],
            "isAnimal": result["is_animal"],
            "reviewed": False,
            # Trash: photos with no animal start in the Trash
            "trashed": trash["trashed"],
            "trashReason": trash["trashReason"],
            "trashedBy": "auto" if trash["trashed"] else None,
            "trashedAt": firestore.SERVER_TIMESTAMP if trash["trashed"] else None,
            # Weather and moon:
            "temperatureC": temperature,
            "humidity": humidity,
            "moonPhase": phase,
            "moonName": phase_name,
            "moonIllumination": illumination,
            # Where the trap was when this photo arrived:
            "lat": trap.get("lat"),
            "lon": trap.get("lon"),
        })
    except AlreadyExists:
        # The same photo arrived twice at the same moment. Keep the first one.
        for path in (f"photos/{name}", f"thumbs/{name}"):
            bucket.blob(path).delete()
        return "duplicate", None

    result["trashed"] = trash["trashed"]
    print(f"{device} ({source}): {result['label']} ({result['confidence']:.0%}), {temperature}°C, {phase_name}"
          + (f", to Trash ({trash['trashReason']})" if trash["trashed"] else ""), flush=True)
    return "added", result


def logged_in_member():
    """Check the website login sent with the request. Returns the username."""
    token = request.headers.get("Authorization", "")
    if not token.startswith("Bearer "):
        abort(401, "Please log in")
    try:
        user = fb_auth.verify_id_token(token[len("Bearer "):])
    except Exception:
        abort(401, "Your login has expired. Please log in again.")
    return (user.get("email") or "").split("@")[0]


# ---------- Routes ----------

@app.get("/")
def health():
    return "Lego Odyssey camera trap service is running."


@app.post("/upload")
def upload():
    """Photos sent live by a trap (checked with the trap's secret key)."""
    device = re.sub(r"[^\w-]", "", request.headers.get("X-Device-Id", ""))[:32]
    if not device or not trap_key_ok(device, request.headers.get("X-Trap-Key", "")):
        abort(403, "Unknown trap or wrong trap key")
    status, result = process_photo(device, source="trap")
    if status == "duplicate":
        return "already have this photo"
    if status == "deleted":
        return "this photo was deleted from the Trash"
    return f"{result['label']} {result['confidence']:.0%}" + (" (moved to Trash)" if result["trashed"] else "")


@app.route("/api/upload-sd", methods=["POST", "OPTIONS"])
def upload_from_sd_card():
    """Photos a logged-in team member uploads from a trap's SD card (website Upload page)."""
    if request.method == "OPTIONS":   # browser pre-check, only needed when testing locally
        return ""
    username = logged_in_member()

    device = re.sub(r"[^\w-]", "", request.headers.get("X-Device-Id", ""))[:32]
    if not device or not db.collection("traps").document(device).get().exists:
        abort(400, "Unknown trap")
    status, result = process_photo(device, source="sd-card", uploaded_by=username)
    if status in ("duplicate", "deleted"):
        return jsonify(status=status)
    return jsonify(status="added", label=result["label"], confidence=result["confidence"], trashed=result["trashed"])


@app.route("/api/empty-trash", methods=["POST", "OPTIONS"])
def empty_trash():
    """Permanently delete every photo in the Trash (any logged-in team member)."""
    if request.method == "OPTIONS":
        return ""
    username = logged_in_member()
    deleted = 0
    for snap in db.collection("sightings").where(filter=FieldFilter("trashed", "==", True)).stream():
        data = snap.to_dict()
        for path in (data.get("photo"), data.get("thumb")):
            if path:
                blob = bucket.blob(path)
                if blob.exists():
                    blob.delete()
        # Remember the deleted photo, so uploading the same SD card again doesn't bring it back.
        db.collection("deletedSightings").document(snap.id).set({
            "device": data.get("device"),
            "photoName": data.get("photoName"),
            "label": data.get("label"),
            "deletedBy": username,
            "deletedAt": firestore.SERVER_TIMESTAMP,
        })
        snap.reference.delete()
        deleted += 1
    print(f"{username} emptied the Trash: {deleted} photos deleted", flush=True)
    return jsonify(deleted=deleted)


if os.environ.get("FIRESTORE_EMULATOR_HOST"):
    # Local testing: the website (localhost:5000) calls this service directly on another port.
    @app.after_request
    def allow_local_website(response):
        response.headers["Access-Control-Allow-Origin"] = "http://localhost:5000"
        response.headers["Access-Control-Allow-Headers"] = "*"
        response.headers["Access-Control-Allow-Methods"] = "POST, OPTIONS"
        return response


if __name__ == "__main__":
    # Local testing only. On Cloud Run, gunicorn starts the app (see Dockerfile).
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 8080)))
