/* test_units - the integer primitives everything else trusts. */
#include "core/dc_tick.h"
#include "host/dc_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("  (%s:%d)\n", __FILE__, __LINE__); ++fails; } } while (0)

static void test_split(void) {
    /* The handoff split must be exact for every total and group size, or
     * requests appear and vanish. */
    static const i64 totals[] = { 0, 1, 2, 7, 999, 1000, 65535, 1000003, 1LL << 40 };
    static const i32 ks[] = { 1, 2, 3, 7, 16, 97, 1024, 4096 };
    for (unsigned a = 0; a < sizeof totals / sizeof *totals; ++a) {
        for (unsigned b = 0; b < sizeof ks / sizeof *ks; ++b) {
            i64 total = totals[a];
            i32 k = ks[b];
            i64 sum = 0, lo = -1, hi = -1;
            for (i32 j = 0; j < k; ++j) {
                i64 v = dc_split_share(total, k, j);
                CHECK(v >= 0, "split negative: total=%lld k=%d j=%d -> %lld",
                      (long long)total, k, j, (long long)v);
                sum += v;
                if (lo < 0 || v < lo) lo = v;
                if (hi < 0 || v > hi) hi = v;
            }
            CHECK(sum == total, "split sum: total=%lld k=%d got %lld",
                  (long long)total, k, (long long)sum);
            CHECK(hi - lo <= 1, "split unfair: total=%lld k=%d spread %lld",
                  (long long)total, k, (long long)(hi - lo));
        }
    }
}

static void test_share(void) {
    CHECK(dc_share_q16(0, 0) == DC_Q16, "no demand is full share");
    CHECK(dc_share_q16(100, 50) == DC_Q16, "slack is full share");
    CHECK(dc_share_q16(50, 50) == DC_Q16, "exactly at capacity is full share");
    CHECK(dc_share_q16(0, 50) == 0, "no capacity is no share");
    CHECK(dc_share_q16(25, 50) == DC_Q16 / 2, "half capacity is half share");
    /* monotone in capacity, never above 1.0 */
    i32 prev = -1;
    for (i64 cap = 0; cap <= 1000; cap += 7) {
        i32 q = dc_share_q16(cap, 1000);
        CHECK(q >= prev, "share not monotone at cap=%lld", (long long)cap);
        CHECK(q <= DC_Q16, "share over 1.0 at cap=%lld", (long long)cap);
        prev = q;
    }
    /* big numbers: 30 MW in mW against a 40 MW cap must not overflow */
    CHECK(dc_share_q16(40000000000LL, 30000000000LL) == DC_Q16, "MW-scale share");
    CHECK(dc_share_q16(15000000000LL, 30000000000LL) == DC_Q16 / 2, "MW-scale half");
}

static void test_q16(void) {
    CHECK(dc_q16_mul(1234, DC_Q16) == 1234, "Q16 identity");
    CHECK(dc_q16_mul(1000, DC_Q16 / 2) == 500, "Q16 half");
    CHECK(dc_q16_mul(1, 1) == 0, "Q16 truncates toward zero");
    /* 10 kW node in mW at a 0.75 clock, twice (the V^2 f term) */
    /* 0.75 is exact in Q16, so 10 kW * 0.75^2 comes out exact: 5.625 kW */
    i64 p = dc_q16_mul(dc_q16_mul(10000000, 49152), 49152);
    CHECK(p == 5625000, "clock^2 on 10 kW: %lld", (long long)p);
    CHECK(dc_ramp(0, DC_Q16, 1000) == 1000, "ramp up is limited");
    CHECK(dc_ramp(DC_Q16, 0, 1000) == DC_Q16 - 1000, "ramp down is limited");
    CHECK(dc_ramp(500, 600, 1000) == 600, "ramp does not overshoot");
}

