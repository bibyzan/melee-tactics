// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen controls for touch screens, in the space around the game: beside
// it in landscape, below it in portrait. They drive GameCube controller 1
// through the engine's virtual pad (browser_touch_pad in main.c), which is all
// Melee Tactics needs: a D-pad for the menus and picks, A, B and Start.

const PAD = { left: 0x0001, right: 0x0002, down: 0x0004, up: 0x0008, a: 0x0100, b: 0x0200, start: 0x1000 };

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
  pad.innerHTML = '<span class="up">▲</span><span class="left">◀</span><span class="right">▶</span><span class="down">▼</span>';
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
    el.textContent = label;
    el.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      el.setPointerCapture(event.pointerId);
      held.set(event.pointerId, PAD[name]);
      el.classList.add('down');
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
