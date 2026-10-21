.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT7932 radio on the MacBook Neo
========================================

J700 MT7932 Wi-Fi uses cfg80211 with firmware-managed WPA2-PSK, WPA3-SAE
and Enhanced Open (OWE) authentication on 2.4 and 5 GHz. Bluetooth uses a
transport on the radio's second PCIe function.

The Wi-Fi driver carries configuration, scan IE and WPA2 AKM compatibility
fixes on top of its original import. Existing source notices are retained.
Co-authored by DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.

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
restrictions. A runtime guard refuses kexec while radio admission or retained
bootstrap DMA ownership is active. Kernels may still support kexec elsewhere.
The station uses 2.4 GHz channels 1-13 and the 5 GHz channels 36-64,
100-144 and 149-165 that the cfg80211 regulatory domain permits. Radar (DFS)
channels stay passive: the firmware listens on them while the station is not
associated, but never probes or joins an access point there, and scans made
while associated skip them. A channel whose restrictions the driver cannot
apply (reduced power, no OFDM, PSD limits) is disabled rather than used, and
so is a channel the country package does not permit: every power limit of
that channel in the applied country package is left undefined.

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
stays unbound while the Wi-Fi driver is missing or has failed, including when
a failed Wi-Fi function stays bound to keep DMA it could not stop. A device link
makes the driver core unbind Bluetooth before Wi-Fi; unloading
``mt7932_bt_pcie`` does not affect Wi-Fi. Wi-Fi pins its module before publishing
DMA pointers, and refuses function reset while its Bluetooth sibling is live.
The probe rejects a function that is
not cold, which is what now prevents a reprobe after a failed or uncertain
admission. The transport also
disables ASPM on the shared link. Like the Wi-Fi driver, it reads its firmware
while probing, so build it as a module. The tested Wi-Fi driver owns function
0 and Bluetooth owns function 1.

Use a saved NetworkManager WPA2 profile matching the current interface.
KDE audio requires BlueZ, PipeWire, its PulseAudio compatibility service,
WirePlumber and Bluetooth audio plugins in the logged-in user's session.

Country packages
----------------

Every regulatory change selects a country package again, without a reload.
For country CC the driver uses the first of these that exists and is valid:

1. ``policy-v2/CC.bin``: J7RP version 2 (version 1 plus 6 GHz VLP pages);
2. ``policy/CC.bin``: J7RP version 1 (2.4 and 5 GHz, at most 16 KiB);
3. ``policy/world-XZ.bin``: the world package.

Country 00 uses only the world package. ``policy/`` holds version 1 only and
``policy-v2/`` version 2 only; a package in the wrong directory is refused.
Kernels without version 2 support never read ``policy-v2/``, so version 2
packages can be installed while those kernels remain bootable.

A country without a usable package of its own falls back to the world
package and logs one ``REGULATORY_FALLBACK`` line. cfg80211 keeps the
requested country and its channel list; the firmware gets domain XZ and
only the channels both the country and the world package permit. The world
package has no 6 GHz section, so 6 GHz stays off. Installing the country's
package takes effect on the next regulatory change. Only a missing or
invalid world package leaves the radio without a regulatory domain.

The applied package can be read back, as root, from
``/sys/kernel/debug/ieee80211/phyN/mt7932_policy``: requested country,
firmware domain, source directory and path, J7RP version, size, the first
eight bytes of its SHA-256, the 6 GHz primaries in the firmware domain, the
fallback reason and the result of each lookup.

Station security
----------------

The driver advertises PSK and SAE with CCMP only. Firmware performs SAE
authentication and the four-way handshake and owns PTK, GTK and IGTK;
there is no host key-install interface or external-auth exchange.

SAE offload accepts nl80211 WPA version 3, the combined version 2/3 mask,
and version 2 for older offload clients. PSK requires the version 2 bit.
The first AKM always selects the firmware arm; a version mask does not
permit security fallback.

