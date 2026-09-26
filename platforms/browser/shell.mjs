// SPDX-License-Identifier: GPL-3.0-or-later
// Melee Tactics' page. The only thing on it is the disc: the first visit asks
// for the player's own Melee image, the page keeps a copy in the browser's
// private storage (OPFS), and later visits go straight into the game. Every
// menu, against the CPU or online, is in the game itself; online play goes
// through Module.tacticsLink (link.mjs).
import { createDiscCache } from './disc-cache.mjs';
import { createLinkManager } from './link.mjs';
import { createFighterPicker, createPlanOverlay, createTouchControls, wantsTouch } from './touch.mjs';

const $ = (id) => document.getElementById(id);
const lines = [];
function log(text) {
  lines.push(String(text));
  $('log').textContent = lines.slice(-200).join('\n');
  console.log(text);
}
const status = (text) => { $('status').textContent = text; };

// Rolling frame times; read by tests/browser/shell-e2e.mjs.
const frames = { count: 0, last: 0, samples: [] };
window.meleeFrames = frames;
function onFrame() {
  const now = performance.now();
  if (frames.last) {
    frames.samples.push(now - frames.last);
    if (frames.samples.length > 7200) frames.samples.shift();
  }
  frames.last = now;
  frames.count++;
  // Drawing: this launch worked (see BOOT_PENDING).
  if (frames.count === 30) {
    try { localStorage.removeItem('melee-boot-pending'); } catch {}
  }
}

function syncfs(populate) {
  return new Promise((resolve, reject) =>
    Module.FS.syncfs(populate, (error) => (error ? reject(error) : resolve())));
}

// The page boots into Melee Tactics' menus. Any MELEE_* query parameter
// becomes an environment variable (docs/testing.md): ?MELEE_SEED=1
const ENV = { MELEE_BOOT_SCENE: 'tactics' };
const params = new URLSearchParams(location.search);
for (const [key, value] of params) {
  if (/^MELEE_[A-Z0-9_]+$/.test(key)) ENV[key] = value;
}

// ---- the screen ------------------------------------------------------------

const touch = wantsTouch(params);
document.body.classList.toggle('touch', touch);

// Browser bars. On an iPhone the page cannot hide Safari's (fullscreen is for
// video only there), but opened from the home screen it has none, so say so.
// Where fullscreen exists (Android, iPad), the first touch on the game asks.
const standalone = matchMedia('(display-mode: fullscreen), (display-mode: standalone)').matches ||
  navigator.standalone === true;
