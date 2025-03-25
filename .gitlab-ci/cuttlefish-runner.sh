#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting
# shellcheck disable=SC1091 # paths only become valid at runtime

. "${SCRIPTS_DIR}/setup-test-env.sh"

section_start cuttlefish_setup "cuttlefish: setup"
set -xe

# Structured tagging check for angle
if [ -n "$ANGLE_TAG" ]; then
    # Bail out if the ANGLE_TAG differs from what is offered in the system
    ci_tag_test_time_check "ANGLE_TAG"
fi

export PATH=/cuttlefish/bin:/android-tools/platform-tools:$PATH
export LD_LIBRARY_PATH=/cuttlefish/lib64:${CI_PROJECT_DIR}/install/lib:$LD_LIBRARY_PATH

# Pick up a vulkan driver
ARCH=$(uname -m)
export VK_DRIVER_FILES=${CI_PROJECT_DIR}/install/share/vulkan/icd.d/${VK_DRIVER:-}_icd.$ARCH.json

syslogd

chown root:kvm /dev/kvm

pushd /cuttlefish

# Add a function to perform some tasks when exiting the script
function my_atexit()
{
  # shellcheck disable=SC2317
  HOME=/cuttlefish stop_cvd -wait_for_launcher=40

  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/logs/logcat $RESULTS_DIR || true
  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/kernel.log $RESULTS_DIR || true
  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/logs/launcher.log $RESULTS_DIR || true
}

# stop cuttlefish if the script ends prematurely or is interrupted
trap 'my_atexit' EXIT
trap 'exit 2' HUP INT PIPE TERM

ulimit -S -n 32768

VSOCK_BASE=10000 # greater than all the default vsock ports
VSOCK_CID=$((VSOCK_BASE + (CI_JOB_ID & 0xfff)))

HOME=/cuttlefish launch_cvd \
  -daemon \
  -verbosity=VERBOSE \
  -file_verbosity=VERBOSE \
  -use_overlay=false \
  -vsock_guest_cid=$VSOCK_CID \
  -enable_audio=false \
  -enable_bootanimation=false \
  -enable_minimal_mode=true \
  -enable_modem_simulator=false \
  -guest_enforce_security=false \
  -report_anonymous_usage_stats=no \
  -gpu_mode="$ANDROID_GPU_MODE" \
  -cpus=${FDO_CI_CONCURRENT:-4} \
  -memory_mb 8192 \
  -kernel_path="/cuttlefish/bzImage" \
  -initramfs_path="/cuttlefish/initramfs.img"

sleep 1

popd

ADB=adb

$ADB wait-for-device root
sleep 1
$ADB shell echo Hi from Android
# shellcheck disable=SC2035
$ADB logcat dEQP:D *:S &

# overlay vendor

OV_TMPFS="/data/overlay-remount"
$ADB shell mkdir -p "$OV_TMPFS"
$ADB shell mount -t tmpfs none "$OV_TMPFS"

$ADB shell mkdir -p "$OV_TMPFS/vendor-upper"
$ADB shell mkdir -p "$OV_TMPFS/vendor-work"

opts="lowerdir=/vendor,upperdir=$OV_TMPFS/vendor-upper,workdir=$OV_TMPFS/vendor-work"
$ADB shell mount -t overlay -o "$opts" none /vendor

$ADB shell setenforce 0

# download Android Mesa from S3
MESA_ANDROID_ARTIFACT_URL=https://${PIPELINE_ARTIFACTS_BASE}/${S3_ANDROID_ARTIFACT_NAME}.tar.zst
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 -o ${S3_ANDROID_ARTIFACT_NAME}.tar.zst ${MESA_ANDROID_ARTIFACT_URL}
mkdir /mesa-android
tar -C /mesa-android -xvf ${S3_ANDROID_ARTIFACT_NAME}.tar.zst
rm "${S3_ANDROID_ARTIFACT_NAME}.tar.zst" &

INSTALL="/mesa-android/install"

# replace on /vendor/lib64

$ADB push "$INSTALL/lib/libgallium_dri.so" /vendor/lib64/libgallium_dri.so
$ADB push "$INSTALL/lib/libEGL.so" /vendor/lib64/egl/libEGL_mesa.so
$ADB push "$INSTALL/lib/libGLESv1_CM.so" /vendor/lib64/egl/libGLESv1_CM_mesa.so
$ADB push "$INSTALL/lib/libGLESv2.so" /vendor/lib64/egl/libGLESv2_mesa.so

$ADB push "$INSTALL/lib/libvulkan_lvp.so" /vendor/lib64/hw/vulkan.lvp.so
$ADB push "$INSTALL/lib/libvulkan_virtio.so" /vendor/lib64/hw/vulkan.virtio.so

$ADB shell rm -f /vendor/lib64/egl/libEGL_emulation.so
$ADB shell rm -f /vendor/lib64/egl/libGLESv1_CM_emulation.so
$ADB shell rm -f /vendor/lib64/egl/libGLESv2_emulation.so

# Remove built-in ANGLE, we'll supply our own if needed
$ADB shell rm -f /vendor/lib64/egl/libEGL_angle.so
$ADB shell rm -f /vendor/lib64/egl/libGLESv1_CM_angle.so
$ADB shell rm -f /vendor/lib64/egl/libGLESv2_angle.so

if [ -n "$ANGLE_TAG" ]; then
  $ADB push /angle/libEGL_angle.so /vendor/lib64/egl/libEGL_angle.so
  $ADB push /angle/libGLESv1_CM_angle.so /vendor/lib64/egl/libGLESv1_CM_angle.so
  $ADB push /angle/libGLESv2_angle.so /vendor/lib64/egl/libGLESv2_angle.so
fi

# Check what GLES implementation Surfaceflinger is using before copying the new mesa libraries
while [ "$($ADB shell dumpsys SurfaceFlinger | grep GLES:)" = "" ] ; do sleep 1; done
$ADB shell dumpsys SurfaceFlinger | grep GLES

# restart Android shell, so that surfaceflinger uses the new libraries
$ADB shell stop
$ADB shell start

# Check what GLES implementation Surfaceflinger is using after copying the new mesa libraries
# Note: we are injecting the ANGLE libs in the vendor partition, so we need to check if the
#       ANGLE libs are being used after the shell restart
while [ "$($ADB shell dumpsys SurfaceFlinger | grep GLES:)" = "" ] ; do sleep 1; done
MESA_RUNTIME_VERSION="$($ADB shell dumpsys SurfaceFlinger | grep GLES:)"

if [ -n "$ANGLE_TAG" ]; then
  ANGLE_HASH=$(head -c 12 /angle/version)
  if ! printf "%s" "$MESA_RUNTIME_VERSION" | grep --quiet "${ANGLE_HASH}"; then
    echo "Fatal: Android is loading a wrong version of the ANGLE libs: ${ANGLE_HASH}" 1>&2
    exit 1
  fi
else
  MESA_BUILD_VERSION=$(cat "$INSTALL/VERSION")
  if ! printf "%s" "$MESA_RUNTIME_VERSION" | grep --quiet "${MESA_BUILD_VERSION}$"; then
     echo "Fatal: Android is loading a wrong version of the Mesa3D libs: ${MESA_RUNTIME_VERSION}" 1>&2
     exit 1
  fi
fi

if [ -n "$USE_ANDROID_CTS" ]; then
  # The script sets EXIT_CODE
  . "$(dirname "$0")/android-cts-runner.sh"
else
  # The script sets EXIT_CODE
  . "$(dirname "$0")/android-deqp-runner.sh"
fi

exit $EXIT_CODE
