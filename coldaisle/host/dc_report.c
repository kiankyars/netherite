/* dc_report.c - summary table, per-tick CSV, terminal heatmap, PPM frames.
 *
 * The limiter histogram is the point of the whole simulator, so it is in every
 * output: it answers "what do I buy next" without guessing. */
#include "dc_host.h"
#include "core/dc_tick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char dc_lim_char[DC_LIM_COUNT] = { '.', 'C', 'P', 'T', 'N', 'X' };
static const char *dc_lim_name[DC_LIM_COUNT] = {
    "idle", "compute", "power", "thermal", "network", "dark"
};

/* Nameplate work per tick for the whole fleet: the denominator of util%. */
static i64 fleet_cap_wu(const DcSim *s) {
    i64 c = 0;
    for (i32 i = 0; i < s->nnode; ++i) c += s->node_cap_wu[i];
    return c;
}

static i64 room_cool_cap_total(const DcSim *s, i32 b) {
    i64 c = 0;
    for (i32 m = 0; m < s->nroom; ++m) c += s->room_cool_cap_mw[dc_mi(s, b, m)];
    return c;
}

static i64 it_power_mw(const DcSim *s, i32 b) {
    i64 p = 0;
    for (i32 m = 0; m < s->nroom; ++m) p += s->room_power_mw[dc_mi(s, b, m)];
    return p;
}

/* Instantaneous worst supply air across the rooms, for the per-tick CSV. */
static i32 max_supply_mc(const DcSim *s, i32 b) {
    i32 t = -100000;
    for (i32 m = 0; m < s->nroom; ++m) {
        i32 v = s->room_supply_now_mc[dc_mi(s, b, m)];
        if (v > t) t = v;
    }
    return t;
}

static void limiter_now(const DcSim *s, i32 b, i64 *out) {
    memset(out, 0, sizeof(i64) * DC_LIM_COUNT);
    for (i32 i = 0; i < s->nnode; ++i) out[s->limiter[dc_ni(s, b, i)]]++;
}

