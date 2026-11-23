"""Lego Odyssey (FLL Team #75368) — camera trap base station.

Run on a laptop on the same Wi-Fi as the trap:
    python server.py

Pages (open http://localhost:5000):
    /          photo gallery with filters
    /traps     where each trap is placed (enter GPS coordinates here)
    /map       map of traps and what they've seen
    /stats     charts: visits by time of day, moon phase, temperature, humidity
    /export.csv   all data for spreadsheets

The trap POSTs each photo to /upload with these headers:
    X-Device-Id, X-Captured-At (Unix time, UTC), X-Temperature-C, X-Humidity

Optional phone alerts: set the NTFY_TOPIC environment variable and install the
free "ntfy" app on a phone, subscribed to the same topic name.
"""

import csv
import io
import json
import os
import re
import sqlite3
import urllib.request
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

from flask import Flask, Response, abort, redirect, render_template, request, send_from_directory, url_for

from classifier import classify
from moon import PHASE_NAMES, icon_for, moon_phase

BASE_DIR = Path(__file__).parent
PHOTO_DIR = BASE_DIR / "photos"
DB_PATH = BASE_DIR / "sightings.db"
NTFY_TOPIC = os.environ.get("NTFY_TOPIC")  # e.g. "team-trap-x7q2" (pick something hard to guess)

# Photos from the same trap less than this far apart count as ONE animal visit.
# Without this, a burst of 3 photos of one raccoon would count as 3 raccoons.
VISIT_GAP = timedelta(seconds=60)

PHOTO_DIR.mkdir(exist_ok=True)
app = Flask(__name__)
app.jinja_env.globals["moon_icon"] = icon_for
# Shows the team logo in the header once it is saved as static/logo.png
app.jinja_env.globals["has_logo"] = lambda: (BASE_DIR / "static" / "logo.png").exists()


# ---------- Database ----------

def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def setup_database():
    with db() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS sightings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                filename TEXT NOT NULL,
                device TEXT,
                received_at TEXT,
                is_animal INTEGER,
                label TEXT,
                confidence REAL,
                details TEXT
            )""")
        # Columns added in version 2. Adding them this way also upgrades an
        # older sightings.db without losing any photos.
        new_columns = {
            "captured_at": "TEXT",        # local time the photo was taken
            "time_source": "TEXT",        # 'trap' (trap's clock) or 'server' (time it arrived)
            "temperature_c": "REAL",
            "humidity": "REAL",
            "moon_phase": "REAL",         # 0 = new, 0.5 = full
            "moon_name": "TEXT",
            "moon_illumination": "REAL",  # 0..1
            "latitude": "REAL",           # copied from the trap's location when the photo arrived
            "longitude": "REAL",
        }
        existing = {row["name"] for row in conn.execute("PRAGMA table_info(sightings)")}
        for column, kind in new_columns.items():
            if column not in existing:
                conn.execute(f"ALTER TABLE sightings ADD COLUMN {column} {kind}")
        conn.execute("UPDATE sightings SET captured_at = received_at, time_source = 'server' "
                     "WHERE captured_at IS NULL")

        conn.execute("""
            CREATE TABLE IF NOT EXISTS traps (
                device_id TEXT PRIMARY KEY,
                name TEXT,
                latitude REAL,
                longitude REAL,
                habitat TEXT,
                placed_on TEXT,
                notes TEXT
            )""")


setup_database()


# ---------- Helpers ----------

def header_float(name, low, high):
    """Read a number from a request header, or None if it's missing or impossible."""
    try:
        value = float(request.headers.get(name, ""))
    except ValueError:
        return None
    return value if low <= value <= high else None


def photo_time():
    """When was the photo taken? Trust the trap's clock if it looks sensible."""
    now = datetime.now(timezone.utc)
    stamp = header_float("X-Captured-At", 1704067200, (now + timedelta(days=1)).timestamp())
    if stamp:
        return datetime.fromtimestamp(stamp, timezone.utc), "trap"
    return now, "server"


