.. SPDX-License-Identifier: GPL-2.0-only

MT7932 radio bring-up on the MacBook Neo
======================================

J700 MT7932 Wi-Fi uses cfg80211 and NetworkManager with firmware-managed
WPA2-CCMP authentication. Bluetooth uses a transport on the radio's second
PCIe function.

The Wi-Fi import is pinned to
``aurora-silicon/linux-neo-cleanroom-eryk-with-wifi``, branch
``wifi/width-qualification-20260912``, commit
``610cb463c03f9bc68a5d020e7ca443af1fda4856``. The import commit preserves all
26 driver files byte-for-byte. A separate commit carries the three tested
compatibility changes: bounded 64/65-record configuration parsing, native scan
IE encoding, and selected WPA2 PSK admission with trailing AKM alternatives.
This contribution imports an existing implementation; it does not claim an
independent rewrite. Existing source notices are retained. Co-authored by
DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.

Platform and configuration
--------------------------

Use the separately reviewed T8140 PCIe bootstrap described in
``Documentation/PCI/controller/apple-t8140.rst``. It runs during boot when
``CONFIG_PCIE_APPLE_PIODMA_DIAG=y``. Public m1n1 must populate the J700
``wifi0`` endpoint's own ``local-mac-address``; a zero placeholder is rejected.

The tested configuration includes ``CONFIG_MT7932_FULLMAC=m``,
``CONFIG_CFG80211=m``, ``CONFIG_BT_MTK7932_PCIE=m``, ``CONFIG_BT_BREDR=y``,
``CONFIG_BT_LE=y``, ``CONFIG_CRYPTO_AES=y`` and ``CONFIG_CRYPTO_CMAC=y``.
The Bluetooth transport can be built with modular Bluetooth and system
sleep. The separately selected bootstrap provider has its own lifecycle
restrictions. Kexec is excluded while either retained experiment is enabled.
The station uses 2.4 GHz channels 1-13 and the 5 GHz channels 36-64,
100-144 and 149-165 that the cfg80211 regulatory domain permits. Radar (DFS)
channels stay passive: the firmware listens on them while the station is not
associated, but never probes or joins an access point there, and scans made
while associated skip them. A channel whose restrictions the driver cannot
apply (reduced power, no OFDM, PSD limits) is disabled rather than used, and
so is a channel the country package does not permit: every power limit of
that channel in ``policy/<CC>.bin`` is left undefined.

Use the ordinary cfg80211 regulatory database and applicable country policy.
The validated first-admission fallback is kernel country 00 with firmware XZ.
A modular cfg80211 loads ``regulatory.db`` from the root filesystem; a
built-in one tries before the root filesystem is mounted and then needs
``iw reg reload``.

The radio functions appear while the kernel boots, before the root
filesystem is mounted, and ``mt7932-fullmac`` reads its firmware while
probing. Build it as a module so that udev loads it from the root
filesystem, or provide its firmware in the initramfs. The driver disables
ASPM and clock power management on its link before starting the device,
matching the tested admission sequence, which used the PCIe ASPM
performance policy.

``mt7932_bt_pcie`` defers its probe until ``mt7932-fullmac`` is bound to
function 0, so the Bluetooth firmware always starts after the Wi-Fi firmware,
as in the tested sequence, however the drivers are loaded. Bluetooth therefore
stays unbound while the Wi-Fi driver is missing or has failed. A device link
makes the driver core unbind Bluetooth before Wi-Fi; unloading
``mt7932_bt_pcie`` does not affect Wi-Fi. The probe rejects a function that is
not cold, which is what now prevents a reprobe after a failed or uncertain
admission; ``enable=0`` leaves the function unbound. The transport also
disables ASPM on the shared link. Like the Wi-Fi driver, it reads its firmware
while probing, so build it as a module. The tested Wi-Fi driver owns function
0 and Bluetooth owns function 1.

