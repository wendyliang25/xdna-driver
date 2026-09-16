// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025, Advanced Micro Devices, Inc. All rights reserved.

#include "../shim_debug.h"
#include "../kmq/pcidev.h"
#include "../umq/pcidev.h"
#include "platform_host.h"
#include "pcidrv_amdxdna.h"
#include "core/pcie/linux/system_linux.h"
#include <fstream>

namespace {

// A platform (non-PCI) amdxdna part is identified by its device-tree compatible,
// "amd,xdna-<id>".  Non-coherent parts are listed by compatible so the policy is
// keyed on the same string the OF match binds on (and stays valid when a partno
// string replaces the numeric id).
constexpr const char *NPU12_COMPATIBLE = "amd,xdna-1234"; // aie2ps platform npu12

int
get_dev_type(const std::string& sysfs)
{
  const std::string dev_type_path = shim_xdna::dev_sysfs_root(sysfs) + "/device_type";

  std::ifstream ifs(dev_type_path);
  if (!ifs.is_open())
    throw std::invalid_argument(dev_type_path + " is missing?");

  std::string line;
  std::getline(ifs, line);
  return static_cast<int>(std::stoi(line));
}

// Return the platform device's device-tree compatible (from <dev>/of_node/
// compatible), or "" for a PCI part (no such node).  The node is a NUL-separated
// list; the first entry is this device's own compatible.
std::string
get_device_compatible(const std::string& sysfs)
{
  const std::string path = shim_xdna::dev_sysfs_root(sysfs) + "/of_node/compatible";

  std::ifstream ifs(path);
  if (!ifs.is_open())
    return {};

  std::string compat;
  std::getline(ifs, compat, '\0'); // first NUL-terminated entry
  return compat;
}

// Per-device DMA cache-coherency policy, kept in one place so the *policy*
// (which parts are non-coherent) is centralised and the *key* (device compatible
// today, a partno string in the future) can change without touching the
// pdev-creation logic.  Coherency is really a device/DT fact; until it is
// sourced from the kernel this is the shim's single source of truth.
bool
device_is_cache_coherent(const std::string& sysfs)
{
  // aie2ps platform npu12 manages coherency in software; everything else
  // (e.g. soundwave PCI) is cache-coherent.
  if (get_device_compatible(sysfs) == NPU12_COMPATIBLE)
    return false;
  return true;
}

struct X
{
  X() {
    xrt_core::pci::register_driver(std::make_shared<shim_xdna::drv_amdxdna>());
    xrt_core::pci::register_driver(std::make_shared<shim_xdna::drv_amdxdna_mgmt>());
  }
} x;

}

namespace shim_xdna {

std::string
drv_amdxdna::
name() const
{
  return "amdxdna";
}

std::string
drv_amdxdna::
dev_node_prefix() const
{
  return "accel";
}

std::string
drv_amdxdna::
dev_node_dir() const
{
  return "accel";
}

std::string
drv_amdxdna::
sysfs_dev_node_dir() const
{
  return "accel";
}

std::shared_ptr<xrt_core::pci::dev>
drv_amdxdna::
create_pcidev(const std::string& sysfs) const
{
  auto driver = std::dynamic_pointer_cast<const drv>(shared_from_this());
  auto platform_driver = std::dynamic_pointer_cast<const platform_drv>(
    std::make_shared<const platform_drv_host>(driver));
  auto device_type = get_dev_type(sysfs);

  if (device_type == AMDXDNA_DEV_TYPE_KMQ)
    return std::make_shared<pdev_kmq>(platform_driver, sysfs);
  if (device_type == AMDXDNA_DEV_TYPE_UMQ) {
    // Non-coherent UMQ parts (e.g. the aie2ps platform npu12) need the shim to
    // do real cache maintenance; coherent ones (e.g. soundwave PCI) do not.
    if (device_is_cache_coherent(sysfs))
      return std::make_shared<pdev_umq>(platform_driver, sysfs);
    return std::make_shared<pdev_umq_nc>(platform_driver, sysfs);
  }
  if (device_type == AMDXDNA_DEV_TYPE_PF)
    return nullptr; // handled by drv_amdxdna_mgmt

  shim_err(EINVAL, "Unknown device type: %d", device_type);
}

bool
drv_amdxdna_mgmt::
is_user() const
{
  return false;
}

std::string
drv_amdxdna_mgmt::
name() const
{
  return "amdxdna";
}

std::string
drv_amdxdna_mgmt::
dev_node_prefix() const
{
  return "accel";
}

std::string
drv_amdxdna_mgmt::
dev_node_dir() const
{
  return "accel";
}

std::string
drv_amdxdna_mgmt::
sysfs_dev_node_dir() const
{
  return "accel";
}

std::shared_ptr<xrt_core::pci::dev>
drv_amdxdna_mgmt::
create_pcidev(const std::string& sysfs) const
{
  if (get_dev_type(sysfs) != AMDXDNA_DEV_TYPE_PF)
    return nullptr;
  auto driver = std::dynamic_pointer_cast<const drv>(shared_from_this());
  auto platform_driver = std::dynamic_pointer_cast<const platform_drv>(
    std::make_shared<const platform_drv_host>(driver));
  return std::make_shared<pdev_pf>(platform_driver, sysfs);
}

}
