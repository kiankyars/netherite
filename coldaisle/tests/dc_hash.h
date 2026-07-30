/* dc_hash.h - FNV-1a over one batch's mutable state. Shared by the
 * determinism and CPU/GPU parity gates so both compare the same bytes. */
#ifndef COLDAISLE_TEST_HASH_H
#define COLDAISLE_TEST_HASH_H

#include "core/dc_state.h"

/* FNV-1a over every mutable per-node, per-rack, per-room and per-batch array
 * belonging to one batch. Anything that drifts changes the hash. */
DC_HD static u64 hash_batch(const DcSim *s, i32 b) {
    u64 h = 14695981039346656037ULL;
#define MIX(p, n) do { const unsigned char *bp_ = (const unsigned char *)(p);   \
        size_t nb_ = (size_t)(n);                                              \
        for (size_t k_ = 0; k_ < nb_; ++k_) { h ^= bp_[k_]; h *= 1099511628211ULL; } \
    } while (0)
    i32 N = s->nnode, R = s->nrack, M = s->nroom, V = s->nsvc;
    MIX(&s->queue_req[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->wu_acc[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->arr_acc[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->clock_q16[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->temp_mc[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->txbl_kib[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->served_req[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->deliv_req[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->power_mw[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->limiter[dc_ni(s, b, 0)], sizeof(i32) * (size_t)N);
    MIX(&s->rack_power_mw[dc_ri(s, b, 0)], sizeof(i64) * (size_t)R);
    MIX(&s->rack_tx_kib[dc_ri(s, b, 0)], sizeof(i64) * (size_t)R);
    MIX(&s->rack_scale_q16[dc_ri(s, b, 0)], sizeof(i32) * (size_t)R);
    MIX(&s->rack_inlet_mc[dc_ri(s, b, 0)], sizeof(i32) * (size_t)R);
    MIX(&s->rack_trip_ticks[dc_ri(s, b, 0)], sizeof(i32) * (size_t)R);
    MIX(&s->rack_dark[dc_ri(s, b, 0)], sizeof(i32) * (size_t)R);
    MIX(&s->room_power_mw[dc_mi(s, b, 0)], sizeof(i64) * (size_t)M);
    MIX(&s->room_scale_q16[dc_mi(s, b, 0)], sizeof(i32) * (size_t)M);
    MIX(&s->room_supply_now_mc[dc_mi(s, b, 0)], sizeof(i32) * (size_t)M);
    MIX(&s->svc_deliv[dc_si(s, b, 0)], sizeof(i64) * (size_t)V);
    MIX(&s->svc_arrived[dc_si(s, b, 0)], sizeof(i64) * (size_t)V);
    MIX(&s->m_arrived[b], sizeof(i64));
    MIX(&s->m_completed[b], sizeof(i64));
    MIX(&s->m_dropped[b], sizeof(i64));
    MIX(&s->m_energy_mwt[b], sizeof(i64));
    MIX(&s->m_served_wu[b], sizeof(i64));
    MIX(&s->m_slo_viol[b], sizeof(i64));
    MIX(&s->m_peak_temp_mc[b], sizeof(i64));
    MIX(&s->m_peak_supply_mc[b], sizeof(i64));
    MIX(&s->m_lim[(size_t)b * DC_LIM_COUNT], sizeof(i64) * DC_LIM_COUNT);
#undef MIX
    return h;
}

#endif /* COLDAISLE_TEST_HASH_H */