def parse_coordinates(text):
    """Accept '45.123, -93.456' (the format Google Maps copies). Returns (lat, lon) or None."""
    numbers = re.findall(r"-?\d+(?:\.\d+)?", text or "")
    if len(numbers) != 2:
        return None
    lat, lon = float(numbers[0]), float(numbers[1])
    if -90 <= lat <= 90 and -180 <= lon <= 180:
        return lat, lon
    return None


def send_alert(label, confidence, device):
    if not NTFY_TOPIC:
        return
    msg = f"{device} spotted: {label} ({confidence:.0%})".encode()
    try:
        urllib.request.urlopen(f"https://ntfy.sh/{NTFY_TOPIC}", data=msg, timeout=5)
    except OSError as e:
        print("Alert failed:", e)


def group_into_visits(rows):
    """Group photos into animal visits (see VISIT_GAP). Returns a list of visits.

    Each visit is a dict with the trap, start time, the animal (the most common
    label among its animal photos), and the weather/moon from its first photo.
    Visits with no animal photos are skipped.
    """
    visits = []
    rows = sorted(rows, key=lambda r: (r["device"], r["captured_at"]))
    current = []

    def finish(photos):
        animal_labels = [p["label"] for p in photos if p["is_animal"]]
        if animal_labels:
            first = photos[0]
            visits.append({
                "device": first["device"],
                "start": datetime.fromisoformat(first["captured_at"]),
                "animal": Counter(animal_labels).most_common(1)[0][0],
                "photos": len(photos),
                "temperature_c": first["temperature_c"],
                "humidity": first["humidity"],
                "moon_name": first["moon_name"],
                "moon_phase": first["moon_phase"],
            })

    for row in rows:
        if current and (row["device"] != current[-1]["device"] or
                        datetime.fromisoformat(row["captured_at"]) -
                        datetime.fromisoformat(current[-1]["captured_at"]) > VISIT_GAP):
            finish(current)
            current = []
        current.append(row)
    if current:
        finish(current)
    return visits


def bar_rows(counter, labels):
    """Turn a Counter into [(label, count, percent_of_max)] for the bar charts."""
    biggest = max(counter.values(), default=0) or 1
    return [(label, counter.get(label, 0), 100 * counter.get(label, 0) / biggest) for label in labels]


# ---------- Upload from the trap ----------

