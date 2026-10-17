// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use crate::proto;
use kernel::prelude::*;

const MAX_INFLIGHT: usize = 8;

const POOL_LEN: usize = (proto::TAG_POOL_LAST - proto::TAG_POOL_FIRST + 1) as usize;

#[derive(Clone, Copy)]
struct Slot {
    used: bool,
    tag: u8,
    reply: Option<proto::ControlReply>,
}

impl Slot {
    const FREE: Slot = Slot {
        used: false,
        tag: 0,
        reply: None,
    };
}

/// After an entropy request goes unanswered, the next one waits this long,
/// doubling with each further failure up to [`ENTROPY_RETRY_MAX_MS`].
pub(crate) const ENTROPY_RETRY_MIN_MS: u64 = 1000;
pub(crate) const ENTROPY_RETRY_MAX_MS: u64 = 60_000;

struct EntropySink {
    value: Option<u32>,
    busy: bool,
    // Requests made and not answered yet, oldest first. A request that timed
    // out may still be answered, and every reply carries the reserved tag,
    // so only their order tells them apart.
    owed: u32,
    // A request went unanswered: none is sent before this time, in ms.
    retry_at: Option<u64>,
    retry_ms: u64,
}

/// How an entropy exchange ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EntropyEnd {
    /// The reply was taken.
    Answered,
    /// No reply in time. It may still come.
    TimedOut,
    /// The mailbox did not take the request; `full` if it ran out of time
    /// waiting for room, so the enclave is not reading it either.
    NotSent { full: bool },
    /// Given up for a removal. The reply may still come.
    Abandoned,
}

pub(crate) enum Delivery {
    Entropy,
    /// The answer to an entropy request that had timed out. `recovered` if
    /// requests were being held back until the enclave answered again.
    EntropyLate {
        recovered: bool,
    },
    Matched,
    Unmatched,
}

pub(crate) struct ControlState {
    slots: [Slot; MAX_INFLIGHT],
    next_tag: u8,
    retired: [u64; 4],
    retired_count: u32,
    entropy: EntropySink,
}

impl ControlState {
    pub(crate) fn new() -> Self {
        ControlState {
            slots: [Slot::FREE; MAX_INFLIGHT],
            next_tag: proto::TAG_POOL_FIRST,
            retired: [0; 4],
            retired_count: 0,
            entropy: EntropySink {
                value: None,
                busy: false,
                owed: 0,
                retry_at: None,
                retry_ms: ENTROPY_RETRY_MIN_MS,
            },
        }
    }

    fn is_retired(&self, tag: u8) -> bool {
        self.retired[(tag >> 6) as usize] & (1u64 << (tag & 0x3f)) != 0
    }

    fn retire(&mut self, tag: u8) {
        if !self.is_retired(tag) {
            self.retired[(tag >> 6) as usize] |= 1u64 << (tag & 0x3f);
            self.retired_count += 1;
        }
    }

    fn reclaim_retired(&mut self, tag: u8) -> bool {
        if !self.is_retired(tag) {
            return false;
        }
        self.retired[(tag >> 6) as usize] &= !(1u64 << (tag & 0x3f));
        self.retired_count -= 1;
        true
    }

    fn tag_in_flight(&self, tag: u8) -> bool {
        self.slots.iter().any(|s| s.used && s.tag == tag)
    }

    pub(crate) fn alloc(&mut self) -> Result<(usize, u8)> {
        let idx = self.slots.iter().position(|s| !s.used).ok_or(EBUSY)?;

        for _ in 0..POOL_LEN {
            let tag = self.next_tag;
            self.next_tag = if tag >= proto::TAG_POOL_LAST {
                proto::TAG_POOL_FIRST
            } else {
                tag + 1
            };
            if !self.tag_in_flight(tag) && !self.is_retired(tag) {
                self.slots[idx] = Slot {
                    used: true,
                    tag,
                    reply: None,
                };
                return Ok((idx, tag));
            }
        }
        Err(EBUSY)
    }

    pub(crate) fn take_reply(&mut self, idx: usize) -> Option<proto::ControlReply> {
        self.slots[idx].reply.take()
    }

    pub(crate) fn release(&mut self, idx: usize) {
        self.slots[idx] = Slot::FREE;
    }

