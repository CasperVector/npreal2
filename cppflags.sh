#!/bin/sh -

case "$1" in

4)
DIST_ID=0
[ -f /etc/kylin-release ] && DIST_ID=4
[ -f /etc/fedora-release ] && DIST_ID=5
flags="-DDIST_ID=$DIST_ID"

if [ -f /etc/redhat-release -o -f /etc/centos-release ]; then
RHE_VER="$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d \")"
RHE_MJ="$(echo "$RHE_VER" | sed 's/\..*//')"
RHE_MN="$(echo "$RHE_VER" | sed 's/.*\.//')"
flags="$flags -DRHEL8 -DRHE_MJ=$RHE_MJ -DRHE_MN=$RHE_MN -DRHE_REL=\$(RHE_REL)"
fi

if [ -f /etc/redhat-release -o -f /etc/centos-release ]; then
echo "RHE_REL := \$(uname -r | sed 's/.*-//; s/\..*//')"
fi
echo "EXTRA_CFLAGS += $flags"; echo
;;

6)

OS_TYPE=0
ID="$(sed -n 's/^ID=//p' /etc/os-release 2> /dev/null | tr -d \")"
[ "$ID" = rocky ] && OS_TYPE=6
flags="-DOS_TYPE=$OS_TYPE"
echo "EXTRA_CFLAGS += $flags"; echo
;;

esac

