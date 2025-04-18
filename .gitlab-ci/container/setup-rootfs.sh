#!/usr/bin/env bash
# shellcheck disable=SC1091 # The relative paths in this file only become valid at runtime.
# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# KERNEL_ROOTFS_TAG
set -ex

. setup-test-env.sh

export DEBIAN_FRONTEND=noninteractive

# Needed for ci-fairy s3cp
pip3 install --break-system-packages "ci-fairy[s3] @ git+http://gitlab.freedesktop.org/freedesktop/ci-templates@$MESA_TEMPLATES_COMMIT"

# Needed for manipulation with traces yaml files.
pip3 install --break-system-packages yq

passwd root -d
chsh -s /bin/sh

cat > /init <<EOF
#!/bin/sh
export PS1=lava-shell:
exec sh
EOF
chmod +x  /init

# Copy timezone file and remove tzdata package
rm -rf /etc/localtime
cp /usr/share/zoneinfo/Etc/UTC /etc/localtime

. strip-rootfs.sh