/* Decimal fields in a blueprint must land on exact integers. */
static void test_blueprint_scalars(void) {
    const char *path = "/tmp/coldaisle_units.dcb";
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL: cannot write %s\n", path); ++fails; return; }
    fprintf(f,
        "room r power_cap_kw=1.5 cool_cap_kw=0.25 supply_c=21.5 spine_gbps=100\n"
        "rackgroup g room=r racks=1 pdu_cap_kw=2.5 uplink_gbps=10 recirc=0.15 airflow_kw_per_c=1.25\n"
        "nodetype t cap_wu=10 idle_w=1.5 dyn_w=2.25 rja_c_per_kw=25 t_throttle_c=80 t_max_c=95\n"
        "stage s wu=1 bytes_kib=1 next=egress\n"
        "fill rackgroup=g type=t stage=s per_rack=1\n"
        "load stage=s rps=12.5\n");
    fclose(f);
    DcHost h;
    char err[256];
    if (dc_build_from_file(&h, path, 0, err, sizeof err)) {
        printf("FAIL: build: %s\n", err); ++fails; return;
    }
    const DcSim *s = &h.sim;
    CHECK(s->room_pwr_cap_mw[0] == 1500000, "1.5 kW is 1500000 mW, got %lld", (long long)s->room_pwr_cap_mw[0]);
    CHECK(s->room_cool_cap_mw[0] == 250000, "0.25 kW is 250000 mW, got %lld", (long long)s->room_cool_cap_mw[0]);
    CHECK(s->room_supply_mc[0] == 21500, "21.5 C is 21500 mC, got %d", s->room_supply_mc[0]);
    CHECK(s->rack_pdu_cap_mw[0] == 2500000, "2.5 kW PDU, got %lld", (long long)s->rack_pdu_cap_mw[0]);
    CHECK(s->rack_recirc_q16[0] == 9830, "0.15 is 9830 in Q16, got %d", s->rack_recirc_q16[0]);
    CHECK(s->rack_airflow[0] == 1250, "1.25 kW/C is 1250 mW/mC, got %d", s->rack_airflow[0]);
    CHECK(s->node_idle_mw[0] == 1500, "1.5 W idle, got %d", s->node_idle_mw[0]);
    CHECK(s->node_dyn_mw[0] == 2250, "2.25 W dynamic, got %d", s->node_dyn_mw[0]);
    /* 100 gbps = 12207 KiB/tick at 10 ticks/s; 10 gbps a tenth of that */
    CHECK(s->room_spine_kib[0] == 1220703, "100 gbps in KiB/tick, got %lld", (long long)s->room_spine_kib[0]);
    CHECK(s->rack_uplink_kib[0] == 122070, "10 gbps in KiB/tick, got %lld", (long long)s->rack_uplink_kib[0]);
    /* 12.5 rps at 10 ticks/s is 1.25 req/tick, i.e. 1250 milli-requests */
    CHECK(s->svc_rate_milli[0] == 1250, "12.5 rps is 1250 milli/tick, got %d", s->svc_rate_milli[0]);
    dc_host_free(&h);
    remove(path);
}

/* A rejected blueprint must say which line, not just fail. */
static void test_blueprint_errors(void) {
    const char *path = "/tmp/coldaisle_bad.dcb";
    const char *cases[] = {
        "room r power_cap_kw=1 cool_cap_kw=1 spine_gbps=1\nwidget x\n",
        "room r power_cap_kw=1 cool_cap_kw=1 spine_gbps=1\n",   /* no nodes */
        "room r cool_cap_kw=1 spine_gbps=1\n",                  /* missing key */
        "room r power_cap_kw=abc cool_cap_kw=1 spine_gbps=1\n", /* not a number */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        FILE *f = fopen(path, "w");
        if (!f) { printf("FAIL: cannot write %s\n", path); ++fails; return; }
        fputs(cases[i], f);
        fclose(f);
        DcHost h;
        char err[256];
        err[0] = 0;
        int rc = dc_build_from_file(&h, path, 0, err, sizeof err);
        CHECK(rc != 0, "case %u should have been rejected", i);
        CHECK(err[0] != 0, "case %u gave no message", i);
        if (rc == 0) dc_host_free(&h);
    }
    remove(path);
}

int main(void) {
    test_split();
    test_share();
    test_q16();
    test_blueprint_scalars();
    test_blueprint_errors();
    if (fails) { printf("test_units: %d FAILURES\n", fails); return 1; }
    printf("test_units: PASS\n");
    return 0;
}
