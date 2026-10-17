// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use kernel::prelude::*;

pub(crate) const MARKER_FIRST: u8 = 0xFC;
pub(crate) const MARKER_NEXT: u8 = 0xFD;
/// Requests the peer's next packet and acks the final one; same marker for both.
pub(crate) const MARKER_REQUEST: u8 = 0xFE;
pub(crate) const MARKER_ERROR: u8 = 0xFF;

// A tag below MARKER_FIRST is not a marker; sound only while every marker >= it.
static_assert!(MARKER_NEXT >= MARKER_FIRST);
static_assert!(MARKER_REQUEST >= MARKER_FIRST);

#[derive(Clone, Copy)]
pub(crate) enum DeviceStatus {
    /// `0xffffffff` is a real refusal (signed −1), not the absence of an answer.
    Answered(u32),
    ReportedWithoutStatus,
    NotAnswered,
    BufferNeverWritten,
}

impl DeviceStatus {
    pub(crate) fn answered(&self) -> Option<u32> {
        match self {
            DeviceStatus::Answered(err) => Some(*err),
            DeviceStatus::ReportedWithoutStatus
            | DeviceStatus::NotAnswered
            | DeviceStatus::BufferNeverWritten => None,
        }
    }

    pub(crate) fn is_ok(&self) -> bool {
        matches!(self, DeviceStatus::Answered(0))
    }
}

impl kernel::fmt::Display for DeviceStatus {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        match self {
            DeviceStatus::Answered(err) => write!(f, "0x{:x}", err),
            DeviceStatus::ReportedWithoutStatus => write!(
                f,
                "an error report from the device carrying no status word (the device DID answer)"
            ),
            DeviceStatus::NotAnswered => write!(
                f,
                "NONE — no message arrived at all (host sentinel, not an enclave value)"
            ),
            DeviceStatus::BufferNeverWritten => write!(
                f,
                "NONE — a payload notification arrived but the outbound buffer was never written (a race with the enclave's DMA, not a silent enclave)"
            ),
        }
    }
}

pub(crate) const HEADER_WORDS: usize = 7;
pub(crate) const HEADER_LEN: usize = HEADER_WORDS * 4;

const VERSION: u32 = 1;

pub(crate) const MAX_TRANSACTION: u32 = 0x4B000;

#[derive(Clone, Copy)]
pub(crate) struct Packet {
    pub(crate) version: u32,
    pub(crate) total: u32,
    pub(crate) offset: u32,
    pub(crate) flags: u32,
    pub(crate) err: u32,
    pub(crate) opcode: u32,
    pub(crate) chunk: u32,
}

fn le32(buf: &[u8], word: usize) -> u32 {
    let o = word * 4;
    u32::from_le_bytes([buf[o], buf[o + 1], buf[o + 2], buf[o + 3]])
}

impl Packet {
    pub(crate) fn decode(buf: &[u8]) -> Result<Packet> {
        if buf.len() < HEADER_LEN {
            return Err(EINVAL);
        }
        Ok(Packet {
            version: le32(buf, 0),
            total: le32(buf, 1),
            offset: le32(buf, 2),
            flags: le32(buf, 3),
            err: le32(buf, 4),
            opcode: le32(buf, 5),
            chunk: le32(buf, 6),
        })
    }

