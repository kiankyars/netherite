/* dc_build.c - blueprint parser and state builder.
 *
 * Blueprint grammar (one declaration per line, `#` starts a comment):
 *
 *   room      NAME  power_cap_kw= cool_cap_kw= supply_c= spine_gbps=
 *   rackgroup NAME  room= racks= pdu_cap_kw= uplink_gbps= recirc= airflow_kw_per_c=
 *                   [vgrad_c=]  (top-of-rack inlet penalty, default 0)
 *   nodetype  NAME  cap_wu= idle_w= dyn_w= queue_max= txbl_max_kib=
 *                   rja_c_per_kw= t_throttle_c= t_max_c=
 *   stage     NAME  wu= bytes_kib= next=NAME|egress slo_ms=
 *   fill      rackgroup= type= stage= per_rack=
 *   load      stage= rps=
 *   fault     at_s= target=NAME KEY=VALUE   (one capacity change, mid-run)
 *   sweep     KEY target=NAME v1 v2 ...       (one axis, KEY as below)
 *
 * sweep KEY is one of cool_cap_kw, power_cap_kw, spine_gbps (target = room),
 * pdu_cap_kw, uplink_gbps (target = rackgroup), or rps (target = stage).
 * Every batch is otherwise identical, which is what makes one launch a sweep.
 *
 * Values accept decimals (recirc=0.15) and are converted to the integer units
 * in SPEC.md at parse time; nothing downstream sees a fraction. */
#include "dc_host.h"
#include "core/dc_tick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXROOM 64
#define MAXGROUP 256
#define MAXTYPE 64
#define MAXSTAGE 64
#define MAXFILL 512
#define MAXSWEEP 256

typedef struct { char name[DC_NAME_MAX]; i64 pwr_mw, cool_mw, supply_mc, spine_kib; } BRoom;
typedef struct { char name[DC_NAME_MAX]; int room; i32 racks;
                 i64 pdu_mw, uplink_kib; i32 recirc_q16, airflow, vgrad_mc; } BGroup;
typedef struct { char name[DC_NAME_MAX]; i32 cap_wu, idle_mw, dyn_mw, queue_max,
                 txbl_max, rja, t_thr_mc, t_max_mc; } BType;
typedef struct { char name[DC_NAME_MAX]; i32 wu, bytes_kib, slo_ticks;
                 char next[DC_NAME_MAX]; int next_idx; i64 rate_milli; } BStage;
typedef struct { int group, type, stage, per_rack; } BFill;
typedef struct { i64 tick; char target[DC_NAME_MAX], key[DC_NAME_MAX]; i64 value_milli; } BFaultDecl;

typedef struct {
    BRoom room[MAXROOM];   int nroom;
    BGroup grp[MAXGROUP];  int ngrp;
    BType type[MAXTYPE];   int ntype;
    BStage stage[MAXSTAGE]; int nstage;
    BFill fill[MAXFILL];   int nfill;
    BFaultDecl fault[DC_MAX_FAULT]; int nfault;
    char sweep_key[DC_NAME_MAX], sweep_target[DC_NAME_MAX];
    i64 sweep_val[MAXSWEEP]; int nsweep;    /* value * 1000 in the file's unit */
} Blueprint;

/* ---- scalars ----------------------------------------------------------- */
/* Parse a decimal into an integer scaled by `scale`, e.g. ("2.5", 1000) ->
 * 2500. Rejects anything that is not [-]digits[.digits]. */
static int parse_scaled(const char *v, i64 scale, i64 *out) {
    int neg = 0;
    const char *p = v;
    if (*p == '-') { neg = 1; ++p; }
    if (!*p) return 0;
    i64 whole = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; ++p) { whole = whole * 10 + (*p - '0'); ++digits; }
    if (!digits) return 0;
    i64 frac = 0, fscale = 1;
    if (*p == '.') {
        ++p;
        for (; *p >= '0' && *p <= '9'; ++p) {
            if (fscale <= scale) { frac = frac * 10 + (*p - '0'); fscale *= 10; }
        }
    }
    if (*p) return 0;
    i64 val = whole * scale + (frac * scale) / fscale;
    *out = neg ? -val : val;
    return 1;
}

static i64 gbps_to_kib_per_tick(i64 gbps_milli) {
    /* gbps_milli is gbps*1000. bytes/s = gbps*1e9/8. KiB/tick = that /1024/TPS. */
    return (gbps_milli * 125000000LL) / 1000 / 1024 / DC_TICKS_PER_SEC;
}

