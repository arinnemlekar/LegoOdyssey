// Photos page: newest photos first, updated live as the traps send them.

import { requireLogin, db, loadTraps, photoUrl, escapeHtml, moonIcon, formatTime, parseAnimalList, moveToTrash }
  from "./common.js";
import { collection, query, where, orderBy, limit, onSnapshot, doc, getDoc, updateDoc,
         serverTimestamp, getCountFromServer }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js";

const { username } = await requireLogin();
const NOT_ANIMALS = ["no animal", "person"];
const PAGE = 40;

const $ = id => document.getElementById(id);
const traps = await loadTraps();
const trapName = id => traps[id]?.name || id;

// Fill the filter menus.
for (const id of Object.keys(traps).sort()) $("trap").add(new Option(trapName(id), id));
const animalsDoc = await getDoc(doc(db, "settings", "animals"));
const listText = animalsDoc.exists() ? animalsDoc.data().text : await fetch("/animals-default.txt").then(r => r.text());
const animalNames = [...Object.keys(parseAnimalList(listText)), ...NOT_ANIMALS];
for (const name of animalNames) $("animal").add(new Option(name, name));

// ---------- Live list of photos ----------

let howMany = PAGE;
let stopListening = null;
const cards = new Map();   // sighting id -> {signature, element}

function listen() {
  if (stopListening) stopListening();
  const filters = [where("trashed", "==", false)];   // photos in the Trash are on the Trash page
  if ($("trap").value) filters.push(where("device", "==", $("trap").value));
  if ($("animal").value) filters.push(where("label", "==", $("animal").value));
  else if ($("hideEmpty").checked) filters.push(where("isAnimal", "==", true));
  const q = query(collection(db, "sightings"), ...filters, orderBy("capturedAt", "desc"), limit(howMany));

  stopListening = onSnapshot(q, snap => {
    render(snap.docs.map(d => ({ id: d.id, ...d.data() })));
    $("more").hidden = snap.size < howMany;
    refreshSummarySoon();
  }, err => {
    $("grid").innerHTML = `<p class="error">Couldn't load photos: ${escapeHtml(err.message)}</p>`;
  });
}

function render(sightings) {
  const onlyUnchecked = $("unchecked").checked;
  const grid = $("grid");
  const shown = [];
  for (const s of sightings) {
    if (onlyUnchecked && s.reviewed) continue;
    const signature = JSON.stringify(s);
    let card = cards.get(s.id);
    if (!card || card.signature !== signature) {
      card = { signature, element: makeCard(s) };
      cards.set(s.id, card);
    }
    shown.push(card.element);
  }
  grid.replaceChildren(...shown);
  if (!shown.length) grid.innerHTML = `<p>No photos here yet. Wave your hand in front of a trap!</p>`;
}

function makeCard(s) {
  const el = document.createElement("div");
  el.className = "card" + (s.isAnimal ? "" : " empty");
  const weather = [
    s.temperatureC != null ? `🌡 ${s.temperatureC.toFixed(1)}°C` : "",
    s.humidity != null ? `💧 ${Math.round(s.humidity)}%` : "",
    s.moonName ? `${moonIcon(s.moonName)} ${s.moonName}` : "",
  ].filter(Boolean).join(" · ");
  const confidence = !s.reviewed && s.isAnimal ? ` (${Math.round(s.aiConfidence * 100)}%)` : "";

  el.innerHTML = `
    <a href="#" class="photo"><img alt="${escapeHtml(s.label)}" loading="lazy"></a>
    <div class="info">
      <div class="label">${escapeHtml(s.label)}${confidence}</div>
      <div class="meta">${formatTime(s.capturedAt)}${s.timeSource === "server" ? "*" : ""}${s.timeSource === "file" ? " (file date)" : ""} · ${escapeHtml(trapName(s.device))}</div>
      ${s.source === "sd-card" ? `<div class="meta">📥 from SD card, uploaded by ${escapeHtml(s.uploadedBy)}</div>` : ""}
      <div class="meta">${weather}</div>
    </div>
    <div class="review"></div>`;

  photoUrl(s.thumb).then(url => (el.querySelector("img").src = url)).catch(() => {});
  el.querySelector(".photo").onclick = async event => {
    event.preventDefault();
    const tab = window.open("", "_blank");
    tab.location = await photoUrl(s.photo);
  };
  drawReview(el.querySelector(".review"), s);
  return el;
}

