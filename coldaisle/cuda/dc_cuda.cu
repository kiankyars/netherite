/* dc_cuda.cu - the batched GPU driver.
 *
 * There is no second model here. Every kernel body is a call into
 * core/dc_tick.h, the same DC_HD functions host/dc_run.c loops over, launched
 * in the same order. The state is integer-only, so the two paths agree exactly
 * and tests/test_parity compares them as integers rather than with a tolerance:
 *
 *   - per-node, per-rack, per-room, per-service passes are grid-stride maps,
 *   - rack and room reductions are one thread per rack or room walking a
 *     contiguous range, which is what the room-major, rack-major node ordering
 *     from host/dc_build.c buys,
 *   - the two cross-element accumulations (delivered per service, and the
 *     per-batch metrics) use integer atomics. Integer addition is associative,
 *     so thread order cannot change the sum. That is the whole reason the model
 *     avoids floating point.
 *
 * A sweep is the point: B datacenters differing only in the capacities you
 * would actually shop for, all advanced by one launch each tick. */
#include "cuda/dc_cuda.h"
#include "core/dc_tick.h"

#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>

#define DC_THREADS 256

static char g_err[256] = "";

static int fail(const char *what, cudaError_t e) {
    snprintf(g_err, sizeof g_err, "%s: %s", what, cudaGetErrorString(e));
    return -1;
}

const char *dc_cuda_last_error(void) { return g_err; }

int dc_cuda_device_count(void) {
    int n = 0;
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) { fail("cudaGetDeviceCount", e); return -1; }
    return n;
}

/* ---- kernels ----------------------------------------------------------- */
#define GRID_STRIDE(total) \
    for (i64 idx = (i64)blockIdx.x * blockDim.x + threadIdx.x; idx < (total); \
         idx += (i64)gridDim.x * blockDim.x)

__global__ void k_clock(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) dc_pass_clock(&s, (i32)(idx / s.nnode), (i32)(idx % s.nnode));
}

__global__ void k_zero_svc(DcSim s) {
    i64 total = (i64)s.nbatch * s.nsvc;
    GRID_STRIDE(total) s.svc_deliv[idx] = 0;
}

__global__ void k_arrive(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) {
        i32 b = (i32)(idx / s.nnode), i = (i32)(idx % s.nnode);
        DcArrive a = dc_pass_arrive(&s, b, i);
        if (a.offered) {
            atomicAdd((unsigned long long *)&s.m_arrived[b], (unsigned long long)a.offered);
            atomicAdd((unsigned long long *)&s.svc_arrived[dc_si(&s, b, s.node_svc[i])],
                      (unsigned long long)a.offered);
        }
        if (a.dropped)
            atomicAdd((unsigned long long *)&s.m_dropped[b], (unsigned long long)a.dropped);
    }
}

__global__ void k_service(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) dc_pass_service(&s, (i32)(idx / s.nnode), (i32)(idx % s.nnode));
}

__global__ void k_power(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) dc_pass_power(&s, (i32)(idx / s.nnode), (i32)(idx % s.nnode));
}

__global__ void k_reduce_rack(DcSim s) {
    i64 total = (i64)s.nbatch * s.nrack;
    GRID_STRIDE(total) dc_reduce_rack(&s, (i32)(idx / s.nrack), (i32)(idx % s.nrack));
}

__global__ void k_reduce_room(DcSim s) {
    i64 total = (i64)s.nbatch * s.nroom;
    GRID_STRIDE(total) dc_reduce_room(&s, (i32)(idx / s.nroom), (i32)(idx % s.nroom));
}

__global__ void k_fabric(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) dc_pass_fabric(&s, (i32)(idx / s.nnode), (i32)(idx % s.nnode));
}