static int find_name(const void *base, int n, size_t stride, const char *name) {
    for (int i = 0; i < n; ++i)
        if (!strcmp((const char *)base + (size_t)i * stride, name)) return i;
    return -1;
}

/* ---- line tokenizer ---------------------------------------------------- */
#define MAXTOK 64
typedef struct { char *tok[MAXTOK]; int n; } Toks;

static void tokenize(char *line, Toks *t) {
    t->n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p || *p == '#') break;
        if (t->n >= MAXTOK) break;
        t->tok[t->n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (*p) *p++ = 0;
    }
}

/* key=value lookup among tokens [from..n). */
static const char *kv(const Toks *t, int from, const char *key) {
    size_t kl = strlen(key);
    for (int i = from; i < t->n; ++i)
        if (!strncmp(t->tok[i], key, kl) && t->tok[i][kl] == '=')
            return t->tok[i] + kl + 1;
    return NULL;
}

/* A repeated key is always a mistake and silently taking the first one hides
 * it, so reject the line instead. Returns the offending key or NULL. */
static const char *dup_key(const Toks *t) {
    for (int i = 0; i < t->n; ++i) {
        const char *eq = strchr(t->tok[i], '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - t->tok[i]);
        for (int j = i + 1; j < t->n; ++j)
            if (!strncmp(t->tok[i], t->tok[j], kl) && t->tok[j][kl] == '=')
                return t->tok[i];
    }
    return NULL;
}

#define FAIL(...) do { snprintf(err, errcap, __VA_ARGS__); return -1; } while (0)
#define NEED(dst, key, scale) do {                                           \
        const char *v_ = kv(&t, 2, key);                                     \
        if (!v_) FAIL("line %d: %s needs %s=", lineno, t.tok[0], key);        \
        i64 x_;                                                              \
        if (!parse_scaled(v_, (scale), &x_)) FAIL("line %d: %s= is not a number: %s", lineno, key, v_); \
        (dst) = x_;                                                          \
    } while (0)
#define OPT(dst, key, scale, dflt) do {                                      \
        const char *v_ = kv(&t, 2, key);                                     \
        i64 x_ = (dflt);                                                     \
        if (v_ && !parse_scaled(v_, (scale), &x_)) FAIL("line %d: %s= is not a number: %s", lineno, key, v_); \
        (dst) = x_;                                                          \
    } while (0)

