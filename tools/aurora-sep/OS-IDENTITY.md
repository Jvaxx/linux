<!-- SPDX-License-Identifier: GPL-2.0-only OR MIT -->
# Keep the SEP OS identity stable

The original T2587 premise was wrong: on a normally installed Neo (T8140),
`/chosen/apfs-preboot-uuid` identifies the booted volume group, so Linux has
its own anti-replay lane. It is not automatically the macOS identity. The
operator's 2026-10-11 hardware check established that distinction; the
September collision was caused by explicitly pinning macOS's identity.
The source review below supports the forwarding and selection mechanism;
it does not repeat that hardware check.

## Selection contract

The driver must preserve the identity used by an existing install:

1. If either `apple_sep.os_uuid_hi` or `apple_sep.os_uuid_lo` is nonzero, use
   both explicit halves, in big-endian order. One zero half is valid. Both
   zero halves mean there is no override.
2. Otherwise use the parsed, nonzero `/chosen/apfs-preboot-uuid`.
3. If the property is missing, malformed or zero, leave the identity
   unavailable. Do not generate an identity or silently choose another lane.

`sep.rs::prepare_os_uuid` uses `os_identity::select`; KUnit guards explicit
precedence, zero halves, byte order, repeatable chosen selection and absence.
The T8140 profile remains `IdentitySource::Chosen`, with a regression test.
This is a software regression guard, not a new persisted runtime binding.
No host-state format, identity-source policy or enrolment state changes.
The explicit-selection log reports only its source, never identity bytes.

At the reviewed SEP topic base, the profile's `identity` field was not read
by selection. In particular, T6020's `HostPersisted` declaration was not an
implemented persistence policy. Changing T8140's enum and wiring it into
selection would change the effective identity on existing machines.

## Boot-chain evidence

The reviewed kernel is `omacom/linux-aurora` commit
`d75387d811a1a400a3fdbf9e69bb3b38c7bbba90` (the SEP topic base).
Its original `sep.rs:1537-1561` implements params, then chosen, then none.
`dt.rs::preboot_uuid` reads that exact property and rejects malformed and
all-zero values. The supplied older reference `44d5afc79da7` has the same
selection order.

The MIT-licensed m1n1 SEP handoff at commit
`851c795061e1ce9f1823b52a3217ce1fd14885a9`, `src/kboot.c`, calls
`dt_forward_chosen_str` for `apfs-preboot-uuid` and `boot-uuid` from the ADT's
`/chosen` node. That helper copies the bytes verbatim to the FDT; it does
not search APFS for macOS or substitute a macOS identity. The comment about
what macOS's driver reads describes the property, not ownership of its value.
The reviewed `v1.6.3-m3air.stage1` commit
`328a7ee2593b83da8e34eaec9d666e086e6a389b` instead formats a binary property
or copies a string, with `boot-uuid` as fallback, into `apfs-preboot-uuid`.
Neither implementation establishes that a particular installed boot chain
contains that code. Older branches may lack forwarding; an absent property
must remain an unavailable identity rather than justify inventing one.

## Existing installs and alternatives

An existing Fedora Neo with explicit parameters keeps those exact parameters
and its lane. Preserve them across updates, recovery entries and boot-loader
changes. Do not replace them with a generated value or remove them merely
because chosen forwarding exists. No re-enrolment is needed for this change.

An existing normal Omarchy Neo using chosen keeps that identity and its
existing enrolments. No migration refusal or re-enrolment is appropriate.
A system actually pinned to macOS's identity is a separate recovery case:
stop automatic SEP consumers and obtain an owner-approved recovery plan.
Do not automatically change the lane or delete either OS's enrolments.
Previously invalidated credentials are not repaired by this patch.

The proposed host-persisted source switch was withdrawn. The unpublished
`ac14bdc7c654` experiment must not be promoted: it would switch fresh chosen
installs to another lane and refuse existing chosen-lane state as a migration.

An installer or boot hook can preserve an explicitly configured identity,
but must never add, remove or rotate one on an enrolled install unattended.
A new random per-install value is unnecessary for a normal chosen install.
Switching boot entries between explicit and chosen is safe only when their
effective bytes are known to be equal; a source-label change alone does not
prove a lane change. Recreating a volume group, restoring boot configuration
or reinstalling can change chosen and needs the same continuity review.

There is no recorded enrolment-to-OS-identity provenance in the current
host-state format. Source labels, the presence of keybag files and generic
SEP errors cannot establish which lane contains an enrolment. Consequently
`aurora-sep-ready` must not infer "migration required" from these facts.
No runtime provenance warning is added. A future opt-in continuity marker
would need a trusted baseline, compare effective identities rather than
labels, preserve old installs on first adoption, and never expose identities
or read SEP payloads merely to generate a warning.

## Hardware plan — not executed

All steps require a new controller request naming the exact device, explicit
policy approval, session/source/resource/ownership gates and owner consent.
This document does not authorize or request a hardware session.

To avoid orphaning existing enrolments, do not boot this candidate or invoke
SEP on any device with valuable enrolments or SEP-protected keys. A spare
Linux volume on such a device is insufficient: SEP and xART are device-wide.
Existing Fedora and Omarchy continuity checks are offline boot-configuration
comparisons only, performed by the authorized owner without printing values.
Preserve original entries and report only equal/different and source labels.

Use only an owner-approved disposable Neo with no valuable credentials in
any OS for runtime qualification. Before any boot, disable automatic SEP
loaders and consumers in every test entry, arrange password/recovery access,
and record the approved source commit and unchanged boot-chain build.
Do not extract, print or attach UUIDs, SEP payloads, MACs or calibration data.
Do not collect raw console logs: older kernel and m1n1 builds print identities.

1. Establish disposable Linux and macOS test enrolments using the known-good
   kernel and normal, unchanged chosen boot entry. Verify both and record
   boolean results only. All test credentials are disposable from the outset.
2. Boot the candidate using the same volume group and boot arguments. First
   confirm the effective identity equals the baseline through a private
   in-memory comparison that emits only equality and source. If it differs,
   stop before loading the driver; return to the unchanged original entry.
3. Only after that comparison, allow the normal loader on the disposable
   device. Verify the existing Linux credential without re-enrolling, reboot
   and verify again, then verify the disposable macOS credential. Matches
   write anti-replay state, so these are not read-only checks.
4. Separately qualify explicit-parameter continuity on a disposable test
   setup: enrol under a known-good explicit entry, preserve both halves
   verbatim, then repeat candidate/reboot verification. Do not change sources
   beneath an already-enrolled setup. The synthetic KUnit cases cover zero
   halves and missing chosen without touching hardware.
5. Roll back only the kernel/boot entry while retaining the current matching
   identity and host state. Never restore stale host-state or xART snapshots
   after SEP writes: file rollback does not roll back enclave anti-replay
   counters. On any mismatch or failed verification, stop consumers and
   preserve failure evidence; do not reset, delete, rotate or re-enrol to make
   the test pass. Retire only disposable test credentials under a separate
   owner-approved cleanup procedure.

No real enrolment migration, lane-switch experiment or destructive recovery
is part of this plan. Report source/equality and pass/fail counts only.
