#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting
# shellcheck disable=SC1091 # paths only become valid at runtime

export PATH=/android-tools/platform-tools:$PATH

# Set default ADB command if not set already

: "${ADB:=adb}"

$ADB wait-for-device root
sleep 1

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

# replace libraries

$ADB shell rm -f /vendor/lib64/libgallium_dri.so*
$ADB shell rm -f /vendor/lib64/egl/libEGL_mesa.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv1_CM_mesa.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv2_mesa.so*

$ADB push "$INSTALL/lib/libgallium_dri.so" /vendor/lib64/libgallium_dri.so
$ADB push "$INSTALL/lib/libEGL.so" /vendor/lib64/egl/libEGL_mesa.so
$ADB push "$INSTALL/lib/libGLESv1_CM.so" /vendor/lib64/egl/libGLESv1_CM_mesa.so
$ADB push "$INSTALL/lib/libGLESv2.so" /vendor/lib64/egl/libGLESv2_mesa.so

$ADB shell rm -f /vendor/lib64/hw/vulkan.lvp.so*
$ADB shell rm -f /vendor/lib64/hw/vulkan.virtio.so*
$ADB shell rm -f /vendor/lib64/hw/vulkan.intel.so*

$ADB push "$INSTALL/lib/libvulkan_lvp.so" /vendor/lib64/hw/vulkan.lvp.so
$ADB push "$INSTALL/lib/libvulkan_virtio.so" /vendor/lib64/hw/vulkan.virtio.so
$ADB push "$INSTALL/lib/libvulkan_intel.so" /vendor/lib64/hw/vulkan.intel.so

$ADB shell rm -f /vendor/lib64/egl/libEGL_emulation.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv1_CM_emulation.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv2_emulation.so*

$ADB shell rm -f /vendor/lib64/egl/libEGL_angle.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv1_CM_angle.so*
$ADB shell rm -f /vendor/lib64/egl/libGLESv2_angle.so*

$ADB push /angle/libEGL_angle.so /vendor/lib64/egl/libEGL_angle.so
$ADB push /angle/libGLESv1_CM_angle.so /vendor/lib64/egl/libGLESv1_CM_angle.so
$ADB push /angle/libGLESv2_angle.so /vendor/lib64/egl/libGLESv2_angle.so

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
