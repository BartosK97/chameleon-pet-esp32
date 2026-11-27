// Served at http://mini-pet.local/ : live view, feed/lamp buttons, status. Polls /status and /frame.
#pragma once

static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket Aquarium</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;background:#0b2540;color:#e8f4ff;margin:0 auto;padding:16px;max-width:420px}
h1{font-size:20px;margin:4px 0 12px}
canvas{width:100%;image-rendering:pixelated;border-radius:14px;background:#123;display:block}
.row{display:flex;gap:8px;margin:12px 0}
button{flex:1;padding:14px;border:0;border-radius:12px;font-size:16px;font-weight:600;background:#1e88e5;color:#fff}
button:active{transform:scale(.97)}
#lamp{background:#f5b301;color:#222}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.card{background:#123a5c;border-radius:12px;padding:10px 12px;font-size:13px;opacity:.95}
.card b{display:block;font-size:22px;margin-top:2px}
.bar{height:8px;background:#0b2540;border-radius:4px;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:#fa0;transition:width .5s}
#msg{min-height:1.2em;font-size:14px;opacity:.85}
h2{font-size:15px;margin:18px 0 8px}
.fish{display:flex;justify-content:space-between;background:#123a5c;border-radius:10px;padding:8px 12px;margin:6px 0;font-size:14px}
.fish small{opacity:.7}
#log{max-height:220px;overflow-y:auto;font-size:14px}
#log p{margin:6px 0}#log .me{text-align:right;opacity:.8}
form{display:flex;gap:8px}input{flex:1;padding:12px;border-radius:12px;border:0;font-size:16px;background:#123a5c;color:#fff}
</style></head><body>
<h1>Pocket Aquarium</h1>
<canvas id="view" width="120" height="160"></canvas>
<div class="row"><button id="feed">Feed</button><button id="lamp">Lamp</button></div>
<p id="msg"></p>
<div class="grid">
  <div class="card">Food<b id="food">-</b><div class="bar"><i id="foodbar"></i></div></div>
  <div class="card">Fish<b id="fish">-</b></div>
  <div class="card">Visitors today<b id="vis">-</b></div>
  <div class="card">Lamp<b id="lmp">-</b></div>
</div>
<h2>Fish</h2><div id="fishlist"></div>
<h2>Talk to the fish</h2>
<div id="log"></div>
<form id="chat"><input id="q" maxlength="200" placeholder="Say something..." autocomplete="off"><button>Send</button></form>
<script>
const $ = id => document.getElementById(id);
const view = $('view'), ctx = view.getContext('2d');
let img = ctx.createImageData(120, 160);  // resized when the device turns to landscape
async function frame() {
  try {
    const px = new Uint16Array(await (await fetch('/frame')).arrayBuffer());
    if (px.length !== img.width * img.height) throw 0;
    for (let i = 0; i < px.length; i++) {
      const v = px[i];
      img.data.set([(v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3, 255], i * 4);
    }
    ctx.putImageData(img, 0, 0);
  } catch (e) {}
  setTimeout(frame, 2000);
}
async function status() {
  try {
    const s = await (await fetch('/status')).json();
    if (s.w !== view.width) { view.width = s.w; view.height = s.h; img = ctx.createImageData(s.w, s.h); }
    const food = Math.round((1 - s.hunger) * 100);
    $('food').textContent = s.alive ? food + ' %' : '-';
    $('foodbar').style.width = (s.alive ? food : 0) + '%';
    $('fish').textContent = s.alive + (s.young ? ' (' + s.young + ' young)' : '') + (s.dead ? ', ' + s.dead + ' dead' : '');
    $('vis').textContent = s.visitors;
    $('lmp').textContent = s.lamp ? 'on' : 'off';
  } catch (e) {}
  setTimeout(status, 2000);
}
$('feed').onclick = async () => {
  const r = await (await fetch('/feed', {method: 'POST'})).json();
  $('msg').textContent = r.ok ? 'Food is on its way.' : 'They were just fed. Try again in a few seconds.';
};
$('lamp').onclick = async () => { await fetch('/lamp', {method: 'POST'}); status(); };
async function fish() {
  try {
    const list = await (await fetch('/fish')).json();
    $('fishlist').innerHTML = list.map(f => `<div class="fish"><span><b>${esc(f.name)}</b> <small>${esc(f.kind)}${f.baby ? ', baby' : ''} · ${esc(f.trait)}</small></span><span>${f.dead ? 'dead' : 'hunger ' + f.hunger + ' %'}</span></div>`).join('');
  } catch (e) {}
  setTimeout(fish, 5000);
}
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
function line(cls, html) { const p = document.createElement('p'); p.className = cls; p.innerHTML = html; $('log').append(p); $('log').scrollTop = 1e9; }
$('chat').onsubmit = async e => {
  e.preventDefault();
  const q = $('q').value.trim();
  if (!q) return;
  $('q').value = '';
  line('me', esc(q));
  try {
    const r = await (await fetch('/chat', {method: 'POST', body: q})).json();
    line('', (r.name ? '<b>' + esc(r.name) + ':</b> ' : '') + esc(r.reply));
  } catch (e) { line('', 'No answer.'); }
};
frame();
status();
fish();
</script></body></html>)HTML";
