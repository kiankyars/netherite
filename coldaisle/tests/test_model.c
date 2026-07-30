/* test_model - the feedback loops, each provoked on purpose.
 *
 * Every case builds a blueprint that makes exactly one constraint bind, runs
 * it, and asserts both the limiter the simulator reports and the physical
 * consequence (clock, temperature, backlog, throughput). Conservation is
 * checked after every case, so no loop can buy throughput by losing work.
 * Throughput is compared as a rate, never as a cumulative count, because the
 * cases do not all run for the same number of ticks. */
#include "core/dc_tick.h"
#include "host/dc_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("  (%s:%d)\n", __FILE__, __LINE__); ++fails; } } while (0)

#define BP "/tmp/coldaisle_model.dcb"

/* One room, four racks, one stage. Every field a test wants to squeeze is a
 * parameter, so no line ever carries a duplicate key. */
typedef struct {
    const char *cool_kw;     /* room cooling budget */
    const char *pdu_kw;      /* per-rack PDU */
    const char *uplink_gbps; /* per-rack uplink */
    int nodes_per_rack;
    int rps;
} Cfg;

static Cfg cfg_default(void) {
    Cfg c;
    c.cool_kw = "1000";
    c.pdu_kw = "100";
    c.uplink_gbps = "100";
    c.nodes_per_rack = 4;
    c.rps = 1200;             /* capacity is 16 nodes x 10 req/tick = 1600 rps */
    return c;
}

static void write_bp(const Cfg *c) {
    FILE *f = fopen(BP, "w");
    if (!f) { printf("FAIL: cannot write %s\n", BP); exit(1); }
    fprintf(f, "room hall power_cap_kw=1000 cool_cap_kw=%s supply_c=22 spine_gbps=1000\n",
            c->cool_kw);
    fprintf(f, "rackgroup row room=hall racks=4 pdu_cap_kw=%s uplink_gbps=%s"
               " recirc=0.10 airflow_kw_per_c=2.0\n", c->pdu_kw, c->uplink_gbps);
    fprintf(f, "nodetype srv cap_wu=300 idle_w=200 dyn_w=800 queue_max=256"
               " txbl_max_kib=4096 rja_c_per_kw=25 t_throttle_c=85 t_max_c=100\n");
    fprintf(f, "stage only wu=30 bytes_kib=16 next=egress slo_ms=500\n");
    fprintf(f, "fill rackgroup=row type=srv stage=only per_rack=%d\n", c->nodes_per_rack);
    fprintf(f, "load stage=only rps=%d\n", c->rps);
    fclose(f);
}

typedef struct {
    double done_rps, offered_rps;
    i64 dropped;
    i64 lim[DC_LIM_COUNT];
    i32 min_clock, max_temp_mc, supply_mc;
    i64 backlog_kib, queued;
    int dark_racks;
} Obs;

static Obs run_cfg(const Cfg *c, i64 ticks) {
    write_bp(c);
    DcHost h;
    char err[512];
    if (dc_build_from_file(&h, BP, 0, err, sizeof err)) {
        printf("FAIL: build: %s\n", err);
        exit(1);
    }
    dc_run_cpu(&h.sim, ticks);
    if (dc_audit(&h, err, sizeof err)) {
        printf("FAIL: conservation broken: %s\n", err);
        ++fails;
    }
    const DcSim *s = &h.sim;
    Obs o;
    memset(&o, 0, sizeof o);
    o.done_rps = (double)s->m_completed[0] / (double)ticks * DC_TICKS_PER_SEC;
    o.offered_rps = (double)s->m_arrived[0] / (double)ticks * DC_TICKS_PER_SEC;
    o.dropped = s->m_dropped[0];
    for (int k = 0; k < DC_LIM_COUNT; ++k) o.lim[k] = s->m_lim[k];
    o.min_clock = DC_Q16;
    o.max_temp_mc = -100000;
    for (i32 i = 0; i < s->nnode; ++i) {
        if (s->clock_q16[i] < o.min_clock) o.min_clock = s->clock_q16[i];
        if (s->temp_mc[i] > o.max_temp_mc) o.max_temp_mc = s->temp_mc[i];
        o.backlog_kib += s->txbl_kib[i];
        o.queued += s->queue_req[i];
    }
    o.supply_mc = s->room_supply_now_mc[0];
    for (i32 r = 0; r < s->nrack; ++r) o.dark_racks += s->rack_dark[r] ? 1 : 0;
    dc_host_free(&h);
    remove(BP);
    return o;
}

