# aie4 platform (non-PCI) port — design

## 1. Goal

Support the aie4 firmware stack on a non-PCI SoC part (aie2ps, T20)
where the NPU is described in the device tree ("amd,amdxdna") and reaches the
remote firmware over a shared-memory + ZynqMP-IPI transport instead of a PCIe
SRAM-BAR mailbox.

`aie4_plat.c` must provide `amdxdna_dev_ops` whose behaviour mirrors the aie4
**classic** mode in `aie4_pci.c` (`aie4_classic_ops`), *except* the PCI-specific
hardware-bring-up:

* no `pcim_enable_device` / BAR iomap / MSI-X (`aie4m_pcidev_init`),
* **no firmware loading** and **no PSP/SMU** (`aie4_fw_load`,
  `aie4_prepare_firmware`, `aie_psp_*`, `aie_smu_*`) — on the SoC the RPU/PLM
  boots the AIE firmware; Linux only opens a channel to already-running FW,
* SRAM-BAR mgmt mailbox replaced by the shmem+IPI mailbox
  (`amdxdna_mailbox_plat.c`),
* PCI doorbell BAR replaced by the shmem+IPI doorbell (in `aie4_plat.c`).

Everything else — **command submission, buffer management, hwctx lifecycle,
async error handling, metadata / clock / power / telemetry queries, DPT (fw
log/trace)** — is identical and is *reused*, not re-implemented.

## 2. Why most of it is already shared

The aie4 stack is already layered so that only bring-up is PCI-aware:

| Layer | File | Transport-aware? |
|-------|------|------------------|
| hwctx lifecycle, cmd submit/wait | `aie4_ctx.c` | only the **doorbell write** |
| mgmt/hsa messages, queries | `aie4_message.c` | no (uses `aie_send_mgmt_msg_wait`) |
| async error decode / register | `aie4_error.c` | no |
| message plumbing, protocol check | `aie.c` | only the **mgmt channel** |
| dev ops orchestration, bring-up | `aie4_pci.c` | **yes** (BAR/IRQ/PSP/SMU/mbox) |

So the port needs: (a) a shared device handle, (b) two small **transport
seams**, and (c) an `aie4_plat.c` that assembles the classic flow minus PCI/FW.

## 3. Shared device handle

`aie4_plat.c` uses the **same** `struct amdxdna_dev_hdl` (`aie4_pci.h`) that
`aie4_pci.c` uses, stored in `xdna->dev_handle`. This is what makes every
shared function (`aie4_ctx.c`, `aie4_message.c`, `aie4_error.c`) work unchanged
— they all take `struct amdxdna_dev_hdl *ndev` and reach `&ndev->aie`.

The PCI-only members (`mbox_base`, `rbuf_base`, `doorbell_base`, `mbox`,
`aie.psp_hdl`, `aie.smu_hdl`) simply stay NULL on the platform path. A small
platform sub-block is added to the handle:

```c
/* amdxdna_dev_hdl, platform transport (NULL on PCI) */
struct amdxdna_mailbox_plat *mbox_plat;   /* mgmt mailbox (shmem+IPI)   */
void                        *db_shmem;    /* mapped doorbell region     */
struct shmem_db_ring        *db_ring;     /* doorbell ring in db_shmem  */
u64                          db_ring_mask;
spinlock_t                   db_lock;
```

`struct aie4_plat_dev` is removed; the doorbell ring ABI (`struct
shmem_db_ring`) stays in `aie4_plat.h`.

## 4. Compile-time transport selection (no runtime ops)

The build is **either PCI or platform**, chosen by the existing
`XDNA_BUS_TYPE` in `Makefile` (`pci` → `OFT_CONFIG_AMDXDNA_PCI=y`; `of` →
`OFT_CONFIG_AMDXDNA_OF=y`, which also passes `-DAMDXDNA_OF`). So there is no
need for a runtime `aie_xcomm_ops` table: the transport is picked by *which
object files are linked*. Each transport-specific symbol has exactly one
definition per build; the shared code just calls it and the linker resolves it.

