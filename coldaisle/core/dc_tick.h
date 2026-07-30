/* dc_tick.h - one tick, eleven passes, integer only.
 *
 * Every function takes (state, batch, element) and touches only that element's
 * outputs, so the CPU driver's `for` loop and the CUDA driver's thread index are
 * the same computation. Nothing here allocates, reads a clock, or branches on a
 * pointer value. Pass order and the one-tick feedback lag are specified in
 * SPEC.md; the drivers must not reorder them. */
#ifndef COLDAISLE_DC_TICK_H
#define COLDAISLE_DC_TICK_H

#include "dc_state.h"

/* ---- 1. clock: apply last tick's power and thermal verdicts ------------ */
DC_HD static inline i32 dc_thermal_scale_q16(const DcSim *s, i32 i, i32 temp_mc) {
    i32 thr = s->node_t_thr_mc[i], tmax = s->node_t_max_mc[i];
    if (temp_mc <= thr) return DC_Q16;
    if (temp_mc >= tmax || tmax <= thr) return DC_CLOCK_FLOOR_Q16;
    /* linear from 1.0 at the throttle point to the floor at t_max */
    i64 span = (i64)(tmax - thr);
    i64 into = (i64)(temp_mc - thr);
    i64 drop = ((i64)(DC_Q16 - DC_CLOCK_FLOOR_Q16) * into) / span;
    return (i32)((i64)DC_Q16 - drop);
}

DC_HD static inline void dc_pass_clock(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), r = s->node_rack[i];
    i32 ri = dc_ri(s, b, r), mi = dc_mi(s, b, s->rack_room[r]);
    if (s->rack_dark[ri]) { s->clock_q16[n] = 0; return; }
    i32 pwr = dc_min32(s->rack_scale_q16[ri], s->room_scale_q16[mi]);
    i32 thm = dc_thermal_scale_q16(s, i, s->temp_mc[n]);
    i32 target = dc_min32(pwr, thm);
    if (target < DC_CLOCK_FLOOR_Q16) target = DC_CLOCK_FLOOR_Q16;
    i32 clk = s->clock_q16[n];
    /* down at once, up on the ramp: see DC_CLOCK_RAMP_Q16 */
    s->clock_q16[n] = target < clk ? target : dc_ramp(clk, target, DC_CLOCK_RAMP_Q16);
}

/* ---- 2. arrivals: ingress load, node-local and exact ------------------- */
typedef struct DcArrive { i32 offered, dropped; } DcArrive;

DC_HD static inline DcArrive dc_pass_arrive(DcSim *s, i32 b, i32 i) {
    DcArrive out; out.offered = 0; out.dropped = 0;
    i32 v = s->node_svc[i];
    i32 rate = s->svc_rate_milli[dc_si(s, b, v)];
    if (rate <= 0) return out;
    i32 n = dc_ni(s, b, i);
    /* rate is requests/tick*1000 for the whole stage; each node takes its
     * exact share, then integrates the fractional part locally. */
    i64 mine = dc_split_share((i64)rate, s->svc_nnode[v], s->node_slot[i]);
    i64 acc = (i64)s->arr_acc[n] + mine;
    i32 got = (i32)(acc / 1000);
    s->arr_acc[n] = (i32)(acc % 1000);
    i32 room = s->node_queue_max[i] - s->queue_req[n];
    if (room < 0) room = 0;
    i32 admit = dc_min32(got, room);
    s->queue_req[n] += admit;
    out.offered = got;
    out.dropped = got - admit;        /* refused at the door */
    return out;
}

/* ---- 3. service: drain the queue under the current clock --------------- */
DC_HD static inline void dc_pass_service(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), v = s->node_svc[i];
    i32 wu_req = s->svc_wu_per_req[v];
    i32 q = s->queue_req[n];
    s->served_req[n] = 0;
    s->tx_kib[n] = 0;
    if (s->rack_dark[dc_ri(s, b, s->node_rack[i])]) {
        s->limiter[n] = DC_LIM_DARK;
        return;
    }
    if (s->txbl_kib[n] >= s->node_txbl_max[i]) {
        s->limiter[n] = DC_LIM_NETWORK;   /* output belt is full */
        return;
    }
    if (q <= 0) {
        s->wu_acc[n] = 0;                 /* idle nodes cannot bank capacity */
        s->limiter[n] = DC_LIM_IDLE;
        return;
    }
    i64 budget = (i64)s->wu_acc[n] + dc_q16_mul((i64)s->node_cap_wu[i], s->clock_q16[n]);
    i32 served = wu_req > 0 ? (i32)dc_min64((i64)q, budget / wu_req) : q;
    s->served_req[n] = served;
    s->queue_req[n] = q - served;
    i64 left = budget - (i64)served * wu_req;
    if (left < 0) left = 0;
    if (wu_req > 0 && left > wu_req - 1) left = wu_req - 1;
    s->wu_acc[n] = (i32)left;
    s->tx_kib[n] = (i32)dc_min64((i64)served * s->svc_bytes_kib[v], (i64)INT32_MAX);

    /* Why was that not more work? Only a node that ENDS the tick with work
     * still queued was limited by something on this node; one that drained its
     * queue could have done more if asked, which is demand, not compute. */
    if (s->queue_req[n] > 0) {
        i32 ri = dc_ri(s, b, s->node_rack[i]);
        i32 mi = dc_mi(s, b, s->rack_room[s->node_rack[i]]);
        i32 pwr = dc_min32(s->rack_scale_q16[ri], s->room_scale_q16[mi]);
        i32 thm = dc_thermal_scale_q16(s, i, s->temp_mc[n]);
        if (thm < DC_Q16 && thm <= pwr)      s->limiter[n] = DC_LIM_THERMAL;
        else if (pwr < DC_Q16)               s->limiter[n] = DC_LIM_POWER;
        else                                 s->limiter[n] = DC_LIM_COMPUTE;
    } else {
        s->limiter[n] = DC_LIM_IDLE;
    }
}

