#!/usr/bin/env python3
"""demo_dragon.py - drive magma_game to the End, kill the dragon, reach credits.

  uv run --no-project python scripts/demo_dragon.py --seed 42 --frames out/
  uv run --no-project python scripts/demo_dragon.py --seed 7          # no frames

Exits nonzero unless the dragon's health reaches 0 through combat and the run
reports the `won` terminal, so it doubles as an end-game smoke gate. For the
no-injection proof of the same route, run game/test_route_e2e.sh: it mines,
crafts and inserts twelve eyes of ender instead of the two hooks below.

Injected here: one end_portal block at the player's feet (what the legit route
earns with the eyes) and one diamond sword. Everything else is the shipped
path - the real dimension transition builds the arena and the dragon, crystals
and dragon damage go through gm_dragon_player_attack, and `won` comes from
walking the exit portal the death sequence generates.

Why the script is built by iteration: --script JSONL is static, but the dragon
chases the player (ender_dragon.h targets player.x / y+10 / z), so its flight
path depends on what we emit. Each pass reads --state-out for the measured
track, appends the next batch of poses and swings, and re-runs; the prefix
replays deterministically, so every batch is built on real state.

Two harness edges this walks into, both fatal and neither obvious:
  - the tick loop ends on `won` (and on death), and ANY unconsumed event after
    that is "script: event lies beyond --ticks". The victory probe therefore
    emits exactly one tick.
  - flight poses leave the player ~100 blocks up. Stop emitting them and the
    fall kills the player mid-death-sequence, which ends the loop too. Land on
    the podium's bedrock column (y 63..66) before idling.
"""
import argparse
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAGMA = os.path.join(ROOT, "c", "magma")

EYE = 1.62                 # PSV_EYE_HEIGHT
DRAGON, CRYSTAL = 9, 8     # GM_ENTITY_DRAGON / GM_ENTITY_CRYSTAL
SWORD = 276                # diamond sword
END_PORTAL = 119
BATCH = 30                 # ticks of tracked flight per pass
DEATH_TICKS = 230          # DragonFightManager death sequence is 200


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--frames", metavar="DIR", help="render every tick to DIR as PPM")
    p.add_argument("--width", type=int, default=854)
    p.add_argument("--height", type=int, default=480)
    p.add_argument("--work", default="/tmp/magma_dragon", help="script/state scratch dir")
    return p.parse_args()


class Sim:
    def __init__(self, args):
        self.a = args
        os.makedirs(args.work, exist_ok=True)
        self.script = os.path.join(args.work, "dragon.jsonl")
        self.state = os.path.join(args.work, "dragon_state.jsonl")

    def run(self, events, ticks, frames_dir=None):
        with open(self.script, "w") as f:
            for e in events:
                f.write(json.dumps(e) + "\n")
        cmd = ["./magma_game", "--headless", "--ticks", str(ticks),
               "--seed", str(self.a.seed), "--script", self.script,
               "--state-out", self.state,
               "--width", str(self.a.width), "--height", str(self.a.height)]
        if frames_dir:
            cmd += ["--frames-out", frames_dir]
        p = subprocess.run(cmd, cwd=MAGMA, capture_output=True, text=True)
        if p.returncode != 0:
            sys.exit("magma_game rc=%d\n%s" % (p.returncode, p.stderr[-2000:]))
        return [json.loads(l) for l in open(self.state)]


def boss(row, kind):
    for e in row.get("entities", []):
        if e.get("type") == kind and "health" in e:
            return e
    return None


def act(tick, **kw):
    return dict(tick=tick, type="action", **kw)


def aim(px, py, pz, tx, ty, tz):
    """MC yaw/pitch looking from the eye at (px, py+EYE, pz) toward the target."""
    dx, dy, dz = tx - px, ty - (py + EYE), tz - pz
    return (-math.degrees(math.atan2(dx, dz)),
            -math.degrees(math.atan2(dy, math.hypot(dx, dz))))


