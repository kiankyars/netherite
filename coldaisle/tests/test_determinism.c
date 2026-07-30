/* test_determinism - the contract in SPEC.md, checked as exact integers.
 *
 * Three properties:
 *   1. two runs of the same blueprint produce byte-identical state,
 *   2. batches do not leak into each other (a batch run alongside others has
 *      the same state as the same batch run alone), which is what makes the
 *      GPU sweep trustworthy,
 *   3. a run split in two halves equals the same run done in one go, so there
 *      is no hidden per-call state. */
#include "core/dc_tick.h"
#include "host/dc_host.h"
#include "tests/dc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("  (%s:%d)\n", __FILE__, __LINE__); ++fails; } } while (0)

#define BP "/tmp/coldaisle_det.dcb"

/* A blueprint with every loop live at once: power capping, a cooling shortfall,
 * a thin uplink and two stages, so the hash covers all of it. */
static void write_bp(void) {
    FILE *f = fopen(BP, "w");
    if (!f) { printf("FAIL: cannot write %s\n", BP); exit(1); }
    fprintf(f,
        "room hall power_cap_kw=60 cool_cap_kw=12 supply_c=23 spine_gbps=2\n"
        "rackgroup row room=hall racks=6 pdu_cap_kw=6 uplink_gbps=0.4"
        " recirc=0.15 airflow_kw_per_c=1.5\n"
        "nodetype srv cap_wu=250 idle_w=180 dyn_w=700 queue_max=200 txbl_max_kib=2048"
        " rja_c_per_kw=30 t_throttle_c=80 t_max_c=95\n"
        "stage a wu=25  bytes_kib=24 next=b      slo_ms=300\n"
        "stage b wu=140 bytes_kib=6  next=egress slo_ms=600\n"
        "fill rackgroup=row type=srv stage=a per_rack=5\n"
        "fill rackgroup=row type=srv stage=b per_rack=5\n"
        "load stage=a rps=2500\n");
    fclose(f);
}

static u64 run_and_hash(int batches, i64 ticks, i32 which, i64 split) {
    DcHost h;
    char err[512];
    if (dc_build_from_file(&h, BP, batches, err, sizeof err)) {
        printf("FAIL: build: %s\n", err);
        exit(1);
    }
    if (split > 0 && split < ticks) {
        dc_run_cpu(&h.sim, split);
        dc_run_cpu(&h.sim, ticks - split);
    } else {
        dc_run_cpu(&h.sim, ticks);
    }
    if (dc_audit(&h, err, sizeof err)) { printf("FAIL: %s\n", err); ++fails; }
    u64 hh = hash_batch(&h.sim, which);
    dc_host_free(&h);
    return hh;
}

int main(void) {
    write_bp();
    const i64 T = 500;

    u64 a = run_and_hash(1, T, 0, 0);
    u64 b = run_and_hash(1, T, 0, 0);
    CHECK(a == b, "two identical runs hashed %016llx and %016llx",
          (unsigned long long)a, (unsigned long long)b);

    u64 split = run_and_hash(1, T, 0, 137);
    CHECK(a == split, "500 ticks in one go hashed %016llx, 137+363 hashed %016llx",
          (unsigned long long)a, (unsigned long long)split);

    /* No sweep in this blueprint, so every batch of a 4-batch run is the same
     * datacenter and must match the single-batch run exactly. */
    for (i32 which = 0; which < 4; ++which) {
        u64 m = run_and_hash(4, T, which, 0);
        CHECK(a == m, "batch %d of 4 hashed %016llx, alone %016llx",
              which, (unsigned long long)m, (unsigned long long)a);
    }

    /* And the interesting negative: a different tick count must NOT hash the
     * same, or the hash is not actually covering the state. */
    u64 shorter = run_and_hash(1, T - 1, 0, 0);
    CHECK(a != shorter, "499 ticks hashed the same as 500, so the hash is blind");

    remove(BP);
    if (fails) { printf("test_determinism: %d FAILURES\n", fails); return 1; }
    printf("test_determinism: PASS\n");
    return 0;
}