/* ---- 4. node power: idle + dynamic, dynamic scaled by clock^2 ---------- */
DC_HD static inline void dc_pass_power(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i);
    if (s->rack_dark[dc_ri(s, b, s->node_rack[i])]) { s->power_mw[n] = 0; return; }
    i32 clk = s->clock_q16[n];
    i32 cap = s->node_cap_wu[i];
    i64 done_wu = (i64)s->served_req[n] * s->svc_wu_per_req[s->node_svc[i]];
    i32 util = cap > 0 ? dc_share_q16(done_wu, (i64)cap) : 0;  /* served / nameplate */
    i64 dyn = dc_q16_mul((i64)s->node_dyn_mw[i], util);
    dyn = dc_q16_mul(dc_q16_mul(dyn, clk), clk);               /* V^2 f, roughly */
    s->power_mw[n] = (i32)dc_min64((i64)s->node_idle_mw[i] + dyn, (i64)INT32_MAX);
}

/* ---- 5. reductions: rack then room ------------------------------------ */
DC_HD static inline void dc_reduce_rack(DcSim *s, i32 b, i32 r) {
    i64 p = 0, tx = 0;
    for (i32 i = s->rack_node_begin[r]; i < s->rack_node_end[r]; ++i) {
        i32 n = dc_ni(s, b, i);
        p += s->power_mw[n];
        tx += (i64)s->txbl_kib[n] + s->tx_kib[n];
    }
    i32 ri = dc_ri(s, b, r);
    s->rack_power_mw[ri] = p;
    s->rack_heat_mw[ri] = p;         /* every watt in becomes heat */
    s->rack_tx_kib[ri] = tx;
}

DC_HD static inline void dc_reduce_room(DcSim *s, i32 b, i32 m) {
    i64 p = 0, tx = 0;
    for (i32 r = s->room_rack_begin[m]; r < s->room_rack_end[m]; ++r) {
        i32 ri = dc_ri(s, b, r);
        p += s->rack_power_mw[ri];
        tx += s->rack_tx_kib[ri];
    }
    i32 mi = dc_mi(s, b, m);
    s->room_power_mw[mi] = p;
    s->room_heat_mw[mi] = p;
    s->room_tx_kib[mi] = tx;
}

/* ---- 6. fabric: leaf uplink then spine, demand-proportional ----------- */
DC_HD static inline void dc_pass_fabric(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), r = s->node_rack[i], m = s->rack_room[r];
    i32 ri = dc_ri(s, b, r), mi = dc_mi(s, b, m);
    i32 v = s->node_svc[i], bytes = s->svc_bytes_kib[v];
    if (bytes <= 0) {                        /* stage produces no traffic */
        s->deliv_req[n] = s->served_req[n];
        s->txbl_kib[n] = 0;
        return;
    }
    i32 leaf = dc_share_q16(s->rack_uplink_kib[ri], s->rack_tx_kib[ri]);
    i32 spine = dc_share_q16(s->room_spine_kib[mi], s->room_tx_kib[mi]);
    i32 share = dc_min32(leaf, spine);
    /* Bytes stream across ticks. `owed` is the bytes still to send for requests
     * that have not fully left, so a payload larger than one tick's share makes
     * partial progress and completes later. Requiring a whole payload inside
     * one tick deadlocks instead: a 2 MiB response under a 121 KiB/tick share
     * never moves, the backlog fills, the node stalls, and its own stall keeps
     * the share small forever. */
    i64 owed = (i64)s->txbl_kib[n] + s->tx_kib[n];
    i64 inflight_before = (owed + bytes - 1) / bytes;
    i64 sent = dc_min64(owed, dc_q16_mul(owed, share));
    owed -= sent;
    i64 inflight_after = (owed + bytes - 1) / bytes;
    s->deliv_req[n] = (i32)(inflight_before - inflight_after);
    s->txbl_kib[n] = (i32)dc_min64(owed, (i64)INT32_MAX);
}

/* ---- 8. handoff: pull this stage's share of every upstream stage ------- */
/* Returns requests dropped for want of queue space. Stage k+1 pulls from every
 * stage whose `next` is k+1, so fan-in works without a reverse index. */