def main():
    args = parse_args()
    if not os.path.exists(os.path.join(MAGMA, "magma_game")):
        sys.exit("build first: make -C c/magma game")
    sim = Sim(args)

    rows = sim.run([act(0)], 4)
    sx, sy, sz = rows[-1]["x"], rows[-1]["y"], rows[-1]["z"]
    print("spawn %.2f,%.1f,%.2f" % (sx, sy, sz))

    ev = [{"tick": 0, "type": "set_inventory", "slot": 0, "item": SWORD, "count": 1, "meta": 0},
          {"tick": 0, "type": "set_block", "x": math.floor(sx), "y": int(sy),
           "z": math.floor(sz), "id": END_PORTAL, "meta": 0}]
    ev += [act(t, hotbar=0) for t in range(6)]
    rows = sim.run(ev, 8)
    d = boss(rows[-1], DRAGON)
    if not d:
        sys.exit("no dragon after the portal transition")
    print("End arena at tick %d: dragon hp %.0f, %d crystals"
          % (rows[-1]["tick"], d["health"],
             len([e for e in rows[-1]["entities"] if e["type"] == CRYSTAL])))

    t = 6
    for e in [e for e in rows[-1]["entities"] if e["type"] == CRYSTAL]:
        ev.append({"tick": t, "type": "set_pose", "x": e["x"], "y": e["y"] - EYE,
                   "z": e["z"] - 2.5, "yaw": 0.0, "pitch": 0.0})
        ev.append(act(t, attack=1, hotbar=0))
        t += 1
        for _ in range(11):
            ev.append(act(t, hotbar=0))
            t += 1
    rows = sim.run(ev, t + 2)
    left = len([e for e in rows[-1]["entities"] if e["type"] == CRYSTAL])
    print("crystals left %d (they heal the dragon while alive)" % left)
    if left:
        sys.exit("crystal phase did not clear the arena")

    while True:
        track = {r["tick"]: boss(r, DRAGON) for r in rows}
        last = track[max(k for k in track if track[k])]
        hp = last["health"]
        if hp <= 0:
            break
        for i in range(BATCH):
            tk = t + i
            b = track.get(tk) or last
            tx, ty, tz = b["x"], b["y"] + 2.0, b["z"]
            px, py, pz = tx, ty - EYE, tz - 3.0
            yaw, pitch = aim(px, py, pz, tx, ty, tz)
            ev.append({"tick": tk, "type": "set_pose", "x": px, "y": py, "z": pz,
                       "yaw": yaw, "pitch": pitch})
            ev.append(act(tk, attack=1, hotbar=0))
        t += BATCH
        rows = sim.run(ev, t + 2)
        d = boss(rows[-1], DRAGON)
        now = d["health"] if d else 0.0
        print("t=%d dragon hp %.0f -> %.0f" % (t, hp, now))
        if d is None:
            break
        if now >= hp and t > 3000:
            sys.exit("no damage landing after %d ticks" % t)
    print("dragon down at tick %d" % t)

    ev.append({"tick": t, "type": "set_pose", "x": 0.5, "y": 67.0, "z": 0.5,
               "yaw": 0.0, "pitch": -30.0})
    for _ in range(DEATH_TICKS):
        ev.append({"tick": t, "type": "set_velocity", "x": 0.0, "y": 0.0, "z": 0.0,
                   "on_ground": 1})
        ev.append(act(t))
        t += 1
    rows = sim.run(ev, t + 2)
    if rows[-1]["dead"]:
        sys.exit("player died during the death sequence")

    won = []
    for (px, py, pz) in ((2.5, 63.0, 0.5), (1.5, 63.0, 0.5), (0.5, 63.0, 2.5),
                         (2.5, 63.0, 2.5), (0.5, 64.0, 2.5)):
        trial = ev + [{"tick": t, "type": "set_pose", "x": px, "y": py, "z": pz,
                       "yaw": 0.0, "pitch": 0.0}, act(t)]
        rows = sim.run(trial, t + 1)
        won = [r for r in rows if r.get("terminal") == "won"]
        if won:
            ev, t = trial, t + 1
            break
    if not won:
        sys.exit("dragon is dead but the exit portal was never entered")
    w = won[0]
    print("WON at tick %d: credits=%d xp_level=%d player_hp=%.0f deaths=%d"
          % (w["tick"], w["credits"], w["xp_level"], w["health"], w["deaths"]))

    if args.frames:
        os.makedirs(args.frames, exist_ok=True)
        rows = sim.run(ev, t, frames_dir=args.frames)
        n = len([f for f in os.listdir(args.frames) if f.endswith(".ppm")])
        print("rendered %d frames to %s" % (n, args.frames))
        print("ffmpeg -framerate 20 -i %s/frame_%%06d.ppm "
              "-vf eq=brightness=0.14:gamma=1.45 -c:v libx264 -pix_fmt yuv420p "
              "dragon.mp4   # the End folds its lightmap, so raw frames are dim"
              % args.frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
