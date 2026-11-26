// Upload page: send photos from a trap's SD card that never reached the website.
//
// The trap saves every photo as  <trap-id>_<number>.jpg  and writes one line per
// photo in log.csv:  photo,unix_time_utc,temperature_c,humidity_pct,uploaded
// We use log.csv to give each photo its real time and weather.

import { requireLogin, loadTraps, escapeHtml, API_BASE } from "./common.js";

const { user } = await requireLogin();
const $ = id => document.getElementById(id);
const traps = await loadTraps();
for (const id of Object.keys(traps).sort()) {
  const name = traps[id].name && traps[id].name !== id ? `${traps[id].name} (${id})` : id;
  $("trap").add(new Option(name, id));
}

const MAX_BYTES = 10 * 1024 * 1024;
const EARLIEST = Date.UTC(2024, 0, 1) / 1000;   // anything earlier means "clock wasn't set"
let photos = [];     // [{file, name, stamp, time, timeSource, temp, humidity, sentByTrap, row}]
let uploading = false;

// ---------- Reading the SD card ----------

$("folder").onchange = e => load([...e.target.files]);
$("files").onchange = e => load([...e.target.files]);

/** Read log.csv into {photoName: {stamp, temp, humidity, uploaded}}. */
function parseLog(text) {
  const lines = text.trim().split(/\r?\n/);
  const header = lines.shift().split(",").map(h => h.trim());
  const col = name => header.indexOf(name);
  const log = {};
  for (const line of lines) {
    const cells = line.split(",");
    const name = cells[col("photo")]?.trim();
    if (!name) continue;
    const num = i => (i >= 0 && cells[i]?.trim() !== "" ? Number(cells[i]) : null);
    log[name] = {
      stamp: num(col("unix_time_utc")) || 0,
      temp: num(col("temperature_c")),
      humidity: num(col("humidity_pct")),
      uploaded: cells[col("uploaded")]?.trim() === "yes",
    };
  }
  return log;
}

async function load(files) {
  if (uploading) return;
  const logFile = files.find(f => f.name.toLowerCase() === "log.csv");
  const log = logFile ? parseLog(await logFile.text()) : {};
  const jpgs = files.filter(f => /\.jpe?g$/i.test(f.name)).sort((a, b) => a.name.localeCompare(b.name));

  photos = jpgs.map(file => {
    const entry = log[file.name];
    const fileStamp = Math.floor(file.lastModified / 1000);
    let time = null, timeSource = null;
    if (entry && entry.stamp >= EARLIEST) { time = entry.stamp; timeSource = "trap"; }
    else if (fileStamp >= EARLIEST && fileStamp <= Date.now() / 1000 + 86400) { time = fileStamp; timeSource = "file"; }
    return {
      file, name: file.name, time, timeSource,
      stamp: entry ? entry.stamp : null,
      temp: entry?.temp ?? null, humidity: entry?.humidity ?? null,
      sentByTrap: entry?.uploaded ?? false,
    };
  });

  // Guess the trap from the file names (e.g. "mission-trail_000123.jpg").
  const prefix = photos[0]?.name.replace(/_\d+\.jpe?g$/i, "");
  if (prefix && traps[prefix] && !$("trap").value) $("trap").value = prefix;

  const warnings = [];
  if (!jpgs.length) warnings.push("No .jpg photos found. Did you pick the SD card?");
  if (jpgs.length && !logFile) warnings.push("No log.csv found. Photos from older trap firmware will be missing temperature and humidity (newer photos carry them inside). Pick the whole SD card folder to include it.");
  const noTime = photos.filter(p => !p.time).length;
  if (noTime) warnings.push(`${noTime} photo(s) have no time. They'll get today's date, marked with a *.`);
  const big = photos.filter(p => p.file.size > MAX_BYTES).length;
  if (big) warnings.push(`${big} photo(s) are bigger than 10 MB and will be skipped. Are they really from the trap?`);
  $("warnings").innerHTML = warnings.map(escapeHtml).join("<br>");
  $("warnings").hidden = !warnings.length;

  drawTable();
  $("review").hidden = false;
}

function toSend() {
  return photos.filter(p => p.file.size <= MAX_BYTES && ($("includeSent").checked || !p.sentByTrap));
}

$("includeSent").onchange = () => drawTable();
$("trap").onchange = () => drawTable();

