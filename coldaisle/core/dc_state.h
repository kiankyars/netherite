/* dc_state.h - the whole simulation state, struct-of-arrays, batched.
 *
 * Batching is what the GPU is for: B independent datacenters share one
 * topology (node -> rack -> room, and which stage each node serves) and differ
 * in the capacities you would actually sweep - cooling kW, PDU and UPS caps,
 * uplink and spine bandwidth, and offered load. One kernel launch sweeps the
 * whole design space instead of one datacenter at a time.
 *
 * Node arrays are indexed [i] because topology is shared. Everything mutable is
 * indexed [b * count + idx]. Nodes are sorted by rack and racks by room, so
 * every reduction is over a contiguous range. */
#ifndef COLDAISLE_DC_STATE_H
#define COLDAISLE_DC_STATE_H

#include "dc_defs.h"

#define DC_EGRESS (-1)     /* service.next value that means "leaves the DC" */

typedef struct DcSim {
    i32 nnode, nrack, nroom, nsvc, nbatch;

    /* ---- topology and per-node nameplate (shared across batches) ---- */
    i32 *node_rack;        /* [nnode] owning rack */
    i32 *node_svc;         /* [nnode] stage this node serves */
    i32 *node_slot;        /* [nnode] index of this node within its stage group */
    i32 *node_cap_wu;      /* [nnode] work units per tick at clock 1.0 */
    i32 *node_idle_mw;     /* [nnode] power at clock 1.0, zero utilization */
    i32 *node_dyn_mw;      /* [nnode] additional power at full utilization */
    i32 *node_queue_max;   /* [nnode] requests before the node drops */
    i32 *node_txbl_max;    /* [nnode] KiB of undelivered egress before it stalls */
    i32 *node_rja_mc_per_w;/* [nnode] junction-to-inlet rise, mC per watt */
    i32 *node_t_thr_mc;    /* [nnode] throttle onset */
    i32 *node_t_max_mc;    /* [nnode] clock floor reached here */
    i32 *node_inlet_bias_mc;/* [nnode] extra inlet temperature from rack height:
                             * hot air rises, so the top of a rack runs warmer
                             * than the bottom and throttles first */

    i32 *rack_room;        /* [nrack] */
    i32 *rack_node_begin;  /* [nrack] */
    i32 *rack_node_end;    /* [nrack] */
    i32 *rack_recirc_q16;  /* [nrack] hot-aisle recirculation fraction */
    i32 *rack_airflow;     /* [nrack] mW of heat per mC of rack-inlet rise */

    i32 *room_rack_begin;  /* [nroom] */
    i32 *room_rack_end;    /* [nroom] */

    i32 *svc_wu_per_req;   /* [nsvc] */
    i32 *svc_bytes_kib;    /* [nsvc] egress KiB per completed request */
    i32 *svc_next;         /* [nsvc] next stage, or DC_EGRESS */
    i32 *svc_nnode;        /* [nsvc] group size, for the exact handoff split */
    i32 *svc_slo_ticks;    /* [nsvc] latency budget; 0 disables the check */

    /* ---- per-batch capacities (the sweep axes) ----
     * These are i64 on purpose: a 30 MW hall is 3e10 mW, which overflows i32,
     * and an overflowed capacity does not fail loudly - it silently reports
     * zero available power and throttles the whole hall to the clock floor. */
    i64 *rack_pdu_cap_mw;  /* [nbatch*nrack] */
    i64 *rack_uplink_kib;  /* [nbatch*nrack] per tick */
    i64 *room_pwr_cap_mw;  /* [nbatch*nroom] */
    i64 *room_cool_cap_mw; /* [nbatch*nroom] heat removal budget */
    i32 *room_supply_mc;   /* [nbatch*nroom] supply air with cooling in budget */
    i64 *room_spine_kib;   /* [nbatch*nroom] per tick */
    i32 *svc_rate_milli;   /* [nbatch*nsvc] ingress requests per tick * 1000 */

    /* ---- mutable per-node state ---- */
    i32 *queue_req;        /* [nbatch*nnode] */
    i32 *wu_acc;           /* [nbatch*nnode] carried work budget */
    i32 *arr_acc;          /* [nbatch*nnode] carried ingress fraction */
    i32 *clock_q16;        /* [nbatch*nnode] */
    i32 *temp_mc;          /* [nbatch*nnode] */
    i32 *txbl_kib;         /* [nbatch*nnode] undelivered egress */
    i32 *served_req;       /* [nbatch*nnode] this tick */
    i32 *tx_kib;           /* [nbatch*nnode] this tick */
    i32 *deliv_req;        /* [nbatch*nnode] this tick */
    i32 *power_mw;         /* [nbatch*nnode] this tick */
    i32 *limiter;          /* [nbatch*nnode] this tick, DC_LIM_* */

    /* ---- mutable per-rack / per-room / per-service ---- */
    i64 *rack_power_mw;    /* [nbatch*nrack] */
    i64 *rack_heat_mw;     /* [nbatch*nrack] */
    i64 *rack_tx_kib;      /* [nbatch*nrack] */
    i32 *rack_scale_q16;   /* [nbatch*nrack] applied next tick */
    i32 *rack_inlet_mc;    /* [nbatch*nrack] */
    i32 *rack_trip_ticks;  /* [nbatch*nrack] */
    i32 *rack_dark;        /* [nbatch*nrack] breaker latched open */

    i64 *room_power_mw;    /* [nbatch*nroom] */
    i64 *room_heat_mw;     /* [nbatch*nroom] */
    i64 *room_tx_kib;      /* [nbatch*nroom] */
    i32 *room_scale_q16;   /* [nbatch*nroom] */
    i32 *room_supply_now_mc;/* [nbatch*nroom] */

    i64 *svc_deliv;        /* [nbatch*nsvc] delivered requests this tick */
    i64 *svc_arrived;      /* [nbatch*nsvc] cumulative, for conservation */

    /* ---- per-batch metrics ---- */
    i64 *m_arrived;        /* [nbatch] requests admitted at ingress */
    i64 *m_completed;      /* [nbatch] requests that left at egress */
    i64 *m_dropped;        /* [nbatch] queue overflow */
    i64 *m_energy_mwt;     /* [nbatch] milliwatt-ticks */
    i64 *m_served_wu;      /* [nbatch] work units actually done, for utilization */
    i64 *m_slo_viol;       /* [nbatch] served requests over their latency budget */
    i64 *m_lim;            /* [nbatch*DC_LIM_COUNT] limiter histogram, node-ticks */
    i64 *m_peak_temp_mc;   /* [nbatch] */
    i64 *m_peak_supply_mc; /* [nbatch] worst supply air seen, not the last tick's */
    i64 *m_ticks;          /* [nbatch] */
} DcSim;

DC_HD static inline i32 dc_ni(const DcSim *s, i32 b, i32 i) { return b * s->nnode + i; }
DC_HD static inline i32 dc_ri(const DcSim *s, i32 b, i32 r) { return b * s->nrack + r; }
DC_HD static inline i32 dc_mi(const DcSim *s, i32 b, i32 m) { return b * s->nroom + m; }
DC_HD static inline i32 dc_si(const DcSim *s, i32 b, i32 v) { return b * s->nsvc + v; }

#endif /* COLDAISLE_DC_STATE_H */
