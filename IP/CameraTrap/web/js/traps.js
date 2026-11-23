// Traps page: where each trap is, its habitat, and when it was placed.

import { requireLogin, db, escapeHtml } from "./common.js";
import { collection, onSnapshot, doc, setDoc, deleteDoc, query, where, getCountFromServer }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js";

const { isAdmin } = await requireLogin();
const $ = id => document.getElementById(id);
let traps = {};
let editing = null;

onSnapshot(collection(db, "traps"), async snap => {
  traps = {};
  snap.docs.forEach(d => (traps[d.id] = { id: d.id, ...d.data() }));
  const ids = Object.keys(traps).sort();
  if (!ids.length) {
    $("table").innerHTML = `<tr><td class="muted">No traps yet. The coach adds traps with
      <code>python admin_tool.py add-trap trap-1</code>.</td></tr>`;
    return;
  }
  const photoCounts = await Promise.all(ids.map(id =>
    getCountFromServer(query(collection(db, "sightings"), where("device", "==", id), where("trashed", "==", false)))
      .then(r => r.data().count)));
  $("table").innerHTML = `
    <tr><th>ID</th><th>Name</th><th>Location</th><th>Habitat</th><th>Placed on</th><th>Photos</th><th>Notes</th><th></th></tr>
    ${ids.map((id, i) => {
      const t = traps[id];
      const where = t.lat != null ? `${t.lat.toFixed(5)}, ${t.lon.toFixed(5)}` : `<span class="muted">not set</span>`;
      return `<tr><td>${escapeHtml(id)}</td><td>${escapeHtml(t.name)}</td><td>${where}</td>
        <td>${escapeHtml(t.habitat)}</td><td>${escapeHtml(t.placedOn)}</td><td>${photoCounts[i]}</td>
        <td>${escapeHtml(t.notes)}</td><td><button class="link" data-edit="${escapeHtml(id)}">edit</button></td></tr>`;
    }).join("")}`;
});

$("table").onclick = event => {
  const id = event.target.dataset.edit;
  if (!id) return;
  editing = id;
  const t = traps[id];
  $("editor-title").textContent = "Edit " + id;
  $("name").value = t.name || "";
  $("coords").value = t.lat != null ? `${t.lat}, ${t.lon}` : "";
  $("habitat").value = t.habitat || "";
  $("placedOn").value = t.placedOn || "";
  $("notes").value = t.notes || "";
  $("delete").hidden = !isAdmin;
  $("error").hidden = true;
  $("editor").hidden = false;
  $("editor").scrollIntoView({ behavior: "smooth" });
};

/** Accept "45.123, -93.456" (the format Google Maps copies). */
function parseCoordinates(text) {
  const numbers = text.match(/-?\d+(?:\.\d+)?/g);
  if (!numbers || numbers.length !== 2) return null;
  const [lat, lon] = numbers.map(Number);
  return Math.abs(lat) <= 90 && Math.abs(lon) <= 180 ? [lat, lon] : null;
}

$("form").onsubmit = async event => {
  event.preventDefault();
  const text = $("coords").value.trim();
  const coords = text ? parseCoordinates(text) : [null, null];
  if (!coords) {
    $("error").textContent = "Couldn't read those coordinates. Use the form 45.1234, -93.5678";
    $("error").hidden = false;
    return;
  }
  await setDoc(doc(db, "traps", editing), {
    name: $("name").value.trim(),
    lat: coords[0],
    lon: coords[1],
    habitat: $("habitat").value.trim(),
    placedOn: $("placedOn").value,
    notes: $("notes").value.trim(),
  });
  $("editor").hidden = true;
};

$("cancel").onclick = () => ($("editor").hidden = true);

$("delete").onclick = async () => {
  if (!confirm(`Delete trap ${editing}? Its photos are kept.`)) return;
  await deleteDoc(doc(db, "traps", editing));
  $("editor").hidden = true;
};

$("here").onclick = () => {
  navigator.geolocation.getCurrentPosition(
    pos => ($("coords").value = `${pos.coords.latitude.toFixed(6)}, ${pos.coords.longitude.toFixed(6)}`),
    () => alert("Couldn't get this device's location. Copy it from Google Maps instead."),
    { enableHighAccuracy: true, timeout: 15000 });
};