    // Retire the tag so a late reply cannot be mistaken for a later request's answer.
    pub(crate) fn abandon(&mut self, idx: usize) -> u8 {
        let tag = self.slots[idx].tag;
        self.retire(tag);
        self.slots[idx] = Slot::FREE;
        tag
    }

    pub(crate) fn retired_count(&self) -> u32 {
        self.retired_count
    }

    pub(crate) fn deliver(&mut self, reply: proto::ControlReply) -> Delivery {
        // The reserved entropy tag is claimed first, outstanding request or not.
        if reply.tag == proto::TAG_ENTROPY {
            return self.deliver_entropy(reply.data_lo);
        }

        if let Some(slot) = self
            .slots
            .iter_mut()
            .find(|s| s.used && s.tag == reply.tag && s.reply.is_none())
        {
            slot.reply = Some(reply);
            return Delivery::Matched;
        }

        if self.reclaim_retired(reply.tag) {
            return Delivery::Unmatched;
        }

        // No match: drop it, never hand it to another waiter.
        Delivery::Unmatched
    }

    // The enclave answers in order, so a reply belongs to the oldest request
    // still owed. Only the one that settles the newest request is its answer;
    // any before it answer requests that timed out, and are dropped.
    fn deliver_entropy(&mut self, value: u32) -> Delivery {
        let entropy = &mut self.entropy;
        if entropy.owed == 0 {
            // Answers a request already written off as lost.
            return Delivery::Unmatched;
        }
        entropy.owed -= 1;
        if entropy.owed == 0 && entropy.busy {
            entropy.value = Some(value);
            return Delivery::Entropy;
        }
        // The enclave is answering again, so stop holding requests back.
        let recovered = !entropy.busy && entropy.retry_at.take().is_some();
        if recovered {
            entropy.retry_ms = ENTROPY_RETRY_MIN_MS;
        }
        Delivery::EntropyLate { recovered }
    }

    /// Claims the entropy exchange for a request to be sent at `now_ms`.
    ///
    /// Refused with EAGAIN after a request the enclave did not answer, until
    /// it answers or the retry time comes. A request that fails then doubles
    /// the wait. Each claim counts as owed until [`Self::entropy_end`],
    /// because the reply can arrive before the sender returns from the
    /// mailbox.
    pub(crate) fn entropy_begin(&mut self, now_ms: u64) -> Result<()> {
        if self.entropy.busy {
            return Err(EBUSY);
        }
        if let Some(at) = self.entropy.retry_at {
            if now_ms < at {
                return Err(EAGAIN);
            }
            // Nothing came back in all that time. Count those replies as
            // lost, or the answer to this request would be taken for one of
            // theirs and no request would be answered again.
            self.entropy.owed = 0;
        }
        self.entropy.busy = true;
        self.entropy.value = None;
        self.entropy.owed = self.entropy.owed.saturating_add(1);
        Ok(())
    }

    pub(crate) fn entropy_take(&mut self) -> Option<u32> {
        self.entropy.value.take()
    }

    /// Ends the exchange claimed by [`Self::entropy_begin`], at `now_ms`.
    pub(crate) fn entropy_end(&mut self, how: EntropyEnd, now_ms: u64) {
        let entropy = &mut self.entropy;
        entropy.busy = false;
        let failed = match how {
            EntropyEnd::Answered => {
                entropy.retry_at = None;
                entropy.retry_ms = ENTROPY_RETRY_MIN_MS;
                false
            }
            EntropyEnd::TimedOut => true,
            EntropyEnd::NotSent { full } => {
                entropy.owed = entropy.owed.saturating_sub(1);
                full
            }
            EntropyEnd::Abandoned => false,
        };
        if failed {
            entropy.retry_at = Some(now_ms.saturating_add(entropy.retry_ms));
            entropy.retry_ms =
                core::cmp::min(entropy.retry_ms.saturating_mul(2), ENTROPY_RETRY_MAX_MS);
        }
    }
}

#[kernel::macros::kunit_tests(apple_sep_control)]
mod tests {
    use super::*;

    fn entropy(value: u32) -> proto::ControlReply {
        proto::ControlReply {
            tag: proto::TAG_ENTROPY,
            data_lo: value,
        }
    }

    // One request answered in time, as the hwrng read makes it.
    fn draw(state: &mut ControlState, now_ms: u64, value: u32) -> Option<u32> {
        state.entropy_begin(now_ms).ok()?;
        let delivered = matches!(state.deliver(entropy(value)), Delivery::Entropy);
        let taken = state.entropy_take();
        state.entropy_end(
            if taken.is_some() {
                EntropyEnd::Answered
            } else {
                EntropyEnd::TimedOut
            },
            now_ms,
        );
        if delivered {
            taken
        } else {
            None
        }
    }

