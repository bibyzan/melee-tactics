// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal host page for the browser build: disc picker, canvas, persistence.
// The engine's whole host interface is the handful of Module fields set here.
import { createDiscCache } from './disc-cache.mjs';
import { createLink } from './link.mjs';

const $ = (id) => document.getElementById(id);
const lines = [];
function log(text) {
  lines.push(String(text));
  $('log').textContent = lines.slice(-80).join('\n');
  console.log(text);
}
const status = (text) => { $('status').textContent = text; };

// Rolling frame statistics; also read by tests/browser/shell-e2e.mjs.
const frames = { count: 0, last: 0, samples: [] };
window.meleeFrames = frames;
function onFrame() {
  const now = performance.now();
  if (frames.last) {
    frames.samples.push(now - frames.last);
    if (frames.samples.length > 7200) frames.samples.shift(); // two minutes
  }
  frames.last = now;
  if (++frames.count % 30 === 0 && frames.samples.length > 60) {
    const recent = frames.samples.slice(-120);
    const sorted = [...recent].sort((a, b) => a - b);
    const fps = 1000 * recent.length / recent.reduce((a, b) => a + b, 0);
    $('stats').textContent = `${fps.toFixed(1)} fps · p99 ${sorted[Math.floor(sorted.length * 0.99)].toFixed(1)} ms`;
  }
}

function syncfs(populate) {
  return new Promise((resolve, reject) =>
    Module.FS.syncfs(populate, (error) => (error ? reject(error) : resolve())));
}

// Any MELEE_* query parameter becomes an environment variable, so the knobs in
// docs/testing.md work unchanged: ?MELEE_BOOT_SCENE=vs&MELEE_SEED=1
// This page is Melee Tactics: it boots into the tactics draft unless told
// otherwise.
const ENV = { MELEE_BOOT_SCENE: 'tactics' };
const params = new URLSearchParams(location.search);
for (const [key, value] of params) {
  if (/^MELEE_[A-Z0-9_]+$/.test(key)) ENV[key] = value;
}
// ?room=CODE joins the other player's room.
const joinRoom = params.get('room');

window.Module = {
  // preRun is the one point where this works: Emscripten has created ENV but
  // has not yet run the static constructor that snapshots it into environ.
  preRun: [() => Object.assign(Module.ENV, ENV)],
  canvas: $('canvas'),
  print: log,
  printErr: log,
  onFrame,
  onAbort: (reason) => status(`Engine stopped: ${reason}`),
  onGraphicsPreparation: (done, total) =>
    status(done === total ? 'Starting…' : `Preparing graphics… ${Math.floor(done * 100 / total)}%`),
  onRuntimeInitialized: () => { ready = true; status('Choose a GALE01 disc image (.iso or .gcm).'); updateStart(); },
};

// Not `typeof Module.callMain`: that exists as soon as the script runs, while
// the wasm is still compiling, and a disc picked by then started a dead runtime.
let ready = false;
const buttons = ['cpu', 'create', 'join'];
if (joinRoom) {
  $('join').hidden = false;
  $('join').textContent = `Join room ${joinRoom}`;
}
// Local testing only: with ?dev_disc=1 the page streams the disc a local
// server was started with (DEV_DISC), by range requests, instead of asking
// for a file in every tab. It reads like a File: size and slice().
let devDisc = null;
if (params.get('dev_disc')) {
  fetch('./dev/disc', { method: 'HEAD' }).then((res) => {
    if (!res.ok) throw Error('this server was started without DEV_DISC');
    const size = Number(res.headers.get('Content-Length'));
    devDisc = {
      size,
      name: 'dev-disc.iso',
      slice: (start, end) => ({
        arrayBuffer: async () =>
          (await fetch('./dev/disc', { headers: { Range: `bytes=${start}-${end - 1}` } })).arrayBuffer(),
      }),
    };
    $('disc').hidden = true;
    updateStart();
  }).catch((error) => status(`No dev disc: ${error.message}`));
}

function updateStart() {
  for (const id of buttons) $(id).disabled = !(ready && (devDisc || $('disc').files.length));
}
$('disc').addEventListener('change', updateStart);

// ICE servers come from the page server, so a deployment can add TURN
// without a new build.
async function iceServers() {
  try {
    const config = await (await fetch('./config')).json();
    if (Array.isArray(config.iceServers)) return config.iceServers;
  } catch {}
  return [{ urls: 'stun:stun.l.google.com:19302' }];
}

function roomCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  return Array.from(crypto.getRandomValues(new Uint8Array(6)), (b) => alphabet[b % alphabet.length]).join('');
}

$('cpu').addEventListener('click', () => start(null));
$('create').addEventListener('click', async () => {
  const room = roomCode();
  const url = `${location.origin}${location.pathname}?room=${room}`;
  $('share-link').href = url;
  $('share-link').textContent = url;
  $('share').hidden = false;
  start(createLink({ room, host: true, iceServers: await iceServers(), log }));
});
$('join').addEventListener('click', async () =>
  start(createLink({ room: joinRoom, host: false, iceServers: await iceServers(), log })));

async function start(link) {
  for (const id of buttons) $(id).disabled = true;
  $('disc').disabled = true;
  // The engine polls this through link_web.c; none means offline.
  Module.tacticsLink = link;
  try {
    // The engine reports adapter and device failures itself (onAbort); this
    // only catches the common case early, before anything is mounted.
    if (!navigator.gpu) throw Error('This browser has no WebGPU. Try a current Chrome or Edge.');
    Module.discFile = devDisc || $('disc').files[0];
    Module.readDisc = createDiscCache(Module.discFile).read;
    for (const dir of ['/saves', '/cache']) {
      Module.FS.mkdirTree(dir);
      Module.FS.mount(Module.FS.filesystems.IDBFS, { autoPersist: dir === '/saves' }, dir);
    }
    await syncfs(true);
    // The pipeline cache is written by a background thread; flush it when the
    // page is hidden rather than on every write.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') syncfs(false).catch(log);
    });
    status('');
    $('canvas').focus();
    Module.callMain([]);
  } catch (error) {
    status(error.message);
    log(error.stack || error);
  }
}

// Threads need a cross-origin isolated page. Where the server cannot send
// COOP/COEP (GitHub Pages), coi-sw.js adds them and the page reloads once
// under its control; the session flag stops a browser that still refuses
// from reloading for ever.
if (crossOriginIsolated) {
  sessionStorage.removeItem('melee-coi-reload');
  const script = document.createElement('script');
  script.src = './melee_browser.js';
  script.onerror = () => status('melee_browser.js is missing: run tools/browser/build.py first.');
  document.head.append(script);
} else if (!isSecureContext) {
  // Plain HTTP on a LAN address: the browser withholds threads. localhost is
  // exempt; for another machine, Chrome can be told to trust this address.
  status(`Browsers only run this game on a secure address. In Chrome, open ` +
    `chrome://flags/#unsafely-treat-insecure-origin-as-secure, add ${location.origin}, ` +
    `enable it and relaunch.`);
} else if (navigator.serviceWorker && !sessionStorage.getItem('melee-coi-reload')) {
  sessionStorage.setItem('melee-coi-reload', '1');
  navigator.serviceWorker.register('./coi-sw.js')
    .then(() => navigator.serviceWorker.ready)
    .then(() => location.reload(), (error) => status(`Cannot enable threads: ${error.message}`));
} else {
  status('This page needs cross-origin isolation for its threads, and this browser did not allow it.');
}
