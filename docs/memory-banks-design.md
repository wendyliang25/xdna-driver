# Multi-bank contiguous DRAM (OF platform)

Implementation design for the multi-bank contiguous-DRAM allocator in
`drivers/accel/amdxdna` on the device-tree (OF) platform (npu3b/aie2ps, Versal
arm64). Complements `docs/memory-banks-rpu-vs-cert.md` (the *analysis* of which
buffers must live where); this is the *implementation*.

## 1. Problem

Device-visible DRAM must be physically contiguous and split by who dereferences
the address, with **fixed, userspace-visible bank ids**:

- **Firmware bank** (id `AMDXDNA_MEM_BANK_FW` = 0) — buffers the **firmware
  processor** (an R5/RPU-class core) reads/writes (kernel fw log/trace, async
  event, work buffer, and userspace firmware debug/log BOs). That processor
  reaches only < 4 GB.
- **App banks** (ids ≥ 1) — user BOs and 64-bit buffers consumed by **AIE/CERT**
  DMA, which is the NPU's own path (this device's stream ID).
- `mgmt`/`doorbell` are predefined CPU-`ioremap` regions — not banks.

Memory comes from **amdxdna's own reserved-memory**, DMA-mapped through **this
device**. The firmware bank's **32-bit reachability is a property of its
reserved-memory placement** (the region is put below 4 GB), *not* of a borrowed
DMA master. This keeps amdxdna a pure `memory-region` consumer that makes no
assumptions about other masters — which is what makes it work uniformly on
native Linux (SMMU on or off) and under Xen dom0less passthrough (domU), where
no firmware-processor node is visible to the guest.

## 2. Why placement, not a borrowed master

The firmware processor is a CPU-class core: it addresses physical DRAM directly
and is not translated by the system SMMU, so the only requirement is that the
buffer's **physical address is < 4 GB**. A reserved-memory region placed below
4 GB guarantees that by construction — no DMA mask and no reference to the
firmware processor are needed. Under Xen dom0less, the guest's reserved-memory
region is mapped 1:1 to host-physical via `direct-map`/`xen,static-mem`, so the
same physical address the driver computes is what the firmware dereferences.

## 3. Data model

```c
/* amdxdna_cbuf.c */
struct amdxdna_mem_bank {
    struct device *dma_dev;  /* this device (xdna->ddev.dev) */
    bool           fw;
    bool           carveout; /* true: drm_mm over reserved region; false: system CMA */
    u64            addr, size;
    struct drm_mm  mm;
    struct mutex   lock;
};

/* struct amdxdna_dev (amdxdna_drv.h) */
struct xarray  banks;      /* keyed by fixed bank id */
```

## 4. Device-tree contract

Idiomatic: `reserved-memory` + `memory-region` + `memory-region-names` (named by
purpose) on the amdxdna node, plus its own `iommus` (AIE stream ID) when an SMMU
is present. **No bank sub-nodes, no `iommus`/`dma-ranges` on memory.**

```dts
amdxdna {
    compatible = "amd,amdxdna";
    iommus = <&smmu AIE_SID>;                 /* amdxdna's own DMA = AIE stream ID */
    mboxes = ...; mbox-names = "tx","rx";
    memory-region = <&mgmt>, <&doorbell>, <&fw_mem>, <&aie_mem0>;
    memory-region-names = "mgmt", "doorbell", "fw", "aie0";
};
```

- `mgmt`/`doorbell`: CPU `ioremap` (unchanged).
- `fw`: firmware pool; its reserved-memory **must be placed < 4 GB**.
- `aie<N>`: AIE app pools. `memory-region` per pool is optional (absent → system
  CMA on this device).
- The driver maps names → **fixed userspace bank ids**: `fw` → 0,
  `aie<N>` → N + 1 (create-BO `flags[7:0]` bitmap; `flags==0` → first app bank).

## 5. Bring-up (`amdxdna_plat_probe`)

