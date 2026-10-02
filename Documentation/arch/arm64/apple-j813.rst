.. SPDX-License-Identifier: GPL-2.0

=====================================
Apple J813 initial native Linux boot
=====================================

J813 is the 13-inch M5 MacBook Air (T8142, Mac17,3). This configuration

The device tree describes all ten CPU affinities. For the initial one-CPU
baseline, keep secondary cores stopped in m1n1 and use ``maxcpus=1``.
The loader removes unavailable CPUs and fills the release addresses of the
remaining nodes. The P-core
affinities are 0x10100 through 0x10103, rather than the 0x100 through 0x103
values in the Apple Device Tree.

The CPU compatible strings identify T8142 explicitly. The firmware's reused
``sawtooth`` and ``everest`` names do not identify the M5 microarchitecture.

SMP bring-up
============

M5 secondary CPUs can return from WFI with cleared general-purpose registers.
Both M5 MIDRs (parts 0x062 and 0x063) select the Apple register-preserving idle
path. Without it, all ten CPUs can start and then panic in their idle tasks.

SMP also requires the M5-capable m1n1 secondary-start path: dense PMGR CPU
masks, correct reset vectors and per-core stacks. Verify all ten cores there
before preparing the FDT, and use ``maxcpus=10`` for the Linux test. The
existing J813 DT and configuration support this handoff.

On J813, this path reached the RAM shell with CPUs 0-9 online. A five-second
sleep advanced every CPU's architectural timer count by 1,251 interrupts.
Ten concurrent, individually pinned jobs each hashed 16 MiB of zeros to
``080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e``.
Verify affinity through ``Cpus_allowed_list`` and retain the console output,
kernel configuration hash and payload hashes when repeating the test.
Hotplug, deep sleep and KVM remain unqualified.

Bootloader requirements
=======================

Use the J813-capable Aurora m1n1 and U-Boot bring-up stack. A stock M1/M2
loader is not sufficient. Before entering Linux, the loader must:

* Supply usable memory and reserved regions. T8142 DRAM starts at
  0x10000000000; the DTS memory size is only a loader placeholder.
* Fill CPU release addresses and remove CPUs which have not been started.
* Fill the firmware framebuffer's address, size, geometry and format before
  enabling it.
* Prepare DMA protection only for devices enabled in the supplied FDT. The
* Leave the console, interrupt controller and display powered. This minimal
  DT does not describe their power domains.
* Give U-Boot a T8142 memory map covering its MMIO and high DRAM addresses.

The AIC driver selects the T8142-specific guest timer masking path from
``apple,t8142-aic3``. Writes to the legacy VM_TMR_FIQ_ENA_EL2 register trap
on this SoC. The driver uses the architectural EL02 timer control masks
instead. Older AIC variants retain their existing path.

Build
=====

On an arm64 Linux build host with the usual kernel build dependencies::

  out="$PWD/../j813-build"
  make ARCH=arm64 O="$out" \
      KCONFIG_ALLCONFIG=arch/arm64/configs/j813.config allnoconfig
  make ARCH=arm64 O="$out" -j"$(nproc)" W=1 Image apple/t8142-j813.dtb

cross-compiling with GCC from another architecture.

With dtschema installed, check the affected bindings and DTB::

  make ARCH=arm64 O="$out" CHECK_DTBS=y \
      apple/t8142-j813.dtb

Boot and qualification
======================

Use a RAM-only initramfs with a serial shell and BusyBox (including
``mount``, ``sleep``, ``cat``, ``uname`` and ``sha256sum``). Its init must
mount proc, sysfs and devtmpfs, keep PID 1 alive and make no disk mounts.
Record the source commit, any uncommitted patch, final kernel config and
SHA-256 hashes of Image, DTB, initramfs, m1n1 and U-Boot with the console
capture. Verify uploaded payload bytes before handing over to U-Boot.

The initial command line is::

  earlycon=dockchannel,0x38812c000 console=tty0 console=ttyDC0 keep_bootcon maxcpus=1 rdinit=/init panic=0

Use m1n1 to load Image, the initramfs, the DTB and U-Boot into RAM and
prepare the FDT. Pass that prepared FDT through U-Boot's ``booti`` command.
Do not install this experimental payload into the resident boot image.

At the shell, capture the following into the host's serial log::

  echo J813_BOOT_PROOF_BEGIN
  uname -a
  tr '\000' '\n' < /proc/device-tree/compatible
  cat /sys/devices/system/cpu/online
  cat /proc/uptime
  cat /proc/interrupts
  sleep 2
  cat /proc/uptime
  cat /proc/interrupts
  head -5 /proc/meminfo
  cat /proc/mounts
  cat /proc/fb
  dmesg
  echo J813_BOOT_PROOF_END

Verify T8142/J813 identity, one online CPU, advancing uptime and timer
interrupt counts, an interactive shell, a registered framebuffer and only
RAM-backed filesystems. Repeat after an idle interval and after a fresh
RAM boot. Preserve the complete console transcript and a hash manifest;
an Image build or successful loader exit alone is not a boot result.
Review captures for device identifiers and boot entropy before publishing.

records its measured resources; the qualified console path is DockChannel.
