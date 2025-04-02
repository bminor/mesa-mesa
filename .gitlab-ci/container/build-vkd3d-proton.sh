#!/bin/bash

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# DEBIAN_TEST_VK_TAG
set -ex

uncollapsed_section_start vkd3d-proton "Building vkd3d-proton"

VKD3D_PROTON_COMMIT="7eef0a64e3fc107a6cb10e3abd1b56d21b575de8"

VKD3D_PROTON_DST_DIR="/vkd3d-proton-tests"
VKD3D_PROTON_SRC_DIR="/vkd3d-proton-src"
VKD3D_PROTON_BUILD_DIR="/vkd3d-proton-build"

git clone https://github.com/HansKristian-Work/vkd3d-proton.git --single-branch -b master --no-checkout "$VKD3D_PROTON_SRC_DIR"
pushd "$VKD3D_PROTON_SRC_DIR"
git checkout "$VKD3D_PROTON_COMMIT"
git submodule update --init --recursive
git submodule update --recursive

meson setup                              \
      -D enable_tests=true               \
      --buildtype release                \
      --prefix "$VKD3D_PROTON_DST_DIR"   \
      --strip                            \
      --libdir "lib"                     \
      "$VKD3D_PROTON_BUILD_DIR/build"

ninja -C "$VKD3D_PROTON_BUILD_DIR/build" install

install -m755 -t "${VKD3D_PROTON_DST_DIR}/" "$VKD3D_PROTON_BUILD_DIR/build/tests/d3d12"

mkdir "$VKD3D_PROTON_DST_DIR/tests"
cp \
  "tests/test-runner.sh" \
  "tests/d3d12_tests.h" \
  "$VKD3D_PROTON_DST_DIR/tests/"
popd

rm -rf "$VKD3D_PROTON_BUILD_DIR"
rm -rf "$VKD3D_PROTON_SRC_DIR"

section_end vkd3d-proton