// ---------- Checking the AI's answer ----------

function drawReview(box, s) {
  if (s.reviewed) {
    box.innerHTML = s.label === s.aiLabel
      ? `<span class="badge">✓ checked by ${escapeHtml(s.reviewedBy)}</span>`
      : `<span class="badge fixed">✎ fixed by ${escapeHtml(s.reviewedBy)} — AI said “${escapeHtml(s.aiLabel)}”</span>`;
    box.insertAdjacentHTML("beforeend", `<button class="link" data-act="fix">change</button>`);
  } else {
    box.innerHTML = `<button data-act="ok">✓ Right</button><button class="secondary" data-act="fix">✎ Fix</button>`;
  }
  box.insertAdjacentHTML("beforeend", `<button class="link" data-act="trash" style="margin-left:auto" title="Move to the Trash">🗑 Trash</button>`);

  box.onclick = async event => {
    const act = event.target.dataset.act;
    if (act === "ok") await saveReview(s, s.label);
    if (act === "fix") showFixForm(box, s);
    if (act === "trash") await moveToTrash(s.id, `moved by ${username}`, username);
  };
}

// Picking from a menu (instead of typing) keeps names consistent, so
// "racoon" and "raccoon" don't show up as two different animals in the stats.
function showFixForm(box, s) {
  box.onclick = null;
  box.innerHTML = `
    <select class="cap" style="flex:1; min-width:0">
      <option value="">what is it really?</option>
      ${animalNames.map(n => `<option>${escapeHtml(n)}</option>`).join("")}
      <option value="__other">other (not on the list)…</option>
    </select>
    <input placeholder="animal name" style="flex:1; min-width:0" hidden>
    <button>Save</button><button class="secondary">Cancel</button>`;
  const [menu, other, save, cancel] = box.querySelectorAll("select, input, button");
  menu.focus();
  menu.onchange = () => { other.hidden = menu.value !== "__other"; if (!other.hidden) other.focus(); };
  const chosen = () => (menu.value === "__other" ? other.value : menu.value).trim();
  save.onclick = () => chosen() && saveReview(s, chosen());
  other.onkeydown = e => { if (e.key === "Enter") save.click(); };
  cancel.onclick = () => drawReview(box, s);
}

async function saveReview(s, label) {
  label = label.trim().toLowerCase().slice(0, 60);
  const noAnimal = NOT_ANIMALS.includes(label);
  try {
    await updateDoc(doc(db, "sightings", s.id), {
      label,
      isAnimal: !noAnimal,
      reviewed: true,
      reviewedBy: username,
      reviewedAt: serverTimestamp(),
      // Photos with no animal (or a person) go to the Trash, just like the AI's.
      ...(noAnimal ? { trashed: true, trashReason: `marked "${label}" by ${username}`,
                       trashedBy: username, trashedAt: serverTimestamp() } : {}),
    });
  } catch (e) {
    console.error("Couldn't save review:", e);
    alert("Couldn't save that. Check your internet connection and try again.\n\n" + e.message);
  }
}

// ---------- Counts and filters ----------

async function updateSummary() {
  const all = collection(db, "sightings");
  const [total, animals, trash] = await Promise.all([
    getCountFromServer(query(all, where("trashed", "==", false))),
    getCountFromServer(query(all, where("trashed", "==", false), where("isAnimal", "==", true))),
    getCountFromServer(query(all, where("trashed", "==", true))),
  ]);
  $("summary").innerHTML = `${total.data().count} photos · ${animals.data().count} with animals · ` +
    `<a href="/trash">🗑 Trash (${trash.data().count})</a> · * = time the photo arrived (the trap's clock wasn't set)`;
}

// Recount when photos change (at most once a second), plus once a minute.
let summaryTimer = null;
function refreshSummarySoon() {
  clearTimeout(summaryTimer);
  summaryTimer = setTimeout(updateSummary, 1000);
}

for (const id of ["trap", "animal", "hideEmpty"]) $(id).onchange = () => { howMany = PAGE; listen(); };
$("unchecked").onchange = () => listen();
$("more").onclick = () => { howMany += PAGE; listen(); };

listen();
updateSummary();
setInterval(updateSummary, 60000);
