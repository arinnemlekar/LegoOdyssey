// Trash page: photos with no animal (or moved here by a team member).
// Restore puts a photo back on the Photos page. Empty Trash deletes everything here for good.

import { requireLogin, db, loadTraps, photoUrl, escapeHtml, formatTime, API_BASE } from "./common.js";
import { collection, query, where, orderBy, limit, onSnapshot, doc, updateDoc }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js";

const { user } = await requireLogin();
const $ = id => document.getElementById(id);
const traps = await loadTraps();
const trapName = id => traps[id]?.name || id;
const SHOW = 200;
let count = 0;

const q = query(collection(db, "sightings"), where("trashed", "==", true), orderBy("trashedAt", "desc"), limit(SHOW));
onSnapshot(q, snap => {
  count = snap.size;
  const docs = snap.docs.map(d => ({ id: d.id, ...d.data() }));
  $("count").innerHTML = count
    ? `<b>${count}${count === SHOW ? "+" : ""}</b> photo${count === 1 ? "" : "s"} in the Trash`
    : "The Trash is empty.";
  drawEmptyButton();
  $("grid").replaceChildren(...docs.map(makeCard));
}, err => {
  $("count").innerHTML = `<span class="error">Couldn't load the Trash: ${escapeHtml(err.message)}</span>`;
});

function reasonText(s) {
  const r = s.trashReason || "";
  if (r === "blank photo") return "Blank photo";
  if (r.startsWith("AI: ")) return `The AI saw: ${r.slice(4)}`;
  return r.charAt(0).toUpperCase() + r.slice(1);
}

function makeCard(s) {
  const el = document.createElement("div");
  el.className = "card empty";
  el.innerHTML = `
    <a href="#" class="photo"><img alt="" loading="lazy"></a>
    <div class="info">
      <div class="label">${escapeHtml(s.label)}</div>
      <div class="meta">${formatTime(s.capturedAt)} · ${escapeHtml(trapName(s.device))}</div>
      <div class="reason">🗑 ${escapeHtml(reasonText(s))}</div>
    </div>
    <div class="review"><button class="secondary" data-act="restore">↩ Restore</button></div>`;
  photoUrl(s.thumb).then(url => (el.querySelector("img").src = url)).catch(() => {});
  el.querySelector(".photo").onclick = async event => {
    event.preventDefault();
    const tab = window.open("", "_blank");
    tab.location = await photoUrl(s.photo);
  };
  el.querySelector("[data-act=restore]").onclick = async e => {
    e.target.disabled = true;
    // Back to the Photos page. If the AI said "no animal", fix the name there with ✎ Fix.
    await updateDoc(doc(db, "sightings", s.id), { trashed: false, trashReason: null, trashedBy: null, trashedAt: null });
  };
  return el;
}

// ---------- Emptying the Trash ----------

function drawEmptyButton() {
  const actions = $("actions");
  if (actions.dataset.confirming) return;
  actions.innerHTML = count ? `<button id="empty" class="danger">Empty Trash</button>` : "";
  if (count) $("empty").onclick = askToConfirm;
}

function askToConfirm() {
  const actions = $("actions");
  actions.dataset.confirming = "yes";
  actions.innerHTML = `<span class="confirm">Delete ${count} photo${count === 1 ? "" : "s"} forever?
    <button id="yes" class="danger">Yes, delete forever</button> <button id="no" class="secondary">Cancel</button></span>`;
  $("no").onclick = () => { delete actions.dataset.confirming; drawEmptyButton(); };
  $("yes").onclick = emptyTrash;
}

function show(message, ok) {
  $("message").className = ok ? "ok" : "error";
  $("message").textContent = message;
  $("message").hidden = false;
}

async function emptyTrash() {
  const actions = $("actions");
  actions.innerHTML = `<span class="muted">Deleting… (can take up to a minute)</span>`;
  try {
    const response = await fetch(API_BASE + "/api/empty-trash", {
      method: "POST",
      headers: { Authorization: "Bearer " + (await user.getIdToken()) },
    });
    if (!response.ok) throw new Error((await response.text()).match(/<p>(.*?)<\/p>/)?.[1] || `error ${response.status}`);
    const { deleted } = await response.json();
    show(`Deleted ${deleted} photo${deleted === 1 ? "" : "s"} forever.`, true);
  } catch (e) {
    show("Couldn't empty the Trash: " + e.message, false);
  }
  delete actions.dataset.confirming;
  drawEmptyButton();
}
