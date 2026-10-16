# Shared xART gigalocker safety gate

## Current default

`xart_writes=1` is the driver default. The APFS extent-ownership checks,
record validation and write-ordering checks remain mandatory; unsupported
layouts fail closed. `xart_writes=0 provision_keybag=0` explicitly selects
read-only operation. `provision_keybag=1` is the default: create the Linux
identity if missing and recover an existing identity without replacing it.
The historical evidence below records the earlier opt-in deployments and
does not establish qualification on every supported Mac or APFS layout.

## Other OSes' records: `xart_guard`

The gigalocker is shared by every OS on the Mac, and nothing in a session
record says which OS wrote it. With `xart_guard=1` the driver lists every
session record the enclave creates while Linux runs, in
`/var/lib/aurora-sep-xart-owned.bin`, before the record is written. It then
refuses to rewrite or delete any session record that was already in the
gigalocker and is not on that list. `xart_guard=2` also refuses writes to the
two shared root records. A refused write fails the enclave's request the way
a read-only gigalocker does: Touch ID may then be unavailable, but no other
OS's record changes.

The default, `xart_guard=-1`, takes the platform's level:

| Platform | Default |
| --- | --- |
| M1 (T8103), M1 Pro/Max (T6000/T6001) | 1 |
| M2 (T8112), M2 Pro/Max (T6020/T6021) | 1 |
| M3 (T8122) | 1 |
| MacBook Neo (T8140) | 0 |

The Neo stays at 0 because it already holds Linux records from before the
list existed, and a guarded boot would treat those as another OS's.

Two cases need a decision on an M1, M2 or M3 Mac:

- On a cold-boot M1-family Mac, creating the identity keybag also rewrites a
  session record from before Linux. This was seen on an M1 Pro (T6000,
  J316s) with the macOS 13.5 sepOS the driver loads. The guard refuses that
  write, so Linux cannot create its keybag there unless
  `options apple_sep xart_guard=0` is set. Set it only once the rewrite has
  been accepted for that Mac.
- Records Linux created while the guard was off are not on the list, so a
  guarded boot refuses them too. A Mac that already used Touch ID with the
  guard off needs `xart_guard=0` to keep using those records.

## Cold-boot SEP Macs rewrite a pre-Linux record (2026-10-16)

On Macs where the driver cold-boots the SEP, creating Linux's identity
keybag reaches beyond Linux's own records. This was observed on an M1 Pro
(T6000, J316s), with the macOS 13.5 sepOS loaded by the driver.

Creating the keybag makes the enclave do three things:

- write one new session record;
- rewrite a shared root record;
- rewrite one session record that existed before Linux ever ran.

The rewritten record is one of the four session records the enclave reads at
every attach, and it grows by 71 bytes per keybag created. It is not tied to
the OS identity, so giving Linux its own identity does not keep this write
away from records macOS also reads. A driver that refuses to rewrite records
Linux did not create cannot create a keybag on these Macs.

Warm-attach Macs behave differently. On an M2 (T8112, J413), keybag creation
made no xART writes at all. On the J700, the macOS-era records stayed
byte-identical through Linux enrolment.

On a T600x Mac where Linux has created its keybag, treat macOS Touch ID as
unverified until macOS Touch ID unlock and keychain access have been checked
there. The other cold-boot SoCs (T8103, T6001, T6002) probably behave the
same way; that is untested.

## Earlier deployment policy

The iBoot APFS container and its XART `.gl` file are shared with macOS. The
deployed `m2fix4` kernel remains read-only. The `m2fix5` source has an explicit
`xart_writes=1` opt-in; it accepts writes only when the APFS-resolved extent is
single-owner, unsnapshotted, un-cloned, and all existing records validate.
`xart_start_sector` remains only an assertion against the APFS result. The
opt-in is for the controlled J414s hardware test, not general APFS write
support or a promise that restoring an old raw image reverses SEP anti-replay
state. Do not enable it on other installations without equivalent checks.

## Evidence collected on 2026-09-21

