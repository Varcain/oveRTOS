#!/bin/bash
# One-time network setup for QEMU shared-memory Ethernet bridge.
#
# Creates a TAP interface and configures NAT so the QEMU guest can
# reach the internet and the host can reach the guest's web dashboard.
#
# Addresses match the oveRTOS networking example app:
#   Guest:   172.1.1.2/24 (static IP in app.c)
#   Gateway: 172.1.1.1/24 (this host TAP interface)
#
# Usage:
#   sudo ./qemu-net-setup.sh            # Create TAP + NAT
#   sudo ./qemu-net-setup.sh --teardown # Remove TAP + NAT
#   sudo ./qemu-net-setup.sh --status   # Show current state

set -euo pipefail

TAP_DEV="tap-ove"
TAP_ADDR="172.1.1.1/24"
TAP_SUBNET="172.1.1.0/24"

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must be run as root (sudo $0)" >&2
    exit 1
fi

# Resolve the real user behind sudo
REAL_USER="${SUDO_USER:-$(logname 2>/dev/null || echo root)}"

teardown() {
    echo "=== Tearing down QEMU network ==="
    # Remove NAT rule (best-effort)
    iptables -t nat -D POSTROUTING -s "${TAP_SUBNET}" -j MASQUERADE 2>/dev/null || true
    # Remove TAP interface
    if ip link show "${TAP_DEV}" &>/dev/null; then
        ip link delete "${TAP_DEV}"
        echo "Removed ${TAP_DEV}"
    else
        echo "${TAP_DEV} does not exist"
    fi
    echo "Done."
}

status() {
    echo "=== QEMU Network Status ==="
    if ip link show "${TAP_DEV}" &>/dev/null; then
        echo "TAP interface: UP"
        ip addr show "${TAP_DEV}" 2>/dev/null | grep -E "inet |state "
    else
        echo "TAP interface: NOT CONFIGURED"
        echo "Run: sudo $0"
    fi
    echo ""
    echo "NAT rule:"
    iptables -t nat -L POSTROUTING -n 2>/dev/null | grep "${TAP_SUBNET}" || echo "  (none)"
    echo ""
    echo "IP forwarding: $(cat /proc/sys/net/ipv4/ip_forward)"
}

setup() {
    echo "=== Setting up QEMU network ==="

    # Create TAP interface owned by the real user
    if ip link show "${TAP_DEV}" &>/dev/null; then
        echo "${TAP_DEV} already exists — skipping creation"
    else
        ip tuntap add dev "${TAP_DEV}" mode tap user "${REAL_USER}"
        echo "Created ${TAP_DEV} (user: ${REAL_USER})"
    fi

    # Configure address
    if ! ip addr show "${TAP_DEV}" | grep -q "${TAP_ADDR}"; then
        ip addr add "${TAP_ADDR}" dev "${TAP_DEV}"
    fi
    ip link set "${TAP_DEV}" up
    echo "Address: ${TAP_ADDR}"

    # Enable IP forwarding
    sysctl -q -w net.ipv4.ip_forward=1
    echo "IP forwarding: enabled"

    # Add NAT masquerade rule (idempotent)
    if ! iptables -t nat -C POSTROUTING -s "${TAP_SUBNET}" -j MASQUERADE 2>/dev/null; then
        iptables -t nat -A POSTROUTING -s "${TAP_SUBNET}" -j MASQUERADE
    fi
    echo "NAT: ${TAP_SUBNET} -> masquerade"

    echo ""
    echo "=== Setup complete ==="
    echo "Guest IP: 172.1.1.2  (configured in example app)"
    echo "Gateway:  172.1.1.1  (this host, TAP interface)"
    echo "Dashboard: http://172.1.1.2/ (after QEMU starts)"
    echo ""
    echo "To run:     make qemu_freertos_example_net_defconfig && make && make run"
    echo "To remove:  sudo $0 --teardown"
}

case "${1:-}" in
    --teardown) teardown ;;
    --status)   status ;;
    *)          setup ;;
esac
