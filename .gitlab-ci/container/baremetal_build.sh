#!/usr/bin/env bash
# shellcheck disable=SC2154 # arch is assigned in previous scripts

set -e
set -o xtrace

# Fetch the arm-built rootfs image and unpack it in our x86_64 container (saves
# network transfer, disk usage, and runtime on test jobs)

S3_PATH="https://${S3_HOST}/${S3_KERNEL_BUCKET}"

if curl -L --retry 3 -f --retry-delay 10 -s --head "${S3_PATH}/${FDO_UPSTREAM_REPO}/${LAVA_DISTRIBUTION_TAG}/lava-rootfs.tar.zst"; then
  ARTIFACTS_URL="${S3_PATH}/${FDO_UPSTREAM_REPO}/${LAVA_DISTRIBUTION_TAG}"
else
  ARTIFACTS_URL="${S3_PATH}/${CI_PROJECT_PATH}/${LAVA_DISTRIBUTION_TAG}"
fi

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    "${ARTIFACTS_URL}"/lava-rootfs.tar.zst -o rootfs.tar.zst
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
    curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
        -O "${KERNEL_IMAGE_BASE}"/arm64/cheza-kernel

    DEVICE_TREES=""
    DEVICE_TREES="$DEVICE_TREES apq8016-sbc-usb-host.dtb"
    DEVICE_TREES="$DEVICE_TREES apq8096-db820c.dtb"

    for DTB in $DEVICE_TREES; do
	curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
            -O "${KERNEL_IMAGE_BASE}/arm64/$DTB"
    done

    popd

    # Download and extract the firmware for the Qualcomm DUTs
    curl --location --fail --retry-connrefused --retry 3 --retry-delay 10 \
        -O http://ftp.de.debian.org/debian/pool/non-free-firmware/f/firmware-nonfree/firmware-qcom-soc_20230210-5_all.deb

    mkdir firmware/
    dpkg-deb -x firmware-qcom-soc_20230210-5_all.deb firmware/

    # Copy only the firmware files for the devices in CI
    install -Dm644 firmware/lib/firmware/qcom/a300_pfp.fw /rootfs-arm64/lib/firmware/qcom/a300_pfp.fw
    install -Dm644 firmware/lib/firmware/qcom/a300_pm4.fw /rootfs-arm64/lib/firmware/qcom/a300_pm4.fw
    install -Dm644 firmware/lib/firmware/qcom/a530_pfp.fw /rootfs-arm64/lib/firmware/qcom/a530_pfp.fw
    install -Dm644 firmware/lib/firmware/qcom/a530_pm4.fw /rootfs-arm64/lib/firmware/qcom/a530_pm4.fw
    install -Dm644 firmware/lib/firmware/qcom/a530_zap.mdt /rootfs-arm64/lib/firmware/qcom/a530_zap.mdt
    install -Dm644 firmware/lib/firmware/qcom/a530v3_gpmu.fw2 /rootfs-arm64/lib/firmware/qcom/a530v3_gpmu.fw2
    install -Dm644 firmware/lib/firmware/qcom/apq8096/a530_zap.mbn /rootfs-arm64/lib/firmware/qcom/apq8096/a530_zap.mbn
    install -Dm644 firmware/lib/firmware/qcom/a630_gmu.bin /rootfs-arm64/lib/firmware/qcom/a630_gmu.bin
    install -Dm644 firmware/lib/firmware/qcom/a630_sqe.fw /rootfs-arm64/lib/firmware/qcom/a630_sqe.fw
    install -Dm644 firmware/lib/firmware/qcom/sdm845/a630_zap.mbn /rootfs-arm64/lib/firmware/qcom/sdm845/a630_zap.mbn

    rm firmware-qcom-soc_20230210-5_all.deb
    rm -rf firmware/
fi