For SAE, userspace supplies SAE authentication, an 8-63 byte password and
required or optional management frame protection (MFP). Firmware uses required
MFP for the SAE arm. On a PSK+SAE transition network, the first AKM supplied
by userspace selects the arm: PSK keeps the existing binary-PMK request and
disables MFP, while SAE uses the password and required MFP. PSK with required
MFP and PSK-SHA256 are unsupported.

BIP-CMAC-128 is not advertised because WPA2 with required MFP has not been
implemented or qualified. Advertising BIP would also make supplicants request
required MFP on WPA2 networks. iwd requires BIP-CMAC-128 before selecting SAE,
so it remains WPA2-only with this driver, including on transition networks;
it cannot join a pure-SAE network. SAE offload support here targets
NetworkManager with wpa_supplicant, pending the hardware qualification below.

The standard SAE PWE settings (unspecified, hunting-and-pecking,
hash-to-element and both) are accepted; firmware chooses the exchange without
a host PWE field. Exact PWE policy behavior on 2.4 and 5 GHz still needs
qualification. SAE-PK, SAE-EXT-KEY and additional ciphers are not advertised. OWE and
6 GHz support are described below.

J700 hardware testing with NetworkManager and wpa_supplicant on a PSK+SAE
transition network has confirmed, on 2.4 GHz channel 6 and 5 GHz channel 149:

* SAE with required MFP (``key-mgmt=sae``), including DHCP, DNS and HTTPS;
* PSK with MFP disabled, and a ``wpa-psk`` profile with optional MFP, which
  joins the transition network as PSK;
* repeated reconnection through NetworkManager after each change of profile.

The request layout, transition admission and PMK derivation also have
automated tests. A pure WPA3 network, exact PWE policy behavior, WPA2
throughput regression comparisons and long-duration reliability still need
qualification. 2.4 GHz throughput is lower than 5 GHz with every security
mode.

The SAE key cipher is at offset 0x44; offset 0x48 is the 16-bit key-flags
field and stays zero.

Enhanced Open (OWE)
-------------------

The station advertises the OWE AKM and ``OWE_OFFLOAD`` on the supported 2.4
and 5 GHz channels. Use an OWE profile with CCMP, open-system authentication
and required management frame protection, without a PSK or SAE password.
Firmware performs the Diffie-Hellman exchange, generates RSN information and
owns the handshake and key installation. Userspace must honor the offload
feature instead of expecting a host-generated Diffie-Hellman element in the
association response.

Before the first OWE association of a firmware session, the driver reserves
temporary live peer IDs and performs a typed disconnect and queue cleanup.
An OWE authentication timeout, firmware rejection or firmware disconnect
then permits one retry inside the connect worker. Before retrying, the
driver waits for its typed disconnect and retires queues and the old peer
through the normal ownership fences. A
failed retirement, cancellation or RF failure ends the request. The retry
shares a ten-second association-event deadline and programs the station
identity again before association. No further retry is made by the worker.

For OWE transition mode, the supplicant selects the encrypted BSS and can use
a directed scan to find its hidden SSID. Pure OWE, transition mode and
first-authentication behavior after a cold boot still need hardware
qualification on every band.

6 GHz country packages
----------------------

The qualified two-stream PHY profile exposes UNII-5..8 with station HE and
HE 6 GHz capabilities, up to 80 MHz. Firmware band 3 is independent of the
single-interface MAC index. Channel numbers always carry their nl80211 band.
Only SAE and OWE with required MFP are admitted on 6 GHz. Hunting-and-pecking
only is refused there; firmware owns H2E. These checks extend the SAE and
OWE layers without another crypto or authentication implementation.

Wildcard 6 GHz scans listen passively. Hidden SSIDs use directed scanning
when NO_IR permits it. RNR-only requests remain passive until target-list
serialization is implemented. Full-band scans are split into bounded
32-channel commands. Firmware event channel layout is checked before OCA
association calibration, queue binding and the two association power pages.