static int parse_blueprint(const char *path, Blueprint *bp, char *err, int errcap) {
    FILE *f = fopen(path, "r");
    if (!f) FAIL("cannot open %s", path);
    memset(bp, 0, sizeof *bp);
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineno;
        Toks t;
        tokenize(line, &t);
        if (t.n == 0) continue;
        const char *what = t.tok[0];
        const char *dk = dup_key(&t);
        if (dk) FAIL("line %d: repeated key in %.*s", lineno,
                     (int)(strchr(dk, '=') - dk), dk);

        if (!strcmp(what, "room")) {
            if (t.n < 2) FAIL("line %d: room needs a name", lineno);
            if (bp->nroom >= MAXROOM) FAIL("line %d: too many rooms", lineno);
            BRoom *r = &bp->room[bp->nroom++];
            snprintf(r->name, DC_NAME_MAX, "%s", t.tok[1]);
            i64 kw, ckw, sc, gb;
            NEED(kw, "power_cap_kw", 1000);
            NEED(ckw, "cool_cap_kw", 1000);
            OPT(sc, "supply_c", 1000, 24000);
            NEED(gb, "spine_gbps", 1000);
            r->pwr_mw = kw * 1000;          /* kw*1000 -> mW */
            r->cool_mw = ckw * 1000;
            r->supply_mc = sc;
            r->spine_kib = gbps_to_kib_per_tick(gb);
        } else if (!strcmp(what, "rackgroup")) {
            if (t.n < 2) FAIL("line %d: rackgroup needs a name", lineno);
            if (bp->ngrp >= MAXGROUP) FAIL("line %d: too many rackgroups", lineno);
            BGroup *g = &bp->grp[bp->ngrp++];
            snprintf(g->name, DC_NAME_MAX, "%s", t.tok[1]);
            const char *rn = kv(&t, 2, "room");
            if (!rn) FAIL("line %d: rackgroup needs room=", lineno);
            g->room = find_name(bp->room, bp->nroom, sizeof(BRoom), rn);
            if (g->room < 0) FAIL("line %d: unknown room %s", lineno, rn);
            i64 racks, pdu, up, rec, af;
            NEED(racks, "racks", 1);
            NEED(pdu, "pdu_cap_kw", 1000);
            NEED(up, "uplink_gbps", 1000);
            OPT(rec, "recirc", DC_Q16, 0);
            NEED(af, "airflow_kw_per_c", 1000);
            i64 vg;
            OPT(vg, "vgrad_c", 1000, 0);
            if (racks <= 0) FAIL("line %d: racks must be positive", lineno);
            g->racks = (i32)racks;
            g->pdu_mw = pdu * 1000;
            g->uplink_kib = gbps_to_kib_per_tick(up);
            g->recirc_q16 = (i32)rec;
            g->airflow = (i32)af;           /* kw/C*1000 == mW per mC */
            g->vgrad_mc = (i32)vg;
        } else if (!strcmp(what, "nodetype")) {
            if (t.n < 2) FAIL("line %d: nodetype needs a name", lineno);
            if (bp->ntype >= MAXTYPE) FAIL("line %d: too many nodetypes", lineno);
            BType *y = &bp->type[bp->ntype++];
            snprintf(y->name, DC_NAME_MAX, "%s", t.tok[1]);
            i64 cap, idle, dyn, qmax, txbl, rja, thr, tmax;
            NEED(cap, "cap_wu", 1);
            NEED(idle, "idle_w", 1000);
            NEED(dyn, "dyn_w", 1000);
            OPT(qmax, "queue_max", 1, 1024);
            OPT(txbl, "txbl_max_kib", 1, 65536);
            NEED(rja, "rja_c_per_kw", 1);
            NEED(thr, "t_throttle_c", 1000);
            NEED(tmax, "t_max_c", 1000);
            y->cap_wu = (i32)cap; y->idle_mw = (i32)idle; y->dyn_mw = (i32)dyn;
            y->queue_max = (i32)qmax; y->txbl_max = (i32)txbl; y->rja = (i32)rja;
            y->t_thr_mc = (i32)thr; y->t_max_mc = (i32)tmax;
            if (y->t_max_mc <= y->t_thr_mc) FAIL("line %d: t_max_c must exceed t_throttle_c", lineno);
        } else if (!strcmp(what, "stage")) {
            if (t.n < 2) FAIL("line %d: stage needs a name", lineno);
            if (bp->nstage >= MAXSTAGE) FAIL("line %d: too many stages", lineno);
            BStage *g = &bp->stage[bp->nstage++];
            snprintf(g->name, DC_NAME_MAX, "%s", t.tok[1]);
            i64 wu, by, slo;
            NEED(wu, "wu", 1);
            NEED(by, "bytes_kib", 1);
            OPT(slo, "slo_ms", 1, 0);
            const char *nx = kv(&t, 2, "next");
            if (!nx) FAIL("line %d: stage needs next=", lineno);
            snprintf(g->next, DC_NAME_MAX, "%s", nx);
            g->wu = (i32)wu; g->bytes_kib = (i32)by;
            g->slo_ticks = (i32)(slo / DC_TICK_MS);
        } else if (!strcmp(what, "fill")) {
            if (bp->nfill >= MAXFILL) FAIL("line %d: too many fills", lineno);
            BFill *fl = &bp->fill[bp->nfill++];
            const char *gn = kv(&t, 1, "rackgroup"), *tn = kv(&t, 1, "type"),
                       *sn = kv(&t, 1, "stage"), *pr = kv(&t, 1, "per_rack");
            if (!gn || !tn || !sn || !pr)
                FAIL("line %d: fill needs rackgroup= type= stage= per_rack=", lineno);
            fl->group = find_name(bp->grp, bp->ngrp, sizeof(BGroup), gn);
            fl->type = find_name(bp->type, bp->ntype, sizeof(BType), tn);
            fl->stage = find_name(bp->stage, bp->nstage, sizeof(BStage), sn);
            if (fl->group < 0) FAIL("line %d: unknown rackgroup %s", lineno, gn);
            if (fl->type < 0) FAIL("line %d: unknown type %s", lineno, tn);
            if (fl->stage < 0) FAIL("line %d: unknown stage %s", lineno, sn);
            i64 n;
            if (!parse_scaled(pr, 1, &n) || n <= 0)
                FAIL("line %d: per_rack must be positive", lineno);
            fl->per_rack = (int)n;
        } else if (!strcmp(what, "load")) {
            const char *sn = kv(&t, 1, "stage"), *rp = kv(&t, 1, "rps");
            if (!sn || !rp) FAIL("line %d: load needs stage= rps=", lineno);
            int si = find_name(bp->stage, bp->nstage, sizeof(BStage), sn);
            if (si < 0) FAIL("line %d: unknown stage %s", lineno, sn);
            i64 rps;
            if (!parse_scaled(rp, 1000, &rps)) FAIL("line %d: rps is not a number", lineno);
            /* requests/tick*1000 = rps*1000/TPS, and rps arrived scaled by 1000 */
            bp->stage[si].rate_milli = rps / DC_TICKS_PER_SEC;
        } else if (!strcmp(what, "fault")) {
            if (bp->nfault >= DC_MAX_FAULT) FAIL("line %d: too many faults", lineno);
            const char *tg = kv(&t, 1, "target");
            const char *at = kv(&t, 1, "at_s");
            if (!tg || !at) FAIL("line %d: fault needs at_s= target=", lineno);
            i64 secs;
            if (!parse_scaled(at, 1000, &secs)) FAIL("line %d: at_s is not a number", lineno);
            BFaultDecl *fd = &bp->fault[bp->nfault++];
            fd->tick = secs * DC_TICKS_PER_SEC / 1000;
            snprintf(fd->target, DC_NAME_MAX, "%s", tg);
            fd->key[0] = 0;
            for (int i = 1; i < t.n; ++i) {
                const char *eq = strchr(t.tok[i], '=');
                if (!eq) continue;
                if (!strncmp(t.tok[i], "at_s=", 5) || !strncmp(t.tok[i], "target=", 7)) continue;
                if (fd->key[0]) FAIL("line %d: one capacity per fault line", lineno);
                snprintf(fd->key, DC_NAME_MAX, "%.*s", (int)(eq - t.tok[i]), t.tok[i]);
                if (!parse_scaled(eq + 1, 1000, &fd->value_milli))
                    FAIL("line %d: %s is not a number", lineno, fd->key);
            }
            if (!fd->key[0]) FAIL("line %d: fault needs a KEY=VALUE to change", lineno);
        } else if (!strcmp(what, "sweep")) {
            if (t.n < 3) FAIL("line %d: sweep needs KEY target=NAME v1 [v2 ...]", lineno);
            if (bp->nsweep) FAIL("line %d: only one sweep axis is supported", lineno);
            snprintf(bp->sweep_key, DC_NAME_MAX, "%s", t.tok[1]);
            const char *tg = kv(&t, 2, "target");
            if (!tg) FAIL("line %d: sweep needs target=", lineno);
            snprintf(bp->sweep_target, DC_NAME_MAX, "%s", tg);
            for (int i = 2; i < t.n; ++i) {
                if (strchr(t.tok[i], '=')) continue;
                if (bp->nsweep >= MAXSWEEP) FAIL("line %d: too many sweep values", lineno);
                i64 v;
                if (!parse_scaled(t.tok[i], 1000, &v))
                    FAIL("line %d: sweep value is not a number: %s", lineno, t.tok[i]);
                bp->sweep_val[bp->nsweep++] = v;
            }
            if (!bp->nsweep) FAIL("line %d: sweep needs at least one value", lineno);
        } else {
            FAIL("line %d: unknown declaration %s", lineno, what);
        }
    }
    fclose(f);

    for (int i = 0; i < bp->nstage; ++i) {
        if (!strcmp(bp->stage[i].next, "egress")) { bp->stage[i].next_idx = DC_EGRESS; continue; }
        int nx = find_name(bp->stage, bp->nstage, sizeof(BStage), bp->stage[i].next);
        if (nx < 0) FAIL("stage %s: unknown next=%s", bp->stage[i].name, bp->stage[i].next);
        bp->stage[i].next_idx = nx;
    }
    if (!bp->nroom) FAIL("blueprint declares no room");
    if (!bp->nfill) FAIL("blueprint places no nodes (needs a fill line)");
    return 0;
}

