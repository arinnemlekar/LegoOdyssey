// Animals page: the list of animals the AI chooses from.

import { requireLogin, db, parseAnimalList, formatTime, escapeHtml } from "./common.js";
import { doc, getDoc, setDoc, serverTimestamp }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js";

const { username } = await requireLogin();
const $ = id => document.getElementById(id);
const listDoc = doc(db, "settings", "animals");

async function load() {
  const snap = await getDoc(listDoc);
  if (snap.exists()) {
    const data = snap.data();
    $("text").value = data.text;
    $("updated").textContent = data.updatedBy ? `Last changed by ${data.updatedBy} on ${formatTime(data.updatedAt)}` : "";
  } else {
    $("text").value = await fetch("/animals-default.txt").then(r => r.text());
    $("updated").textContent = "This is the starting list. Save it to start using your own.";
  }
}

function show(message, ok) {
  $("status").className = ok ? "ok" : "error";
  $("status").textContent = message;
  $("status").hidden = false;
}

$("save").onclick = async () => {
  const text = $("text").value;
  const count = Object.keys(parseAnimalList(text)).length;
  if (count === 0) return show("The list needs at least one animal.", false);
  if (text.length > 5000) return show("The list is too long (5000 characters max).", false);
  $("save").disabled = true;
  try {
    await setDoc(listDoc, { text, updatedBy: username, updatedAt: serverTimestamp() });
    show(`Saved! The AI will look for ${count} animals in new photos.`, true);
    await load();
  } catch (e) {
    show("Couldn't save: " + escapeHtml(e.message), false);
  }
  $("save").disabled = false;
};

load();
