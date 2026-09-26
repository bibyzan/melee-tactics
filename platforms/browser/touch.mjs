// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen controls for touch screens, in the space around the game: beside
// it in landscape, below it in portrait. They drive GameCube controller 1
// through the engine's virtual pad (browser_touch_pad in main.c), which is all
// Melee Tactics needs: a D-pad for the menus and picks, A, B and Start.

const PAD = { left: 0x0001, right: 0x0002, down: 0x0004, up: 0x0008, a: 0x0100, b: 0x0200, start: 0x1000 };

// A ring that spreads from where the thumb landed, so a press is felt even
// under the thumb.
function ripple(el, event) {
  const box = el.getBoundingClientRect();
  const ring = document.createElement('i');
  ring.className = 'ripple';
  ring.style.left = `${event.clientX - box.left}px`;
  ring.style.top = `${event.clientY - box.top}px`;
  ring.addEventListener('animationend', () => ring.remove());
  el.append(ring);
}

export function createTouchControls({ left, right, send }) {
  let dpad = 0;
  const held = new Map(); // pointerId -> button bit, for the right-hand buttons
  let last = -1;

  function update() {
    let mask = dpad;
    for (const bit of held.values()) mask |= bit;
    if (mask !== last) {
      if (mask & ~last && navigator.vibrate) navigator.vibrate(8);
      last = mask;
      send(mask);
    }
  }

  // The D-pad reads the direction from where the thumb is, so sliding round
  // it changes direction without lifting.
  const pad = document.createElement('div');
  pad.className = 'dpad';
  pad.innerHTML = '<b class="hub"></b><span class="up">▲</span><span class="left">◀</span>' +
    '<span class="right">▶</span><span class="down">▼</span>';
  left.append(pad);
  let dpadPointer = null;
  function aim(event) {
    const box = pad.getBoundingClientRect();
    const x = event.clientX - (box.left + box.width / 2);
    const y = event.clientY - (box.top + box.height / 2);
    const dead = box.width * 0.12;
    if (Math.hypot(x, y) < dead) dpad = 0;
    else if (Math.abs(x) > Math.abs(y)) dpad = x < 0 ? PAD.left : PAD.right;
    else dpad = y < 0 ? PAD.up : PAD.down;
    pad.dataset.dir = Object.keys(PAD).find((k) => PAD[k] === dpad) || '';
    update();
  }
  pad.addEventListener('pointerdown', (event) => {
    event.preventDefault();
    dpadPointer = event.pointerId;
    pad.setPointerCapture(event.pointerId);
    aim(event);
    ripple(pad, event);
  });
  pad.addEventListener('pointermove', (event) => event.pointerId === dpadPointer && aim(event));
  const release = (event) => {
    if (event.pointerId !== dpadPointer) return;
    dpadPointer = null;
    dpad = 0;
    pad.dataset.dir = '';
    update();
  };
  pad.addEventListener('pointerup', release);
  pad.addEventListener('pointercancel', release);

  function button(name, label) {
    const el = document.createElement('button');
    el.className = `btn btn-${name}`;
    el.append(Object.assign(document.createElement('span'), { className: 'btn-label', textContent: label }));
    el.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      el.setPointerCapture(event.pointerId);
      held.set(event.pointerId, PAD[name]);
      el.classList.add('down');
      ripple(el, event);
      update();
    });
    const up = (event) => {
      if (!held.delete(event.pointerId)) return;
      el.classList.remove('down');
      update();
    };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('contextmenu', (event) => event.preventDefault());
    return el;
  }
  const face = document.createElement('div');
  face.className = 'face';
  face.append(button('start', 'START'), button('b', 'B'), button('a', 'A'));
  right.append(face);
}

// Touch controls where the screen is touch-first, or with ?touch=1.
export function wantsTouch(params) {
  if (params.get('touch') === '1') return true;
  if (params.get('touch') === '0') return false;
  return matchMedia('(pointer: coarse)').matches;
}

// A stripe colour for each kind of pick, so the rock-paper-scissors reads at
// a glance: the part of the label before ':' or '>'.
const INTENT_COLOURS = {
  Attack: '#e0503a', Smash: '#ff7a1a', 'Dash in': '#e0503a', 'Anti-air': '#f0a030',
  Grab: '#a060e0', Shield: '#40a0f0', 'Jump in': '#40c070', 'Back off': '#9a9ab4', Zone: '#f0d040',
  'Air Dodge': '#40a0f0', 'Jump Away': '#9a9ab4', 'Drift Away': '#9a9ab4', 'Fight back': '#e0503a',
  Juggle: '#40c070', Cover: '#f0a030', Wait: '#6a6a80',
  'Stand Up': '#9a9ab4', 'Roll In': '#40c070', 'Roll Away': '#9a9ab4', 'Get-up Attack': '#e0503a',
  'Stay Down': '#6a6a80',
};