function drawTable() {
  const sending = new Set(toSend());
  const sent = photos.filter(p => p.sentByTrap).length;
  $("summary").innerHTML = `Found <b>${photos.length}</b> photos. ` +
    (sent ? `The trap's log says <b>${sent}</b> already reached the website. ` : "") +
    `Ready to upload <b>${sending.size}</b>.`;
  $("go").disabled = uploading || !sending.size || !$("trap").value;
  $("go").textContent = $("trap").value ? `Upload ${sending.size} photos` : "Choose a trap first";

  const fmt = p => (p.time ? new Date(p.time * 1000).toLocaleString() : "—") + (p.timeSource === "file" ? " (file date)" : "");
  $("list").innerHTML = `<tr><th>Photo</th><th>Taken</th><th>Temp</th><th>Humidity</th><th>Status</th></tr>` +
    photos.map((p, i) => `<tr>
      <td>${escapeHtml(p.name)}</td><td>${fmt(p)}</td>
      <td>${p.temp != null ? p.temp.toFixed(1) + "°C" : "—"}</td>
      <td>${p.humidity != null ? Math.round(p.humidity) + "%" : "—"}</td>
      <td class="status" id="st-${i}">${p.file.size > MAX_BYTES ? '<span class="st-skip">too big, skipped</span>'
        : sending.has(p) ? "waiting" : '<span class="st-skip">already sent by trap</span>'}</td></tr>`).join("");
}

// ---------- Uploading ----------

function setStatus(p, html) {
  document.getElementById("st-" + photos.indexOf(p)).innerHTML = html;
}

async function uploadOne(p, device) {
  const headers = {
    Authorization: "Bearer " + (await user.getIdToken()),
    "Content-Type": "image/jpeg",
    "X-Device-Id": device,
    "X-Photo-Name": p.name,
  };
  if (p.stamp != null) headers["X-Photo-Stamp"] = String(p.stamp);
  if (p.time) { headers["X-Captured-At"] = String(p.time); headers["X-Time-Source"] = p.timeSource; }
  if (p.temp != null) headers["X-Temperature-C"] = String(p.temp);
  if (p.humidity != null) headers["X-Humidity"] = String(p.humidity);

  const response = await fetch(API_BASE + "/api/upload-sd", { method: "POST", headers, body: p.file });
  if (!response.ok) {
    const text = await response.text();
    const message = text.match(/<p>(.*?)<\/p>/)?.[1] || `error ${response.status}`;
    throw new Error(message);
  }
  return response.json();
}

$("go").onclick = async () => {
  const device = $("trap").value;
  const queue = toSend();
  uploading = true;
  $("go").disabled = $("trap").disabled = $("includeSent").disabled = true;
  $("progress").hidden = false;
  $("progress").max = queue.length;
  let added = 0, duplicates = 0, failed = 0, toTrash = 0;

  for (const [i, p] of queue.entries()) {
    $("progress").value = i;
    $("progress-text").textContent = `Uploading ${i + 1} of ${queue.length}…` +
      (i === 0 ? " (the first one can take up to a minute while the AI wakes up)" : "");
    setStatus(p, "uploading…");
    try {
      const result = await uploadOne(p, device);
      if (result.status === "duplicate") {
        duplicates++;
        setStatus(p, '<span class="st-dup">already on the website</span>');
      } else if (result.status === "deleted") {
        duplicates++;
        setStatus(p, '<span class="st-dup">deleted from the Trash before, skipped</span>');
      } else if (result.trashed) {
        added++; toTrash++;
        setStatus(p, `<span class="st-dup">🗑 ${escapeHtml(result.label)}, moved to Trash</span>`);
      } else {
        added++;
        setStatus(p, `<span class="st-added">✓ ${escapeHtml(result.label)} (${Math.round(result.confidence * 100)}%)</span>`);
      }
    } catch (e) {
      failed++;
      setStatus(p, `<span class="st-err">✗ ${escapeHtml(e.message)}</span>`);
    }
  }

  $("progress").value = queue.length;
  $("progress-text").textContent = `Done: ${added} added` + (toTrash ? ` (${toTrash} with no animal went to the Trash)` : "") +
    `, ${duplicates} already on the website or deleted` +
    (failed ? `, ${failed} failed. Try uploading again; photos that worked will be skipped.` : ".");
  uploading = false;
  $("trap").disabled = $("includeSent").disabled = false;
  $("go").disabled = failed === 0;
  $("go").textContent = failed ? "Try again" : "Done";
};

window.addEventListener("beforeunload", e => {
  if (uploading) { e.preventDefault(); e.returnValue = ""; }
});
