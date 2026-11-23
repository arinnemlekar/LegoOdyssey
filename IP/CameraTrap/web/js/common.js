// Shared code for every page: connects to Firebase, checks the user is logged
// in, draws the header, and has helper functions the other pages use.

import { initializeApp } from "https://www.gstatic.com/firebasejs/10.14.1/firebase-app.js";
import { getAuth, connectAuthEmulator, onAuthStateChanged, signOut }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-auth.js";
import { getFirestore, connectFirestoreEmulator, terminate, collection, getDocs, query, orderBy,
         doc, updateDoc, serverTimestamp }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js";
import { getStorage, connectStorageEmulator, ref, getDownloadURL }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-storage.js";

// Kids log in with a username; Firebase needs an email, so we add this.
// Must match USERNAME_DOMAIN in cloud/admin_tool.py.
export const USERNAME_DOMAIN = "team75368.example";

// Firebase Hosting gives every site its own settings at this address.
const config = await fetch("/__/firebase/init.json").then(r => r.json());
export const app = initializeApp(config);
export const auth = getAuth(app);
export const db = getFirestore(app);
export const storage = getStorage(app);

// When running on this computer (`firebase emulators:start`), use the local
// test emulators instead of the real Firebase project.
export const USING_EMULATORS = ["localhost", "127.0.0.1"].includes(location.hostname);
if (USING_EMULATORS) {
  connectAuthEmulator(auth, "http://127.0.0.1:9099", { disableWarnings: true });
  connectFirestoreEmulator(db, "127.0.0.1", 8080);
  connectStorageEmulator(storage, "127.0.0.1", 9199);
}

// Close the database connection when leaving a page, so connections don't pile up.
window.addEventListener("pagehide", () => terminate(db));

// The AI service. On the real site, Firebase Hosting forwards /api/... to Cloud Run
// (see "rewrites" in firebase.json). When testing locally, it runs on port 8081.
export const API_BASE = USING_EMULATORS ? "http://127.0.0.1:8081" : "";

// ---------- Login ----------

export function usernameOf(user) {
  return (user.email || "").replace("@" + USERNAME_DOMAIN, "");
}

/** Wait until we know who is logged in. Sends them to the login page if nobody is. */
export function requireLogin() {
  return new Promise(resolve => {
    const stop = onAuthStateChanged(auth, async user => {
      stop();
      if (!user) {
        location.href = "/login?next=" + encodeURIComponent(location.pathname + location.search);
        return;
      }
      const token = await user.getIdTokenResult();
      const isAdmin = token.claims.role === "admin";
      drawHeader(user, isAdmin);
      resolve({ user, isAdmin, username: usernameOf(user) });
    });
  });
}

function drawHeader(user, isAdmin) {
  const pages = [["/", "Photos"], ["/stats", "Stats"], ["/map", "Map"], ["/traps", "Traps"], ["/animals", "Animals"], ["/upload", "Upload"], ["/trash", "Trash"], ["/guide", "Guide"]];
  const here = location.pathname.replace(/\.html$/, "").replace(/\/index$/, "/");
  document.getElementById("nav").innerHTML = `
    <a class="brand" href="/">
      <img src="/img/logo.png" alt="Lego Odyssey logo">
      <span><b>Lego Odyssey Camera Trap</b><small>FLL Team #75368</small></span>
    </a>
    ${pages.map(([href, name]) => `<a href="${href}" class="${here === href ? "on" : ""}">${name}</a>`).join("")}
    <span class="who">${escapeHtml(usernameOf(user))}${isAdmin ? " (coach)" : ""}
      <button id="signout" class="link">Log out</button></span>`;
  document.getElementById("signout").onclick = () => signOut(auth).then(() => (location.href = "/login"));
}

// ---------- Helpers ----------

