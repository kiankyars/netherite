# coldaisle

A datacenter simulator built like Factorio. Requests are the items, servers are
the assemblers, network links are the belts, the power tree is the electric
network, and heat is the pollution that comes back for you. The point is not a
pretty dashboard: it is that the loops close, so you cannot fix one subsystem
without moving another, and the simulator will tell you which one is actually
holding you back.

```
$ ./coldaisle run blueprints/hall10k.dcb --ticks 600

coldaisle: 10000 nodes, 700 racks, 1 room, 3 stages, 1 batch, 600 ticks (100 ms each)

batch            offer/s    done/s  drop%  util%    IT kW    PUE supply pk  die pk   req/kW  limiter mix (idle/comp/pwr/therm/net/dark)
0                  18000     15287    0.0   52.4  18109.5   1.37     24.0C   61.6C      0.6  85/15/ 0/ 0/ 0/ 0

best batch 0 is compute-limited while 85% of node-ticks sat idle, so offered load is the ceiling
```

## Why it is interesting

Every node reports, every tick, why it did not do more work: `idle`, `compute`,
`power`, `thermal`, `network`, `dark`. That histogram is the whole product. It is
the Factorio bottleneck mod for a datacenter, and it answers "what do I buy
next" without guessing.

The feedback is what makes the answer non-obvious:

- Throttling for heat lowers power, which lowers heat, which lets the clock back
  up. A hall with too little cooling does not melt; it finds a hotter, slower
  equilibrium. `blueprints/chiller_fail.dcb` loses most of its cooling at
  t=30 s: supply air jumps 22 C -> 74.5 C, dies cross 80 C within two ticks, the
  top of every rack throttles, IT power *falls* 1734 kW -> 1435 kW, and that drop
  pulls supply air back down to 58.5 C where it settles. The backlog grows to
  446k requests over the next minute and drains after recovery, when the hall
  briefly runs *harder* than nominal (1920 kW) to catch up.
- Backpressure idles compute. A thin spine leaves responses stuck in a node's
  output backlog; the node stalls, its queue backs up into the stage in front of
  it, and the racks cool down while serving less.
- Cooling capacity is not free. Plant power is charged against the capacity you
  provision, not the heat you make, so over-cooling shows up as PUE. There is a
  real optimum and `req/kW` finds it.

```
$ ./coldaisle run blueprints/cooling_sweep.dcb --ticks 600

cool_cap_kw      offer/s    done/s  drop%  util%    IT kW    PUE supply pk  die pk   req/kW  limiter mix
9000               18000     14400    0.0   49.4  15305.0   1.15     85.4C   83.7C      0.8  80/ 5/ 0/15/ 0/ 0
12000              18000     15140    0.0   51.9  17628.7   1.17     60.1C   82.0C      0.7  84/10/ 0/ 6/ 0/ 0
15000              18000     15287    0.0   52.4  18109.5   1.21     44.8C   76.3C      0.7  85/15/ 0/ 0/ 0/ 0
18000              18000     15287    0.0   52.4  18109.5   1.25     34.7C   69.0C      0.7  85/15/ 0/ 0/ 0/ 0
21000              18000     15287    0.0   52.4  18109.5   1.29     27.5C   63.8C      0.7  85/15/ 0/ 0/ 0/ 0
27000              18000     15287    0.0   52.4  18109.5   1.37     24.0C   61.6C      0.6  85/15/ 0/ 0/ 0/ 0
```

Six datacenters, one run. Below 15 MW the hall throttles (see the thermal column of the limiter mix) and
throughput falls; above it the extra chillers are pure PUE. `util%` is against
nameplate work, so 52% here means the CPU stages are half idle while the GPU
stage is the ceiling. That is the sweep the GPU path exists for.

## Build and run

```bash
make                 # ./coldaisle          (CPU, no dependencies beyond libc)
make test            # every gate below
make cuda            # ./coldaisle_cuda     (needs nvcc; NVARCH=sm_86 by default)
```

```bash
./coldaisle run   blueprints/chiller_fail.dcb --ticks 1500 --heatmap
./coldaisle run   blueprints/cooling_sweep.dcb --ticks 600 --csv out.csv
./coldaisle run   blueprints/chiller_fail.dcb --ticks 1500 --frames frames/ --frame-every 5
./coldaisle bench blueprints/hall10k.dcb --ticks 2000
./coldaisle_cuda  blueprints/cooling_sweep.dcb --ticks 6000 --bench
```

`--frames` writes a rack elevation view per tick group: one column per rack, one
cell per node from the bottom slot up, coloured by die temperature. Turn it into
a video with

```bash
ffmpeg -framerate 20 -i frames/frame_%06d.ppm -c:v libx264 -pix_fmt yuv420p out.mp4
```

`--heatmap` is the same thing in a terminal, one line per rack, one character per
node: `.` idle, `C` compute-bound, `P` power-capped, `T` thermal-throttled,
`N` stalled on the network, `X` breaker open.