// The pick list as big buttons in the space the game leaves (plan_web.c
// mirrors it here), over the on-screen controls while a break waits for this
// player: below the game in portrait, split across the two side bands in
// landscape. A tap picks and locks in that move; while the other player is
// choosing it shows that instead.
export function createPlanOverlay({ stage, pick }) {
  const left = document.createElement('div');
  const right = document.createElement('div');
  left.className = 'plan plan-left';
  right.className = 'plan plan-right';
  left.hidden = right.hidden = true;
  stage.append(left, right);
  const landscape = matchMedia('(orientation: landscape)');
  let last = null;

  function row(label, index, cursor) {
    const button = document.createElement('button');
    button.className = index === cursor ? 'plan-row current' : 'plan-row';
    // The intent ("Attack: Down Tilt", "Shield > Grab") sets the stripe colour.
    const intent = (label.split(/[:>]/)[0] || '').trim();
    button.style.setProperty('--tint', INTENT_COLOURS[intent] || '#8a8aa0');
    const [lead, rest] = label.includes(':') ? label.split(/:\s*/, 2) : [null, label];
    if (lead) button.append(Object.assign(document.createElement('small'), { textContent: lead }));
    button.append(Object.assign(document.createElement('span'), { textContent: rest }));
    button.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      if (navigator.vibrate) navigator.vibrate(10);
      for (const b of stage.querySelectorAll('.plan-row')) b.classList.remove('current');
      button.classList.add('current', 'chosen');
      ripple(button, event);
      pick(index);
    });
    return button;
  }

  function head(text, strong) {
    const el = document.createElement('div');
    el.className = 'plan-head';
    el.append(Object.assign(document.createElement(strong ? 'b' : 'span'), { textContent: text }));
    return el;
  }

  function show(parts, cursor) {
    last = parts ? [parts, cursor] : null;
    if (!parts) {
      left.hidden = right.hidden = true;
      return;
    }
    const [title, sub, ...labels] = parts;
    const rows = labels.map((label, index) => row(label, index, cursor));
    if (landscape.matches) {
      // Half the rows each side, in order: left first.
      const half = Math.ceil(rows.length / 2);
      left.replaceChildren(head(title, true), ...rows.slice(0, half));
      right.replaceChildren(head(sub, false), ...rows.slice(half));
      right.hidden = false;
    } else {
      const both = head(title, true);
      both.append(' ', Object.assign(document.createElement('span'), { textContent: sub }));
      left.replaceChildren(both, ...rows);
      right.hidden = true;
    }
    left.hidden = false;
  }

  // Turning the phone moves the list.
  landscape.addEventListener('change', () => last && show(...last));
  return show;
}

// Melee's roster in its own select-screen order, by character kind (the
// engine's CharacterKind), with a short name that fits a tile and a colour
// for its stripe. The Ice Climbers are left out: Melee Tactics cannot drive
// the pair.
const ROSTER = [
  [22, 'Dr. Mario', '#e8e8f0'], [8, 'Mario', '#e0403a'], [7, 'Luigi', '#3cb850'],
  [5, 'Bowser', '#6a9a30'], [12, 'Peach', '#f08cc0'], [17, 'Yoshi', '#58c858'],
  [1, 'DK', '#9a5a2a'], [0, 'Falcon', '#c03040'], [25, 'Ganon', '#5a3a7a'],
  [20, 'Falco', '#4a70d0'], [2, 'Fox', '#d89a40'], [11, 'Ness', '#e05050'],
  [4, 'Kirby', '#f4a0c0'], [16, 'Samus', '#e07020'], [18, 'Zelda', '#d0a0e0'],
  [19, 'Sheik', '#6070b0'], [6, 'Link', '#40a040'], [21, 'Y. Link', '#70c050'],
  [24, 'Pichu', '#f0e070'], [13, 'Pikachu', '#f0d030'], [15, 'Jiggly', '#f8b0d0'],
  [10, 'Mewtwo', '#a080c0'], [3, 'G&W', '#404040'], [9, 'Marth', '#3a5ac0'],
  [23, 'Roy', '#c04030'],
];