/* ---- allocation ------------------------------------------------------- */
static void *halloc(DcHost *h, size_t bytes) {
    void *p = calloc(1, bytes ? bytes : 1);
    if (!p) { fprintf(stderr, "coldaisle: out of memory\n"); exit(1); }
    if (h->nalloc < DC_MAX_ALLOC) h->allocs[h->nalloc++] = p;
    return p;
}
#define A32(n) (i32 *)halloc(h, sizeof(i32) * (size_t)(n))
#define A64(n) (i64 *)halloc(h, sizeof(i64) * (size_t)(n))

void dc_host_free(DcHost *h) {
    for (int i = 0; i < h->nalloc; ++i) free(h->allocs[i]);
    h->nalloc = 0;
    memset(&h->sim, 0, sizeof h->sim);
}

/* Apply the sweep axis to batch b. */
static int apply_sweep(DcHost *h, const Blueprint *bp, i32 b, char *err, int errcap) {
    if (!bp->nsweep) return 0;
    DcSim *s = &h->sim;
    i64 v = bp->sweep_val[b % bp->nsweep];
    const char *k = bp->sweep_key, *tg = bp->sweep_target;
    int hit = 0;
    for (i32 m = 0; m < s->nroom; ++m) {
        if (strcmp(h->room_name[m], tg)) continue;
        i32 mi = dc_mi(s, b, m);
        if (!strcmp(k, "cool_cap_kw")) { s->room_cool_cap_mw[mi] = v * 1000; hit = 1; }
        else if (!strcmp(k, "power_cap_kw")) { s->room_pwr_cap_mw[mi] = v * 1000; hit = 1; }
        else if (!strcmp(k, "spine_gbps")) { s->room_spine_kib[mi] = gbps_to_kib_per_tick(v); hit = 1; }
        else if (!strcmp(k, "supply_c")) { s->room_supply_mc[mi] = (i32)v; hit = 1; }
    }
    for (i32 r = 0; r < s->nrack; ++r) {
        if (strcmp(h->rack_name[r], tg)) continue;
        i32 ri = dc_ri(s, b, r);
        if (!strcmp(k, "pdu_cap_kw")) { s->rack_pdu_cap_mw[ri] = v * 1000; hit = 1; }
        else if (!strcmp(k, "uplink_gbps")) { s->rack_uplink_kib[ri] = gbps_to_kib_per_tick(v); hit = 1; }
    }
    for (i32 g = 0; g < s->nsvc; ++g) {
        if (strcmp(h->svc_name[g], tg)) continue;
        if (!strcmp(k, "rps")) { s->svc_rate_milli[dc_si(s, b, g)] = (i32)(v / DC_TICKS_PER_SEC); hit = 1; }
    }
    if (!hit) FAIL("sweep %s target=%s matched nothing", k, tg);
    h->sweep_value_milli[b] = v;
    return 0;
}