## Blueprints

A blueprint is a flat text file, one declaration per line, `#` for comments.
Every value may be decimal and is converted to integers at parse time; a
repeated key is an error rather than a silent override.

```
room      NAME  power_cap_kw= cool_cap_kw= supply_c= spine_gbps=
rackgroup NAME  room= racks= pdu_cap_kw= uplink_gbps= recirc= airflow_kw_per_c= [vgrad_c=]
nodetype  NAME  cap_wu= idle_w= dyn_w= rja_c_per_kw= t_throttle_c= t_max_c=
                [queue_max=] [txbl_max_kib=]
stage     NAME  wu= bytes_kib= next=NAME|egress [slo_ms=]
fill      rackgroup= type= stage= per_rack=
load      stage= rps=
fault     at_s= target=NAME KEY=VALUE
sweep     KEY target=NAME v1 v2 ...
```

A pipeline is stages chained by `next`, which is Factorio's recipe graph: a stage
costs `wu` of work per request and hands `bytes_kib` to the next one. Fan-in
works (several stages may name the same `next`). `fault` schedules a mid-run
capacity change, which is how you look at degradation instead of only steady
state. `sweep` gives one axis of batch width: every batch is the same datacenter
with one capacity changed.

Shipped blueprints: `smoke` (32 nodes, nothing binding), `hall10k` (10,000 nodes,
three-stage inference pipeline), `cooling_sweep` (hall10k across six cooling
capacities), `oversubscribed` (hall10k with 2 MiB responses and a 200 gbps
spine, which costs 28% of throughput and 85% of the latency budget), and
`chiller_fail` (a cooling failure and recovery).

## Why CUDA, and why integers

A tick is a handful of integer operations per node, so the model is bandwidth
bound and embarrassingly parallel: eleven passes, each a per-element map or a
reduction over a contiguous range. `cuda/dc_cuda.cu` is not a second model - every
kernel body calls the same `core/dc_tick.h` functions the CPU driver loops over.

Batching is the real reason for the GPU. B datacenters share a topology and
differ in the capacities you would actually shop for, so one launch per pass
advances the whole design space. A 64-point sweep of a 100k-node hall over an
hour of simulated time is 2.3e11 node-ticks, which is hours on one CPU core and
the natural shape of a GPU workload.

Everything in the tick is an integer - milliwatts, millicelsius, KiB, Q16 fixed
point - which buys three things:

1. CPU and GPU agree **exactly**, so `tests/test_parity` compares full state
   hashes and every metric as integers instead of within a tolerance.
2. Integer addition is associative, so the two cross-element accumulations can
   use atomics and still be reproducible: thread order cannot change a sum.
3. A run is reproducible across machines and compilers, which is what makes a
   sweep comparable at all.

Measured on one core of this container's CPU (`./coldaisle bench`):

| Blueprint | Scale | Rate |
|-----------|-------|------|
| hall10k | 10k nodes, 1 batch | 30.3 M node-ticks/s |
| cooling_sweep | 10k nodes, 6 batches | 28.9 M node-ticks/s |
| hall10k scaled to 100k nodes | 100k nodes, 1 batch | 23.7 M node-ticks/s |

That is roughly 300x realtime for a 10k-node hall on one core. The GPU numbers
are deliberately absent: the container this was written in has no CUDA device,
so `cuda/dc_cuda.cu` is compile-verified with nvcc 12.0 for sm_86 and
`tests/test_parity` skips itself rather than reporting a number nobody measured.
Run `make cuda && tests/test_parity` on a machine with a card to close that gap.

## Gates

`make test` runs, and refuses to pass on:

- `tests/no_float.sh` - preprocesses `core/` and fails on any float keyword or
  literal, because the determinism contract starts there.
- `test_units` - the exact-split, share, and Q16 primitives across their ranges,
  plus unit conversion and blueprint rejection with line numbers.
- `test_model` - each feedback loop provoked on purpose: power capping without
  tripping, a breaker that does trip, a thermal cascade that settles rather than
  runs away, network backpressure that backs queues up, monotonicity in cooling,
  a two-stage pipeline whose slow stage sets the rate, and idle reported as idle.
  Request conservation is audited after every case.
- `test_determinism` - two runs byte-identical, a split run identical to a whole
  one, batches independent, and a negative control so the hash cannot be blind.
- `test_parity` - CPU against GPU, exactly, when a device exists.

Every run also audits conservation: everything offered at a door either left
through egress, was dropped, is still queued, or is part-way out of a node.

## Reading order

`SPEC.md` is the model and the determinism contract, and it names what is
deliberately not modelled. Then `core/dc_tick.h` for the eleven passes,
`host/dc_run.c` for the readable definition of a tick, and `cuda/dc_cuda.cu` for
the same thing with launch bounds.