// The fighter menus as a grid of tiles in the space the game leaves (plan_web.c
// mirrors which menu is up): against the CPU, P1's grid on one side and P2's
// on the other with Back and Fight; online, only the player's own, beside the
// usual controls. A tap chooses at once; -1 is Random. icons, when it
// resolves, is each fighter's stock icon from the disc (icons.mjs).
export function createFighterPicker({ stage, pick, press, tapRow, icons }) {
  const sides = ['left', 'right'].map((side) => {
    const el = document.createElement('div');
    el.className = `picker picker-${side}`;
    el.hidden = true;
    stage.append(el);
    return el;
  });
  let art = null;
  let last = null;

  function grid(slot, current, withRandom) {
    const tiles = document.createElement('div');
    tiles.className = 'picker-grid';
    const entries = withRandom ? [[-1, 'Random', '#e0b030'], ...ROSTER] : ROSTER;
    for (const [ckind, name, colour] of entries) {
      const tile = document.createElement('button');
      tile.className = ckind === current ? 'tile chosen' : 'tile';
      tile.style.setProperty('--tint', colour);
      if (ckind >= 0 && art?.get(ckind)) {
        tile.append(Object.assign(document.createElement('img'), { src: art.get(ckind), alt: '', className: 'stock' }));
      }
      tile.append(Object.assign(document.createElement('span'), { textContent: ckind < 0 ? '?' : name }));
      if (ckind < 0) tile.classList.add('tile-random');
      tile.addEventListener('pointerdown', (event) => {
        event.preventDefault();
        if (navigator.vibrate) navigator.vibrate(10);
        for (const t of tiles.children) t.classList.remove('chosen');
        tile.classList.add('chosen');
        ripple(tile, event);
        pick(slot, ckind);
      });
      tiles.append(tile);
    }
    return tiles;
  }

  function head(text, name) {
    const el = document.createElement('div');
    el.className = 'picker-head';
    el.append(Object.assign(document.createElement('b'), { textContent: text }), ` ${name}`);
    return el;
  }

  // A menu row as a button. A click, not a pointerdown: sharing an invite
  // needs the tap to count as the user's gesture.
  function rowButton(label, row) {
    const el = document.createElement('button');
    el.className = /share|ready|create/i.test(label) ? 'picker-action fight' : 'picker-action back';
    // The game's rows are in capitals; buttons read better in sentence case.
    el.textContent = label === label.toUpperCase() ? label.charAt(0) + label.slice(1).toLowerCase() : label;
    el.addEventListener('pointerdown', (event) => ripple(el, event));
    el.addEventListener('click', (event) => {
      event.preventDefault();
      if (navigator.vibrate) navigator.vibrate(12);
      tapRow(row);
    });
    return el;
  }

  function action(label, cls, bits) {
    const el = document.createElement('button');
    el.className = `picker-action ${cls}`;
    el.textContent = label;
    el.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      if (navigator.vibrate) navigator.vibrate(12);
      ripple(el, event);
      press(bits);
    });
    return el;
  }

  const nameOf = (ckind) => (ckind < 0 ? 'Random' : ROSTER.find(([k]) => k === ckind)?.[1] || '');

  // The icons come in after the grid may already be up.
  icons?.then((map) => {
    art = map;
    if (last) show(...last);
  });

  return show;
  function show(mode, p1, p2, rows = []) {
    last = [mode, p1, p2, rows];
    const [left, right] = sides;
    left.hidden = right.hidden = mode === 0;
    stage.classList.toggle('picking', mode !== 0);
    if (mode === 1) {
      left.replaceChildren(head('P1', nameOf(p1)), grid(0, p1, true), action('Back', 'back', PAD.b));
      right.replaceChildren(head('P2', nameOf(p2)), grid(1, p2, true), action('Fight!', 'fight', PAD.start));
    } else if (mode === 2) {
      // The grid covers the D-pad here, so the screen's other rows go on the
      // other side as buttons (menu row i + 1 each), with Back.
      const buttons = document.createElement('div');
      buttons.className = 'picker-rows';
      rows.forEach((label, i) => buttons.append(rowButton(label, i + 1)));
      buttons.append(action('Back', 'back', PAD.b));
      left.replaceChildren(head('You', nameOf(p1)), grid(0, p1, true));
      right.replaceChildren(head('Online', ''), buttons);
    }
  }
}
