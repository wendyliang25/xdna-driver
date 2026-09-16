// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025, Advanced Micro Devices, Inc. All rights reserved.

#ifndef PCIDEV_UMQ_H
#define PCIDEV_UMQ_H

#include "../pcidev.h"

namespace shim_xdna {

class pdev_umq : public pdev
{
public:
  using pdev::pdev;

public:
  bool
  is_cache_coherent() const override;

  uint64_t
  get_heap_paddr() const override;

  void *
  get_heap_vaddr() const override;

  bool
  is_umq() const override;

  void
  create_drm_bo(bo_info *arg) const override;

private:
  virtual void
  on_first_open() const override;

  virtual void
  on_last_close() const override;
};

class pdev_pf : public pdev_umq
{
public:
  pdev_pf(std::shared_ptr<const platform_drv>& driver,
          const std::string& sysfs_name);

  bool
  is_umq() const override;

  void
  create_drm_bo(bo_info *arg) const override;
};

// A UMQ device that is NOT DMA cache-coherent (the aie2ps platform npu12).
// Identical to pdev_umq except it reports non-coherent, so buffer::sync()
// performs real cache maintenance (routed through the driver SYNC_BO on the
// non-coherent platform) instead of skipping it.
class pdev_umq_nc : public pdev_umq
{
public:
  using pdev_umq::pdev_umq;

  bool
  is_cache_coherent() const override;
};

}

#endif