__global__ void k_svc_deliv(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) {
        i32 b = (i32)(idx / s.nnode), i = (i32)(idx % s.nnode);
        i32 d = s.deliv_req[dc_ni(&s, b, i)];
        if (d) atomicAdd((unsigned long long *)&s.svc_deliv[dc_si(&s, b, s.node_svc[i])],
                         (unsigned long long)d);
    }
}

__global__ void k_handoff(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) {
        i32 b = (i32)(idx / s.nnode), i = (i32)(idx % s.nnode);
        i32 dropped = dc_pass_handoff(&s, b, i);
        if (dropped)
            atomicAdd((unsigned long long *)&s.m_dropped[b], (unsigned long long)dropped);
    }
}

__global__ void k_thermal_room(DcSim s) {
    i64 total = (i64)s.nbatch * s.nroom;
    GRID_STRIDE(total) {
        i32 b = (i32)(idx / s.nroom), m = (i32)(idx % s.nroom);
        dc_pass_thermal_room(&s, b, m);
        i64 sup = s.room_supply_now_mc[dc_mi(&s, b, m)];
        if (sup > 0) atomicMax((unsigned long long *)&s.m_peak_supply_mc[b],
                               (unsigned long long)sup);
    }
}

__global__ void k_thermal_rack(DcSim s) {
    i64 total = (i64)s.nbatch * s.nrack;
    GRID_STRIDE(total) dc_pass_thermal_rack(&s, (i32)(idx / s.nrack), (i32)(idx % s.nrack));
}

__global__ void k_thermal_node(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) dc_pass_thermal_node(&s, (i32)(idx / s.nnode), (i32)(idx % s.nnode));
}

__global__ void k_rack_verdict(DcSim s) {
    i64 total = (i64)s.nbatch * s.nrack;
    GRID_STRIDE(total) dc_pass_rack_verdict(&s, (i32)(idx / s.nrack), (i32)(idx % s.nrack));
}

__global__ void k_room_verdict(DcSim s) {
    i64 total = (i64)s.nbatch * s.nroom;
    GRID_STRIDE(total) dc_pass_room_verdict(&s, (i32)(idx / s.nroom), (i32)(idx % s.nroom));
}

__global__ void k_metrics(DcSim s) {
    i64 total = (i64)s.nbatch * s.nnode;
    GRID_STRIDE(total) {
        i32 b = (i32)(idx / s.nnode), i = (i32)(idx % s.nnode);
        DcNodeMetric mt = dc_node_metric(&s, b, i);
        atomicAdd((unsigned long long *)&s.m_energy_mwt[b], (unsigned long long)mt.energy_mwt);
        if (mt.served_wu)
            atomicAdd((unsigned long long *)&s.m_served_wu[b], (unsigned long long)mt.served_wu);
        if (mt.completed)
            atomicAdd((unsigned long long *)&s.m_completed[b], (unsigned long long)mt.completed);
        if (mt.slo_viol)
            atomicAdd((unsigned long long *)&s.m_slo_viol[b], (unsigned long long)mt.slo_viol);
        atomicAdd((unsigned long long *)&s.m_lim[(i64)b * DC_LIM_COUNT + mt.limiter], 1ULL);
        /* Temperatures are clamped above -50 C and a run always starts warmer
         * than zero, so an unsigned max over the non-negative values is the
         * same peak the CPU loop computes. */
        if (mt.temp_mc > 0)
            atomicMax((unsigned long long *)&s.m_peak_temp_mc[b], (unsigned long long)mt.temp_mc);
    }
}

__global__ void k_tick_count(DcSim s) {
    GRID_STRIDE((i64)s.nbatch) s.m_ticks[idx] += 1;
}

/* ---- upload / run / download ------------------------------------------ */
typedef struct { void **slot; size_t bytes; const void *src; } Move;

#define N_MOVES 64