void dc_report_summary(const DcHost *h, FILE *out) {
    const DcSim *s = &h->sim;
    fprintf(out, "coldaisle: %d node%s, %d rack%s, %d room%s, %d stage%s, %d batch%s,"
                 " %lld ticks (%d ms each)\n",
            s->nnode, s->nnode == 1 ? "" : "s", s->nrack, s->nrack == 1 ? "" : "s",
            s->nroom, s->nroom == 1 ? "" : "s", s->nsvc, s->nsvc == 1 ? "" : "s",
            s->nbatch, s->nbatch == 1 ? "" : "es",
            (long long)(s->nbatch ? s->m_ticks[0] : 0), DC_TICK_MS);
    if (h->sweep_key[0])
        fprintf(out, "sweep: %s on %s\n", h->sweep_key, h->sweep_target);
    fprintf(out, "\n%-14s %9s %9s %6s %6s %8s %6s %9s %7s %8s  %s\n",
            h->sweep_key[0] ? h->sweep_key : "batch",
            "offer/s", "done/s", "drop%", "util%", "IT kW", "PUE", "supply pk", "die pk", "req/kW",
            "limiter mix (idle/comp/pwr/therm/net/dark)");
    for (i32 b = 0; b < s->nbatch; ++b) {
        i64 ticks = s->m_ticks[b] > 0 ? s->m_ticks[b] : 1;
        double offered = (double)s->m_arrived[b] / ticks * DC_TICKS_PER_SEC;
        double done = (double)s->m_completed[b] / ticks * DC_TICKS_PER_SEC;
        double drop = s->m_arrived[b] ? 100.0 * (double)s->m_dropped[b] / (double)s->m_arrived[b] : 0.0;
        double it_kw = (double)s->m_energy_mwt[b] / (double)ticks / 1e6;
        i64 cool = room_cool_cap_total(s, b);
        double cool_kw = (double)dc_q16_mul(cool, (i32)((i64)DC_Q16 * DC_Q16 / DC_COP_Q16)) / 1e6;
        double pue = it_kw > 0 ? (it_kw + cool_kw) / it_kw : 0.0;
        double facility_kw = it_kw + cool_kw;
        double per_kw = facility_kw > 0 ? done / facility_kw : 0.0;
        char label[DC_NAME_MAX + 8];
        if (h->sweep_key[0])
            snprintf(label, sizeof label, "%.10g", (double)h->sweep_value_milli[b] / 1000.0);
        else
            snprintf(label, sizeof label, "%d", b);
        i64 tot = 0;
        const i64 *lm = &s->m_lim[b * DC_LIM_COUNT];
        for (int c = 0; c < DC_LIM_COUNT; ++c) tot += lm[c];
        if (!tot) tot = 1;
        i64 capwu = fleet_cap_wu(s) * ticks;
        double util = capwu > 0 ? 100.0 * (double)s->m_served_wu[b] / (double)capwu : 0.0;
        fprintf(out, "%-14s %9.0f %9.0f %6.1f %6.1f %8.1f %6.2f %8.1fC %6.1fC %8.1f  ",
                label, offered, done, drop, util, it_kw, pue,
                (double)s->m_peak_supply_mc[b] / 1000.0,
                (double)s->m_peak_temp_mc[b] / 1000.0, per_kw);
        for (int c = 0; c < DC_LIM_COUNT; ++c)
            fprintf(out, "%s%2.0f", c ? "/" : "", 100.0 * (double)lm[c] / (double)tot);
        if (s->m_completed[b] && s->m_slo_viol[b])
            fprintf(out, "  slo_viol %.1f%%",
                    100.0 * (double)s->m_slo_viol[b] / (double)s->m_completed[b]);
        fprintf(out, "\n");
    }

    /* Name the binding constraint for the batch that served the most. */
    i32 best = 0;
    for (i32 b = 1; b < s->nbatch; ++b)
        if (s->m_completed[b] > s->m_completed[best]) best = b;
    const i64 *lm = &s->m_lim[best * DC_LIM_COUNT];
    int worst = DC_LIM_COMPUTE;
    for (int c = DC_LIM_COMPUTE; c < DC_LIM_COUNT; ++c) if (lm[c] > lm[worst]) worst = c;
    fprintf(out, "\nbest batch %d is %s-limited", best, dc_lim_name[worst]);
    if (lm[DC_LIM_IDLE] > lm[worst])
        fprintf(out, " while %.0f%% of node-ticks sat idle, so offered load is the ceiling",
                100.0 * (double)lm[DC_LIM_IDLE] /
                (double)(lm[0] + lm[1] + lm[2] + lm[3] + lm[4] + lm[5]));
    fprintf(out, "\n");
}

void dc_report_csv_header(FILE *out) {
    fprintf(out, "tick,batch,offered_cum,completed_cum,dropped_cum,it_mw,supply_mc,"
                 "peak_mc,queued,inflight,idle,compute,power,thermal,network,dark\n");
}

void dc_report_csv_row(const DcHost *h, i32 b, i64 tick, FILE *out) {
    const DcSim *s = &h->sim;
    i64 lim[DC_LIM_COUNT];
    limiter_now(s, b, lim);
    i64 queued = 0, inflight = 0;
    i32 peak = -100000;
    for (i32 i = 0; i < s->nnode; ++i) {
        i32 n = dc_ni(s, b, i);
        queued += s->queue_req[n];
        i32 bytes = s->svc_bytes_kib[s->node_svc[i]];
        if (bytes > 0) inflight += ((i64)s->txbl_kib[n] + bytes - 1) / bytes;
        if (s->temp_mc[n] > peak) peak = s->temp_mc[n];
    }
    fprintf(out, "%lld,%d,%lld,%lld,%lld,%lld,%d,%d,%lld,%lld",
            (long long)tick, b, (long long)s->m_arrived[b],
            (long long)s->m_completed[b], (long long)s->m_dropped[b],
            (long long)it_power_mw(s, b), max_supply_mc(s, b), peak,
            (long long)queued, (long long)inflight);
    for (int c = 0; c < DC_LIM_COUNT; ++c) fprintf(out, ",%lld", (long long)lim[c]);
    fprintf(out, "\n");
}

