// SPDX-License-Identifier: GPL-3.0-or-later
// Melee Tactics' page. The only thing on it is the disc: the first visit asks
// for the player's own Melee image, the page keeps a copy in the browser's
// private storage (OPFS), and later visits go straight into the game. Every
// menu, against the CPU or online, is in the game itself; online play goes
// through Module.tacticsLink (link.mjs).
import { createDiscCache } from './disc-cache.mjs';
import { createLinkManager } from './link.mjs';

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
  onAbort: (reason) => status(`The game stopped: ${reason}. Reload the page to start again.`),
  onGraphicsPreparation: (done, total) =>
    status(done === total ? '' : `Preparing graphics… ${Math.floor(done * 100 / total)}%`),
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
    Module.tacticsLink = createLinkManager({ iceServers: await iceServers(), log });
    for (const dir of ['/saves', '/cache']) {
      Module.FS.mkdirTree(dir);
      Module.FS.mount(Module.FS.filesystems.IDBFS, { autoPersist: dir === '/saves' }, dir);
    }
    await syncfs(true);
    // The pipeline cache is written by a background thread; flush it when
    // the page is hidden rather than on every write.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') syncfs(false).catch(log);
    });
    $('welcome').style.display = 'none';
    $('game').style.display = 'block';
    status('');
    $('canvas').focus();
    Module.callMain([]);
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