/* Resolve fault declarations into (kind, index, value) in state units. Rack
 * targets name a rackgroup and expand to every rack in it. */
static int resolve_faults(DcHost *h, const Blueprint *bp, char *err, int errcap) {
    DcSim *s = &h->sim;
    for (int i = 0; i < bp->nfault; ++i) {
        const BFaultDecl *fd = &bp->fault[i];
        int hit = 0;
        for (i32 m = 0; m < s->nroom; ++m) {
            if (strcmp(h->room_name[m], fd->target)) continue;
            int kind = -1;
            i64 val = 0;
            if (!strcmp(fd->key, "cool_cap_kw")) { kind = DC_FAULT_ROOM_COOL; val = fd->value_milli * 1000; }
            else if (!strcmp(fd->key, "power_cap_kw")) { kind = DC_FAULT_ROOM_POWER; val = fd->value_milli * 1000; }
            else if (!strcmp(fd->key, "spine_gbps")) { kind = DC_FAULT_ROOM_SPINE; val = gbps_to_kib_per_tick(fd->value_milli); }
            if (kind < 0) continue;
            if (h->nfault >= DC_MAX_FAULT) FAIL("too many resolved faults");
            DcFault *f = &h->fault[h->nfault++];
            f->tick = fd->tick; f->kind = kind; f->index = m; f->value = val;
            snprintf(f->what, sizeof f->what, "%s %s", fd->target, fd->key);
            hit = 1;
        }
        for (i32 r = 0; r < s->nrack; ++r) {
            if (strcmp(h->rack_name[r], fd->target)) continue;
            int kind = -1;
            i64 val = 0;
            if (!strcmp(fd->key, "pdu_cap_kw")) { kind = DC_FAULT_RACK_PDU; val = fd->value_milli * 1000; }
            else if (!strcmp(fd->key, "uplink_gbps")) { kind = DC_FAULT_RACK_UPLINK; val = gbps_to_kib_per_tick(fd->value_milli); }
            if (kind < 0) continue;
            if (h->nfault >= DC_MAX_FAULT) FAIL("too many resolved faults (a rackgroup fault expands per rack)");
            DcFault *f = &h->fault[h->nfault++];
            f->tick = fd->tick; f->kind = kind; f->index = r; f->value = val;
            snprintf(f->what, sizeof f->what, "%s %s", fd->target, fd->key);
            hit = 1;
        }
        for (i32 v = 0; v < s->nsvc; ++v) {
            if (strcmp(h->svc_name[v], fd->target) || strcmp(fd->key, "rps")) continue;
            if (h->nfault >= DC_MAX_FAULT) FAIL("too many resolved faults");
            DcFault *f = &h->fault[h->nfault++];
            f->tick = fd->tick; f->kind = DC_FAULT_SVC_RATE; f->index = v;
            f->value = fd->value_milli / DC_TICKS_PER_SEC;
            snprintf(f->what, sizeof f->what, "%s %s", fd->target, fd->key);
            hit = 1;
        }
        if (!hit) FAIL("fault target=%s %s= matched nothing", fd->target, fd->key);
    }
    return 0;
}