J7RP version 1 remains valid and never authorizes 6 GHz. Version 2 packages
are installed under ``policy-v2/``. Generate them from the original text
tables and the installed version-1 packages::

  python3 tools/asahi/mt7932-policy-6ghz.py --tables /path/to/IZUBA \
    --legacy-dir /path/to/policy --output-dir /path/to/new-policy
  python3 tools/asahi/mt7932-policy-6ghz-tests.py

The default outputs are AU.bin, US.bin, CA.bin and DE.bin (the EU test
country). Input hashes pin the macOS 25G83 revision of the tables. All files
are validated before publication and existing output files are not replaced.
Only code is distributed; the power tables and generated packages stay on
the machine they come from.

The producer reads VLP sections and the 6 GHz country matrix. Mode 2 in a
channel's operating-class set grants VLP eligibility, intersected with
available per-rate data. It emits 14 native per-rate pages and 14 legacy
pages, including the eight-entry selector-02 and selector-0f association
pages. The unsupported-rate bitmap is not generated, so the firmware's
default rate accounting applies and still needs qualification.

No regulatory model is replaced: cfg80211 flags, including NO_IR and VLP
client restrictions, remain authoritative. The driver consumes EIRP and PSD
limits by reducing native half-dBm rate cells before publication, preserving
unsupported and unpopulated sentinels. Association reuses the reduced pages.
PSD rounding uses allocation bandwidth lower bounds, so it cannot raise a
limit. Unrepresentable reductions disable the affected rate. Legacy-band
power handling is unchanged. LPI/AFC operation is not added by VLP packages.

J7RP v2 format
~~~~~~~~~~~~~~

The unchanged 16-byte J7RP header, mode records and 13 version-1 tables are
followed by a 24-byte extension header. The outer version is 2 and maximum
package size is 65536 bytes:

* bytes 0..3: ``6GRP``;
* byte 4: class 1 (VLP); byte 5: 14 dBm profile ceiling;
* byte 6: power-page count (1..32); byte 7: zero;
* bytes 8..15: little-endian permission bitmap, bit i = primary 1+4*i,
  with bits 59..63 clear;
* bytes 16..17: alpha2; byte 18: native half-dBm rate-cell contract 1;
* bytes 19..23: zero.

A page has CID, a zero byte, a little-endian 16-bit body length, then body.
CID 0x5d uses version 3, tag 0x2a, band 3, matching alpha2, a 44-byte header
and 122-byte entries. CID 0x95 uses a zero/reserved header without embedded
length or country, band 3 and 33-byte legacy entries with 0x7f payload.
The containing package supplies country identity for the latter. Both
association pages are required exactly once with eight entries and length
1020. Association selectors are not channel permissions.

Association submits those two pages after the queue bind. No transmit
power envelope request is built. Generic runtime D7 calibration requests
for 6 GHz are still rejected. These gaps and the SAE, OWE and per-country
RF behavior need more hardware qualification.

Local firmware inputs
---------------------

Firmware and calibration are not included. Install local inputs for the
target unit under ``/lib/firmware``; another unit's calibration or address
cannot be substituted. Missing or invalid inputs prevent startup.

Wi-Fi requests the following files under ``mediatek/mt7932/``:

* ``IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin`` and ``IZUBA_W7932_2.bin``:
  original firmware containers.
* ``ppr.bin``: the original power-on calibration request payload.
* ``wcal.bin`` and ``oca2.bin``: the unit's factory calibration; OCA2 uses
  its original BLOB container.
* ``config-original.bin``: the bounded J7CF package with the original 64 or
  65 configuration records, including all required keys.
* ``policy/world-XZ.bin`` and, when available, the matching country package
  under ``policy-v2/`` or ``policy/`` (see `Country packages`_): J7RP
  containers with original-derived modes and power tables. Relabeling a
  different country's payload is unsupported.

Bluetooth requests these files under ``mediatek/``:

