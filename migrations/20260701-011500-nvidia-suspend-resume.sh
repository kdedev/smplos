#!/bin/bash
# Migration: Fix NVIDIA suspend/resume + DPMS black-screen on existing installs.
#
# Context: smplOS's installer configured `nvidia_drm modeset=1` but never set
#          up NVIDIA's power-management path. On wake from DPMS-off or suspend
#          the GPU had lost its framebuffer/VRAM contents, so the monitors lit
#          up (backlight on) but stayed black and unrecoverable — the user had
#          to hard power-cycle.
#
#          The fix (now also baked into the installer for fresh installs):
#            1. /etc/modprobe.d/nvidia.conf gains:
#                 options nvidia_drm modeset=1 fbdev=1
#                 options nvidia NVreg_PreserveVideoMemoryAllocations=1
#               fbdev=1 gives a real nvidia fbcon (clean DPMS/VT handoff);
#               PreserveVideoMemoryAllocations=1 keeps VRAM across power
#               transitions so the display returns instead of a black screen.
#            2. nvidia-suspend/resume/hibernate services enabled — these save
#               and restore VRAM around systemd sleep.
#            3. initramfs rebuilt so the new module options take effect (the
#               nvidia modules load early from the initramfs).
#
# Only runs on machines with an NVIDIA GPU. Idempotent and safe to re-run:
# every step checks state first and the initramfs is only rebuilt when the
# modprobe config actually changed. Takes effect on the next reboot.

set -euo pipefail

# ── Guard: NVIDIA GPU only ───────────────────────────────────────────────────
if ! lspci 2>/dev/null | grep -qi 'nvidia'; then
    echo "  No NVIDIA GPU detected, skipping"
    exit 0
fi

n_changes=0
conf_changed=0
failed=0
pending="$HOME/.local/state/smplos/nvidia-initramfs-pending"

# ── 1. modprobe power-management options ─────────────────────────────────────
MODPROBE="/etc/modprobe.d/nvidia.conf"
DESIRED='options nvidia_drm modeset=1 fbdev=1
options nvidia NVreg_PreserveVideoMemoryAllocations=1'

if [[ ! -f "$MODPROBE" ]] || [[ "$(cat "$MODPROBE" 2>/dev/null)" != "$DESIRED" ]]; then
    if [[ -f "$MODPROBE" ]]; then
        backup="$MODPROBE.smplos-before-$(date +%Y%m%d-%H%M%S)-$$"
        sudo cp -pL -- "$MODPROBE" "$backup"
        sudo cmp -s -- "$MODPROBE" "$backup" || {
            echo "  ERROR: NVIDIA configuration backup could not be verified"
            exit 1
        }
    fi
    mkdir -p "${pending%/*}"
    touch "$pending"
    if printf '%s\n' "$DESIRED" | sudo tee "$MODPROBE" >/dev/null 2>&1; then
        echo "  Updated $MODPROBE (fbdev + PreserveVideoMemoryAllocations)"
        conf_changed=1
        ((n_changes++)) || true
    else
        echo "  WARNING: could not write $MODPROBE (sudo required)"
        failed=1
    fi
else
    echo "  $MODPROBE already current"
fi

# ── 2. Enable NVIDIA sleep helper services ───────────────────────────────────
for svc in nvidia-suspend.service nvidia-resume.service nvidia-hibernate.service; do
    if ! systemctl is-enabled "$svc" >/dev/null 2>&1; then
        if sudo systemctl enable "$svc" >/dev/null 2>&1; then
            echo "  Enabled $svc"
            ((n_changes++)) || true
        else
            echo "  WARNING: could not enable $svc"
            failed=1
        fi
    fi
done

# ── 3. Rebuild initramfs so the new module options load early ────────────────
# Only when the modprobe config changed (the nvidia modules are pulled into the
# initramfs via MODULES+=, so their options must live there too).
if [[ "$conf_changed" -eq 1 || -f "$pending" ]]; then
    if command -v mkinitcpio >/dev/null 2>&1; then
        echo "  Rebuilding initramfs (mkinitcpio -P)…"
        if sudo mkinitcpio -P >/dev/null 2>&1; then
            echo "  initramfs rebuilt"
            rm -f -- "$pending"
            ((n_changes++)) || true
        else
            echo "  WARNING: mkinitcpio -P failed — run it manually before rebooting"
            failed=1
        fi
    else
        echo "  ERROR: mkinitcpio is missing; initramfs repair remains pending"
        failed=1
    fi
fi

if [[ "$failed" -ne 0 ]]; then
    echo "  ERROR: NVIDIA setup remains incomplete; resolve the errors before rebooting"
elif [[ "$n_changes" -gt 0 ]]; then
    echo "  NVIDIA suspend/resume fix applied — takes effect after next reboot"
else
    echo "  NVIDIA suspend/resume already configured, nothing to do"
fi

exit "$failed"