/* Baseline: 1600 rps of capacity against 1200 rps of load, every capacity
 * generous. Nothing may bind and nothing may be lost. */
static Obs test_baseline(void) {
    Cfg c = cfg_default();
    Obs o = run_cfg(&c, 400);
    CHECK(o.dropped == 0, "baseline dropped %lld requests", (long long)o.dropped);
    CHECK(o.done_rps > 1150, "baseline served %.0f rps of 1200 offered", o.done_rps);
    CHECK(o.min_clock == DC_Q16, "baseline throttled to %d", o.min_clock);
    CHECK(o.lim[DC_LIM_POWER] == 0, "baseline reported power limiting");
    CHECK(o.lim[DC_LIM_THERMAL] == 0, "baseline reported thermal limiting");
    CHECK(o.lim[DC_LIM_NETWORK] == 0, "baseline reported network limiting");
    CHECK(o.dark_racks == 0, "baseline tripped a breaker");
    CHECK(o.max_temp_mc < 85000, "baseline node reached %d mC", o.max_temp_mc);
    return o;
}

/* A PDU that cannot feed its rack: the clock comes down, POWER is reported,
 * throughput falls, and the breaker holds (capping is not tripping). */
static void test_power(void) {
    Cfg c = cfg_default();
    /* Load at nameplate and a PDU that cannot cover even half of it. At 75% of
     * nameplate a mild cap changes nothing: the clock settles a little lower,
     * dynamic power falls with clock^2, and the hall still serves every
     * request. To make POWER the binding constraint the cap has to bite below
     * the offered rate. */
    c.rps = 1600;
    /* 2.0 kW against a rack that wants 4.0 kW flat out. Tighter than this and
     * the transient from a cold start sits over the breaker threshold long
     * enough to latch, which is test_breaker's job, not this one. */
    Obs unc = run_cfg(&c, 400);       /* same load, generous PDU: the control */
    c.pdu_kw = "2.0";
    Obs o = run_cfg(&c, 400);
    CHECK(o.min_clock < DC_Q16, "undersized PDU did not throttle the clock");
    CHECK(o.lim[DC_LIM_POWER] > 0, "undersized PDU reported no power limiting");
    CHECK(unc.lim[DC_LIM_POWER] == 0, "the control run was itself power limited");
    CHECK(o.done_rps < unc.done_rps * 0.95, "capped PDU served %.0f rps, uncapped %.0f",
          o.done_rps, unc.done_rps);
    CHECK(o.dark_racks == 0, "power capping should hold the breaker, not trip it");
}

/* Cooling far below the heat load: supply air rises, dies pass their throttle
 * point, THERMAL is reported, and the loop settles instead of running away. */
static void test_thermal(Obs base) {
    Cfg c = cfg_default();
    c.cool_kw = "3";                    /* the hall makes about 13 kW */
    Obs o = run_cfg(&c, 600);
    CHECK(o.supply_mc > 22000, "supply air stayed at %d mC with cooling short", o.supply_mc);
    CHECK(o.max_temp_mc > 85000, "no node passed its 85 C throttle point (max %d mC)",
          o.max_temp_mc);
    CHECK(o.lim[DC_LIM_THERMAL] > 0, "hot hall reported no thermal limiting");
    CHECK(o.min_clock < DC_Q16, "hot hall did not throttle the clock");
    CHECK(o.done_rps < base.done_rps * 0.95, "hot hall served %.0f rps, baseline %.0f",
          o.done_rps, base.done_rps);
    CHECK(o.max_temp_mc < 200000, "thermal runaway to %d mC", o.max_temp_mc);
}

