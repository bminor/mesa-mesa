#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting
# shellcheck disable=SC1091 # paths only become valid at runtime

. "${SCRIPTS_DIR}/setup-test-env.sh"

section_start prepare_rootfs "Preparing root filesystem"

set -ex

# If we run in the fork (not from mesa or Marge-bot), reuse mainline kernel and rootfs, if exist.
BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_MAINLINE_HOST_PATH}"
if [ "$CI_PROJECT_PATH" != "$FDO_UPSTREAM_REPO" ]; then
    if ! curl -s -X HEAD -L --retry 4 -f --retry-delay 60 \
      "https://${BASE_SYSTEM_MAINLINE_HOST_PATH}/done"; then
	echo "Using kernel and rootfs from the fork, cached from mainline is unavailable."
	BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_FORK_HOST_PATH}"
    else
	echo "Using the cached mainline kernel and rootfs."
    fi
fi

rm -rf results
mkdir -p results/job-rootfs-overlay/

artifacts/ci-common/generate-env.sh > results/job-rootfs-overlay/set-job-env-vars.sh
cp artifacts/ci-common/capture-devcoredump.sh results/job-rootfs-overlay/
cp artifacts/ci-common/init-*.sh results/job-rootfs-overlay/
cp artifacts/ci-common/intel-gpu-freq.sh results/job-rootfs-overlay/
cp artifacts/ci-common/kdl.sh results/job-rootfs-overlay/
cp "$SCRIPTS_DIR"/setup-test-env.sh results/job-rootfs-overlay/

tar zcf job-rootfs-overlay.tar.gz -C results/job-rootfs-overlay/ .
ci-fairy s3cp --token-file "${S3_JWT_FILE}" job-rootfs-overlay.tar.gz "https://${JOB_ROOTFS_OVERLAY_PATH}"

# Prepare env vars for upload.
section_switch variables "Environment variables passed through to device:"
cat results/job-rootfs-overlay/set-job-env-vars.sh

section_switch lava_submit "Submitting job for scheduling"

touch results/lava.log
tail -f results/lava.log &
PYTHONPATH=artifacts/ artifacts/lava/lava_job_submitter.py \
	--farm "${FARM}" \
	--device-type "${DEVICE_TYPE}" \
	--boot-method "${BOOT_METHOD}" \
	--job-timeout-min ${JOB_TIMEOUT:-30} \
	--dump-yaml \
	--pipeline-info "$CI_JOB_NAME: $CI_PIPELINE_URL on $CI_COMMIT_REF_NAME ${CI_NODE_INDEX}/${CI_NODE_TOTAL}" \
	--rootfs-url "${ROOTFS_URL}" \
	--kernel-url-prefix "${KERNEL_IMAGE_BASE}/${DEBIAN_ARCH}" \
	--kernel-external "${EXTERNAL_KERNEL_TAG}" \
	--first-stage-init artifacts/ci-common/init-stage1.sh \
	--dtb-filename "${DTB}" \
	--jwt-file "${S3_JWT_FILE}" \
	--kernel-image-name "${KERNEL_IMAGE_NAME}" \
	--kernel-image-type "${KERNEL_IMAGE_TYPE}" \
	--visibility-group "${VISIBILITY_GROUP}" \
	--lava-tags "${LAVA_TAGS}" \
	--mesa-job-name "$CI_JOB_NAME" \
	--structured-log-file "results/lava_job_detail.json" \
	--ssh-client-image "${LAVA_SSH_CLIENT_IMAGE}" \
	--project-name "${CI_PROJECT_NAME}" \
	--starting-section "${CURRENT_SECTION}" \
	--job-submitted-at "${CI_JOB_STARTED_AT}" \
	- append-overlay \
		--name=mesa-build \
		--url="https://${PIPELINE_ARTIFACTS_BASE}/${LAVA_S3_ARTIFACT_NAME:?}.tar.zst" \
		--compression=zstd \
		--path="${CI_PROJECT_DIR}" \
		--format=tar \
	- append-overlay \
		--name=job-overlay \
		--url="https://${JOB_ROOTFS_OVERLAY_PATH}" \
		--compression=gz \
		--path="/" \
		--format=tar \
	- submit \
	>> results/lava.log
