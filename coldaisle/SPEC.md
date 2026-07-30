# coldaisle: model and determinism contract

A datacenter is a factory. Requests are the items, servers are the assemblers,
network links are the belts, the power tree is the electric network, and heat is
the pollution that comes back to bite you. Factorio's three teaching pressures
are all here, unchanged in spirit:

| Factorio | coldaisle |
|----------|-----------|
| assembler with a recipe | node running one pipeline stage (`wu` of work per request) |
| belt backs up, machine stops | link saturates, `tx_backlog` fills, node stalls |
| not enough power, everything runs slow | PDU/UPS demand over cap, `clock` scales down |
| pollution accumulates, evolution bites | heat accumulates, supply air rises, nodes thermal-throttle |
| bottleneck: which machine is starved | per-node limiter histogram every tick |
| blueprint | `.dcb` blueprint file |

The interesting part is the same as in Factorio: the loops close. Throttling for
heat lowers power, which lowers heat, which raises the clock again. Backpressure
from a saturated spine idles compute, which cools the hall, which lets the
remaining nodes boost. You do not get to reason about one subsystem alone.

## Units: integers only

Every stored quantity is an integer. No floats anywhere in the tick, which buys
three things: the CPU and CUDA paths are bit-identical by construction rather
than by tolerance, reductions are associative so any thread order gives the same
sum, and a run is reproducible across machines and compilers.

| Quantity | Unit | Type |
|----------|------|------|
| power, heat | milliwatt (mW) | `i32` per node, `i64` in sums |
| temperature | millicelsius (mC) | `i32` |
| work | work unit (WU) | `i32` |
| traffic | KiB per tick | `i32` per node, `i64` in sums |
| scale factors (clock, link share) | Q16 fixed point, `65536 == 1.0` | `i32` |
| requests | count | `i32` per node, `i64` in totals |
| energy | milliwatt-ticks | `i64` |

One tick is `DC_TICK_MS` (100 ms, so 10 ticks per second). Compute and queueing
resolve inside a tick; thermal and power feedback take effect on the next one.

## Tick order

Eleven passes. Each is either a pure per-node map or a segmented integer
reduction over a contiguous range, which is exactly what makes the GPU version a
straight port rather than a rewrite. Shared-resource feedback (power scale, link
share, supply air) is computed from this tick's sums and applied on the next
tick, so no pass ever depends on its own output.

1. **clock** - `clock = min(power_scale, thermal_scale)`, both from last tick.
   A cut applies immediately and a boost ramps at `DC_CLOCK_RAMP_Q16` per tick.
   Ramping both ways leaves a two-tick limit cycle that parks PDU demand 15-30%
   over nameplate; capping fast and boosting slow is also what real DVFS does.
2. **arrivals** - ingress stages only. Node-local: `arr_acc += rate_milli;
   arrivals = arr_acc / 1000; arr_acc %= 1000`. No RNG, no atomics, exact.
3. **service** - `budget = wu_acc + cap_wu * clock`;
   `served = min(queue, budget / wu_per_req)`; leftover budget carries, capped at
   `wu_per_req - 1`, and only while the queue is non-empty (an idle node cannot
   bank capacity). A stalled node (output backlog full) serves nothing.
4. **node power** - `p = idle + dyn * util * clock^2`. The square is what makes
   throttling actually save power, and it is why brownouts recover.
5. **reduce** - per rack and per room: power, heat, egress bytes.
6. **fabric** - `leaf_share = min(1, uplink / rack_tx)`,
   `spine_share = min(1, spine_cap / room_tx)`, and a node sends
   `owed * min(shares)` bytes of what it owes. Bytes stream: a payload bigger
   than one tick's share makes partial progress and the request completes when
   its last byte leaves, so `ceil(owed / bytes_per_req)` requests are in flight.
   A node whose backlog passes `tx_backlog_max` stalls next tick. Fair share is
   demand-proportional, the standard single-pass approximation to max-min
   fairness, exact when every talker is identical.
7. **reduce** - per service: delivered requests.
8. **handoff** - stage `k+1` nodes pull their share of stage `k`'s delivered
   total: node `j` of `K` takes `(total + K-1-j) / K`. The shares sum to the
   total exactly, need no shared state, and do not depend on evaluation order.
   Overflow past `queue_max` is dropped and counted.
9. **thermal** - `room_supply = supply_base + DC_OVER_GAIN_MC * max(0, heat - cool_cap) / cool_cap`;
   `rack_inlet = room_supply + rack_heat * recirc / airflow`;
   `node_target = rack_inlet + height_bias + node_heat * r_ja`, where the bias
   spreads a rackgroup's `vgrad_c` linearly from bottom slot to top because hot
   air rises, so the top of a rack throttles first; the node moves a
   `DC_THERMAL_TAU_Q16` fraction of the way to its target each tick.
10. **power scale** - per PDU and per room: `min(1, cap / demand)`, applied next
    tick. Sustained demand over `DC_BREAKER_TRIP_Q16` for `DC_BREAKER_TICKS`
    trips the PDU: every node under it is dark until reset.
11. **metrics** - completions at egress, drops, energy, per-node limiter class,
    and a Little's-law latency estimate (`queue / served`), which is the
    approximation that lets 100k nodes run without per-request state.

## Limiter classes

Every node reports why it did not do more work this tick. This is the whole
point of the simulator, so it is a first-class output, not a debug print.

| Class | Meaning |
|-------|---------|
| `IDLE` | queue empty at the end of the tick: upstream or ingress is the limit, and the node would have taken more |
| `COMPUTE` | ended the tick with work still queued at full clock: add nodes |
| `POWER` | clock held down by PDU or UPS cap |
| `THERMAL` | clock held down by die temperature |
| `NETWORK` | stalled on output backlog |
| `DARK` | breaker tripped |

## Determinism contract

1. No floating point in `core/`. The tests fail the build if a float sneaks in
   (`test_units` greps the preprocessed core).
2. Reductions are integer sums over contiguous, sorted ranges; any order gives
   the same result.
3. No time, no RNG, no pointer values enter the state.
4. Same blueprint plus same tick count gives the same metrics on CPU and GPU,
   compared as exact integers by `tests/test_parity`.

## What this deliberately does not model

Named so nobody mistakes an approximation for a claim: per-request tracing (the
latency figure is Little's law on queue depth), TCP dynamics and incast, cache
and memory hierarchy, storage IO, VM placement and live migration, failure
domains beyond the breaker, humidity and airside economization, and per-phase
electrical imbalance. Cooling is a single heat-removal budget per room with a
linear penalty above capacity, not a CFD model.
