/* dc_run.c - the CPU driver. Calls the same core passes the CUDA kernels do,
 * in the order SPEC.md fixes. This file is also the readable definition of a
 * tick: if the GPU and CPU ever disagree, this is the side that is right. */
#include "dc_host.h"
#include "core/dc_tick.h"

#include <stdio.h>
#include <string.h>

void dc_tick_cpu(DcSim *s) {
    for (i32 b = 0; b < s->nbatch; ++b) {
        i64 offered = 0, dropped = 0;

        for (i32 v = 0; v < s->nsvc; ++v) s->svc_deliv[dc_si(s, b, v)] = 0;

        for (i32 i = 0; i < s->nnode; ++i) dc_pass_clock(s, b, i);
        for (i32 i = 0; i < s->nnode; ++i) {
            DcArrive a = dc_pass_arrive(s, b, i);
            offered += a.offered;
            dropped += a.dropped;
            s->svc_arrived[dc_si(s, b, s->node_svc[i])] += a.offered;
        }
        for (i32 i = 0; i < s->nnode; ++i) dc_pass_service(s, b, i);
        for (i32 i = 0; i < s->nnode; ++i) dc_pass_power(s, b, i);
        for (i32 r = 0; r < s->nrack; ++r) dc_reduce_rack(s, b, r);
        for (i32 m = 0; m < s->nroom; ++m) dc_reduce_room(s, b, m);
        for (i32 i = 0; i < s->nnode; ++i) dc_pass_fabric(s, b, i);
        for (i32 i = 0; i < s->nnode; ++i)
            s->svc_deliv[dc_si(s, b, s->node_svc[i])] += s->deliv_req[dc_ni(s, b, i)];
        for (i32 i = 0; i < s->nnode; ++i) dropped += dc_pass_handoff(s, b, i);
        for (i32 m = 0; m < s->nroom; ++m) {
            dc_pass_thermal_room(s, b, m);
            i64 sup = s->room_supply_now_mc[dc_mi(s, b, m)];
            if (sup > s->m_peak_supply_mc[b]) s->m_peak_supply_mc[b] = sup;
        }
        for (i32 r = 0; r < s->nrack; ++r) dc_pass_thermal_rack(s, b, r);
        for (i32 i = 0; i < s->nnode; ++i) dc_pass_thermal_node(s, b, i);
        for (i32 r = 0; r < s->nrack; ++r) dc_pass_rack_verdict(s, b, r);
        for (i32 m = 0; m < s->nroom; ++m) dc_pass_room_verdict(s, b, m);

        i64 energy = 0, completed = 0, slo = 0, work = 0, peak = s->m_peak_temp_mc[b];
        i64 lim[DC_LIM_COUNT];
        memset(lim, 0, sizeof lim);
        for (i32 i = 0; i < s->nnode; ++i) {
            DcNodeMetric mt = dc_node_metric(s, b, i);
            energy += mt.energy_mwt;
            work += mt.served_wu;
            completed += mt.completed;
            slo += mt.slo_viol;
            lim[mt.limiter]++;
            if (mt.temp_mc > peak) peak = mt.temp_mc;
        }
        s->m_arrived[b] += offered;
        s->m_dropped[b] += dropped;
        s->m_energy_mwt[b] += energy;
        s->m_served_wu[b] += work;
        s->m_completed[b] += completed;
        s->m_slo_viol[b] += slo;
        s->m_peak_temp_mc[b] = peak;
        for (int c = 0; c < DC_LIM_COUNT; ++c) s->m_lim[b * DC_LIM_COUNT + c] += lim[c];
        s->m_ticks[b]++;
    }
}

void dc_run_cpu(DcSim *s, i64 ticks) {
    for (i64 t = 0; t < ticks; ++t) dc_tick_cpu(s);
}

/* Requests are conserved: everything offered at a door either left through
 * egress, was dropped, is still queued, or is part-way out of a node. A backlog
 * of `owed` bytes belongs to ceil(owed / bytes_per_request) requests, counting
 * the partially transmitted one (dc_pass_fabric streams bytes across ticks). */
int dc_audit(const DcHost *h, char *err, int errcap) {
    const DcSim *s = &h->sim;
    for (i32 b = 0; b < s->nbatch; ++b) {
        i64 queued = 0, inflight = 0;
        for (i32 i = 0; i < s->nnode; ++i) {
            i32 n = dc_ni(s, b, i);
            queued += s->queue_req[n];
            i32 bytes = s->svc_bytes_kib[s->node_svc[i]];
            if (bytes > 0) inflight += ((i64)s->txbl_kib[n] + bytes - 1) / bytes;
        }
        i64 lhs = s->m_arrived[b];
        i64 rhs = s->m_completed[b] + s->m_dropped[b] + queued + inflight;
        if (lhs != rhs) {
            snprintf(err, errcap,
                     "batch %d: offered %lld != completed %lld + dropped %lld + "
                     "queued %lld + inflight %lld (off by %lld)",
                     b, (long long)lhs, (long long)s->m_completed[b],
                     (long long)s->m_dropped[b], (long long)queued,
                     (long long)inflight, (long long)(lhs - rhs));
            return -1;
        }
    }
    return 0;
}