static int copy_all(Move *mv, int n, int to_device) {
    for (int i = 0; i < n; ++i) {
        cudaError_t e = to_device
            ? cudaMemcpy(*mv[i].slot, mv[i].src, mv[i].bytes, cudaMemcpyHostToDevice)
            : cudaMemcpy((void *)mv[i].src, *mv[i].slot, mv[i].bytes, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) return fail("cudaMemcpy", e);
    }
    return 0;
}

/* Every array in DcSim, paired with its element size and count, so upload,
 * download and free all walk one list instead of fifty hand-written lines. */
static int build_moves(const DcSim *h, DcSim *d, Move *mv, int *nmv, int *nmutable) {
    i64 B = h->nbatch, N = h->nnode, R = h->nrack, M = h->nroom, V = h->nsvc;
    int k = 0;
#define ADD(field, count) do { \
        mv[k].slot = (void **)&d->field; mv[k].bytes = sizeof(*h->field) * (size_t)(count); \
        mv[k].src = h->field; ++k; } while (0)
    /* immutable topology first */
    ADD(node_rack, N); ADD(node_svc, N); ADD(node_slot, N); ADD(node_cap_wu, N);
    ADD(node_idle_mw, N); ADD(node_dyn_mw, N); ADD(node_queue_max, N);
    ADD(node_txbl_max, N); ADD(node_rja_mc_per_w, N); ADD(node_t_thr_mc, N);
    ADD(node_t_max_mc, N); ADD(node_inlet_bias_mc, N);
    ADD(rack_room, R); ADD(rack_node_begin, R); ADD(rack_node_end, R);
    ADD(rack_recirc_q16, R); ADD(rack_airflow, R);
    ADD(room_rack_begin, M); ADD(room_rack_end, M);
    ADD(svc_wu_per_req, V); ADD(svc_bytes_kib, V); ADD(svc_next, V);
    ADD(svc_nnode, V); ADD(svc_slo_ticks, V);
    ADD(rack_pdu_cap_mw, B * R); ADD(rack_uplink_kib, B * R);
    ADD(room_pwr_cap_mw, B * M); ADD(room_cool_cap_mw, B * M);
    ADD(room_supply_mc, B * M); ADD(room_spine_kib, B * M);
    ADD(svc_rate_milli, B * V);
    int immutable = k;
    /* mutable state, the part download brings back */
    ADD(queue_req, B * N); ADD(wu_acc, B * N); ADD(arr_acc, B * N);
    ADD(clock_q16, B * N); ADD(temp_mc, B * N); ADD(txbl_kib, B * N);
    ADD(served_req, B * N); ADD(tx_kib, B * N); ADD(deliv_req, B * N);
    ADD(power_mw, B * N); ADD(limiter, B * N);
    ADD(rack_power_mw, B * R); ADD(rack_heat_mw, B * R); ADD(rack_tx_kib, B * R);
    ADD(rack_scale_q16, B * R); ADD(rack_inlet_mc, B * R);
    ADD(rack_trip_ticks, B * R); ADD(rack_dark, B * R);
    ADD(room_power_mw, B * M); ADD(room_heat_mw, B * M); ADD(room_tx_kib, B * M);
    ADD(room_scale_q16, B * M); ADD(room_supply_now_mc, B * M);
    ADD(svc_deliv, B * V); ADD(svc_arrived, B * V);
    ADD(m_arrived, B); ADD(m_completed, B); ADD(m_dropped, B);
    ADD(m_energy_mwt, B); ADD(m_served_wu, B); ADD(m_slo_viol, B); ADD(m_lim, B * DC_LIM_COUNT);
    ADD(m_peak_temp_mc, B); ADD(m_peak_supply_mc, B); ADD(m_ticks, B);
#undef ADD
    if (k > N_MOVES) {
        snprintf(g_err, sizeof g_err, "move table too small: %d entries", k);
        return -1;
    }
    *nmv = k;
    *nmutable = k - immutable;
    return 0;
}