/* A 50 mbps uplink against 16 KiB per request: the output backlog fills, nodes
 * stall on NETWORK, and requests pile up in queues rather than vanishing. */
static void test_network(Obs base) {
    Cfg c = cfg_default();
    /* 120 req/tick x 16 KiB spread over 4 racks is 480 KiB/tick per uplink,
     * so the link has to be under that to saturate: 10 mbps is 122 KiB/tick. */
    c.uplink_gbps = "0.01";
    Obs o = run_cfg(&c, 400);
    CHECK(o.lim[DC_LIM_NETWORK] > 0, "saturated uplink reported no network limiting");
    CHECK(o.backlog_kib > 0, "saturated uplink left no output backlog");
    CHECK(o.done_rps < base.done_rps * 0.95, "saturated uplink served %.0f rps, baseline %.0f",
          o.done_rps, base.done_rps);
    CHECK(o.queued > 0, "backpressure did not back the queues up");
}

/* Four 1 kW nodes on a 0.4 kW PDU sit over 120% of the breaker rating, so
 * after DC_BREAKER_TICKS every rack latches dark and stays dark. */
static void test_breaker(Obs base) {
    Cfg c = cfg_default();
    c.pdu_kw = "0.4";
    Obs o = run_cfg(&c, 200);
    CHECK(o.dark_racks == 4, "%d of 4 racks tripped", o.dark_racks);
    CHECK(o.min_clock == 0, "dark rack still had clock %d", o.min_clock);
    /* The rack serves normally for the DC_BREAKER_TICKS the breaker takes to
     * decide, so the run average is not zero; what must hold is that dark
     * node-ticks dominate and throughput collapses against the baseline. */
    i64 total = 0;
    for (int k = 0; k < DC_LIM_COUNT; ++k) total += o.lim[k];
    CHECK(o.lim[DC_LIM_DARK] * 2 > total, "only %lld of %lld node-ticks were dark",
          (long long)o.lim[DC_LIM_DARK], (long long)total);
    CHECK(o.done_rps < base.done_rps * 0.4, "a tripped datacenter served %.0f rps,"
          " baseline %.0f", o.done_rps, base.done_rps);
}

/* Monotone in cooling: more capacity can never serve less. The whole
 * cooling-sizing exercise rests on this. */
static void test_cooling_monotone(void) {
    static const char *caps[] = { "3", "5", "8", "12", "20", "1000" };
    double prev = -1.0;
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; ++i) {
        Cfg c = cfg_default();
        c.cool_kw = caps[i];
        Obs o = run_cfg(&c, 600);
        CHECK(o.done_rps >= prev - 0.5, "cooling %s kW served %.1f rps, less than the step below (%.1f)",
              caps[i], o.done_rps, prev);
        prev = o.done_rps;
    }
}

/* Load well under capacity: everything offered comes out, and idle node-ticks
 * dominate so the simulator can say "you are demand-limited". */
static void test_idle_is_reported(void) {
    Cfg c = cfg_default();
    c.rps = 100;
    Obs o = run_cfg(&c, 300);
    CHECK(o.lim[DC_LIM_IDLE] > o.lim[DC_LIM_COMPUTE],
          "light load reported %lld idle vs %lld compute",
          (long long)o.lim[DC_LIM_IDLE], (long long)o.lim[DC_LIM_COMPUTE]);
    CHECK(o.dropped == 0, "light load dropped %lld", (long long)o.dropped);
    CHECK(o.done_rps > 95 && o.done_rps < 105, "light load served %.1f rps of 100", o.done_rps);
}

/* Overload: offered above capacity must show COMPUTE, must drop the excess,
 * and must still serve roughly nameplate. */
