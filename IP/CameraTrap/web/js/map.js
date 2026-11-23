// Map page: where the traps are and what each one has seen.

import { requireLogin, loadAllSightings, loadTraps, groupIntoVisits, escapeHtml } from "./common.js";

await requireLogin();
const [sightings, traps] = await Promise.all([loadAllSightings(), loadTraps()]);
const visits = groupIntoVisits(sightings);
const content = document.getElementById("content");

const placed = Object.values(traps).filter(t => t.lat != null && t.lon != null);
if (!placed.length) {
  content.innerHTML = `<p>No trap locations yet. <a href="/traps">Add GPS coordinates on the Traps page.</a></p>`;
} else {
  const markers = placed.map(t => {
    const counts = {};
    visits.filter(v => v.device === t.id).forEach(v => (counts[v.animal] = (counts[v.animal] || 0) + 1));
    const top = Object.entries(counts).sort((a, b) => b[1] - a[1]);
    return { ...t, name: t.name || t.id, visits: top.reduce((sum, [, n]) => sum + n, 0), top };
  });

  content.innerHTML = `
    <div id="map"></div>
    <div class="panel table-wrap" style="margin-top:16px"><table>
      <tr><th>Trap</th><th>Location</th><th>Habitat</th><th>Animal visits</th><th>Most seen</th></tr>
      ${markers.map(m => `<tr><td>${escapeHtml(m.name)}</td><td>${m.lat.toFixed(5)}, ${m.lon.toFixed(5)}</td>
        <td>${escapeHtml(m.habitat)}</td><td>${m.visits}</td>
        <td class="cap">${m.top.slice(0, 5).map(([a, n]) => `${escapeHtml(a)} (${n})`).join(", ")}</td></tr>`).join("")}
    </table></div>`;

  if (typeof L === "undefined") {
    document.getElementById("map").outerHTML = `<p class="error">The map couldn't load (no internet?). Trap locations are listed below.</p>`;
  } else {
    const map = L.map("map");
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
      maxZoom: 19, attribution: "&copy; OpenStreetMap contributors",
    }).addTo(map);
    for (const m of markers) {
      const top = m.top.slice(0, 5).map(([a, n]) => `${escapeHtml(a)} (${n})`).join("<br>") || "no animals yet";
      L.marker([m.lat, m.lon]).addTo(map)
        .bindPopup(`<b>${escapeHtml(m.name)}</b><br>${escapeHtml(m.habitat)}<br>${m.visits} visits<br><br>${top}`);
    }
    map.fitBounds(markers.map(m => [m.lat, m.lon]), { padding: [40, 40], maxZoom: 17 });
  }
}
