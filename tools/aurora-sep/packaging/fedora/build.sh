#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Fedora packages for Touch ID on the Apple SEP:
#
#  - libfprint: Fedora's own source package, rebuilt with the aurora driver
#    patch from ../../patches and nothing else changed. The release gains an
#    ".aurora1" suffix, so it replaces the distribution's build in place.
#  - aurora-sep: the driver loader, its service and the fprintd policy.
#
# It installs the build dependencies with dnf, so run it as root in a
# throwaway container of the Fedora release you are building for:
#
#   podman run --rm -v <linux tree>:/src:ro,z -v <output dir>:/out:z \
#       registry.fedoraproject.org/fedora:45 /src/<path to this script> /out
#
# The binary and source packages land in the output directory.

set -eu

out=${1:?usage: build.sh <output directory>}
here=$(cd "$(dirname "$0")" && pwd)
sep=$(cd "$here/../.." && pwd)
patch=$sep/patches/libfprint-1.94.100-apple-sep.patch
header=${APPLE_SEP_HEADER:-$sep/../../include/uapi/linux/apple-sep.h}
patched_version=1.94.100

for f in "$patch" "$header"; do
    [ -f "$f" ] || { echo "build.sh: $f is missing" >&2; exit 1; }
done
mkdir -p "$out"

top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
rpmtop=$top/rpmbuild

dnf -y install rpm-build systemd-rpm-macros

# Fedora's libfprint, exactly as the distribution builds it.
(cd "$top" && dnf -y download --source libfprint)
srpm=$(ls "$top"/libfprint-*.src.rpm)
rpm -i --define "_topdir $rpmtop" "$srpm"
spec=$rpmtop/SPECS/libfprint.spec

version=$(rpmspec -q --srpm --qf '%{version}\n' "$spec")
if [ "$version" != "$patched_version" ]; then
    echo "build.sh: this Fedora ships libfprint $version, the patch is for $patched_version" >&2
    exit 1
fi
cp "$patch" "$header" "$rpmtop/SOURCES/"

# Add the patch and the header, mark the release, and point the compiler at
# the header: <linux/apple-sep.h> is not in the distribution's kernel headers.
# %autosetup applies every PatchN. Stop if any anchor has moved.
awk -v patch="$(basename "$patch")" -v date="$(LC_ALL=C date -u '+%a %b %d %Y')" '
    /^Release:/ && !release {
        sub(/[[:space:]]*$/, ".aurora1"); print; release = 1; next
    }
    /^Source0:/ && !source {
        print
        print "Source9000:     apple-sep.h"
        print "Patch9000:      " patch
        source = 1; next
    }
    /^%meson[[:space:]]/ && !build {
        print "# The aurora driver includes <linux/apple-sep.h> from the Aurora kernel."
        print "mkdir -p aurora-include/linux"
        print "cp -p %{SOURCE9000} aurora-include/linux/"
        print "export CFLAGS=\"%{build_cflags} -I$PWD/aurora-include\""
        print; build = 1; next
    }
    /^%changelog/ && !changelog {
        print
        print "* " date " Aurora Silicon - " "%{version}-%{release}"
        print "- Add the Apple SEP (aurora) driver from aurora-silicon/linux"
        print "  tools/aurora-sep/patches/" patch
        print ""
        changelog = 1; next
    }
    { print }
    END {
        if (!(release && source && build && changelog)) {
            print "build.sh: libfprint.spec changed shape; update the edit" > "/dev/stderr"
            exit 1
        }
    }
' "$spec" >"$spec.aurora"
mv "$spec.aurora" "$spec"

dnf -y builddep "$spec"
rpmbuild --define "_topdir $rpmtop" -ba "$spec"

# The loader, its units and the desktop integration, from this tree.
dnf -y builddep "$here/aurora-sep.spec"
rpmbuild --define "_topdir $rpmtop" --define "_sourcedir $sep" \
    -ba "$here/aurora-sep.spec"

cp -p "$rpmtop"/RPMS/*/*.rpm "$rpmtop"/SRPMS/*.rpm "$out/"
ls -l "$out"