const ios = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
  (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
if (ios && !standalone) $('ios-tip').hidden = false;
if (touch && !standalone) {
  $('stage').addEventListener('pointerdown', () => {
    const root = document.documentElement;
    const ask = root.requestFullscreen || root.webkitRequestFullscreen;
    try {
      ask?.call(root)?.catch?.(() => {});
    } catch {}
  }, { once: true });
}

// The upscale: the engine draws Melee's 640x480 frame at this multiple, then
// fits it to the canvas. ?scale= wins, then the RESOLUTION the player last set
// in the game's main menu (render_scale.c keeps it here), then 3x on a desktop
// and 1x on a touch screen, whose GPU pays most for every pixel.
// A launch that never drew anything leaves this behind; the next visit then
// drops the saved resolution, in case that was the cause.
const BOOT_PENDING = 'melee-boot-pending';
try {
  if (localStorage.getItem(BOOT_PENDING)) {
    localStorage.removeItem(BOOT_PENDING);
    if (localStorage.getItem('melee-render-scale')) {
      localStorage.removeItem('melee-render-scale');
      console.log('The last launch never drew; back to the default resolution.');
    }
  }
} catch {}

function renderScale() {
  const valid = (n) => Number.isFinite(n) && n >= 1 && n <= 4;
  const forced = Number(params.get('scale'));
  if (valid(forced)) return forced;
  let saved = NaN;
  try {
    saved = Number(localStorage.getItem('melee-render-scale'));
  } catch {}
  if (valid(saved)) return saved;
  return touch ? 1 : 3;
}

// A saved resolution the device cannot run must not lock the player out: if
// the game stops, or draws nothing for a while after starting, forget it and
// reload once at the default. True when that is what happens.
function resetResolution() {
  let saved = null;
  try {
    saved = localStorage.getItem('melee-render-scale');
    if (!saved || Number(saved) <= 1 || params.get('scale') || sessionStorage.getItem('melee-scale-reset')) {
      return false;
    }
    localStorage.removeItem('melee-render-scale');
    sessionStorage.setItem('melee-scale-reset', '1');
  } catch {
    return false;
  }
  log(`The game did not start at ${saved}x; going back to the default resolution.`);
  status('That resolution is too much for this device. Going back to the default…');
  setTimeout(() => location.reload(), 1500);
  return true;
}

// The game counts as started once it draws. Shader preparation on a first
// visit can take a while, so the clock runs from when that is done, up to
// 90 seconds in all.
let prepared = false;
function watchStart(since) {
  setTimeout(() => {
    if (frames.count >= 30) {
      try { localStorage.removeItem(BOOT_PENDING); } catch {}
      try { sessionStorage.removeItem('melee-scale-reset'); } catch {}
    } else if (prepared || Date.now() - since > 90000) {
      resetResolution();
    } else {
      watchStart(since);
    }
  }, 20000);
}

// ---- the disc ------------------------------------------------------------

const DISC = 'melee.iso';
const DISC_DONE = 'melee.iso.done'; // written last: the copy is whole

async function isMelee(file) {
  const head = new Uint8Array(await file.slice(0, 6).arrayBuffer());
  return String.fromCharCode(...head) === 'GALE01';
}

async function rememberedDisc() {
  try {
    const dir = await navigator.storage.getDirectory();
    const done = await (await (await dir.getFileHandle(DISC_DONE)).getFile()).text();
    const file = await (await dir.getFileHandle(DISC)).getFile();
    if (String(file.size) === done && await isMelee(file)) return file;
  } catch {}
  return null;
}

async function forgetDisc() {
  const dir = await navigator.storage.getDirectory();
  for (const name of [DISC_DONE, DISC]) await dir.removeEntry(name).catch(() => {});
}

// Copy the disc into the page's private storage in the background, so the
// next visit needs no file. Skipped quietly where the browser cannot.
async function rememberDisc(file) {
  try {
    const dir = await navigator.storage.getDirectory();
    const estimate = await navigator.storage.estimate?.();
    if (estimate && estimate.quota - estimate.usage < file.size + 64 * 1024 * 1024) {
      log('Not enough browser storage to remember the disc.');
      return;
    }
    await navigator.storage.persist?.();
    await forgetDisc();
    const handle = await dir.getFileHandle(DISC, { create: true });
    if (!handle.createWritable) {
      log('This browser cannot remember the disc; it will ask again next time.');
      return;
    }
    await file.stream().pipeTo(await handle.createWritable());
    const done = await (await dir.getFileHandle(DISC_DONE, { create: true })).createWritable();
    await done.write(String(file.size));
    await done.close();
    log('Disc remembered for next time.');
  } catch (error) {
    log(`Could not remember the disc: ${error.message}`);
  }
}

// Local testing only: ?dev_disc=1 streams the disc a local server was
// started with (DEV_DISC) by range requests. It reads like a File.
async function devDisc() {
  const res = await fetch('./dev/disc', { method: 'HEAD' });
  if (!res.ok) throw Error('this server was started without DEV_DISC');
  const size = Number(res.headers.get('Content-Length'));
  return {
    size,
    name: 'dev-disc.iso',
    slice: (start, end) => ({
      arrayBuffer: async () =>
        (await fetch('./dev/disc', { headers: { Range: `bytes=${start}-${end - 1}` } })).arrayBuffer(),
    }),
  };
}

// ---- the engine ----------------------------------------------------------

async function iceServers() {
  try {
    const config = await (await fetch('./config')).json();
    if (Array.isArray(config.iceServers)) return config.iceServers;
  } catch {}
  return [{ urls: 'stun:stun.l.google.com:19302' }];
}

let ready = false;
let disc = null;
let started = false;

window.Module = {
  // preRun is the one point where this works: Emscripten has created ENV but
  // has not yet run the static constructor that snapshots it into environ.
  preRun: [() => Object.assign(Module.ENV, ENV)],
  canvas: $('canvas'),
  print: log,
  printErr: log,
  onFrame,
  onAbort: (reason) => {
    if (!resetResolution()) status(`The game stopped: ${reason}. Reload the page to start again.`);
  },
  onGraphicsPreparation: (done, total) => {
    if (done === total) prepared = true;
    status(done === total ? '' : `Preparing graphics… ${Math.floor(done * 100 / total)}%`);
  },
  onRuntimeInitialized: () => { ready = true; begin(); },
};

async function begin() {
  if (!ready || !disc || started) {
    if (ready && !disc) status('');
    return;
  }
  started = true;
  try {
    if (!navigator.gpu) throw Error('This browser has no WebGPU. Try a current Chrome, Edge or Safari.');
    Module.discFile = disc;
    Module.readDisc = createDiscCache(disc).read;
    // ?relay=1 skips the direct connection and goes through the server.
    Module.tacticsLink = createLinkManager({ iceServers: await iceServers(), log,
                                            forceRelay: params.get('relay') === '1' });
    for (const dir of ['/saves', '/cache']) {
      Module.FS.mkdirTree(dir);
      Module.FS.mount(Module.FS.filesystems.IDBFS, { autoPersist: dir === '/saves' }, dir);
    }
    // Saved data (settings, the graphics cache) must not hold the start up:
    // Safari on iOS can stop answering IndexedDB after a page was reloaded
    // mid-write, until Safari is closed.
    const loaded = await Promise.race([
      syncfs(true).then(() => true, (error) => { log(`Saved data unavailable: ${error.message}`); return true; }),
      new Promise((resolve) => setTimeout(() => resolve(false), 8000)),
    ]);
    if (!loaded) log('Saved data did not load in time; starting without it this visit.');
    // The pipeline cache is written by a background thread; flush it when
    // the page is hidden rather than on every write.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') syncfs(false).catch(log);
    });
    $('welcome').style.display = 'none';
    document.body.classList.add('playing');
    Module.renderScale = renderScale();
    log(`Rendering at ${640 * Module.renderScale}x${480 * Module.renderScale} (${Module.renderScale}x).`);
    if (touch) {
      createTouchControls({ left: $('pad-left'), right: $('pad-right'),
                            send: (buttons) => Module._browser_touch_pad(buttons) });
      Module.onPlan = createPlanOverlay({ stage: $('stage'), pick: (index) => Module._browser_plan_pick(index) });
      // A tap on Back or Fight is a press of B or START on the pad.
      const press = (bits) => {
        Module._browser_touch_pad(bits);
        setTimeout(() => Module._browser_touch_pad(0), 90);
      };
      Module.onFighters = createFighterPicker({ stage: $('stage'), press,
        pick: (slot, ckind) => Module._browser_pick_fighter(slot, ckind) });
    }
    status('');
    $('canvas').focus();
    Module.callMain([]);
    try { localStorage.setItem(BOOT_PENDING, '1'); } catch {}
    watchStart(Date.now());
  } catch (error) {
    status(error.message);
    log(error.stack || error);
  }
}

$('disc').addEventListener('change', async () => {
  const file = $('disc').files[0];
  if (!file) return;
  if (!(await isMelee(file))) {
    status('That is not a Super Smash Bros. Melee (GALE01) disc image.');
    return;
  }
  disc = file;
  status('Starting…');
  begin();
  rememberDisc(file);
});

$('forget').addEventListener('click', () =>
  forgetDisc().then(() => status('The disc is forgotten. Reload to choose another.')));

// A remembered disc (or the local dev disc) starts the game without asking.
(async () => {
  try {
    disc = params.get('dev_disc') ? await devDisc() : await rememberedDisc();
  } catch (error) {
    status(`No dev disc: ${error.message}`);
  }
  if (disc) {
    $('welcome').style.display = 'none';
    status('Starting…');
    begin();
  } else {
    $('disc').disabled = false;
    if (!ready) status('');
  }
})();

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
