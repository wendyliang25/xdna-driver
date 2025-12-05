#! /bin/bash -

# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2024-2025, Advanced Micro Devices, Inc.
#

SCRIPT_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))

if [ -x "$(command -v apt-get)" ]; then
    apt-get install -y jq
    # Install Google Test for vxdna unit tests
    apt-get install -y libgtest-dev
elif [ -x "$(command -v dnf)" ]; then
    dnf install -y jq
    # Install Google Test for vxdna unit tests
    dnf install -y gtest-devel
fi

$SCRIPT_DIR/../xrt/src/runtime_src/tools/scripts/xrtdeps.sh
