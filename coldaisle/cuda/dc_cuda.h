/* dc_cuda.h - host-callable entry points for the batched GPU driver. */
#ifndef COLDAISLE_DC_CUDA_H
#define COLDAISLE_DC_CUDA_H

#include "core/dc_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of visible CUDA devices, or a negative CUDA error code. */
int dc_cuda_device_count(void);

/* Mirror `host` onto the device. `dev` receives the same struct with device
 * pointers; topology is copied once, mutable state is copied as-is so a
 * partially advanced run can be handed over. Returns 0 on success. */
int dc_cuda_upload(const DcSim *host, DcSim *dev);

/* Advance `ticks` ticks on the device. Every batch runs concurrently. */
int dc_cuda_run(DcSim *dev, i64 ticks);

/* Copy the mutable arrays back into `host`. */
int dc_cuda_download(const DcSim *dev, DcSim *host);

void dc_cuda_free(DcSim *dev);

/* Last CUDA error string, for the CLI to print. */
const char *dc_cuda_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* COLDAISLE_DC_CUDA_H */
