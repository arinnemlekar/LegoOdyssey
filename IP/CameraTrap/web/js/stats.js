// Stats page: when, and in what conditions, do animals visit?

import { requireLogin, loadAllSightings, loadTraps, groupIntoVisits, barChart, downloadCsv,
         escapeHtml, MOON_PHASES, VISIT_GAP_SECONDS } from "./common.js";

await requireLogin();
const $ = id => document.getElementById(id);
$("csv").onclick = () => downloadCsv();

const [sightings, traps] = await Promise.all([loadAllSightings(), loadTraps()]);
const allVisits = groupIntoVisits(sightings);

// Animal menu, most-seen first.
const species = count(allVisits.map(v => v.animal));
for (const [name, n] of Object.entries(species).sort((a, b) => b[1] - a[1])) {
  $("animal").add(new Option(`${name} (${n})`, name));
}
const wanted = new URLSearchParams(location.search).get("animal");
if (wanted) $("animal").value = wanted;
$("animal").onchange = () => {
  history.replaceState(null, "", $("animal").value ? `?animal=${encodeURIComponent($("animal").value)}` : "/stats");
  draw();
};

function count(items) {
  const counts = {};
  for (const item of items) if (item != null) counts[item] = (counts[item] || 0) + 1;
  return counts;
}

/** Turn counts into chart rows in a fixed order (so empty bars still show). */
function rowsFor(counts, labels) {
  return labels.map(label => [label, counts[label] || 0]);
}

function band(value, size) {
  const low = Math.floor(value / size) * size;
  return low;
}

function draw() {
  const animal = $("animal").value;
  $("title-animal").textContent = animal ? ": " + animal : "";
  const visits = allVisits.filter(v => !animal || v.animal === animal);

  const first = visits.length ? visits[0] : null;
  const dates = sightings.map(s => s.capturedAt.toDate());
  $("summary").textContent = `${visits.length} animal visits from ${sightings.length} photos` +
    (dates.length ? ` between ${dates[0].toLocaleDateString()} and ${dates[dates.length - 1].toLocaleDateString()}` : "") +
    `. Photos from the same trap within ${VISIT_GAP_SECONDS} seconds count as one visit.`;

  // Time of day
  const hours = Array.from({ length: 24 }, (_, h) => String(h).padStart(2, "0") + ":00");
  const byHour = rowsFor(count(visits.map(v => hours[v.start.getHours()])), hours);

  // Moon
  const byMoon = rowsFor(count(visits.map(v => v.moonName)), MOON_PHASES);

  // Temperature in 5 °C bands
  const temps = visits.map(v => v.temperatureC).filter(t => t != null);
  const tempLabel = low => `${low} to ${low + 5}°C`;
  let tempLabels = [];
  if (temps.length) {
    for (let t = band(Math.min(...temps), 5); t <= band(Math.max(...temps), 5); t += 5) tempLabels.push(tempLabel(t));
  }
  const byTemp = rowsFor(count(temps.map(t => tempLabel(band(t, 5)))), tempLabels);

  // Humidity in 10% bands
  const humLabel = low => `${low}–${low + 10}%`;
  const humLabels = Array.from({ length: 10 }, (_, i) => humLabel(i * 10));
  const byHum = rowsFor(count(visits.map(v => v.humidity).filter(h => h != null)
    .map(h => humLabel(Math.min(band(h, 10), 90)))), humLabels);

  // Trap
  const trapName = id => traps[id]?.name || id;
  const byTrap = Object.entries(count(visits.map(v => trapName(v.device)))).sort((a, b) => b[1] - a[1]);

  $("charts").innerHTML =
    accuracyPanel() +
    barChart("Visits by time of day", byHour) +
    barChart("Visits by moon phase", byMoon,
      "Each phase lasts about 3–4 days. If the traps ran for less than a month, some phases had fewer nights to record visits.") +
    barChart("Visits by temperature", byTemp) +
    barChart("Visits by humidity", byHum) +
    barChart("Visits by trap", byTrap) +
    (animal ? "" : barChart("Visits by animal", Object.entries(species).sort((a, b) => b[1] - a[1])));
}

/** How often was the AI right, on photos the team has checked? */
function accuracyPanel() {
  const checked = sightings.filter(s => s.reviewed);
  if (!checked.length) {
    return `<div class="panel chart"><h3>How accurate is the AI?</h3>
      <p class="muted">No photos checked yet. On the Photos page, press ✓ Right or ✎ Fix on each photo.</p></div>`;
  }
  const right = checked.filter(s => s.label === s.aiLabel).length;
  const mistakes = Object.entries(count(checked.filter(s => s.label !== s.aiLabel)
    .map(s => `AI said “${s.aiLabel}” → really ${s.label}`))).sort((a, b) => b[1] - a[1]).slice(0, 8);
  return `<div class="panel chart"><h3>How accurate is the AI?</h3>
    <div class="big">${Math.round((100 * right) / checked.length)}%</div>
    <p>Right on <b>${right}</b> of <b>${checked.length}</b> photos the team checked.</p>
    ${mistakes.length ? `<p class="muted" style="margin-bottom:4px">Most common mistakes:</p>
      ${mistakes.map(([m, n]) => `<div style="font-size:13px">${escapeHtml(m)} <span class="muted">(${n})</span></div>`).join("")}` : ""}
  </div>`;
}

draw();