- A read-only inspection of `m2`'s iBoot partition found a complete, valid
  checkpoint at transaction 57, one XART volume (role `0x100`), and one
  unencrypted `.gl` extent: 6 MiB beginning at APFS block 1236. All 11 live
  gigalocker records in that extent passed their payload CRC checks. The raw
  signature heuristic happened to select the same block on this snapshot.
- A synthetic image with the first root moved to slot 10 reproduces a legacy
  locator failure: the highest-scoring window begins 90 APFS blocks after the
  true file start and extends beyond `.gl`. This proves ambiguity in that
  locator, **not** the cause of the previously damaged installation.
- Static inspection of the macOS 27.2 AppleSEPManager kernel collection found
  `gl_rec_write` allocating a fresh 0x9000-byte slot, writing it, then clearing
  0x1000 bytes at the former slot. The low-level write path also calls a disk
  cache-synchronization ioctl. `gl_fixup_rev_init` validates records and
  handles duplicates during open. These observations do not prove that our
  APFS ownership and failure ordering match macOS.

## Before authorizing writes

1. Confirm the exact macOS vnode/raw-device and extent-locking contract,
   including whether any APFS copy-on-write, snapshot, encryption, or remap
   state can invalidate the physical mapping during Linux's lifetime.
2. Verify record ordering, duplicate selection, failed-write recovery,
   barriers/cache synchronization, and revision behavior against both macOS
   code and controlled non-production tests.
3. Compare a pre/post macOS-and-Linux power-cycle trace, with APFS checksums,
   `.gl` mapping and records checked before and after. Keep a recoverable image
   of the iBoot partition before the first write experiment.

The current APFS locator supports only a complete checkpoint, single-node
object maps and file-system tree, and a single unencrypted 6 MiB `.gl` extent.
Unsupported layouts fail closed; this is not a general APFS implementation.

## Read-only J414s recovery image (2026-09-23)

After the minimal stage2 made the SEP mailbox and RNG functional, a fresh
read-only inspection still found one unencrypted 6 MiB `.gl` extent at APFS
block 1236, 11 live records with valid payload CRCs, and no malformed records.
The selected container checkpoint was xid 136. No xART writes or keybag
provisioning were attempted.

The 524,288,000-byte `iBootSystemContainer` partition was copied through a
read-only block handle and compressed to
`work/m2-tb-sep-deploy/iboot-system-container-2026-09-22.img.zst` (7,857,330
bytes, mode 0600). The local archive passed `zstd -t`; its decompressed
SHA-256 matched a separate read of the partition:
`09206fcf0500f5646ea04ec37cf1fc8a0493f1c5ee3271c2af562ed9b39a6d37`.
The compressed archive SHA-256 is
`8a885a2397c26404cf8395ca64b279efc793f9118eae408f2a6d84fe9af1ded8`.
The temporary m2 copy was removed after verification. This is a recovery
artifact, **not** evidence that Linux may safely write APFS-owned `.gl` blocks.

## Narrow write-ownership evidence (2026-09-23)

The live m2 partition SHA-256 still matches the independent backup hash above.
The updated read-only inspector selected checkpoint xid 136 and checked the
current xART volume superblock and checksum-validated extent-reference tree:
volume flags `0x1` (unencrypted only), zero snapshots, zero pending revert
fields, and an inode with no clone flag. The sole `.gl` extent is APFS blocks
1236–2771 (1536 blocks, 6 MiB); its only physical extent-reference record is
kind `APFS_KIND_NEW`, owner 16 (the inode's private ID), reference count 1.
The inspector now rejects missing, overlapping, shared, cloned/snapshotted,
or mismatched references, and four synthetic tests pass. The `m2fix5` kernel
independently checks the same narrow ownership conditions on its block handle
before arming writes and refuses a writable scan with malformed or duplicate
xART records. These checks establish the current physical mapping and lack of
APFS sharing; they do not by themselves establish macOS's full record-update
or SEP anti-replay recovery semantics. The APFS field meanings come from the
[Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf).