### Seam #1 — mgmt mailbox channel (keep `struct mailbox_channel`)

`struct mailbox` / `struct mailbox_channel` are **opaque** — only
forward-declared in `amdxdna_mailbox.h`, defined inside `amdxdna_mailbox.c`. So
the platform keeps `aie->mgmt_chann` as a `struct mailbox_channel *` and simply
provides a *different implementation* of the channel, selected at compile time.
**`aie.c`, `aie_send_mgmt_msg_wait()` and `amdxdna_mailbox_helper.c` do not
change.**

The only channel operations the shared code calls are:

```c
int  xdna_mailbox_send_msg(chann, msg, timeout);   /* via xdna_send_msg_wait */
void xdna_mailbox_stop_channel(chann);             /* via aie_destroy_chann  */
void xdna_mailbox_free_channel(chann);             /* via aie_destroy_chann  */
```

Two implementations, one linked per build (Kbuild):

* **PCI** — `amdxdna_mailbox.c`: `struct mailbox_channel` over iomem SRAM rings
  + MSI-X IRQ (today's code, unchanged).
* **plat** — `amdxdna_mailbox_plat.c`: its own `struct mailbox_channel` over the
  shmem rings + IPI. It implements the three shared ops above (matching
  signatures) and its own private `create`/`alloc`/`start` (called only by the
  plat `aie4_mailbox_init` hook). The RX work resolves `msg->id` → `notify_cb`,
  exactly as the PCI mailbox IRQ path does, so `xdna_send_msg_wait`'s completion
  fires identically.

Since exactly one of the two `.o`s is compiled, both defining
`struct mailbox_channel` is fine (no ODR conflict). `amdxdna_mailbox.o` moves to
the PCI-only Kbuild group; `amdxdna_mailbox_plat.o` to the OF-only group.

No new `amdxdna_mgmt_send_wait` symbol is introduced (that earlier proposal is
dropped).

### Seam #2 — kernel-mode doorbell

`aie4_ctx.c` rings the kernel doorbell with `writel()` to
`priv->doorbell_addr` (a PCI BAR mapping computed at ctx-create). Replace the
ring sites with `amdxdna_ring_ctx_doorbell(ndev, hw_ctx_id)`:

* **PCI** impl (in `aie4_pci.c`): `writel(..., priv->doorbell_addr)`.
* **plat** impl (in `aie4_plat.c`): `aie4_plat_ring_doorbell()` — shmem ring +
  IPI kick.

The ctx-create doorbell *setup* also differs (PCI maps `doorbell_base +
doorbell_off + resp.doorbell_offset` and bounds-checks against the BAR,
`aie4_ctx.c:250-261`; plat just records `hw_ctx_id`/offset for ring indexing).
Factor that into `amdxdna_ctx_setup_doorbell(ndev, hwctx, resp)` with one
definition per transport (PCI in `aie4_pci.c`, plat in `aie4_plat.c`).

### The one shared lifecycle in `aie4.c` + transport hooks

`aie4_classic_hw_start()` is exactly **four** transport calls
(`aie4_fw_load`/`_unload`, `aie4_mailbox_init`/`_fini`) wrapped around an
all-shared middle (`calibrate_clock`, `attach_work_buffer`,
`set_ctx_hysteresis`, `query_fw`, `query_aie`, `partition_init`,
`async_events_alloc`, `restore_power_mode`). So the whole classic lifecycle
moves to `aie4.c` **once** and both the PCI classic ops and the plat ops call
it; only the hooks differ. The hooks are compile-time symbols (one definition
per build):

```c
/* declared in aie4.h; PCI defs in aie4_pci.c, plat defs in aie4_plat.c */
int  aie4_fw_load(struct amdxdna_dev_hdl *ndev);      /* PCI: psp+smu   plat: 0    */
void aie4_fw_unload(struct amdxdna_dev_hdl *ndev);    /* PCI: psp+smu   plat: noop */
int  aie4_mailbox_init(struct amdxdna_dev_hdl *ndev); /* PCI: SRAM mbox plat: shmem*/
void aie4_mailbox_fini(struct amdxdna_dev_hdl *ndev);
int  aie4_dev_setup(struct amdxdna_dev *xdna);        /* PCI: pcidev_init plat: noop*/
int  aie4_hw_resume_prepare(struct amdxdna_dev *xdna);/* PCI: pci_enable  plat: noop*/
void aie4_hw_resume_cleanup(struct amdxdna_dev *xdna);/* PCI: pci_disable plat: noop*/
```

Shared functions in `aie4.c` (identical flow for PCI-classic and plat):

```c
int aie4_hw_start(ndev)  = fw_load → mailbox_init → calibrate_clock →
                           attach_work_buffer → set_ctx_hysteresis →
                           query_fw → query_aie → partition_init →
                           async_events_alloc → restore_power_mode
                           (same goto unwind as aie4_classic_hw_start)
void aie4_hw_stop(ndev)  = partition_fini → suspend_fw → mailbox_fini →
                           async_events_free → fw_unload
int aie4_init(xdna)      = aie4_dev_setup → alloc_work_buffer → aie4_hw_start →
                           aie4_msg_init → amdxdna_dpt_init → amdxdna_pm_init
void aie4_fini(xdna)     = amdxdna_pm_fini → amdxdna_dpt_fini → aie4_hw_stop →
                           free_work_buffer
int aie4_suspend(xdna)   = aie4_hwctx_suspend_all → aie4_hw_stop
int aie4_resume(xdna)    = aie4_hw_resume_prepare → aie4_hw_start →
                           aie4_hwctx_resume_all  (cleanup hook on error)
```

`aie4_classic_ops` and `aie4_plat_ops` then point `.init/.fini/.suspend/
.resume/.runtime_*` at these shared functions. The PF/VF ops keep their own
variants (their flow legitimately differs — VF has no fw_load/work-buffer, PF is
a supervisor with no submission path). The non-upstream `AMDXDNA_NPU3A` echo
becomes an optional `aie4_hw_start_quirk(ndev)` hook (no-op unless that build
flag is set) so it stays out of the shared body.

## 6. Functions to move to a new common `aie4.c`

With compile-time selection, `aie4_pci.c` is **not built** in the OF
configuration, so every transport-independent helper it currently holds
**must** move to a file that is built in both configs. Create a new common
`aie4.c` (built unconditionally) and relocate the functions below. (In the
first patch these were merely un-`static`'d in place; under compile-time
selection the physical move is required, not optional.) These are already
transport-independent (no `pci_dev`, no BAR, no PSP/SMU).

**Move to `aie4.c`** (declare in `aie4_pci.h`):

| Function | Why shared |
|----------|-----------|
| `aie4_get_info` | ioctl dispatcher; all cases arch-generic |
| `aie4_get_power_mode` | reads `ndev->pw_mode` |
| `aie4_query_clock_metadata` | `aie_update_counters` |
| `aie4_query_resource_info` | dpm/tops from handle |
| `aie4_set_power_mode`, `aie4_set_state` | mgmt msg only |
| `aie4_get_array` | dispatcher; SRCU/dev_lock generic |
| `aie4_query_fw` | npu+cert version msgs |
| `aie4_query_aie` | version+metadata+dpm msgs |
| `aie4_partition_init` / `_fini` | create/destroy partition msgs |
| `aie4_restore_power_mode` | `aie4_msg_set_power_mode` |
| `aie4_alloc_work_buffer` / `_free_work_buffer` | `amdxdna_alloc_msg_buff` |
| `aie4_hwctx_suspend_all` / `_resume_all` / `_cleanup_all` | walk clients, call `aie4_ctx.c` |

**Keep in `aie4_pci.c`** (PCI-only): `aie4m_pcidev_init`,
`aie4_prepare_firmware`, `aie4_request_firmware`, `aie4_release_firmware`,
`aie4_fw_load` / `_unload`, `aie4_irq_init`, `mailbox_info`,
`aie4_read_mbox_info`, `aie4_fw_is_alive`, `aie4_mailbox_info` /`_start`
/`_init` /`_fini`, `aie4_doorbell_mmap` / `_pfn`, the `aie4_*_hw_start`
/`_hw_stop`, the `aie4_pf/vf/classic_*` init/fini/suspend/resume, and the ops
tables.

**Already shared, no move** (keep where they are, `aie4_plat_ops` points at
them directly): `aie4_hwctx_init/fini/config`, `aie4_cmd_submit`,
`aie4_cmd_wait` (`aie4_ctx.c`); `aie4_async_event_register`,
`aie4_handle_dev_event` (`aie4_error.c`); `aie4_calibrate_clock`,
`aie4_set_ctx_hysteresis`, `aie4_attach_work_buffer`, `aie4_suspend_fw`,
`aie4_msg_init` (`aie4_message.c`).

**`amdxdna_dpt.c` is common (shared group), no PF/VF specifics.** The DPT
framework (ring-buffer mgmt, version parse, polling timer, dmesg dump, sysfs/
debugfs) is transport-agnostic. Its only PCI coupling is the best-effort MSI
"data-ready" interrupt, isolated to `amdxdna_dpt_irq_init()`
(`pci_irq_vector(to_pci_dev(...))` + `request_irq`), `amdxdna_dpt_irq_handler()`
(`writel(dpt->io_base + dpt->msi_address)`), `amdxdna_dpt_irq_fini()`, and the
`<linux/pci.h>` include. This path self-gates: `irq_init` returns `-EINVAL` when
`msi_idx`/`msi_address` are 0 and the caller falls back to on-demand polling
(`amdxdna_dpt_timer_get`). On the OF build `aie4_fw_log_init`/`_trace_init` leave
`io_base`/`msi_address` unset, so DPT runs via polling with **no change**. Only
if an IPI-driven DPT interrupt is wanted later does the MSI trio need to route
through the compile-time seam.

`aie4_debugfs_init` calls `to_pci_dev(...)->is_virtfn`; split the PCI-only knob
selection so a plat variant (`aie4_plat_debugfs_init`) exposes only the
`ctx_switch_hysteresis_us` / `kernel_mode_submission` files.

## 7. `aie4_plat.c` — what it actually contains

With the lifecycle shared in `aie4.c` (§4), `aie4_plat.c` reduces to the handle,
the transport hooks (the plat side of the compile-time symbols), and the
doorbell backend. It does **not** re-implement hw_start/hw_stop/init/fini/
suspend/resume — `aie4_plat_ops` points those at the shared `aie4.c` functions.

* **`aie4_plat_ndev_alloc(xdna)`** — `drmm_kzalloc` a `struct amdxdna_dev_hdl`;
  set `aie.xdna`, `priv = dev_info->dev_priv`, `kernel_submit=true`,
  `ctx_switch_hysteresis_us`, `pw_mode=DEFAULT`; `xa_init(cert_comp_xa)`,
  `mutex_init(cert_comp_lock)`, `spin_lock_init(db_lock)`; store in
  `xdna->dev_handle`.

* **`aie4_dev_setup(xdna)`** *(hook; PCI = `aie4m_pcidev_init`)* — plat: no-op /
  bind the ndev + create the plat mailbox (or leave in `plat_probe`); returns 0.
* **`aie4_fw_load/_unload(ndev)`** *(hooks)* — plat: no-op (RPU/PLM booted FW).
* **`aie4_mailbox_init(ndev)`** *(hook; PCI = SRAM handshake)* — plat: build the
  shmem `struct mailbox_channel` and set `ndev->aie.mgmt_chann`. **`aie4_mailbox_fini`** frees it.
* **`aie4_hw_resume_prepare/_cleanup(xdna)`** *(hooks; PCI = pci_enable/disable)*
  — plat: no-op.
* **`aie4_plat_doorbell_init(ndev)`** — map the "doorbell" reserved-memory region
  (`devm_ioremap_wc`), lay out `db_ring`, cache `db_ring_mask`. Called from the
  plat `aie4_mailbox_init` hook (or `aie4_dev_setup`).
* **`amdxdna_ring_ctx_doorbell(ndev, hw_ctx_id)`** *(seam #2)* — under `db_lock`
  SPSC-produce `hw_ctx_id` into `db_ring`, then `amdxdna_mailbox_plat_kick()`.
* **`amdxdna_ctx_setup_doorbell(ndev, hwctx, resp)`** *(seam #2)* — record
  `hw_ctx_id`/offset for ring indexing (no BAR mapping).

Because `aie4_fw_load/unload` are no-ops and `aie4_mailbox_init` sets up the
shmem channel, the shared `aie4_hw_start()` runs the **identical** classic
sequence on plat, with FW load/PSP/SMU elided by the hooks — which is exactly
the "same flow as aie4" you asked for.

### Submission mode: platform is kernel-mode only

The kernel doorbell on PCI is an MMIO write; user-mode submission works by
`mmap`-ing the doorbell BAR page to the process (`aie4_doorbell_mmap`) so
userspace rings it with no per-submit syscall. On the platform the doorbell is
an **IPI**, which can only be triggered in kernel space (mailbox framework →
TRIG/SMC); there is nothing to `mmap`. Therefore:

* `aie4_plat` forces `ndev->kernel_submit = true` (set in `aie4_plat_ndev_alloc`).
* `aie4_plat_ops` does **not** set `.mmap` (no doorbell BAR) and does **not**
  expose the `kernel_mode_submission` debugfs toggle, so user-mode cannot be
  selected. If a `aie4_plat_debugfs_init` is added later it must keep that knob
  hidden (or reject `= 0`).
* Every submit is `EXEC_CMD` ioctl → shared `aie4_cmd_submit` →
  `amdxdna_ring_ctx_doorbell()` (kernel IPI). The user-mode branches in
  `aie4_ctx.c` (`doorbell_offset` handed to userspace, `aie4_doorbell_mmap`) are
  never exercised on platform. No change to the shared submit logic is needed
  beyond the doorbell seam.

## 8. `aie4_plat_ops` vs `aie4_classic_ops`

```
                 classic (aie4_pci.c)      plat (aie4_plat.c)
 init            aie4_init  ————— same (aie4.c) —————   (hooks differ)
 fini            aie4_fini  ————— same (aie4.c) —————
 suspend/resume  aie4_suspend/resume ——— same (aie4.c) ——— (hooks differ)
 hwctx_*         aie4_ctx.c   ————— same —————
 cmd_submit/wait aie4_ctx.c   ————— same —————  (doorbell via seam #2)
 get/set/array   aie4.c       ————— same —————
 async events    aie4_error.c ————— same —————
 mmap            aie4_doorbell_mmap        (unset)
 debugfs         aie4_debugfs_init         aie4_plat_debugfs_init
```

## 9. Build / verify

* `OFT_CONFIG_AMDXDNA_OF` (the OF/platform build) is enabled only when the
  kernel has both `CONFIG_ARM64` and `CONFIG_OF` — the shmem+ZynqMP-IPI
  transport is ARM64 SoC (ZynqMP/Versal) + device-tree only. On any other
  config it stays off and the PCI build (`OFT_CONFIG_AMDXDNA_PCI`) is used.
* Add `aie4.o` and `aie4_plat.o` (already) to `Kbuild`.
* Single-object `-Werror` compile of `aie4.c`, `aie4_pci.c`, `aie4_plat.c`.
* `checkpatch.pl --strict` on all touched files.
* On a DT with `compatible = "amd,amdxdna"`: probe maps mgmt+doorbell regions,
  arms tx/rx IPI, opens the mgmt channel, runs the classic query flow against
  live FW, registers the accel node. PCI parts unaffected (they link the PCI
  mailbox `.o` and PCI hook defs, not the plat ones).

## 10. Staging (recommended order)

1. **(done)** Unify the handle: `aie4_plat.c` uses `struct amdxdna_dev_hdl`,
   platform sub-block added to it, `aie4_plat.c`/`aie4_plat_ops` mirror classic.
2. Create common `aie4.c`; **move** the §6 helpers there from `aie4_pci.c` and
   the shared lifecycle (`aie4_hw_start/_stop/init/fini/suspend/resume`); declare
   in a new `aie4.h`. (Supersedes the interim un-`static` step.)
3. Turn the four hooks (`aie4_fw_load/_unload`, `aie4_mailbox_init/_fini`) plus
   `aie4_dev_setup`, `aie4_hw_resume_prepare/_cleanup` into link-time symbols:
   keep the PCI defs in `aie4_pci.c`, add plat defs in `aie4_plat.c`. Add the
   seam #2 doorbell (`amdxdna_ring_ctx_doorbell` / `amdxdna_ctx_setup_doorbell`).
   **`aie.c` is not touched.**
4. Add the plat mailbox impl in `amdxdna_mailbox_plat.c`: its own
   `struct mailbox_channel` + `xdna_mailbox_send_msg/stop_channel/free_channel`
   (matching signatures) + private create/alloc/start; RX work resolves `msg->id`
   → `notify_cb`. Fill the plat SPSC rings.
5. `Kbuild` split (§11): move `amdxdna_mailbox.o` to PCI-only,
   `amdxdna_mailbox_plat.o` to OF-only; move the shared aie4 stack + new `aie4.o`
   to the always-built group. Module-entry split; extract shared DRM code from
   `amdxdna_pci_drv.c` so the OF build does not pull PCI.
6. DPT log/trace `io_base`/`msi_address` for the shmem region (§6 follow-up).

## 11. Concrete change list (existing files)

**`aie.c`** — **no change.** `aie_send_mgmt_msg_wait()` keeps using
`aie->mgmt_chann` + `xdna_send_msg_wait()`; the plat just supplies a different
`struct mailbox_channel`.

**`aie.h`** — no change to the mgmt path. Optionally declare the seam #2 doorbell
symbols (`amdxdna_ring_ctx_doorbell`, `amdxdna_ctx_setup_doorbell`) here, or in
`aie4.h`.

**`amdxdna_mailbox.h`** — no change (the channel API stays the contract).

**`amdxdna_mailbox.c`** — no code change; moves to the **PCI-only** Kbuild group.

**`aie4_pci.c`** —
* keep the PCI defs of the hooks (`aie4_fw_load/_unload`, `aie4_mailbox_init/_fini`,
  `aie4_dev_setup=aie4m_pcidev_init`, `aie4_hw_resume_prepare/_cleanup`);
* add PCI impls `amdxdna_ring_ctx_doorbell()` (`writel(priv->doorbell_addr)`) and
  `amdxdna_ctx_setup_doorbell()` (BAR map + bounds check moved from `aie4_ctx.c`);
* **remove** the §6 transport-independent helpers **and** the classic lifecycle
  (they move to `aie4.c`); `aie4_classic_ops` now points at the shared funcs;
* keep the rest PCI-only (`aie4m_pcidev_init`, firmware prepare/request/release,
  `aie4_irq_init`, SRAM `mailbox_*`, `aie4_doorbell_mmap/_pfn`, `aie4_pf/vf_*`,
  ops tables).

**`aie4_ctx.c`** (shared) —
* ctx create: replace the inline doorbell-address mapping + BAR bounds check
  (`~lines 244-312`) with `amdxdna_ctx_setup_doorbell(ndev, hwctx, resp)`;
* submit/destroy: replace the `writel(priv->doorbell_addr)` /
  `WRITE_ONCE(priv->doorbell_addr, ...)` doorbell sites with
  `amdxdna_ring_ctx_doorbell(ndev, hw_ctx_id)` and a transport-neutral
  “doorbell valid?” predicate. No other logic changes.

**`aie4_message.c`, `aie4_error.c`** (shared) — no change (already transport
neutral via `aie_send_mgmt_msg_wait`).

**`amdxdna_mailbox_plat.c`** — becomes the plat implementation of the mailbox
*channel* API: define its own `struct mailbox`/`struct mailbox_channel`, and
implement `xdna_mailbox_send_msg()`, `xdna_mailbox_stop_channel()`,
`xdna_mailbox_free_channel()` (signatures per `amdxdna_mailbox.h`) plus private
create/alloc/start called by the plat `aie4_mailbox_init` hook. `send_msg`
SPSC-produces into the mgmt TX ring + IPI; the RX work SPSC-consumes, resolves
`msg->id` → `notify_cb`, so `xdna_send_msg_wait`'s completion fires. (The mgmt
ring layout removed earlier is reintroduced here as the *impl*.)

**`aie4_plat.c`** — provide the plat side of the hooks (`aie4_fw_load/_unload` =
no-op, `aie4_mailbox_init/_fini` = build/free the shmem channel + `ndev->aie.mgmt_chann`,
`aie4_dev_setup`, `aie4_hw_resume_prepare/_cleanup` = no-op) and the doorbell
(`amdxdna_ring_ctx_doorbell`, `amdxdna_ctx_setup_doorbell`,
`aie4_plat_doorbell_init`). `aie4_plat_ops` points `.init/.fini/.suspend/.resume`
at the shared `aie4.c` functions. Drop the interim bespoke
`aie4_plat_hw_start/_stop/_init/_fini/_suspend/_resume` (superseded by `aie4.c`).

**`amdxdna_pci_drv.c`** — revert the combined pci+platform `module_init`: gate
so the **PCI build** registers only `amdxdna_pci_driver` and the **OF build**
registers only `amdxdna_plat_driver`. The shared DRM glue in this file
(`amdxdna_drm_drv`, ioctls, `amdxdna_drm_open/close`, fops, the get/set/array
ioctl wrappers, `amdxdna_drm_gem_mmap`) should be extracted into a new shared
`amdxdna_drm.c` so the OF build links the DRM layer without the PCI driver.

**`aie4_pci.h`** (or new `aie4.h`) — move the §6 prototypes next to the new
`aie4.c`; keep the platform sub-block in `struct amdxdna_dev_hdl`.

**`Kbuild`** — three groups:
* base/shared (always): `amdxdna_ctx/gem/iommu/mailbox_helper/pm/sysfs/
  ubuf/cbuf/cma_buf/xen/sensors`, the new `amdxdna_drm.o`, `aie.o`,
  `aie4_ctx.o`, `aie4_error.o`, `aie4_message.o`, `aie4.o`, `amdxdna_dpt.o`,
  `amdxdna_error.o`, `npu3b_regs.o`;
* `amdxdna-$(OFT_CONFIG_AMDXDNA_PCI)`: `amdxdna_pci_drv.o`, `aie4_pci.o`,
  `amdxdna_mailbox.o` (PCI channel impl), `aie2_*`, `aie_psp.o`, `aie_smu.o`,
  `npu{1,3,4,5,6}_regs.o`, `aie4_sriov.o`;
* `amdxdna-$(OFT_CONFIG_AMDXDNA_OF)`: `amdxdna_platform.o`,
  `amdxdna_mailbox_plat.o` (plat channel impl), `aie4_plat.o`.

Note `amdxdna_mailbox.o` moves out of the always-built group into PCI-only,
since `amdxdna_mailbox_plat.o` now provides the same channel symbols for OF.

(Currently `aie4_ctx/message/error`, `amdxdna_dpt`, `amdxdna_error` sit under
`OFT_CONFIG_AMDXDNA_PCI`; they move to the shared group so the OF build gets
them.)
