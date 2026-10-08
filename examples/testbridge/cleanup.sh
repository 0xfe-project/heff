#!/bin/sh
# What testbridge made, deleted (kubenet's cmd/testbridge/cleanup.sh):
# the bridge, and the veth pair, by its peer.
ip link del kube-bridge
ip link del kni0