@app.post("/upload")
def upload():
    data = request.get_data()
    if not data.startswith(b"\xff\xd8"):  # every JPEG starts with these bytes
        abort(400, "Expected a JPEG image")

    device = re.sub(r"[^\w-]", "", request.headers.get("X-Device-Id", "unknown"))[:32] or "unknown"
    taken_utc, time_source = photo_time()
    taken_local = taken_utc.astimezone().replace(tzinfo=None)
    temperature = header_float("X-Temperature-C", -40, 80)   # DHT22 range
    humidity = header_float("X-Humidity", 0, 100)
    phase, phase_name, illumination = moon_phase(taken_utc)

    received = datetime.now()
    filename = f"{taken_local:%Y%m%d_%H%M%S}_{received:%f}_{device}.jpg"
    path = PHOTO_DIR / filename
    path.write_bytes(data)

    result = classify(path)
    with db() as conn:
        trap = conn.execute("SELECT latitude, longitude FROM traps WHERE device_id = ?", (device,)).fetchone()
        conn.execute(
            """INSERT INTO sightings (filename, device, received_at, captured_at, time_source,
                   is_animal, label, confidence, details, temperature_c, humidity,
                   moon_phase, moon_name, moon_illumination, latitude, longitude)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (filename, device, received.isoformat(timespec="seconds"),
             taken_local.isoformat(timespec="seconds"), time_source,
             int(result["is_animal"]), result["label"], result["confidence"], json.dumps(result),
             temperature, humidity, phase, phase_name, illumination,
             trap["latitude"] if trap else None, trap["longitude"] if trap else None),
        )

    weather = f"{temperature}°C {humidity}%" if temperature is not None else "no sensor data"
    print(f"{taken_local:%H:%M:%S} {device}: {result['label']} ({result['confidence']:.0%}), "
          f"{weather}, {phase_name}")
    if result["is_animal"]:
        send_alert(result["label"], result["confidence"], device)
    return f"{result['label']} {result['confidence']:.0%}"


@app.get("/photos/<path:name>")
def photo(name):
    return send_from_directory(PHOTO_DIR, name)


# ---------- Gallery ----------

@app.get("/")
def gallery():
    animals_only = request.args.get("animals") == "1"
    device = request.args.get("trap", "")
    label = request.args.get("animal", "")

    where, params = [], []
    if animals_only:
        where.append("is_animal = 1")
    if device:
        where.append("device = ?")
        params.append(device)
    if label:
        where.append("label = ?")
        params.append(label)
    where_sql = ("WHERE " + " AND ".join(where)) if where else ""

    with db() as conn:
        sightings = conn.execute(
            f"SELECT * FROM sightings {where_sql} ORDER BY captured_at DESC, id DESC LIMIT 200", params
        ).fetchall()
        total = conn.execute("SELECT COUNT(*) FROM sightings").fetchone()[0]
        animal_total = conn.execute("SELECT COUNT(*) FROM sightings WHERE is_animal = 1").fetchone()[0]
        devices = [r[0] for r in conn.execute("SELECT DISTINCT device FROM sightings ORDER BY device")]
        labels = [r[0] for r in conn.execute(
            "SELECT label FROM sightings WHERE is_animal = 1 GROUP BY label ORDER BY COUNT(*) DESC")]
    return render_template("gallery.html", sightings=sightings, total=total, animal_total=animal_total,
                           devices=devices, labels=labels, animals_only=animals_only,
                           device=device, label=label)


# ---------- Traps ----------

@app.route("/traps", methods=["GET", "POST"])
def traps():
    error = None
    if request.method == "POST":
        form = request.form
        device_id = re.sub(r"[^\w-]", "", form.get("device_id", ""))[:32]
        coords = parse_coordinates(form.get("coordinates"))
        if not device_id:
            error = "Device ID is required (it must match DEVICE_ID in the trap's config.h)."
        elif form.get("coordinates", "").strip() and not coords:
            error = "Couldn't read those coordinates. Use the form 45.1234, -93.5678"
        else:
            with db() as conn:
                conn.execute(
                    """INSERT INTO traps (device_id, name, latitude, longitude, habitat, placed_on, notes)
                       VALUES (?, ?, ?, ?, ?, ?, ?)
                       ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, latitude=excluded.latitude,
                           longitude=excluded.longitude, habitat=excluded.habitat,
                           placed_on=excluded.placed_on, notes=excluded.notes""",
                    (device_id, form.get("name", "").strip(),
                     coords[0] if coords else None, coords[1] if coords else None,
                     form.get("habitat", "").strip(), form.get("placed_on", "").strip(),
                     form.get("notes", "").strip()),
                )
                if coords and form.get("apply_to_old"):
                    conn.execute("UPDATE sightings SET latitude = ?, longitude = ? WHERE device = ?",
                                 (coords[0], coords[1], device_id))
            return redirect(url_for("traps"))

    with db() as conn:
        trap_rows = conn.execute("""
            SELECT t.*, COUNT(s.id) AS photos, SUM(s.is_animal) AS animal_photos
            FROM traps t LEFT JOIN sightings s ON s.device = t.device_id
            GROUP BY t.device_id ORDER BY t.device_id""").fetchall()
        unknown = [r[0] for r in conn.execute(
            "SELECT DISTINCT device FROM sightings WHERE device NOT IN (SELECT device_id FROM traps)")]
    edit = next((t for t in trap_rows if t["device_id"] == request.args.get("edit")), None)
    return render_template("traps.html", traps=trap_rows, unknown=unknown, edit=edit, error=error,
                           new_device=request.args.get("new", ""))


# ---------- Map ----------

@app.get("/map")
def map_page():
    with db() as conn:
        trap_rows = conn.execute("SELECT * FROM traps WHERE latitude IS NOT NULL").fetchall()
        rows = conn.execute("SELECT * FROM sightings").fetchall()
    visits = group_into_visits(rows)
    markers = []
    for t in trap_rows:
        animals = Counter(v["animal"] for v in visits if v["device"] == t["device_id"])
        markers.append({
            "device": t["device_id"], "name": t["name"] or t["device_id"],
            "lat": t["latitude"], "lon": t["longitude"], "habitat": t["habitat"] or "",
            "visits": sum(animals.values()), "top": animals.most_common(5),
        })
    return render_template("map.html", markers=markers)


# ---------- Stats ----------

@app.get("/stats")
def stats():
    animal = request.args.get("animal", "")
    with db() as conn:
        rows = conn.execute("SELECT * FROM sightings").fetchall()
    all_visits = group_into_visits(rows)
    species = Counter(v["animal"] for v in all_visits)
    visits = [v for v in all_visits if not animal or v["animal"] == animal]

    hours = Counter(v["start"].hour for v in visits)
    by_hour = bar_rows(hours, list(range(24)))
    by_hour = [(f"{h:02d}:00", n, pct) for h, n, pct in by_hour]

    by_moon = bar_rows(Counter(v["moon_name"] for v in visits), PHASE_NAMES)

    temps = [v["temperature_c"] for v in visits if v["temperature_c"] is not None]
    temp_bands = []
    if temps:
        low = int(min(temps) // 5 * 5)
        high = int(max(temps) // 5 * 5)
        temp_bands = [f"{t} to {t + 5}°C" for t in range(low, high + 5, 5)]
    by_temp = bar_rows(Counter(f"{int(t // 5 * 5)} to {int(t // 5 * 5) + 5}°C" for t in temps), temp_bands)

    hums = [v["humidity"] for v in visits if v["humidity"] is not None]
    hum_labels = [f"{h}–{h + 10}%" for h in range(0, 100, 10)]
    by_hum = bar_rows(Counter(f"{min(int(h // 10 * 10), 90)}–{min(int(h // 10 * 10), 90) + 10}%"
                              for h in hums), hum_labels)

    by_trap = bar_rows(Counter(v["device"] for v in visits), sorted({v["device"] for v in visits}))

    nights = sorted({(v["start"] - timedelta(hours=12)).date() for v in all_visits})
    return render_template("stats.html", animal=animal, species=species.most_common(), visits=visits,
                           photo_count=len(rows), by_hour=by_hour, by_moon=by_moon, by_temp=by_temp,
                           by_hum=by_hum, by_trap=by_trap, visit_gap=int(VISIT_GAP.total_seconds()),
                           first_day=nights[0] if nights else None, last_day=nights[-1] if nights else None)


# ---------- CSV export ----------

@app.get("/export.csv")
def export_csv():
    with db() as conn:
        rows = conn.execute("""
            SELECT s.id, s.captured_at, s.time_source, s.device, t.name AS trap_name, t.habitat,
                   s.latitude, s.longitude, s.is_animal, s.label, s.confidence,
                   s.temperature_c, s.humidity, s.moon_name, s.moon_phase, s.moon_illumination,
                   s.filename
            FROM sightings s LEFT JOIN traps t ON t.device_id = s.device
            ORDER BY s.captured_at""").fetchall()
    out = io.StringIO()
    writer = csv.writer(out)
    writer.writerow(["id", "captured_at", "time_source", "trap_id", "trap_name", "habitat",
                     "latitude", "longitude", "is_animal", "animal", "confidence",
                     "temperature_c", "humidity_pct", "moon_phase_name", "moon_phase_0_to_1",
                     "moon_illumination", "photo_file", "hour", "weekday"])
    for r in rows:
        when = datetime.fromisoformat(r["captured_at"])
        writer.writerow(list(r) + [when.hour, when.strftime("%A")])
    return Response(out.getvalue(), mimetype="text/csv",
                    headers={"Content-Disposition": "attachment; filename=camera_trap_data.csv"})


if __name__ == "__main__":
    print("Camera trap server running. Open http://localhost:5000")
    app.run(host="0.0.0.0", port=5000)