/* Terminal view: one line per rack, one character per node, Factorio bottleneck
 * colours in ASCII. `.` idle, `C` compute-bound, `P` power, `T` thermal, `N`
 * network backlog, `X` breaker open. */
void dc_report_heatmap(const DcHost *h, i32 b, FILE *out) {
    const DcSim *s = &h->sim;
    fprintf(out, "batch %d  legend: . idle  C compute  P power  T thermal  N network  X dark\n", b);
    for (i32 r = 0; r < s->nrack; ++r) {
        i32 ri = dc_ri(s, b, r);
        i32 hot = -100000;
        for (i32 i = s->rack_node_begin[r]; i < s->rack_node_end[r]; ++i) {
            i32 t = s->temp_mc[dc_ni(s, b, i)];
            if (t > hot) hot = t;
        }
        fprintf(out, "%-10s r%-4d %5.1fkW in %4.1fC hot %5.1fC %s ",
                h->rack_name[r], r,
                (double)s->rack_power_mw[ri] / 1e6,
                (double)s->rack_inlet_mc[ri] / 1000.0,
                (double)hot / 1000.0,
                s->rack_dark[ri] ? "OPEN" : "    ");
        for (i32 i = s->rack_node_begin[r]; i < s->rack_node_end[r] && i - s->rack_node_begin[r] < 96; ++i)
            fputc(dc_lim_char[s->limiter[dc_ni(s, b, i)]], out);
        fputc('\n', out);
    }
}

/* Rack elevation view: one column per rack, one cell per node with slot 0 at
 * the bottom, coloured from the room supply air (blue) to the node's own t_max
 * (red); a dark rack is grey. With a vgrad_c gradient in the blueprint the top
 * of each rack visibly cooks first, which is the point of drawing it at all. */
int dc_report_frame_ppm(const DcHost *h, i32 b, const char *path, int scale) {
    const DcSim *s = &h->sim;
    if (scale < 1) scale = 1;
    i32 tallest = 1;
    for (i32 r = 0; r < s->nrack; ++r) {
        i32 h_ = s->rack_node_end[r] - s->rack_node_begin[r];
        if (h_ > tallest) tallest = h_;
    }
    int cell = scale, gut = 1;
    int W = s->nrack * (cell + gut) + gut;
    int H = tallest * cell + 2 * gut;
    unsigned char *img = (unsigned char *)calloc((size_t)W * H * 3, 1);
    if (!img) return -1;
    for (i32 r = 0; r < s->nrack; ++r) {
        i32 ri = dc_ri(s, b, r);
        i32 supply = s->room_supply_now_mc[dc_mi(s, b, s->rack_room[r])];
        for (i32 i = s->rack_node_begin[r]; i < s->rack_node_end[r]; ++i) {
            i32 n = dc_ni(s, b, i);
            i32 lo = supply, hi = s->node_t_max_mc[i];
            i32 t = s->temp_mc[n];
            int f = hi > lo ? (int)(255LL * (t - lo) / (hi - lo)) : 0;
            if (f < 0) f = 0;
            if (f > 255) f = 255;
            unsigned char cr, cg, cb;
            if (s->rack_dark[ri]) { cr = cg = cb = 90; }
            else if (f < 128) { cr = (unsigned char)(f * 2); cg = (unsigned char)(120 + f); cb = (unsigned char)(255 - f); }
            else { cr = 255; cg = (unsigned char)(248 - (f - 128) * 2); cb = (unsigned char)((255 - f) / 2); }
            int x0 = gut + r * (cell + gut);
            /* slot 0 is the bottom of the rack, so draw it at the bottom of the
             * image: an elevation view where the hot top row reads as the top */
            int y0 = gut + (s->rack_node_end[r] - 1 - i) * cell;
            for (int y = y0; y < y0 + cell; ++y)
                for (int x = x0; x < x0 + cell; ++x) {
                    size_t o = ((size_t)y * W + x) * 3;
                    img[o] = cr; img[o + 1] = cg; img[o + 2] = cb;
                }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(img); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, (size_t)W * H * 3, f);
    fclose(f);
    free(img);
    return 0;
}