static void test_overload(void) {
    Cfg c = cfg_default();
    c.rps = 4000;                       /* capacity is 1600 */
    Obs o = run_cfg(&c, 400);
    CHECK(o.lim[DC_LIM_COMPUTE] > o.lim[DC_LIM_IDLE], "overload reported %lld compute vs %lld idle",
          (long long)o.lim[DC_LIM_COMPUTE], (long long)o.lim[DC_LIM_IDLE]);
    CHECK(o.dropped > 0, "overload dropped nothing");
    CHECK(o.done_rps > 1500 && o.done_rps < 1700, "overload served %.0f rps, nameplate 1600",
          o.done_rps);
}

/* Two stages in series: the slow stage sets the rate and holds the backlog. */
static void test_pipeline_bottleneck(void) {
    FILE *f = fopen(BP, "w");
    if (!f) { printf("FAIL: cannot write %s\n", BP); exit(1); }
    fprintf(f,
        "room hall power_cap_kw=1000 cool_cap_kw=1000 supply_c=22 spine_gbps=1000\n"
        "rackgroup row room=hall racks=2 pdu_cap_kw=100 uplink_gbps=100 recirc=0.1 airflow_kw_per_c=2\n"
        "nodetype srv cap_wu=300 idle_w=200 dyn_w=800 queue_max=256 txbl_max_kib=8192"
        " rja_c_per_kw=25 t_throttle_c=85 t_max_c=100\n"
        "stage fast wu=10  bytes_kib=8 next=slow   slo_ms=500\n"   /* 30 req/tick/node */
        "stage slow wu=300 bytes_kib=8 next=egress slo_ms=500\n"   /* 1 req/tick/node */
        "fill rackgroup=row type=srv stage=fast per_rack=4\n"
        "fill rackgroup=row type=srv stage=slow per_rack=4\n"
        "load stage=fast rps=2000\n");
    fclose(f);
    DcHost h;
    char err[512];
    if (dc_build_from_file(&h, BP, 0, err, sizeof err)) {
        printf("FAIL: build: %s\n", err);
        exit(1);
    }
    dc_run_cpu(&h.sim, 400);
    if (dc_audit(&h, err, sizeof err)) { printf("FAIL: %s\n", err); ++fails; }
    const DcSim *s = &h.sim;
    /* 8 slow nodes at 1 request per tick is 80 rps; that is the ceiling. */
    double done_rps = (double)s->m_completed[0] / (double)s->m_ticks[0] * DC_TICKS_PER_SEC;
    CHECK(done_rps > 70 && done_rps < 90, "series pipeline served %.0f rps, expected ~80", done_rps);
    i64 q_fast = 0, q_slow = 0;
    for (i32 i = 0; i < s->nnode; ++i)
        (s->node_svc[i] == 0 ? &q_fast : &q_slow)[0] += s->queue_req[i];
    CHECK(q_slow > q_fast, "backlog sat in front of the fast stage (%lld) not the slow one (%lld)",
          (long long)q_fast, (long long)q_slow);
    dc_host_free(&h);
    remove(BP);
}

/* A repeated key is a typo, not an override: the parser must refuse it. */
static void test_duplicate_key_rejected(void) {
    FILE *f = fopen(BP, "w");
    if (!f) { printf("FAIL: cannot write %s\n", BP); exit(1); }
    fprintf(f, "room hall power_cap_kw=10 cool_cap_kw=10 spine_gbps=1 cool_cap_kw=999\n");
    fclose(f);
    DcHost h;
    char err[512];
    err[0] = 0;
    int rc = dc_build_from_file(&h, BP, 0, err, sizeof err);
    CHECK(rc != 0, "repeated cool_cap_kw was accepted");
    CHECK(strstr(err, "cool_cap_kw") != NULL, "error did not name the key: %s", err);
    if (rc == 0) dc_host_free(&h);
    remove(BP);
}

int main(void) {
    Obs base = test_baseline();
    test_power();
    test_thermal(base);
    test_network(base);
    test_breaker(base);
    test_cooling_monotone();
    test_idle_is_reported();
    test_overload();
    test_pipeline_bottleneck();
    test_duplicate_key_rejected();
    if (fails) { printf("test_model: %d FAILURES\n", fails); return 1; }
    printf("test_model: PASS\n");
    return 0;
}