int dc_apply_faults(DcHost *h, i64 tick, FILE *log) {
    DcSim *s = &h->sim;
    int n = 0;
    for (int i = 0; i < h->nfault; ++i) {
        const DcFault *f = &h->fault[i];
        if (f->tick != tick) continue;
        for (i32 b = 0; b < s->nbatch; ++b) {
            switch (f->kind) {
            case DC_FAULT_ROOM_COOL:  s->room_cool_cap_mw[dc_mi(s, b, f->index)] = f->value; break;
            case DC_FAULT_ROOM_POWER: s->room_pwr_cap_mw[dc_mi(s, b, f->index)] = f->value; break;
            case DC_FAULT_ROOM_SPINE: s->room_spine_kib[dc_mi(s, b, f->index)] = f->value; break;
            case DC_FAULT_RACK_PDU:   s->rack_pdu_cap_mw[dc_ri(s, b, f->index)] = f->value; break;
            case DC_FAULT_RACK_UPLINK:s->rack_uplink_kib[dc_ri(s, b, f->index)] = f->value; break;
            case DC_FAULT_SVC_RATE:   s->svc_rate_milli[dc_si(s, b, f->index)] = (i32)f->value; break;
            default: break;
            }
        }
        ++n;
        if (log && (i == 0 || h->fault[i - 1].tick != tick || strcmp(h->fault[i - 1].what, f->what)))
            fprintf(log, "t=%.1fs fault: %s -> %lld\n", (double)tick / DC_TICKS_PER_SEC,
                    f->what, (long long)f->value);
    }
    return n;
}

