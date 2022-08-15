#!/bin/bash
# Copyright 2020 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

set -ex
root="$(cd $(dirname ${BASH_SOURCE[0]}) && pwd)"
if [[ -z ${CC} ]]; then export CC=clang; fi
if [[ -z ${CXX} ]]; then export CXX=clang++; fi
if [[ -z ${DEPOT_TOOLS_DIR} ]]; then DEPOT_TOOLS_DIR=/opt/depot_tools; fi
if [[ -z ${CCACHE_DIR} && -e /dev/shm ]]; then
  mkdir -p /dev/shm/redpanda
  export CCACHE_DIR=/dev/shm/redpanda
fi

ccache -p # print the config
ccache -s # print the stats before reusing
ccache -z # zero the stats

go=$(type -P go) # `which` isn't available on fedora:36 docker image

# oss build doesn't succeed with python3.10, which is the system-wide python ver of fedora:36
# we use python3.9 explicitly for the build
virtualenv --python="/usr/bin/python3.9" venv
. venv/bin/activate
pip3 install jinja2

# Change Debug via  -DCMAKE_BUILD_TYPE=Debug
cmake -DCMAKE_BUILD_TYPE=Release \
  -B$root/build \
  -H$root \
  -GNinja \
  -DCMAKE_C_COMPILER=$CC \
  -DCMAKE_CXX_COMPILER=$CXX \
  -DCMAKE_GO_BINARY=$go \
  -DDEPOT_TOOLS_DIR=$DEPOT_TOOLS_DIR \
  "$@"

(cd $root/build && ninja)

ccache -s # print the stats after the build

(cd $root/build && ctest --output-on-failure -R _rpunit)