    pub(crate) fn encode(&self, buf: &mut [u8]) -> Result<()> {
        if buf.len() < HEADER_LEN {
            return Err(EINVAL);
        }
        for (word, value) in [
            self.version,
            self.total,
            self.offset,
            self.flags,
            self.err,
            self.opcode,
            self.chunk,
        ]
        .into_iter()
        .enumerate()
        {
            buf[word * 4..word * 4 + 4].copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    }

    pub(crate) fn request_next(opcode: u32, received: u32, total: u32) -> Packet {
        Packet {
            version: VERSION,
            total,
            offset: received,
            flags: 0,
            err: 0,
            opcode,
            chunk: 0,
        }
    }
}

struct Active {
    opcode: u32,
    total: u32,
    payload: KVec<u8>,
    seq: u16,
}

pub(crate) struct Completed {
    pub(crate) status: DeviceStatus,
    pub(crate) payload: KVec<u8>,
}

pub(crate) struct Continuation {
    opcode: u32,
    received: u32,
    total: u32,
    seq: u16,
}

impl Continuation {
    pub(crate) fn opcode(&self) -> u32 {
        self.opcode
    }
    pub(crate) fn seq(&self) -> u16 {
        self.seq
    }
    pub(crate) fn packet(&self) -> Packet {
        Packet::request_next(self.opcode, self.received, self.total)
    }
}

pub(crate) enum Progress {
    NeedMore(Continuation),
    Complete,
    /// A stray chunk, dropped without touching transfer state — distinct from
    /// `Failed`, which kills the transfer.
    Ignored,
    Grant,
    Notification,
    Failed,
}

pub(crate) struct Reassembly {
    active: Option<Active>,
    done: Option<Completed>,
    sending: Option<u32>,
    grants: u32,
    // Counts exchanges begun. A reply is matched only by its opcode, so a
    // late reply to an exchange that timed out would otherwise complete the
    // next exchange with the same opcode.
    generation: u64,
}

impl Reassembly {
    pub(crate) fn new() -> Reassembly {
        Reassembly {
            active: None,
            done: None,
            sending: None,
            grants: 0,
            generation: 0,
        }
    }

    /// The exchange a message arriving now can belong to. Sampled before the
    /// message is read and passed back to [`Self::on_chunk`].
    pub(crate) fn generation(&self) -> u64 {
        self.generation
    }

    pub(crate) fn begin(&mut self, opcode: u32) -> Result<()> {
        if self.active.is_some() {
            return Err(EBUSY);
        }
        self.done = None;
        self.generation = self.generation.wrapping_add(1);
        self.active = Some(Active {
            opcode,
            total: 0,
            payload: KVec::new(),
            seq: 0,
        });
        Ok(())
    }

    pub(crate) fn begin_send(&mut self, opcode: u32) {
        self.sending = Some(opcode);
        self.grants = 0;
    }

    pub(crate) fn end_send(&mut self) {
        self.sending = None;
        self.grants = 0;
    }

    pub(crate) fn take_grant(&mut self) -> bool {
        if self.grants > 0 {
            self.grants -= 1;
            return true;
        }
        false
    }

    pub(crate) fn take_done(&mut self) -> Option<Completed> {
        self.done.take()
    }

    pub(crate) fn abort(&mut self) {
        self.abort_with(DeviceStatus::NotAnswered);
    }

    pub(crate) fn abort_with(&mut self, status: DeviceStatus) {
        if self.active.take().is_some() {
            self.done = Some(Completed {
                status,
                payload: KVec::new(),
            });
        }
    }

    fn fail(&mut self) {
        self.abort();
    }

