#!/usr/bin/env bash
# shellcheck disable=SC2154 # arch is assigned in previous scripts

set -e
set -o xtrace

# Fetch the arm-built rootfs image and unpack it in our x86_64 container (saves
# network transfer, disk usage, and runtime on test jobs)

# shellcheck disable=SC2034 # S3_BASE_PATH is used in find_s3_project_artifact
S3_BASE_PATH="${S3_HOST}/${S3_KERNEL_BUCKET}"
ARTIFACTS_PATH="${LAVA_DISTRIBUTION_TAG}/lava-rootfs.tar.zst"
ARTIFACTS_URL="$(find_s3_project_artifact "${ARTIFACTS_PATH}")"

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    "${ARTIFACTS_URL}" -o rootfs.tar.zst
mkdir -p /rootfs-"$arch"
tar -C /rootfs-"$arch" '--exclude=./dev/*' --zstd -xf rootfs.tar.zst
rm rootfs.tar.zst

if [[ $arch == "arm64" ]]; then
    mkdir -p /baremetal-files
    pushd /baremetal-files

    curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
        -O "${KERNEL_IMAGE_BASE}"/arm64/Image
    curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
        -O "${KERNEL_IMAGE_BASE}"/arm64/Image.gz
    popd
fi