1. `amdxdna_mem_banks_init()` — `xa_init`, then parse `memory-region-names`:
   `"fw"` → id 0 (fw), `"aie<N>"` → id N+1; region base/size via
   `of_reserved_mem_lookup()` (carveout) or none (system CMA). On the OF platform
   the firmware bank (id 0) and the first app bank (id 1) are **always available**
   — any bank the DT did not name defaults to system CMA on this device.
2. Cleanup: `amdxdna_mem_banks_fini()` (probe error paths and remove).

## 6. Allocation

- **User BOs** (`amdxdna_get_cbuf(dev, size, align, flags)`): requested bank id
  bits → `xa_load`; else first app bank; else debugfs carveout; else system CMA
  on this device. carveout = `drm_mm` + `dma_map_resource` + `ioremap_cache`;
  system CMA = `dma_alloc_noncoherent`.
- **Firmware/kernel buffers** (`aie.c` → `amdxdna_cbuf_kalloc(xdna, size, fw, …)`):
  the caller passes `fw` — `true` for buffers the firmware processor dereferences
  (async event, fw log/trace, work buffer) → bank id 0; `false` for buffers
  written/read by AIE/CERT shim DMA (coredump, column dump, tile mem, telemetry)
  → first app bank. Falls back to debugfs carveout, then system CMA (x86).

## 7. Coherency

Cacheable (`ioremap_cache` / `dma_alloc_noncoherent`), `DMA_ATTR_SKIP_CPU_SYNC`;
coherency via `SYNC_BO` (`drm_clflush_*`) for user BOs and the DPT/query read
paths for firmware buffers. Correct on a coherent interconnect (Versal CCI) or
with those flushes.

## 8. Files changed

| File | Change |
|---|---|
| `amdxdna_drv.h` | `struct xarray banks` |
| `include/uapi/drm/amdxdna_accel.h` | `AMDXDNA_MEM_BANK_FW/_AIE`; `create_bo.flags` bank-id bitmap |
| `amdxdna_cbuf.c` / `.h` | banks over xarray; carveout + system-CMA backings; get_cbuf/kalloc (with `fw`) + fallback chain; parse `memory-region-names` |
| `amdxdna_platform.c` | `mem_banks_init/_fini` |
| `aie.c` / `aie.h` | firmware buffers via `amdxdna_cbuf_kalloc`; `amdxdna_alloc_msg_buff(..., fw)` |
| `aie4.c`, `amdxdna_dpt.c`, `amdxdna_error.c` | classify fw buffers (`fw=true`) |
| `aie.c`, `aie2_message.c`, `aie4_message.c` | classify AIE/CERT buffers (`fw=false`) |
| `amdxdna_gem.c` | `flags[7:0]` bank-id bitmap threaded to `amdxdna_get_cbuf` |
| `dt-bindings/amd,amdxdna.yaml` | `memory-region(-names)` `fw`/`aie<N>`, node `iommus` |

`amdxdna_cma_buf.c/.h` (NPU3A) untouched.

## 9. Open items / caveats

- **SMMU + firmware processor with its own stream ID**: this design assumes the
  firmware processor addresses physical DRAM (no SMMU stream ID), so placement
  suffices. If a platform genuinely translates the firmware processor through the
  SMMU, the firmware bank needs a DMA mapping in *that* master's domain — handled
  by an optional firmware DMA-master extension (separate change).
- **No-SMMU + >4 GB app pool**: the amdxdna node needs a `dma-ranges` to widen
  its mask past 32 bits (integrator DT), since without the SMMU the mask defaults
  to 32-bit.

## 10. Verification

- checkpatch `--strict`; single-object compile under OF (`OFT_CONFIG_AMDXDNA_OF=y`,
  `-DAMDXDNA_OF`) and PCI (`OFT_CONFIG_AMDXDNA_PCI=y`).
- Matrix: (a) `fw` + `aie0` → fw buffers in the fw bank (< 4 GB), aie BOs in the
  app bank; (b) no `fw` region → firmware bank defaults to system CMA; (c)
  `flags==0` → first app bank, `flags=BIT(0)` → fw bank. PCI/x86 unchanged.