    /// Takes one chunk of a reply. `generation` is [`Self::generation`] as
    /// sampled when the message arrived: a chunk that arrived before the
    /// current exchange began belongs to an earlier one and is dropped.
    pub(crate) fn on_chunk(
        &mut self,
        generation: u64,
        marker: u8,
        packet: &Packet,
        payload: &[u8],
    ) -> Progress {
        if marker < MARKER_FIRST {
            return Progress::Notification;
        }

        // 0xFE is flow control for the request being sent, answered from `sending`.
        if marker == MARKER_REQUEST {
            return match self.sending {
                Some(_) => {
                    // Counted under the caller's lock, closing the race with the waiter.
                    self.grants = self.grants.saturating_add(1);
                    Progress::Grant
                }
                None => Progress::Ignored,
            };
        }

        let Some(active_opcode) = self.active.as_ref().map(|a| a.opcode) else {
            return Progress::Ignored;
        };
        if generation != self.generation {
            return Progress::Ignored;
        }

        // Checked before the opcode test below, deliberately.
        if marker == MARKER_ERROR {
            let status = if packet.err != 0 {
                DeviceStatus::Answered(packet.err)
            } else {
                DeviceStatus::ReportedWithoutStatus
            };
            self.finish(status, KVec::new());
            return Progress::Complete;
        }

        // The device echoes the opcode on a data chunk.
        if packet.opcode != active_opcode {
            return Progress::Ignored;
        }

        if packet.version != VERSION {
            self.fail();
            return Progress::Failed;
        }
        if packet.chunk as usize != payload.len() {
            self.fail();
            return Progress::Failed;
        }
        if packet.total > MAX_TRANSACTION {
            self.fail();
            return Progress::Failed;
        }

        match marker {
            MARKER_FIRST => {
                let Some(active) = self.active.as_ref() else {
                    return Progress::Ignored;
                };
                if !active.payload.is_empty() {
                    self.fail();
                    return Progress::Failed;
                }
                if packet.offset != 0 {
                    self.fail();
                    return Progress::Failed;
                }
                if let Some(active) = self.active.as_mut() {
                    active.total = packet.total;
                }
            }
            MARKER_NEXT => {
                let Some(active) = self.active.as_ref() else {
                    return Progress::Ignored;
                };
                if packet.offset as usize != active.payload.len() {
                    self.fail();
                    return Progress::Failed;
                }
                if packet.total != active.total {
                    self.fail();
                    return Progress::Failed;
                }
            }
            _ => {
                self.fail();
                return Progress::Failed;
            }
        }

        let Some(active) = self.active.as_mut() else {
            return Progress::Ignored;
        };

        if packet.err != 0 {
            let status = DeviceStatus::Answered(packet.err);
            let collected = core::mem::take(&mut active.payload);
            self.finish(status, collected);
            return Progress::Complete;
        }

        if active
            .payload
            .extend_from_slice(payload, GFP_KERNEL)
            .is_err()
        {
            self.fail();
            return Progress::Failed;
        }

        let received = active.payload.len() as u32;
        let total = active.total;

        if received >= total {
            let collected = core::mem::take(&mut active.payload);
            self.finish(DeviceStatus::Answered(0), collected);
            Progress::Complete
        } else {
            active.seq = active.seq.wrapping_add(1);
            Progress::NeedMore(Continuation {
                opcode: active_opcode,
                received,
                total,
                seq: active.seq,
            })
        }
    }

    fn finish(&mut self, status: DeviceStatus, payload: KVec<u8>) {
        self.active = None;
        self.done = Some(Completed { status, payload });
    }
}

#[kernel::macros::kunit_tests(apple_sep_transfer)]
mod tests {
    use super::*;

    const OPCODE: u32 = 0x48;

    fn reply(err: u32) -> Packet {
        Packet {
            version: VERSION,
            total: 0,
            offset: 0,
            flags: 0,
            err,
            opcode: OPCODE,
            chunk: 0,
        }
    }

    #[test]
    fn a_reply_to_the_current_exchange_completes_it() {
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &reply(0), &[]),
            Progress::Complete
        ));
        assert!(rx.take_done().is_some_and(|done| done.status.is_ok()));
    }

    #[test]
    fn a_late_reply_does_not_complete_the_next_exchange() {
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        // The reply to the first exchange is being read when it times out.
        let late = rx.generation();
        rx.abort();
        assert!(rx.take_done().is_some());
        // A retry begins the same opcode before the late reply is delivered.
        assert!(rx.begin(OPCODE).is_ok());
        assert!(matches!(
            rx.on_chunk(late, MARKER_FIRST, &reply(0), &[]),
            Progress::Ignored
        ));
        assert!(matches!(
            rx.on_chunk(late, MARKER_ERROR, &reply(0x16), &[]),
            Progress::Ignored
        ));
        assert!(rx.take_done().is_none());
        let current = rx.generation();
        assert!(matches!(
            rx.on_chunk(current, MARKER_FIRST, &reply(0), &[]),
            Progress::Complete
        ));
    }
}