* ``MT7932B1_OS_TypeB_2.0.177.0_260706180356.bin`` or the supported fallback
  ``MT7932B1_OS_TypeB_0.1.133.0_260128190103.bin``;
* ``j700-mt7932-btcal.bin`` (388 bytes);
* ``MT7932_PTB_IzubaA_0.1.0.0_20251021141303.ptx`` (198 bytes);
* ``j700-mt7932-bdaddr.bin`` (the unit's six-byte Bluetooth address).

Extraction and packaging require the original local assets. Keep these
assets and unit identities out of commits and public test reports.

Repeatable physical network test
--------------------------------

The manual test activates a saved profile and checks scan, WPA2, DHCP,
gateway replies, DNS, HTTPS, KDE and kernel diagnostics. It is not run
automatically. Run as root, with a fresh attempt name::

  python3 tools/testing/selftests/drivers/net/mt7932_e2e.py \
    --expected-release "$(uname -r)" --profile "YOUR SAVED PROFILE" \
    --connect --band a --attempt five-ghz

Use ``--band bg`` for 2.4 GHz. Require ``WIFI_PHYSICAL_NETWORK_E2E_PASS`` in
the JSON artifact printed on completion. The artifact records three traffic
samples and response hashes, omitting network names, addresses and credentials.

Receive diagnostics
-------------------

``ethtool -S <interface>`` reports lifetime host counters for malformed,
unsupported and integrity-rejected RX records, wrong peers, unprotected
data on protected connections, retry duplicates and allocation failures.
Take before/after differences for each traffic sample; reconnecting does
not reset these counters. Descriptor rejection counters include management
records and off-channel traffic. The other counters describe the admitted
peer's Ethernet receive path.

Retry and A-MSDU counters count received MSDU records, including split
A-MSDU subframes, before protection and duplicate filtering. They are not
over-air MPDU counts, BA-window sizes, TX retries or firmware ACK counters.
Duplicate suppression records sequences only after the protection check,
so a rejected unprotected frame cannot suppress a later decrypted retry.

Use the current RX bitrate from ``iw dev <interface> station dump`` while
traffic is running. The driver expires that sample after one second; an
idle or unsupported RX vector can leave it absent. TX bitrate is not
reported because the firmware's TX-vector query cannot attest peer
identity or freshness. Missing TX bitrate is not a zero-rate measurement.

Dynamic debug messages ``ASSOC_HT_CAPABILITIES``,
``ASSOC_HE_CAPABILITIES`` and ``ASSOC_START`` describe the firmware-generated
station capabilities and channel event. ``POWER_SAVE_SUBMITTED`` records
the requested mode and submission status, without asserting firmware
readback. These messages contain neither credentials nor packet payloads.

Limitations
-----------

* PCI bootstrap memory remains retained until external reset. Controller
  removal and memory reuse are unqualified. Runtime guards refuse reset and
  kexec; suspend-to-idle retains the successfully admitted arena.
* Bluetooth system sleep uses a bounded firmware quiesce/restore handshake
  around HCI suspend/resume, preserving its DMA arena. Hibernation is rejected.
  Removal first quiesces firmware, then stops work and interrupts, disables
  bus mastering and drains pending
  PCI transactions before freeing DMA. An unconfirmed drain retains storage
  until reset. A controller that stops answering commands, or reports a
  hardware error, is reset over HCI and set up again; if that setup fails,
  the transport faults. Firmware reloading and warm reprobe are
  unsupported; use a fresh cold boot after removing or faulting an active
  transport.
* The Bluetooth software queue limit does not provide HCI backpressure;
  saturation can drop an accounted frame.
* Arbitrary scan IEs, AP/P2P, general country-package generation, roaming and
  long-duration reliability are unqualified. SAE and required MFP qualification
  are described under Station security.
* SCO/headset microphone, LE Audio/ISO and simultaneous headset audio are
  unqualified.
