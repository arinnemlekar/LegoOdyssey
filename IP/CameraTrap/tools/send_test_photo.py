"""Pretend to be a camera trap: send a photo to the server with made-up sensor data.

Lets you test the server and web pages without the trap hardware.

Examples (laptop server):
    python send_test_photo.py fox.jpg
    python send_test_photo.py fox.jpg --trap trap-2 --temp 4.5 --humidity 88 --time "2026-11-20 22:15"

Example (cloud — use the trap's real key from admin_tool.py add-trap):
    python send_test_photo.py fox.jpg --server https://YOUR-SERVICE.run.app/upload --trap trap-1 --key ABC...
"""

import argparse
import urllib.error
import urllib.request
from datetime import datetime

parser = argparse.ArgumentParser()
parser.add_argument("photo", help="a .jpg file")
parser.add_argument("--server", default="http://localhost:5000/upload")
parser.add_argument("--trap", default="trap-1", help="device ID, like DEVICE_ID in config.h")
parser.add_argument("--key", default="", help="trap key, like TRAP_KEY in config.h (cloud only)")
parser.add_argument("--temp", type=float, help="temperature in °C")
parser.add_argument("--humidity", type=float, help="humidity in %%")
parser.add_argument("--time", help='when the photo was "taken", e.g. "2026-11-20 22:15" (default: now)')
args = parser.parse_args()

headers = {"Content-Type": "image/jpeg", "X-Device-Id": args.trap, "X-Trap-Key": args.key}
when = datetime.fromisoformat(args.time) if args.time else datetime.now()
headers["X-Captured-At"] = str(int(when.timestamp()))   # local time -> Unix time, like the trap sends
if args.temp is not None:
    headers["X-Temperature-C"] = str(args.temp)
if args.humidity is not None:
    headers["X-Humidity"] = str(args.humidity)

with open(args.photo, "rb") as f:
    request = urllib.request.Request(args.server, data=f.read(), headers=headers, method="POST")
try:
    with urllib.request.urlopen(request, timeout=90) as response:
        print("Server says:", response.read().decode())
except urllib.error.HTTPError as e:
    print(f"Server refused the photo ({e.code}):", e.read().decode()[:200])
