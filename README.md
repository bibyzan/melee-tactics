# Melee Tactics

Melee Tactics is a tactics game played inside a real match of Super Smash Bros. Melee. You pick two fighters. At each break the match freezes, and you choose one exchange from the moves and combos that fit where the fighters actually are. They play that exchange. The fight continues from that moment until someone is knocked off the stage.

Between your picks, Melee's own level 9 CPU moves both fighters: spacing, chasing, recovering, the ledge. It never attacks on its own. Every attack is one you picked, and it still has to connect under Melee's own hitboxes, physics, and timing.

## How this uses the Melee port

This repository is a fork of [melee-pc](https://github.com/999sian/melee-pc). melee-pc is a native port of Melee (NTSC-U 1.02), built from [doldecomp/melee](https://github.com/doldecomp/melee) on [aurora](https://github.com/encounter/aurora) and SDL3. A tactics match is a normal VS match on that port: the same fighters, stage, camera, HUD, hitboxes, and physics.

The mode is `GM_TACTICS`, in `src/melee/tactics`. It is wired in at three places:

- **VS Mode > TACTICS** opens the draft, then one stock on Final Destination with no items and no clock. The draft picks the fighters and whether P2 is a CPU. `play-tactics.cmd` sets `MELEE_BOOT_SCENE=tactics` so the game opens that draft directly.
- **The CPU think is shared.** While a pick is queued, `tactics_Think` writes the stick and buttons for it. Otherwise Melee's CPU think (`ftCo_800B3900`) runs, and `tactics_FilterAi` strips A, Z, the C-stick, and B (except offstage), so the CPU moves but never attacks. Melee decides whether each input comes out.
- **A break freezes the simulation.** When both queues are spent, neither fighter is attacking, grabbed, in hitstun, or still recovering to the stage, and the two are about to meet (or a chaser is about to reach a launched fighter), the frame loop applies the same processor mask Melee uses for pause. Positions, damage, and velocity stay put while the next queue is chosen.

The launcher, renderer, audio, and controller mapping are the port's. Models, textures, audio, and fonts are read at runtime from a disc image you supply. Nothing from the Melee disc is in this repository.

Port setup, other platforms, and port bugs belong to upstream melee-pc. Its [project site](https://999sian.github.io/melee-pc/) covers the supported disc, Windows first run, and known issues. `docs/building.md`, `docs/architecture.md`, and `docs/porting-notes.md` in this tree describe the port.

To take a newer port:

```
git remote add upstream https://github.com/999sian/melee-pc.git
git fetch upstream
git merge upstream/master
```

## Play

From a Windows build:

```
play-tactics.cmd "path\to\your\disc.iso"
```

With no path, the launcher asks for the disc, then opens the draft. You can also reach it from VS Mode > TACTICS.

On the keyboard, arrows or WASD move the cursor, A/D or left/right change the selected row, Enter starts or confirms, and Z backs out of the draft. T/G/F/H are the D-pad and do the same job. A gamepad uses the stick or D-pad, A or Start to confirm, and B to leave the draft.

The draft is the two fighters and whether P2 plays itself. P2 is a CPU by default, so one controller is enough. Enter starts the stock. Melee's CPU moves both fighters in. Just before they meet, the match freezes and lists every exchange that fits the spacing, the height, and the damage. A row is one move or a two-move combo, such as down tilt into up air. Up and down move through that list. A or Start plays the highlighted row against the CPU's row. The CPU picks only when the exchange starts, so you never see its choice. With two humans, P1 locks in first, then P2 picks from their own list. Neither side's pick is shown.

A launch opens a second kind of break. When a fighter is knocked into tumble, Melee's CPU chases with the other one. Just before the chaser reaches them, the match freezes. The chaser picks a move or a combo (up air, up air into up air, neutral air into forward air, up smash to cover the landing) or waits for the read. The launched fighter picks a reaction only once it can act, so the reaction comes out right away: air dodge away, jump away, drift away, or an aerial. While it is still in hitstun, only the chaser picks. The instant hitstun ends in the air, the match freezes again for the reaction. If the follow-up launches again, the chase starts over, so juggles become a series of reads. Launches offstage and trades keep playing.

Every pick waits for its moment. Melee's own CPU attack selector decides when: each character's real frames to the hitbox and hitbox box, from `PlCo.dat`, with both fighters' motion predicted to that frame. That covers normals, aerials, the grab, and the specials a character's CPU tables list (0x11 neutral B, 0x1B/0x1C/0x1D side B, 0x1F up B, 0x26 down B). A move with no entry uses a generic reach. `MELEE_TACTICS_AI_DUMP=1` logs the tables for the fighters in a match, and `=all` logs every character's.

A break only waits for the players who have a choice. If only the CPU has one, it picks and play goes on without a pause. If both fighters stand idle with nothing queued for half a second, that is a break too, so nobody stands around. Getting hit, a knockdown, the ledge, or being knocked offstage throws out the rest of your queue, and Melee's CPU takes over: it techs, gets up, and recovers. The result returns to the draft.

The menus offer normals, aerials, grabs with a throw, and each character's specials (Zelda and Sheik cannot transform). Ice Climbers are left out until the partner can share the controller policy.

## Build on Windows

Requires MSYS2 UCRT64 GCC, CMake, Ninja, and Python. Put `C:/msys64/ucrt64/bin` on `PATH`, then from this repository:

```
cmake -B build/win -G Ninja --toolchain ../../cmake/msys2-ucrt64.cmake
cmake --build build/win -j 8
```

The compiler has to be GCC. The decompiled game code uses a big-endian layout attribute that other compilers ignore. No disc is required to compile. A disc is required to run.

Other platforms use the port's build instructions in [docs/building.md](docs/building.md).

## License

Two bodies of code share this tree, spelled out in [licenses/LICENSE.md](licenses/LICENSE.md):

- The decompiled game code in `src/melee` and `src/sysdolphin` is **not licensed**. It remains the property of its copyright holders.
- The port code in `src/pc`, `tools`, `platforms`, `cmake`, and `.github` is **GPL-3.0-or-later** ([licenses/COPYING](licenses/COPYING)).

The Tactics mode in `src/melee/tactics` is our addition on that fork. Because the decompiled game code cannot be relicensed, the repository as a whole is not distributable under the GPL. No game assets are in this repository. The supported disc is Melee USA revision 2 (NTSC-U 1.02, GALE01).