int dc_build_from_file(DcHost *h, const char *path, int nbatch_override,
                       char *err, int errcap) {
    Blueprint bp;
    memset(h, 0, sizeof *h);
    if (parse_blueprint(path, &bp, err, errcap)) return -1;

    DcSim *s = &h->sim;
    s->nroom = bp.nroom;
    s->nsvc = bp.nstage;
    s->nbatch = nbatch_override > 0 ? nbatch_override
                                    : (bp.nsweep ? bp.nsweep : 1);

    /* rack and node counts, and the room-major rack order */
    i32 nrack = 0, nnode = 0;
    for (int m = 0; m < bp.nroom; ++m)
        for (int g = 0; g < bp.ngrp; ++g)
            if (bp.grp[g].room == m) nrack += bp.grp[g].racks;
    for (int g = 0; g < bp.ngrp; ++g) {
        int per = 0;
        for (int f = 0; f < bp.nfill; ++f) if (bp.fill[f].group == g) per += bp.fill[f].per_rack;
        nnode += per * bp.grp[g].racks;
    }
    if (!nnode) FAIL("blueprint places no nodes");
    s->nrack = nrack;
    s->nnode = nnode;

    i32 B = s->nbatch, N = nnode, R = nrack, M = s->nroom, V = s->nsvc;
    h->room_name = (char (*)[DC_NAME_MAX])halloc(h, (size_t)M * DC_NAME_MAX);
    h->svc_name = (char (*)[DC_NAME_MAX])halloc(h, (size_t)V * DC_NAME_MAX);
    h->rack_name = (char (*)[DC_NAME_MAX])halloc(h, (size_t)R * DC_NAME_MAX);
    h->sweep_value_milli = A64(B);

    s->node_rack = A32(N); s->node_svc = A32(N); s->node_slot = A32(N);
    s->node_cap_wu = A32(N); s->node_idle_mw = A32(N); s->node_dyn_mw = A32(N);
    s->node_queue_max = A32(N); s->node_txbl_max = A32(N);
    s->node_rja_mc_per_w = A32(N); s->node_t_thr_mc = A32(N); s->node_t_max_mc = A32(N);
    s->node_inlet_bias_mc = A32(N);
    s->rack_room = A32(R); s->rack_node_begin = A32(R); s->rack_node_end = A32(R);
    s->rack_recirc_q16 = A32(R); s->rack_airflow = A32(R);
    s->room_rack_begin = A32(M); s->room_rack_end = A32(M);
    s->svc_wu_per_req = A32(V); s->svc_bytes_kib = A32(V); s->svc_next = A32(V);
    s->svc_nnode = A32(V); s->svc_slo_ticks = A32(V);
    s->rack_pdu_cap_mw = A64((size_t)B * R); s->rack_uplink_kib = A64((size_t)B * R);
    s->room_pwr_cap_mw = A64((size_t)B * M); s->room_cool_cap_mw = A64((size_t)B * M);
    s->room_supply_mc = A32((size_t)B * M); s->room_spine_kib = A64((size_t)B * M);
    s->svc_rate_milli = A32((size_t)B * V);
    s->queue_req = A32((size_t)B * N); s->wu_acc = A32((size_t)B * N);
    s->arr_acc = A32((size_t)B * N); s->clock_q16 = A32((size_t)B * N);
    s->temp_mc = A32((size_t)B * N); s->txbl_kib = A32((size_t)B * N);
    s->served_req = A32((size_t)B * N); s->tx_kib = A32((size_t)B * N);
    s->deliv_req = A32((size_t)B * N); s->power_mw = A32((size_t)B * N);
    s->limiter = A32((size_t)B * N);
    s->rack_power_mw = A64((size_t)B * R); s->rack_heat_mw = A64((size_t)B * R);
    s->rack_tx_kib = A64((size_t)B * R); s->rack_scale_q16 = A32((size_t)B * R);
    s->rack_inlet_mc = A32((size_t)B * R); s->rack_trip_ticks = A32((size_t)B * R);
    s->rack_dark = A32((size_t)B * R);
    s->room_power_mw = A64((size_t)B * M); s->room_heat_mw = A64((size_t)B * M);
    s->room_tx_kib = A64((size_t)B * M); s->room_scale_q16 = A32((size_t)B * M);
    s->room_supply_now_mc = A32((size_t)B * M);
    s->svc_deliv = A64((size_t)B * V); s->svc_arrived = A64((size_t)B * V);
    s->m_arrived = A64(B); s->m_completed = A64(B); s->m_dropped = A64(B);
    s->m_energy_mwt = A64(B); s->m_served_wu = A64(B); s->m_slo_viol = A64(B);
    s->m_lim = A64((size_t)B * DC_LIM_COUNT);
    s->m_peak_temp_mc = A64(B); s->m_peak_supply_mc = A64(B); s->m_ticks = A64(B);

    for (int v = 0; v < V; ++v) {
        snprintf(h->svc_name[v], DC_NAME_MAX, "%s", bp.stage[v].name);
        s->svc_wu_per_req[v] = bp.stage[v].wu;
        s->svc_bytes_kib[v] = bp.stage[v].bytes_kib;
        s->svc_next[v] = bp.stage[v].next_idx;
        s->svc_slo_ticks[v] = bp.stage[v].slo_ticks;
    }
    for (int m = 0; m < M; ++m) snprintf(h->room_name[m], DC_NAME_MAX, "%s", bp.room[m].name);

    /* racks room-major, nodes rack-major: every reduction stays contiguous */
    i32 rack = 0, node = 0;
    for (int m = 0; m < M; ++m) {
        s->room_rack_begin[m] = rack;
        for (int g = 0; g < bp.ngrp; ++g) {
            if (bp.grp[g].room != m) continue;
            for (i32 k = 0; k < bp.grp[g].racks; ++k, ++rack) {
                s->rack_room[rack] = m;
                s->rack_recirc_q16[rack] = bp.grp[g].recirc_q16;
                s->rack_airflow[rack] = bp.grp[g].airflow;
                snprintf(h->rack_name[rack], DC_NAME_MAX, "%s", bp.grp[g].name);
                s->rack_node_begin[rack] = node;
                for (int f = 0; f < bp.nfill; ++f) {
                    if (bp.fill[f].group != g) continue;
                    const BType *y = &bp.type[bp.fill[f].type];
                    for (int q = 0; q < bp.fill[f].per_rack; ++q, ++node) {
                        s->node_rack[node] = rack;
                        s->node_svc[node] = bp.fill[f].stage;
                        s->node_cap_wu[node] = y->cap_wu;
                        s->node_idle_mw[node] = y->idle_mw;
                        s->node_dyn_mw[node] = y->dyn_mw;
                        s->node_queue_max[node] = y->queue_max;
                        s->node_txbl_max[node] = y->txbl_max;
                        s->node_rja_mc_per_w[node] = y->rja;
                        s->node_t_thr_mc[node] = y->t_thr_mc;
                        s->node_t_max_mc[node] = y->t_max_mc;
                    }
                }
                s->rack_node_end[rack] = node;
                {   /* bottom of the rack sees room air, top sees the gradient */
                    i32 first = s->rack_node_begin[rack], last = node - 1;
                    i32 span = last - first;
                    for (i32 q = first; q <= last; ++q)
                        s->node_inlet_bias_mc[q] = span > 0
                            ? (i32)((i64)bp.grp[g].vgrad_mc * (q - first) / span) : 0;
                }
                for (i32 bb = 0; bb < B; ++bb) {
                    s->rack_pdu_cap_mw[dc_ri(s, bb, rack)] = bp.grp[g].pdu_mw;
                    s->rack_uplink_kib[dc_ri(s, bb, rack)] = bp.grp[g].uplink_kib;
                }
            }
        }
        s->room_rack_end[m] = rack;
        for (i32 bb = 0; bb < B; ++bb) {
            i32 mi = dc_mi(s, bb, m);
            s->room_pwr_cap_mw[mi] = bp.room[m].pwr_mw;
            s->room_cool_cap_mw[mi] = bp.room[m].cool_mw;
            s->room_supply_mc[mi] = (i32)bp.room[m].supply_mc;
            s->room_spine_kib[mi] = bp.room[m].spine_kib;
        }
    }

    /* stage group sizes and each node's slot inside its group */
    for (i32 i = 0; i < N; ++i) {
        i32 v = s->node_svc[i];
        s->node_slot[i] = s->svc_nnode[v]++;
    }
    for (int v = 0; v < V; ++v)
        if (!s->svc_nnode[v]) FAIL("stage %s has no nodes", h->svc_name[v]);

    for (i32 bb = 0; bb < B; ++bb)
        for (int v = 0; v < V; ++v)
            s->svc_rate_milli[dc_si(s, bb, v)] = (i32)bp.stage[v].rate_milli;

    /* initial conditions: cold, unthrottled, empty */
    for (i32 bb = 0; bb < B; ++bb) {
        for (i32 i = 0; i < N; ++i) {
            i32 n = dc_ni(s, bb, i);
            s->clock_q16[n] = DC_Q16;
            s->temp_mc[n] = s->room_supply_mc[dc_mi(s, bb, s->rack_room[s->node_rack[i]])];
        }
        for (i32 r = 0; r < R; ++r) s->rack_scale_q16[dc_ri(s, bb, r)] = DC_Q16;
        for (i32 m = 0; m < M; ++m) {
            s->room_scale_q16[dc_mi(s, bb, m)] = DC_Q16;
            s->room_supply_now_mc[dc_mi(s, bb, m)] = s->room_supply_mc[dc_mi(s, bb, m)];
        }
    }

    snprintf(h->sweep_key, DC_NAME_MAX, "%s", bp.sweep_key);
    snprintf(h->sweep_target, DC_NAME_MAX, "%s", bp.sweep_target);
    for (i32 bb = 0; bb < B; ++bb)
        if (apply_sweep(h, &bp, bb, err, errcap)) return -1;
    if (resolve_faults(h, &bp, err, errcap)) return -1;
    return 0;
}
