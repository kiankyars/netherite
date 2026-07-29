# Cloud container bring-up

Agent entry is root `AGENTS.md`. This file is the anvil-free path: a fresh
headless Linux container with root apt, no Minecraft launcher, no GPU, no
display, no `java/oracle-src`. Verified 2026-07-29 on Ubuntu 24.04 x86_64,
4 cores, 15 GB RAM, no NVIDIA device.

`scripts/setup_and_verify.sh` is still the right entry on a box you own,
because it starts at `bootstrap_oracle.sh` (ForgeGradle decompile) and
`--skip-bootstrap` refuses to run without `java/oracle-src`. Neither the C
build nor the `--quick` pyramid needs that tree, so on a container run the
steps below instead.

## What runs here

| Step | Status |
|------|--------|
| `bash scripts/bootstrap_assets.sh` (13 texture headers) | works |
| `make -C c/magma game` + windowed and headless play | works |
| `bash netherite_sweep.sh --quick` | green, 13/13, no SKIPs |
| RL snapshots (`rl/blaze/make_snapshots.py`, t0 + curriculum) | works |
| `make -C c/magma test-light-brightness` (JDK 8 golden regen) | PASS, 0 LSB |
| `bash netherite_sweep.sh --full` CUDA gates | needs a GPU |
| tape replay / `mc_capture` pixel gates | SKIP, captures are not distributed |
| `java/` Forge client, `scripts/bootstrap_oracle.sh` | not exercised here |

## Packages

```bash
apt-get install -y libsdl2-dev ffmpeg xvfb x11vnc openbox mesa-utils \
                   libgl1-mesa-dri imagemagick
apt-get install -y openjdk-8-jdk     # only for the two JDK 8 users below
```

JDK 8 is not needed to build or run the game, nor for `--quick`. It is needed
by `make -C c/magma test-light-brightness` (recompiles `tests/Golden.java` and
diffs the C light curve against it) and by `scripts/bootstrap_oracle.sh`.

## Bring-up

```bash
bash scripts/fetch_mc_jar.sh          # your jar, Mojang endpoints, sha1 checked
bash scripts/bootstrap_assets.sh      # 13 generated headers in c/magma/assets
make -C c/magma game
cd c/magma && make blaze_so blaze_verify
T0=1 uv run --no-project --with numpy python rl/blaze/make_snapshots.py
uv run --no-project --with numpy,torch,matplotlib python rl/blaze/make_snapshots.py
cd ../.. && bash netherite_sweep.sh --quick
```

Two gotchas that cost a sweep run each:

- `make verify-harsh` runs `c/magma/tests/check_jar_models.py`, whose jar
  candidate list is fixed and starts at `~/.gradle/caches` - it does not read
  `MC_JAR`. `scripts/fetch_mc_jar.sh` lands the jar there for that reason.
- `blaze-cpu-gate` needs the `s*_d*.bsnp` curriculum snapshots, not just the
  `T0=1` ones. With only t0 snapshots present it reports
  `FAIL: 0/0 streams zero-diff`, which reads like a gate failure and is a
  missing input.

## Headless play (no display)

```bash
cd c/magma
./magma_game --headless --ticks 600 --seed 42 --script play.jsonl \
             --frames-out frames/ --state-out state.jsonl --width 854 --height 480
ffmpeg -framerate 20 -i frames/frame_%06d.ppm -c:v libx264 -pix_fmt yuv420p out.mp4
```

`--script` is tick-sorted JSONL. An `action` event applies to **exactly one
tick** (`gm_script_run` zeroes `GmAction` at the top of every iteration), so
continuous movement needs one event per tick, not one event per phase:

```json
{"tick": 0, "type": "action", "forward": 1.0, "sprint": 1}
{"tick": 1, "type": "action", "forward": 1.0, "sprint": 1}
```

Walking into a hillside stalls at 0 blocks travelled with no error, and mining
resets its progress whenever the look target changes, so check
`--state-out` (`x/y/z`, `look`, `inventory`) before blaming the renderer.

## Windowed play (Xvfb, no monitor)

```bash
Xvfb :1 -screen 0 1280x720x24 +extension GLX +render -noreset &
x11vnc -display :1 -rfbauth ~/.vnc/passwd -localhost -forever -rfbport 5900 &
DISPLAY=:1 ./magma_game --seed 42 --render window --width 1280 --height 720
```

`java/start_vnc_client.sh` is the same idea for the Java client and also brings
up openbox; magma_game needs no window manager and no GL (SDL2 software
framebuffer, llvmpipe is only there for the Java path).

Do not chain a kill with a launch (`AGENTS.md` gotcha). `pkill -f
'[m]agma_game'` still matches the surrounding shell when that shell's own
command line contains `./magma_game`, which kills the launch you just issued;
run the kill in its own command.

For a single frame through the interactive loop without a viewer, use the
developer capture controls instead of screenshotting X:

```bash
DISPLAY=:1 ./magma_game --seed 42 --render window --frames 40 --ppm frame.ppm
```