NetworkManager supplies the existing saved WPA2 profile. Use a unique profile
name or UUID and the current interface name; an older ``neo-wifi0`` profile
will not match an interface named ``wlan0``. KDE audio requires BlueZ,
PipeWire, its PulseAudio compatibility service, WirePlumber and the Bluetooth
audio plugins in the logged-in user's session.

Local firmware inputs
---------------------

No firmware, calibration, unit address, credentials or captured command
payloads are distributed in this contribution. Install lawfully obtained
local inputs for the target unit under ``/lib/firmware``. Do not substitute
another unit's calibration or address. A missing or invalid input must remain
a startup failure rather than an excuse to invent calibration or power data.

Wi-Fi requests the following files under ``mediatek/mt7932/``:

* ``IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin`` and ``IZUBA_W7932_2.bin``:
  the original firmware containers accepted by the driver's parser.
* ``ppr.bin``: the original power-on calibration request payload.
* ``wcal.bin`` and ``oca2.bin``: the unit's factory calibration; OCA2 retains
  its original BLOB container, rather than an older replay-stream wrapper.
* ``config-original.bin``: the bounded J7CF package with the original 64 or
  65 configuration records. Required keys and record shapes are validated.
* ``policy/world-XZ.bin`` and, when available, the matching country package
  under ``policy/``: J7RP containers with original-derived modes and power
  tables. The parser checks the country and table shapes. Relabeling a
  different country's payload is not a supported conversion.

Bluetooth requests these files under ``mediatek/``:

* ``MT7932B1_OS_TypeB_2.0.177.0_260706180356.bin`` or the supported fallback
  ``MT7932B1_OS_TypeB_0.1.133.0_260128190103.bin``;
* ``j700-mt7932-btcal.bin`` (388 bytes);
* ``MT7932_PTB_IzubaA_0.1.0.0_20251021141303.ptx`` (198 bytes);
* ``j700-mt7932-bdaddr.bin`` (the unit's six-byte Bluetooth address).

Extraction and packaging require the original local assets. The Linux loader
does not grant redistribution rights to them. Do not include those assets or
their unit-specific digests in bug reports, commits or test artifacts.

Repeatable physical network test
--------------------------------

The manual test is installed as a selftest data file rather than an automatic
test: activating a real saved network requires an explicit profile selection.
It selects the profile's requested band and checks an actual scan result,
WPA2 activation, DHCP route, gateway replies, DNS, verified HTTPS response,
KDE processes and fatal kernel/driver diagnostics. It never asks for or prints
the saved password. Run as root, with a fresh attempt name::

  python3 tools/testing/selftests/drivers/net/mt7932_e2e.py \
    --expected-release "$(uname -r)" --profile "YOUR SAVED PROFILE" \
    --connect --band a --attempt five-ghz

Use ``--band bg`` for 2.4 GHz. The JSON artifact path is printed on completion;
each of three traffic samples includes the downloaded response's size and
SHA256. Connection names, SSIDs, unit addresses and credentials are omitted
from the result. A successful host serial command is not a test pass: require
``WIFI_PHYSICAL_NETWORK_E2E_PASS`` in the target artifact.

Qualification and remaining work
--------------------------------

* PCI bootstrap memory remains retained until external reset. Controller
  removal, memory reuse, suspend and kexec are unqualified.
* Bluetooth system sleep uses a bounded firmware quiesce/restore handshake
  around HCI suspend/resume, preserving its DMA arena. Hibernation is rejected.
  Removal first quiesces firmware, then stops work and interrupts, disables
  bus mastering and drains pending
  PCI transactions before freeing DMA. An unconfirmed drain retains storage
  until reset. Firmware recovery and warm reprobe are unsupported; use a fresh
  cold boot after removing or faulting an active transport.
* The Bluetooth software queue limit does not provide HCI backpressure;
  saturation can drop an accounted frame.
* Arbitrary scan IEs, WPA3/SAE, required MFP, AP/P2P, general country-package
  generation, roaming and long-duration reliability are unqualified.
* SCO/headset microphone, LE Audio/ISO and simultaneous headset audio are
  unqualified.
