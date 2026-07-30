/* dc_defs.h - units, fixed-point helpers, tuning constants.
 *
 * Compiles as C99 and as CUDA C++ from the same text: every function here is
 * DC_HD and integer-only. See SPEC.md for the unit table. */
#ifndef COLDAISLE_DC_DEFS_H
#define COLDAISLE_DC_DEFS_H

#include <stdint.h>

#if defined(__CUDACC__)
#define DC_HD __host__ __device__
#else
#define DC_HD
#endif

typedef int32_t  i32;
typedef int64_t  i64;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- fixed point ------------------------------------------------------- */
#define DC_Q16       65536          /* 1.0 in Q16 */
#define DC_Q16_SHIFT 16

/* ---- tick rate -------------------------------------------------------- */
#define DC_TICK_MS        100
#define DC_TICKS_PER_SEC  (1000 / DC_TICK_MS)

/* ---- thermal ---------------------------------------------------------- */
/* Supply air rise when the hall's heat exceeds its cooling budget, expressed
 * against the budget rather than in absolute watts so the cliff is the same
 * shape for a 100 kW closet and a 30 MW hall: 100% over budget is +40 C at the
 * inlet, 10% over is +4 C. Cooling is a budget, not a CFD model (SPEC.md). */
#define DC_OVER_GAIN_MC 40000
/* Fraction of the way a die moves toward its steady-state target per tick.
 * 0.06 per 100 ms tick is a ~1.6 s time constant, the right order for a
 * package plus heatsink. */
#define DC_THERMAL_TAU_Q16 3932

/* ---- clock control ---------------------------------------------------- */
/* Boosting is gradual, capping is immediate. Both directions gradual leaves a
 * two-tick limit cycle that parks demand 15-30% above the PDU nameplate, which
 * is also wrong physically: real DVFS power capping reacts in milliseconds
 * while turbo ramps in. 0.08 per tick is 1.25 s from floor to full. */
#define DC_CLOCK_RAMP_Q16  5243
#define DC_CLOCK_FLOOR_Q16 6554     /* 0.10: a throttled node still creeps */

/* ---- facility overhead ------------------------------------------------ */
/* Cooling plant power is charged against the cooling capacity you PROVISION,
 * not against the heat you happen to produce: chillers and fans sized for
 * 1 MW cost money whether the hall is busy or not. That is what puts a real
 * optimum in the cooling sweep - too little capacity throttles the nodes, too
 * much capacity wrecks PUE. COP 4.0. */
#define DC_COP_Q16 262144

/* ---- breaker ---------------------------------------------------------- */
#define DC_BREAKER_TRIP_Q16 78643   /* 1.20 x nameplate */
#define DC_BREAKER_TICKS    30      /* held for 3 s */

/* ---- limiter classes (SPEC.md) ---------------------------------------- */
enum {
    DC_LIM_IDLE = 0,
    DC_LIM_COMPUTE = 1,
    DC_LIM_POWER = 2,
    DC_LIM_THERMAL = 3,
    DC_LIM_NETWORK = 4,
    DC_LIM_DARK = 5,
    DC_LIM_COUNT = 6
};

/* ---- integer helpers -------------------------------------------------- */
DC_HD static inline i32 dc_min32(i32 a, i32 b) { return a < b ? a : b; }
DC_HD static inline i32 dc_max32(i32 a, i32 b) { return a > b ? a : b; }
DC_HD static inline i64 dc_min64(i64 a, i64 b) { return a < b ? a : b; }
DC_HD static inline i64 dc_max64(i64 a, i64 b) { return a > b ? a : b; }
DC_HD static inline i32 dc_clamp32(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* a * q >> 16 with a 64-bit intermediate, truncating toward zero for a >= 0.
 * Every caller passes a >= 0 (power, work, bytes), so truncation is floor. */
DC_HD static inline i64 dc_q16_mul(i64 a, i32 q) {
    return (a * (i64)q) >> DC_Q16_SHIFT;
}

/* Demand-proportional share of a capacity, in Q16, saturating at 1.0.
 * cap and demand are non-negative; demand == 0 means nobody is asking, which
 * is full share by convention so the caller needs no special case. */
DC_HD static inline i32 dc_share_q16(i64 cap, i64 demand) {
    if (demand <= 0) return DC_Q16;
    if (cap >= demand) return DC_Q16;
    if (cap <= 0) return 0;
    return (i32)((cap * (i64)DC_Q16) / demand);
}

/* Exact split of `total` across `k` slots with no shared state: slot j takes
 * (total + k-1-j)/k. Sum over j is exactly total for any total >= 0, k > 0,
 * and the value depends only on (total, k, j), never on evaluation order. */
DC_HD static inline i64 dc_split_share(i64 total, i32 k, i32 j) {
    if (k <= 0 || total <= 0) return 0;
    return (total + (i64)(k - 1 - j)) / (i64)k;
}

/* Move v toward target by at most step. */
DC_HD static inline i32 dc_ramp(i32 v, i32 target, i32 step) {
    if (target > v) return dc_min32(target, v + step);
    if (target < v) return dc_max32(target, v - step);
    return v;
}

#endif /* COLDAISLE_DC_DEFS_H */