export function escapeHtml(text) {
  return String(text ?? "").replace(/[&<>"']/g, c =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

export const MOON_PHASES = ["New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
                            "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent"];
const MOON_ICONS = ["🌑", "🌒", "🌓", "🌔", "🌕", "🌖", "🌗", "🌘"];
export function moonIcon(name) {
  const i = MOON_PHASES.indexOf(name);
  return i >= 0 ? MOON_ICONS[i] : "";
}

export function formatTime(timestamp) {
  if (!timestamp) return "";
  const d = timestamp.toDate();
  return d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) + " " +
         d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
}

const urlCache = new Map();
/** Web address for a photo in Cloud Storage (only works when logged in). */
export async function photoUrl(path) {
  if (!urlCache.has(path)) urlCache.set(path, getDownloadURL(ref(storage, path)));
  return urlCache.get(path);
}

/** Read the team's animal list: {label: description}. Same rules as cloud/classifier.py. */
export function parseAnimalList(text) {
  const animals = {};
  for (let line of text.split("\n")) {
    line = line.trim();
    if (!line || line.startsWith("#")) continue;
    const [label, description] = line.split("|").map(s => s.trim());
    if (label) animals[label.toLowerCase()] = description || "a " + label;
  }
  return animals;
}

// Photos from the same trap less than this far apart count as ONE animal visit.
// Without this, a burst of 3 photos of one raccoon would count as 3 raccoons.
export const VISIT_GAP_SECONDS = 60;

/** Group photos into animal visits. Each visit gets the most common animal among its photos. */
export function groupIntoVisits(sightings) {
  const rows = sightings
    .filter(s => s.capturedAt)
    .sort((a, b) => a.device.localeCompare(b.device) || a.capturedAt.toMillis() - b.capturedAt.toMillis());
  const visits = [];
  let current = [];

  const finish = photos => {
    const animals = photos.filter(p => p.isAnimal).map(p => p.label);
    if (!animals.length) return;
    const counts = {};
    animals.forEach(a => (counts[a] = (counts[a] || 0) + 1));
    const first = photos[0];
    visits.push({
      device: first.device,
      start: first.capturedAt.toDate(),
      animal: Object.keys(counts).sort((a, b) => counts[b] - counts[a])[0],
      photos: photos.length,
      temperatureC: first.temperatureC,
      humidity: first.humidity,
      moonName: first.moonName,
    });
  };

  for (const row of rows) {
    const last = current[current.length - 1];
    if (last && (row.device !== last.device ||
        row.capturedAt.toMillis() - last.capturedAt.toMillis() > VISIT_GAP_SECONDS * 1000)) {
      finish(current);
      current = [];
    }
    current.push(row);
  }
  if (current.length) finish(current);
  return visits;
}

/** Load every sighting that isn't in the Trash (for stats, map, and CSV). */
export async function loadAllSightings() {
  const snap = await getDocs(query(collection(db, "sightings"), orderBy("capturedAt")));
  return snap.docs.map(d => ({ id: d.id, ...d.data() })).filter(s => !s.trashed);
}

/** Move a photo to the Trash (it can be restored from the Trash page until the Trash is emptied). */
export async function moveToTrash(id, reason, username) {
  await updateDoc(doc(db, "sightings", id), {
    trashed: true, trashReason: reason, trashedBy: username, trashedAt: serverTimestamp(),
  });
}

/** Load every trap as {deviceId: trap}. */
export async function loadTraps() {
  const snap = await getDocs(collection(db, "traps"));
  const traps = {};
  snap.docs.forEach(d => (traps[d.id] = { id: d.id, ...d.data() }));
  return traps;
}

/** Download all data as a CSV file for Excel / Google Sheets. */
export async function downloadCsv() {
  const [sightings, traps] = await Promise.all([loadAllSightings(), loadTraps()]);
  const header = ["captured_at", "time_source", "trap_id", "trap_name", "habitat", "latitude", "longitude",
    "is_animal", "animal", "ai_animal", "ai_confidence", "checked_by_team", "temperature_c", "humidity_pct",
    "moon_phase_name", "moon_phase_0_to_1", "moon_illumination", "hour", "weekday", "photo",
    "arrived_from", "uploaded_by"];
  const lines = [header.join(",")];
  for (const s of sightings) {
    const when = s.capturedAt.toDate();
    const trap = traps[s.device] || {};
    const pad = n => String(n).padStart(2, "0");
    const local = `${when.getFullYear()}-${pad(when.getMonth() + 1)}-${pad(when.getDate())} ` +
                  `${pad(when.getHours())}:${pad(when.getMinutes())}:${pad(when.getSeconds())}`;
    const row = [local, s.timeSource, s.device, trap.name, trap.habitat, s.lat, s.lon,
      s.isAnimal, s.label, s.aiLabel, s.aiConfidence, s.reviewed ? s.reviewedBy : "",
      s.temperatureC, s.humidity, s.moonName, s.moonPhase, s.moonIllumination,
      when.getHours(), when.toLocaleDateString("en-US", { weekday: "long" }), s.photo,
      s.source || "trap", s.uploadedBy || ""];
    lines.push(row.map(v => {
      const text = String(v ?? "");
      return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
    }).join(","));
  }
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([lines.join("\n")], { type: "text/csv" }));
  link.download = "lego_odyssey_camera_trap.csv";
  link.click();
}

/** Draw a simple bar chart. rows = [[label, count], ...] */
export function barChart(title, rows, note = "") {
  const biggest = Math.max(1, ...rows.map(r => r[1]));
  return `<div class="panel chart"><h3>${escapeHtml(title)}</h3>
    ${rows.length ? rows.map(([label, n]) => `
      <div class="row"><span>${escapeHtml(label)}</span>
        <div class="track"><div class="fill" style="width:${(100 * n) / biggest}%"></div></div>
        <span class="n">${n}</span></div>`).join("") : `<p class="muted">No data yet.</p>`}
    ${note ? `<p class="muted note">${escapeHtml(note)}</p>` : ""}</div>`;
}