    #[test]
    fn a_request_is_answered() {
        let mut state = ControlState::new();
        assert_eq!(draw(&mut state, 0, 0x1234_5678), Some(0x1234_5678));
        assert_eq!(draw(&mut state, 1, 0x9abc_def0), Some(0x9abc_def0));
    }

    #[test]
    fn a_late_reply_is_not_the_next_requests_answer() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // The retry is held back until the enclave answers.
        assert_eq!(state.entropy_begin(1), Err(EAGAIN));
        // It answers the request that timed out: dropped, and reads resume.
        assert!(matches!(
            state.deliver(entropy(1)),
            Delivery::EntropyLate { recovered: true }
        ));
        assert_eq!(draw(&mut state, 2, 2), Some(2));
    }

    #[test]
    fn a_reply_later_than_the_retry_shifts_nothing_after_it() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // The retry goes out once its time comes, the first reply written off.
        let at = ENTROPY_RETRY_MIN_MS;
        assert!(state.entropy_begin(at).is_ok());
        // That reply comes after all. The retry takes it: it is enclave
        // output nobody has used.
        assert!(matches!(state.deliver(entropy(1)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(1));
        state.entropy_end(EntropyEnd::Answered, at);
        // The retry's own reply is then owed to nobody.
        assert!(matches!(state.deliver(entropy(2)), Delivery::Unmatched));
        assert_eq!(draw(&mut state, at + 1, 3), Some(3));
    }

    #[test]
    fn a_request_abandoned_for_removal_does_not_shift_the_answers() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::Abandoned, 0);
        assert!(state.entropy_begin(0).is_ok());
        assert!(matches!(
            state.deliver(entropy(1)),
            Delivery::EntropyLate { recovered: false }
        ));
        assert_eq!(state.entropy_take(), None);
        assert!(matches!(state.deliver(entropy(2)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(2));
        state.entropy_end(EntropyEnd::Answered, 0);
    }

    #[test]
    fn a_lost_reply_does_not_stop_the_channel_for_good() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // That reply never comes. The retry must still get its own answer.
        let at = ENTROPY_RETRY_MIN_MS;
        assert_eq!(draw(&mut state, at, 9), Some(9));
        assert_eq!(draw(&mut state, at + 1, 10), Some(10));
    }

    #[test]
    fn a_full_mailbox_holds_requests_back_too() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::NotSent { full: true }, 0);
        assert_eq!(state.entropy_begin(ENTROPY_RETRY_MIN_MS - 1), Err(EAGAIN));
        // Nothing was owed: the next request's reply is its own.
        assert_eq!(draw(&mut state, ENTROPY_RETRY_MIN_MS, 4), Some(4));
    }

    #[test]
    fn a_failed_send_owes_nothing() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::NotSent { full: false }, 0);
        assert_eq!(draw(&mut state, 0, 8), Some(8));
    }

    #[test]
    fn retries_back_off_up_to_a_limit() {
        let mut state = ControlState::new();
        let mut now = 0;
        let mut wait = ENTROPY_RETRY_MIN_MS;
        for _ in 0..12 {
            assert!(state.entropy_begin(now).is_ok());
            state.entropy_end(EntropyEnd::TimedOut, now);
            assert_eq!(state.entropy_begin(now + wait - 1), Err(EAGAIN));
            now += wait;
            wait = core::cmp::min(wait * 2, ENTROPY_RETRY_MAX_MS);
        }
        assert_eq!(wait, ENTROPY_RETRY_MAX_MS);
        // An answer resets the wait.
        assert_eq!(draw(&mut state, now, 1), Some(1));
        assert!(state.entropy_begin(now).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, now);
        assert!(state.entropy_begin(now + ENTROPY_RETRY_MIN_MS).is_ok());
        state.entropy_end(EntropyEnd::Abandoned, now);
    }

    #[test]
    fn a_reply_nobody_is_owed_is_dropped() {
        let mut state = ControlState::new();
        assert!(matches!(state.deliver(entropy(1)), Delivery::Unmatched));
        assert_eq!(draw(&mut state, 0, 2), Some(2));
    }
}
