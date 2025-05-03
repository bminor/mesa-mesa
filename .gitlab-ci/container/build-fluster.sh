#!/usr/bin/env bash

# shellcheck disable=SC1091 # The relative paths in this file only become valid at runtime.
# shellcheck disable=SC2034 # Variables are used in scripts called from here
# shellcheck disable=SC2086 # we want word splitting

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# DEBIAN_TEST_VIDEO_TAG

# Install fluster in /fluster.

# Do a very early check to make sure the tag is correct without the need of
# setting up the environment variables locally
ci_tag_build_time_check "FLUSTER_TAG"

set -uex

section_start fluster "Installing fluster"

FLUSTER_REVISION="e997402978f62428fffc8e5a4a709690d9ca9bc5"

git clone https://github.com/fluendo/fluster.git --single-branch --no-checkout

export SKIP_UPDATE_FLUSTER_VECTORS=false

check_fluster()
{
    S3_FLUSTER_TAR="${S3_HOST}/${S3_KERNEL_BUCKET}/$1/${DATA_STORAGE_PATH}/fluster/${FLUSTER_TAG}/vectors.tar.zst"
    if curl -L --retry 4 -f --retry-connrefused --retry-delay 30 -s --head \
      "https://${S3_FLUSTER_TAR}"; then
        echo "Fluster vectors are up-to-date, skip rebuilding them."
        export SKIP_UPDATE_FLUSTER_VECTORS=true
    fi
}

check_fluster "${FDO_UPSTREAM_REPO}"
if ! $SKIP_UPDATE_FLUSTER_VECTORS; then
    check_fluster "${CI_PROJECT_PATH}"
fi

pushd fluster || exit
git checkout "${FLUSTER_REVISION}"
popd || exit

if ! $SKIP_UPDATE_FLUSTER_VECTORS; then
    # Download the necessary vectors: H264, H265 and VP9
    # When updating FLUSTER_REVISION, make sure to update the vectors if necessary or
    # fluster-runner will report Missing results.
    fluster/fluster.py download -j ${FDO_CI_CONCURRENT:-4} \
	JVT-AVC_V1 JVT-FR-EXT JVT-MVC JVT-SVC_V1 \
	JCT-VC-3D-HEVC JCT-VC-HEVC_V1 JCT-VC-MV-HEVC JCT-VC-RExt JCT-VC-SCC JCT-VC-SHVC \
	VP9-TEST-VECTORS-HIGH VP9-TEST-VECTORS

    # Build fluster vectors archive and upload it
    tar --zstd -cf "vectors.tar.zst" fluster/resources/
    ci-fairy s3cp --token-file "${S3_JWT_FILE}" "vectors.tar.zst" "https://${S3_FLUSTER_TAR}"
fi

mv fluster/ /

if $SKIP_UPDATE_FLUSTER_VECTORS; then
    curl -L --retry 4 -f --retry-connrefused --retry-delay 30 \
      "${FDO_HTTP_CACHE_URI:-}https://${S3_FLUSTER_TAR}" | tar --zstd -x -C /
fi

section_end fluster
