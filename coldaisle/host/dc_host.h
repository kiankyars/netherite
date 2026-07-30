/* dc_host.h - host-side build, run and report. Nothing here is device code. */
#ifndef COLDAISLE_DC_HOST_H
#define COLDAISLE_DC_HOST_H

#include "core/dc_state.h"

#include <stdio.h>

#define DC_NAME_MAX 32
#define DC_MAX_ALLOC 96

/* A scheduled capacity change: "the chiller fails at t=30 s". Sizing a
 * datacenter is half the exercise; watching one degrade is the other half, and
 * a steady-state-only simulator cannot show the second. */
enum { DC_FAULT_ROOM_COOL, DC_FAULT_ROOM_POWER, DC_FAULT_ROOM_SPINE,
       DC_FAULT_RACK_PDU, DC_FAULT_RACK_UPLINK, DC_FAULT_SVC_RATE };
typedef struct DcFault {
    i64 tick;        /* when it lands */
    int kind;        /* DC_FAULT_* */
    i32 index;       /* room, rackgroup-expanded rack, or stage index */
    i64 value;       /* new value in the state's own integer unit */
    char what[DC_NAME_MAX * 2];
} DcFault;

#define DC_MAX_FAULT 64

typedef struct DcHost {
    DcSim sim;
    void *allocs[DC_MAX_ALLOC];
    int   nalloc;
    char (*room_name)[DC_NAME_MAX];   /* [nroom] */
    char (*svc_name)[DC_NAME_MAX];    /* [nsvc] */
    char (*rack_name)[DC_NAME_MAX];   /* [nrack] group name of each rack */
    /* what the batch axis varied, for reports: "cool_cap_kw" etc, and the value
     * each batch was given (in the blueprint's own unit, scaled by 1000). */
    char  sweep_key[DC_NAME_MAX];
    char  sweep_target[DC_NAME_MAX];
    i64  *sweep_value_milli;          /* [nbatch] */
    DcFault fault[DC_MAX_FAULT];
    int nfault;
} DcHost;


/* Build from a blueprint file. Returns 0 on success; on failure fills err with
 * "line N: message" and returns nonzero. */
int  dc_build_from_file(DcHost *h, const char *path, int nbatch_override,
                        char *err, int errcap);
void dc_host_free(DcHost *h);

/* Apply every fault scheduled for `tick` to every batch. Returns how many
 * landed, and names them on `log` when it is not NULL. */
int dc_apply_faults(DcHost *h, i64 tick, FILE *log);


/* One tick on the CPU, all eleven passes in SPEC.md order, every batch. */
void dc_tick_cpu(DcSim *s);
void dc_run_cpu(DcSim *s, i64 ticks);

/* Reports. */
void dc_report_summary(const DcHost *h, FILE *out);
void dc_report_csv_header(FILE *out);
void dc_report_csv_row(const DcHost *h, i32 b, i64 tick, FILE *out);
void dc_report_heatmap(const DcHost *h, i32 b, FILE *out);
int  dc_report_frame_ppm(const DcHost *h, i32 b, const char *path, int scale);

/* Conservation audit: requests admitted must equal completed + dropped +
 * queued + in flight. Returns 0 when the books balance for every batch. */
int  dc_audit(const DcHost *h, char *err, int errcap);

#endif /* COLDAISLE_DC_HOST_H */