DC_HD static inline i32 dc_pass_handoff(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), v = s->node_svc[i], slot = s->node_slot[i];
    i32 k = s->svc_nnode[v];
    i64 pull = 0;
    for (i32 u = 0; u < s->nsvc; ++u)
        if (s->svc_next[u] == v)
            pull += dc_split_share(s->svc_deliv[dc_si(s, b, u)], k, slot);
    if (pull <= 0) return 0;
    i32 room = s->node_queue_max[i] - s->queue_req[n];
    if (room < 0) room = 0;
    i32 admit = (i32)dc_min64(pull, (i64)room);
    s->queue_req[n] += admit;
    return (i32)(pull - admit);
}

/* ---- 9. thermal ------------------------------------------------------- */
DC_HD static inline void dc_pass_thermal_room(DcSim *s, i32 b, i32 m) {
    i32 mi = dc_mi(s, b, m);
    i64 cap = s->room_cool_cap_mw[mi];
    i64 over = s->room_heat_mw[mi] - cap;
    if (over < 0) over = 0;
    /* rise is proportional to the OVERSHOOT FRACTION, so the cliff has the same
     * shape at any hall size. No cooling at all pins the rise at the gain. */
    i64 rise = cap > 0 ? (over * DC_OVER_GAIN_MC) / cap
                       : (over > 0 ? (i64)DC_OVER_GAIN_MC * 4 : 0);
    s->room_supply_now_mc[mi] =
        (i32)dc_min64((i64)s->room_supply_mc[mi] + rise, 1000000);
}

DC_HD static inline void dc_pass_thermal_rack(DcSim *s, i32 b, i32 r) {
    i32 ri = dc_ri(s, b, r), mi = dc_mi(s, b, s->rack_room[r]);
    i32 airflow = s->rack_airflow[r] > 0 ? s->rack_airflow[r] : 1;
    i64 rise = dc_q16_mul(s->rack_heat_mw[ri] / airflow, s->rack_recirc_q16[r]);
    s->rack_inlet_mc[ri] =
        (i32)dc_min64((i64)s->room_supply_now_mc[mi] + rise, (i64)INT32_MAX);
}

DC_HD static inline void dc_pass_thermal_node(DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), ri = dc_ri(s, b, s->node_rack[i]);
    i64 self = ((i64)s->power_mw[n] * s->node_rja_mc_per_w[i]) / 1000;
    i64 target = (i64)s->rack_inlet_mc[ri] + s->node_inlet_bias_mc[i] + self;
    i64 t = (i64)s->temp_mc[n];
    t += dc_q16_mul(target - t, DC_THERMAL_TAU_Q16);
    s->temp_mc[n] = (i32)dc_clamp32((i32)t, -50000, 2000000);
}

/* ---- 10. power verdict for next tick, and the breaker ------------------ */
DC_HD static inline void dc_pass_rack_verdict(DcSim *s, i32 b, i32 r) {
    i32 ri = dc_ri(s, b, r);
    i64 cap = s->rack_pdu_cap_mw[ri], dem = s->rack_power_mw[ri];
    s->rack_scale_q16[ri] = dc_share_q16(cap, dem);
    if (dem > dc_q16_mul(cap, DC_BREAKER_TRIP_Q16)) {
        if (++s->rack_trip_ticks[ri] >= DC_BREAKER_TICKS) s->rack_dark[ri] = 1;
    } else {
        s->rack_trip_ticks[ri] = 0;
    }
}

DC_HD static inline void dc_pass_room_verdict(DcSim *s, i32 b, i32 m) {
    i32 mi = dc_mi(s, b, m);
    s->room_scale_q16[mi] = dc_share_q16(s->room_pwr_cap_mw[mi],
                                         s->room_power_mw[mi]);
}

/* ---- 11. per-node metric contributions (drivers accumulate) ----------- */
typedef struct DcNodeMetric {
    i64 energy_mwt;    /* power this tick, in milliwatt-ticks */
    i64 served_wu;     /* work units done this tick */
    i64 completed;     /* requests that left the datacenter */
    i64 slo_viol;      /* served requests whose queue wait blew the budget */
    i32 limiter;       /* DC_LIM_* */
    i32 temp_mc;
} DcNodeMetric;

DC_HD static inline DcNodeMetric dc_node_metric(const DcSim *s, i32 b, i32 i) {
    i32 n = dc_ni(s, b, i), v = s->node_svc[i];
    DcNodeMetric mt;
    mt.energy_mwt = s->power_mw[n];
    mt.served_wu = (i64)s->served_req[n] * s->svc_wu_per_req[v];
    mt.completed = s->svc_next[v] == DC_EGRESS ? s->deliv_req[n] : 0;
    mt.limiter = s->limiter[n];
    mt.temp_mc = s->temp_mc[n];
    mt.slo_viol = 0;
    i32 slo = s->svc_slo_ticks[v];
    if (slo > 0 && s->served_req[n] > 0) {
        /* Little's law: mean wait is backlog over service rate (SPEC.md). */
        i32 lat = s->queue_req[n] / s->served_req[n];
        if (lat > slo) mt.slo_viol = s->served_req[n];
    }
    return mt;
}

#endif /* COLDAISLE_DC_TICK_H */
