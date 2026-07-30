/* coldaisle_cuda - run a blueprint sweep on the GPU.
 *
 *   coldaisle_cuda BLUEPRINT [--ticks N] [--batches N] [--heatmap] [--bench]
 *
 * Same model, same report as the CPU binary; the point is batch width. One
 * launch per pass advances every datacenter in the sweep, so asking "how much
 * cooling, how much PDU, how much spine" costs one run instead of N. */
#include "host/dc_host.h"
#include "cuda/dc_cuda.h"
#include "core/dc_tick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: coldaisle_cuda BLUEPRINT [--ticks N] [--batches N]"
               " [--heatmap] [--bench] [--no-audit]\n");
        return 2;
    }
    const char *path = argv[1];
    i64 ticks = 600;
    int batches = 0, heat = 0, bench = 0, audit = 1;
    for (int i = 2; i < argc; ++i) {
        int last = i + 1 >= argc;
        if (!strcmp(argv[i], "--ticks") && !last) ticks = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--batches") && !last) batches = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--heatmap")) heat = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--no-audit")) audit = 0;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    int ndev = dc_cuda_device_count();
    if (ndev <= 0) {
        fprintf(stderr, "coldaisle_cuda: no CUDA device (%s).\n"
                        "The CPU binary runs the same model: ./coldaisle run %s\n",
                ndev < 0 ? dc_cuda_last_error() : "device count is 0", path);
        return 1;
    }

    DcHost h;
    char err[512];
    if (dc_build_from_file(&h, path, batches, err, sizeof err)) {
        fprintf(stderr, "coldaisle_cuda: %s\n", err);
        return 1;
    }

    DcSim dev;
    if (dc_cuda_upload(&h.sim, &dev)) {
        fprintf(stderr, "coldaisle_cuda: upload: %s\n", dc_cuda_last_error());
        return 1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (dc_cuda_run(&dev, ticks)) {
        fprintf(stderr, "coldaisle_cuda: run: %s\n", dc_cuda_last_error());
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (dc_cuda_download(&dev, &h.sim)) {
        fprintf(stderr, "coldaisle_cuda: download: %s\n", dc_cuda_last_error());
        return 1;
    }
    dc_cuda_free(&dev);

    double secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    int rc = 0;
    if (audit && dc_audit(&h, err, sizeof err)) {
        fprintf(stderr, "coldaisle_cuda: conservation FAILED: %s\n", err);
        rc = 1;
    }
    dc_report_summary(&h, stdout);
    if (heat) { printf("\n"); dc_report_heatmap(&h, 0, stdout); }
    if (bench) {
        double nt = (double)h.sim.nnode * (double)h.sim.nbatch * (double)ticks;
        printf("\nbench: %d nodes x %d batches x %lld ticks in %.3f s"
               "  = %.2f M node-ticks/s (device)\n",
               h.sim.nnode, h.sim.nbatch, (long long)ticks, secs, nt / secs / 1e6);
    }
    dc_host_free(&h);
    return rc;
}
