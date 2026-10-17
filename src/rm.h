// rm.h: a minimal NVIDIA RM client for macOS on nullmoth's driver. It talks to GSP-RM through the NVAccel/NVRM user
// client (the escape interface NVK uses): IOServiceOpen type 0x4E560000|0 (control) / |1 (device), selector 3 binds
// an fd number, selector 1 = RM escape (NV_ESC_RM_ALLOC / CONTROL / FREE ...). Structures: NVIDIA
// open-gpu-kernel-modules (the RM version the driver runs).
#pragma once
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include "nvtypes.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv-ioctl.h"

typedef struct {
    io_connect_t ctl, dev;            // control and device user clients
    NvHandle client, device, subdevice;
    NvHandle vaspace;                 // our GPU address space (rm_va_init)
    nv_ioctl_card_info_t card;
    NvU32 nextHandle;                 // handles for objects we allocate
    bool verbose;
} Rm;

int  rm_open(Rm *rm);                                                    // client + device + subdevice; 0 = ok
void rm_close(Rm *rm);
NvHandle rm_handle(Rm *rm);                                              // a fresh object handle
NV_STATUS rm_alloc(Rm *rm, NvHandle parent, NvHandle obj, NvU32 cls, void *params);
NV_STATUS rm_free(Rm *rm, NvHandle parent, NvHandle obj);
NV_STATUS rm_ctrl(Rm *rm, NvHandle obj, NvU32 cmd, void *params, NvU32 size);
int rm_esc(Rm *rm, io_connect_t conn, NvU32 cmd, void *data, size_t size);

// A user-client connection bound to an fd number: RM ties memory mappings to such an fd ("memFd").
typedef struct { io_connect_t conn; int fd; } RmConn;
int rm_conn_open(RmConn *c, NvU32 type);                                  // type 0 = control node, 1 = device node
void rm_conn_close(RmConn *c);

// Memory: system (host, GPU-coherent) or video memory, optionally CPU-mapped and given a GPU address.
typedef struct {
    NvHandle hPhys, hVirt;
    NvU64 size;
    bool sysmem;
    void *cpu;                        // CPU mapping (NULL if none)
    NvU64 gpu;                        // GPU virtual address in rm->vaspace (0 if none)
    RmConn map;                       // the connection that carries the CPU mapping
} RmMem;
NV_STATUS rm_va_init(Rm *rm);                                            // FERMI_VASPACE_A for our objects
NV_STATUS rm_mem_alloc(Rm *rm, RmMem *m, NvU64 size, NvU64 align, bool sysmem, bool cpuMap, bool gpuMap);
void rm_mem_free(Rm *rm, RmMem *m);
// CPU-map any RM object that supports it (memory, usermode doorbell page) through a fresh connection
NV_STATUS rm_map_cpu(Rm *rm, NvHandle parent, NvHandle obj, NvU64 size, bool sysmem, RmConn *conn, void **cpu);
