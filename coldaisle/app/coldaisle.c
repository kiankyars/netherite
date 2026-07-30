/* coldaisle - datacenter factory simulator. CLI.
 *
 *   coldaisle run   BLUEPRINT [options]
 *   coldaisle bench BLUEPRINT [options]
 *
 * See README.md for the blueprint grammar and SPEC.md for the model. */
#include "host/dc_host.h"
#include "core/dc_tick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    printf("usage: coldaisle run|bench BLUEPRINT [options]\n"
           "  --ticks N         ticks to simulate (default 600 = 60 s)\n"
           "  --batches N       override batch count (default: the sweep width)\n"
           "  --csv FILE        per-tick CSV\n"
           "  --csv-every N     CSV row every N ticks (default 1)\n"
           "  --heatmap         print the rack heatmap at the end\n"
           "  --heatmap-batch B which batch to print (default 0)\n"
           "  --frames DIR      write DIR/frame_%%06d.ppm floor plans\n"
           "  --frame-every N   frame every N ticks (default 10)\n"
           "  --scale N         pixels per node in a frame (default 4)\n"
           "  --no-audit        skip the request-conservation check\n"
           "  --quiet           suppress the summary table\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return argc < 2 ? 2 : 0; }
    const char *cmd = argv[1], *path = argv[2];
    int bench = !strcmp(cmd, "bench");
    if (!bench && strcmp(cmd, "run")) { usage(); return 2; }

    i64 ticks = 600;
    int batches = 0, heat = 0, heat_b = 0, audit = 1, quiet = 0;
    int csv_every = 1, frame_every = 10, scale = 4;
    const char *csv_path = NULL, *frames_dir = NULL;
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        int last = i + 1 >= argc;
#define ARG(name) (!strcmp(a, name) && !last && (++i, 1))
        if (ARG("--ticks")) ticks = atoll(argv[i]);
        else if (ARG("--batches")) batches = atoi(argv[i]);
        else if (ARG("--csv")) csv_path = argv[i];
        else if (ARG("--csv-every")) csv_every = atoi(argv[i]);
        else if (ARG("--frames")) frames_dir = argv[i];
        else if (ARG("--frame-every")) frame_every = atoi(argv[i]);
        else if (ARG("--scale")) scale = atoi(argv[i]);
        else if (ARG("--heatmap-batch")) heat_b = atoi(argv[i]);
        else if (!strcmp(a, "--heatmap")) heat = 1;
        else if (!strcmp(a, "--no-audit")) audit = 0;
        else if (!strcmp(a, "--quiet")) quiet = 1;
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
#undef ARG
    }
    if (ticks < 0 || csv_every < 1 || frame_every < 1) {
        fprintf(stderr, "ticks, --csv-every and --frame-every must be positive\n");
        return 2;
    }

    DcHost h;
    char err[512];
    if (dc_build_from_file(&h, path, batches, err, sizeof err)) {
        fprintf(stderr, "coldaisle: %s\n", err);
        return 1;
    }
    if (heat_b < 0 || heat_b >= h.sim.nbatch) heat_b = 0;

    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) { fprintf(stderr, "cannot write %s\n", csv_path); dc_host_free(&h); return 1; }
        dc_report_csv_header(csv);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i64 t = 0; t < ticks; ++t) {
        dc_apply_faults(&h, t, quiet ? NULL : stdout);
        dc_tick_cpu(&h.sim);
        if (csv && (t % csv_every) == 0)
            for (i32 b = 0; b < h.sim.nbatch; ++b) dc_report_csv_row(&h, b, t, csv);
        if (frames_dir && (t % frame_every) == 0) {
            char fp[1024];
            snprintf(fp, sizeof fp, "%s/frame_%06lld.ppm", frames_dir,
                     (long long)(t / frame_every));
            if (dc_report_frame_ppm(&h, heat_b, fp, scale)) {
                fprintf(stderr, "cannot write %s (does the directory exist?)\n", fp);
                dc_host_free(&h);
                if (csv) fclose(csv);
                return 1;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    if (csv) fclose(csv);

    int rc = 0;
    if (audit && dc_audit(&h, err, sizeof err)) {
        fprintf(stderr, "coldaisle: conservation FAILED: %s\n", err);
        rc = 1;
    }
    if (!quiet) dc_report_summary(&h, stdout);
    if (heat) { printf("\n"); dc_report_heatmap(&h, heat_b, stdout); }
    if (bench) {
        double nt = (double)h.sim.nnode * (double)h.sim.nbatch * (double)ticks;
        printf("\nbench: %d nodes x %d batches x %lld ticks in %.3f s"
               "  = %.2f M node-ticks/s\n",
               h.sim.nnode, h.sim.nbatch, (long long)ticks, secs, nt / secs / 1e6);
    }
    dc_host_free(&h);
    return rc;
}