int dc_cuda_upload(const DcSim *host, DcSim *dev) {
    *dev = *host;                        /* counts come along; pointers get replaced */
    Move mv[N_MOVES];
    int n = 0, nmut = 0;
    if (build_moves(host, dev, mv, &n, &nmut)) return -1;
    for (int i = 0; i < n; ++i) {
        void *p = NULL;
        cudaError_t e = cudaMalloc(&p, mv[i].bytes);
        if (e != cudaSuccess) return fail("cudaMalloc", e);
        *mv[i].slot = p;
    }
    return copy_all(mv, n, 1);
}

int dc_cuda_download(const DcSim *dev, DcSim *host) {
    Move mv[N_MOVES];
    int n = 0, nmut = 0;
    DcSim tmp = *dev;
    if (build_moves(host, &tmp, mv, &n, &nmut)) return -1;
    return copy_all(mv + (n - nmut), nmut, 0);
}

void dc_cuda_free(DcSim *dev) {
    Move mv[N_MOVES];
    int n = 0, nmut = 0;
    DcSim host = *dev;                   /* same shape; only sizes are read */
    if (build_moves(&host, dev, mv, &n, &nmut)) return;
    for (int i = 0; i < n; ++i) if (*mv[i].slot) cudaFree(*mv[i].slot);
    memset(dev, 0, sizeof *dev);
}

static int launch_error(const char *what) {
    cudaError_t e = cudaGetLastError();
    if (e != cudaSuccess) return fail(what, e);
    return 0;
}

int dc_cuda_run(DcSim *dev, i64 ticks) {
    i64 nodes = (i64)dev->nbatch * dev->nnode;
    i64 racks = (i64)dev->nbatch * dev->nrack;
    i64 rooms = (i64)dev->nbatch * dev->nroom;
    i64 svcs = (i64)dev->nbatch * dev->nsvc;
    int bn = (int)((nodes + DC_THREADS - 1) / DC_THREADS);
    int br = (int)((racks + DC_THREADS - 1) / DC_THREADS);
    int bm = (int)((rooms + DC_THREADS - 1) / DC_THREADS);
    int bv = (int)((svcs + DC_THREADS - 1) / DC_THREADS);
    int bb = (dev->nbatch + DC_THREADS - 1) / DC_THREADS;
    if (bn < 1) bn = 1;
    if (br < 1) br = 1;
    if (bm < 1) bm = 1;
    if (bv < 1) bv = 1;
    if (bb < 1) bb = 1;

    for (i64 t = 0; t < ticks; ++t) {
        /* SPEC.md pass order; host/dc_run.c is the readable copy of this list */
        k_zero_svc<<<bv, DC_THREADS>>>(*dev);
        k_clock<<<bn, DC_THREADS>>>(*dev);
        k_arrive<<<bn, DC_THREADS>>>(*dev);
        k_service<<<bn, DC_THREADS>>>(*dev);
        k_power<<<bn, DC_THREADS>>>(*dev);
        k_reduce_rack<<<br, DC_THREADS>>>(*dev);
        k_reduce_room<<<bm, DC_THREADS>>>(*dev);
        k_fabric<<<bn, DC_THREADS>>>(*dev);
        k_svc_deliv<<<bn, DC_THREADS>>>(*dev);
        k_handoff<<<bn, DC_THREADS>>>(*dev);
        k_thermal_room<<<bm, DC_THREADS>>>(*dev);
        k_thermal_rack<<<br, DC_THREADS>>>(*dev);
        k_thermal_node<<<bn, DC_THREADS>>>(*dev);
        k_rack_verdict<<<br, DC_THREADS>>>(*dev);
        k_room_verdict<<<bm, DC_THREADS>>>(*dev);
        k_metrics<<<bn, DC_THREADS>>>(*dev);
        k_tick_count<<<bb, DC_THREADS>>>(*dev);
        if (launch_error("kernel launch")) return -1;
    }
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) return fail("cudaDeviceSynchronize", e);
    return 0;
}
