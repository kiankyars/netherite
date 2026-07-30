/* test_parity - CPU and GPU must agree exactly, not approximately.
 *
 * Builds one blueprint twice, advances one copy with host/dc_run.c and the
 * other with cuda/dc_cuda.cu, and compares the full mutable state of every
 * batch as a hash plus each metric individually. Because core/ is integer-only
 * and the two cross-element accumulations use integer atomics, "exactly" is a
 * fair thing to demand: any difference is a bug, never rounding.
 *
 * Needs a CUDA device. With none visible it says so and exits 0 so the gate can
 * live in `make test` on a machine without a GPU. */
#include "core/dc_tick.h"
#include "host/dc_host.h"
#include "cuda/dc_cuda.h"
#include "tests/dc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("  (%s:%d)\n", __FILE__, __LINE__); ++fails; } } while (0)

#define BP "/tmp/coldaisle_parity.dcb"

/* Every loop live at once, plus a real sweep so the batches differ: if a kernel
 * indexed a per-batch capacity wrongly, identical batches would hide it. */
static void write_bp(void) {
    FILE *f = fopen(BP, "w");
    if (!f) { printf("FAIL: cannot write %s\n", BP); exit(1); }
    fprintf(f,
        "room hall power_cap_kw=400 cool_cap_kw=90 supply_c=23 spine_gbps=8\n"
        "rackgroup row room=hall racks=12 pdu_cap_kw=9 uplink_gbps=0.6"
        " recirc=0.14 airflow_kw_per_c=1.6\n"
        "nodetype srv cap_wu=260 idle_w=190 dyn_w=760 queue_max=220 txbl_max_kib=3072"
        " rja_c_per_kw=28 t_throttle_c=82 t_max_c=96\n"
        "stage a wu=24  bytes_kib=20 next=b      slo_ms=300\n"
        "stage b wu=150 bytes_kib=6  next=egress slo_ms=700\n"
        "fill rackgroup=row type=srv stage=a per_rack=6\n"
        "fill rackgroup=row type=srv stage=b per_rack=6\n"
        "load stage=a rps=6000\n"
        "sweep cool_cap_kw target=hall 20 45 70 90 140 400\n");
    fclose(f);
}

static void report_metric(const char *name, i32 b, i64 cpu, i64 gpu) {
    CHECK(cpu == gpu, "batch %d %s: cpu %lld gpu %lld (delta %lld)", b, name,
          (long long)cpu, (long long)gpu, (long long)(cpu - gpu));
}

int main(void) {
    int ndev = dc_cuda_device_count();
    if (ndev <= 0) {
        printf("test_parity: SKIP (no CUDA device visible: %s)\n",
               ndev < 0 ? dc_cuda_last_error() : "count is 0");
        return 0;
    }
    write_bp();
    const i64 T = 400;
    char err[512];

    DcHost cpu, gpu;
    if (dc_build_from_file(&cpu, BP, 0, err, sizeof err) ||
        dc_build_from_file(&gpu, BP, 0, err, sizeof err)) {
        printf("FAIL: build: %s\n", err);
        return 1;
    }
    dc_run_cpu(&cpu.sim, T);

    DcSim dev;
    if (dc_cuda_upload(&gpu.sim, &dev)) { printf("FAIL: upload: %s\n", dc_cuda_last_error()); return 1; }
    if (dc_cuda_run(&dev, T)) { printf("FAIL: run: %s\n", dc_cuda_last_error()); return 1; }
    if (dc_cuda_download(&dev, &gpu.sim)) { printf("FAIL: download: %s\n", dc_cuda_last_error()); return 1; }
    dc_cuda_free(&dev);

    CHECK(cpu.sim.nbatch == gpu.sim.nbatch, "batch counts differ");
    for (i32 b = 0; b < cpu.sim.nbatch; ++b) {
        u64 hc = hash_batch(&cpu.sim, b), hg = hash_batch(&gpu.sim, b);
        CHECK(hc == hg, "batch %d state hash: cpu %016llx gpu %016llx", b,
              (unsigned long long)hc, (unsigned long long)hg);
        report_metric("arrived", b, cpu.sim.m_arrived[b], gpu.sim.m_arrived[b]);
        report_metric("completed", b, cpu.sim.m_completed[b], gpu.sim.m_completed[b]);
        report_metric("dropped", b, cpu.sim.m_dropped[b], gpu.sim.m_dropped[b]);
        report_metric("energy", b, cpu.sim.m_energy_mwt[b], gpu.sim.m_energy_mwt[b]);
        report_metric("served_wu", b, cpu.sim.m_served_wu[b], gpu.sim.m_served_wu[b]);
        report_metric("slo", b, cpu.sim.m_slo_viol[b], gpu.sim.m_slo_viol[b]);
        report_metric("peak_temp", b, cpu.sim.m_peak_temp_mc[b], gpu.sim.m_peak_temp_mc[b]);
        report_metric("peak_supply", b, cpu.sim.m_peak_supply_mc[b], gpu.sim.m_peak_supply_mc[b]);
        for (int k = 0; k < DC_LIM_COUNT; ++k)
            report_metric("limiter", b, cpu.sim.m_lim[b * DC_LIM_COUNT + k],
                          gpu.sim.m_lim[b * DC_LIM_COUNT + k]);
    }
    /* The sweep must actually have produced different datacenters, otherwise
     * this test proves much less than it looks like it does. */
    int distinct = 0;
    for (i32 b = 1; b < cpu.sim.nbatch; ++b)
        if (cpu.sim.m_completed[b] != cpu.sim.m_completed[0]) distinct = 1;
    CHECK(distinct, "every batch served the same, so the sweep axis did nothing");

    if (dc_audit(&cpu, err, sizeof err)) { printf("FAIL: cpu audit: %s\n", err); ++fails; }
    if (dc_audit(&gpu, err, sizeof err)) { printf("FAIL: gpu audit: %s\n", err); ++fails; }

    dc_host_free(&cpu);
    dc_host_free(&gpu);
    remove(BP);
    if (fails) { printf("test_parity: %d FAILURES\n", fails); return 1; }
    printf("test_parity: PASS (%d batches, %lld ticks, bit-identical)\n",
           (int)cpu.sim.nbatch, (long long)T);
    return 0;
}
