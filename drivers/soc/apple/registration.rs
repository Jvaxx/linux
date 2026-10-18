// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! What a Touch ID sensor registration has to send to the enclave.
//!
//! A registration identifies the sensor (REGISTER_SENSOR), registers its
//! serial and, on profiles with a nonce session, authenticates it. Any of
//! those steps can fail after the enclave has already accepted an earlier
//! one, and then the enclave holds a partial registration. On boot
//! d1962b3a every retry that registered the sensor again without clearing
//! it failed, 42 of them over 800 s. Re-registration that identifies the
//! sensor and then clears the enclave's sensor state, the order every
//! verify uses, has not failed on an idle machine. So the next attempt
//! clears whenever the enclave may hold any registration, complete or not.
//!
//! Clearing does not end everything, though: see [`authenticate`]. This
//! file also holds how often, and how far apart, a bring-up is attempted.

/// Nothing is registered with the enclave yet.
pub(crate) const FRESH: u32 = 0;
/// The sensor is identified; profiles without a nonce session register the
/// serial next.
pub(crate) const IDENTIFIED: u32 = 1;
/// The registration completed.
pub(crate) const ESTABLISHED: u32 = 2;

/// The enclave requests one registration attempt makes, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Plan {
    /// PLATFORM_IDENTITY and REGISTER_SENSOR, before anything else. A
    /// nonce-session profile identifies the sensor on every reset.
    pub(crate) identify_first: bool,
    /// CLEAR_STATE, after any identification.
    pub(crate) clear_state: bool,
    /// REGISTER_SENSOR after the clear, on a profile without a nonce session.
    pub(crate) register_sensor: bool,
}

/// Plans one registration attempt.
///
/// `stage` is the bring-up stage and `holds_sensor` whether an
/// identification of the sensor was submitted to the enclave since it
/// booted, answered or not: a lost reply does not mean the enclave did not
/// act on it. `sepos13` says whether the profile starts from the serial, as
/// the 13.5 key store does.
pub(crate) fn plan(stage: u32, holds_sensor: bool, nonce_session: bool, sepos13: bool) -> Plan {
    Plan {
        identify_first: nonce_session,
        clear_state: stage >= ESTABLISHED || (nonce_session && holds_sensor),
        register_sensor: stage == FRESH && !nonce_session && !sepos13,
    }
}

/// How the enclave answered REGISTER_SERIAL.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SerialAnswer {
    Accepted,
    /// Status 0x16.
    Refused16,
    /// Any other status, or no answer at all.
    Failed,
}

/// The step at which a nonce-session authentication stopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Stopped {
    Serial,
    Nonce,
    RegisterSerial,
    Session,
}

/// How a nonce-session authentication that completed began on the enclave.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Authenticated {
    /// The enclave accepted REGISTER_SERIAL.
    Registered,
    /// The enclave still held the serial of an earlier attempt whose
    /// session never opened, and the session was opened on it.
    Resumed,
}

/// The requests of a nonce-session authentication, which follows the
/// identification of the sensor and any CLEAR_STATE.
pub(crate) trait Authentication {
    type Serial;
    type Nonce;
    /// Reads the serial from the sensor.
    fn read_serial(&mut self) -> Option<Self::Serial>;
    /// Reads a session nonce from the sensor.
    fn read_nonce(&mut self) -> Option<Self::Nonce>;
    /// Sends REGISTER_SERIAL to the enclave.
    fn register_serial(&mut self, serial: &Self::Serial) -> SerialAnswer;
    /// Sends SENSOR_NONCE, the signed root key if it is not loaded yet, and
    /// relays the session share between the enclave and the sensor.
    fn open_session(&mut self, nonce: &Self::Nonce) -> bool;
}

/// Authenticates the sensor on a profile with a nonce session.
///
/// `unfinished` says whether the enclave may hold a serial registration
/// whose session never opened. It is kept up to date here and survives
/// between attempts.
///
/// On boot 0ab402d0 the first attempt had REGISTER_SERIAL accepted and then
/// found the sensor silent when it read the nonce. The next two attempts
/// identified the sensor, cleared the enclave and were refused at
/// REGISTER_SERIAL with 0x16, as a J314s was refused after a failed sequence
/// counter init: CLEAR_STATE does not end a serial registration whose
/// session never opened. So both sensor reads come first, and a sensor that
/// falls silent then leaves the enclave as it was. If the enclave does hold
/// an unfinished registration, because a later step failed, a 0x16 refusal
/// of the new one is the enclave saying so, and the session is opened on
/// the registration it holds.
pub(crate) fn authenticate<A: Authentication>(
    steps: &mut A,
    unfinished: &mut bool,
) -> Result<Authenticated, Stopped> {
    let serial = steps.read_serial().ok_or(Stopped::Serial)?;
    let nonce = steps.read_nonce().ok_or(Stopped::Nonce)?;
    let how = match steps.register_serial(&serial) {
        SerialAnswer::Accepted => Authenticated::Registered,
        SerialAnswer::Refused16 if *unfinished => Authenticated::Resumed,
        SerialAnswer::Refused16 => return Err(Stopped::RegisterSerial),
        SerialAnswer::Failed => {
            // A lost reply does not mean the enclave did not act on it.
            *unfinished = true;
            return Err(Stopped::RegisterSerial);
        }
    };
    *unfinished = true;
    if !steps.open_session(&nonce) {
        return Err(Stopped::Session);
    }
    *unfinished = false;
    Ok(how)
}

/// How many times a sensor bring-up is attempted, and how far apart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Retry {
    attempts: u32,
    /// The pause before the second attempt; each later one doubles it.
    first_pause_ms: u32,
    /// No attempt starts later than this after the first one began.
    budget_ms: u64,
}

/// At attach, on a profile with a nonce session. Across 29 J700 boots, 7
/// needed a second attempt and one failed three attempts made back to back.
/// Pauses of 0.5, 1, 2 and 4 s give a sensor or an enclave that is still
/// settling time to do so, and five attempts end within about 10 s.
pub(crate) const ATTACH: Retry = Retry {
    attempts: 5,
    first_pause_ms: 500,
    budget_ms: 10_000,
};

/// Before a verify, which someone is waiting on: three attempts, back to
/// back.
pub(crate) const VERIFY: Retry = Retry {
    attempts: 3,
    first_pause_ms: 0,
    budget_ms: u64::MAX,
};

impl Retry {
    /// The pause before attempt `next`, counted from 1, when the first
    /// attempt began `elapsed_ms` ago; `None` if there is no such attempt.
    pub(crate) fn pause_before(&self, next: u32, elapsed_ms: u64) -> Option<u32> {
        if next < 2 || next > self.attempts {
            return None;
        }
        let pause = self.first_pause_ms.saturating_mul(1 << (next - 2).min(31));
        if elapsed_ms.saturating_add(u64::from(pause)) > self.budget_ms {
            return None;
        }
        Some(pause)
    }
}

/// What a sensor bring-up with retries came to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// This attempt brought the sensor online.
    Online(u32),
    /// This many attempts failed, all that were allowed.
    GaveUp(u32),
    /// This many attempts failed, and the result was no longer wanted.
    Abandoned(u32),
}

impl Outcome {
    pub(crate) fn online(&self) -> bool {
        matches!(self, Outcome::Online(_))
    }
}

/// What a bring-up with retries needs from the driver.
pub(crate) trait Attempts {
    /// One bring-up, from a power-cycled sensor.
    fn attempt(&mut self) -> bool;
    /// Whether the result is still wanted.
    fn wanted(&self) -> bool;
    /// Milliseconds since the first attempt began.
    fn elapsed_ms(&self) -> u64;
    /// Attempt `attempt` of `of` failed, and the next one starts `pause_ms`
    /// from now.
    fn failed(&mut self, attempt: u32, of: u32, pause_ms: u32);
    /// Waits `ms`. False if it stopped early because the result is no
    /// longer wanted.
    fn pause(&mut self, ms: u32) -> bool;
}

/// Brings the sensor up, starting again after a failed attempt as `retry`
/// allows. Every path ends within the attempts and the budget of `retry`.
pub(crate) fn bring_up(retry: &Retry, steps: &mut impl Attempts) -> Outcome {
    let mut attempt = 1;
    loop {
        if steps.attempt() {
            return Outcome::Online(attempt);
        }
        let Some(pause) = retry.pause_before(attempt + 1, steps.elapsed_ms()) else {
            return Outcome::GaveUp(attempt);
        };
        if !steps.wanted() {
            return Outcome::Abandoned(attempt);
        }
        steps.failed(attempt, retry.attempts, pause);
        if pause > 0 && !steps.pause(pause) {
            return Outcome::Abandoned(attempt);
        }
        attempt += 1;
    }
}

/// The attach of the sensor, as `diag/sensor_attach` reports it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AttachState {
    /// The sensor came online and the enclave's identity view was proven.
    Attached = 1,
    /// The attach-time bring-up fell short and a later one is still to run.
    Retrying = 2,
    /// Every automatic bring-up fell short. An enrol or verify still tries.
    NotReady = 3,
}

/// The attach after a round of bring-up attempts. `complete` says whether
/// the sensor came online and the identity view was proven, and
/// `round_left` whether another round will follow.
pub(crate) fn after_round(complete: bool, round_left: bool) -> AttachState {
    if complete {
        AttachState::Attached
    } else if round_left {
        AttachState::Retrying
    } else {
        AttachState::NotReady
    }
}

#[kernel::macros::kunit_tests(apple_sep_registration)]
mod tests {
    use super::*;

    #[test]
    fn first_registration_after_boot_does_not_clear() {
        assert_eq!(
            plan(FRESH, false, true, false),
            Plan {
                identify_first: true,
                clear_state: false,
                register_sensor: false
            }
        );
    }

    #[test]
    fn retry_after_a_partial_registration_clears() {
        // REGISTER_SENSOR was accepted, then the serial, nonce or session
        // step failed: the bring-up stage is still FRESH.
        let retry = plan(FRESH, true, true, false);
        assert!(retry.identify_first);
        assert!(retry.clear_state);
    }

    #[test]
    fn re_registration_after_a_complete_one_clears() {
        assert!(plan(ESTABLISHED, true, true, false).clear_state);
        assert!(plan(ESTABLISHED, false, false, false).clear_state);
    }

    #[test]
    fn a_fresh_restart_still_clears_what_the_enclave_holds() {
        // Restarting the bring-up from FRESH does not erase the enclave's
        // registration; only CLEAR_STATE does.
        assert!(plan(FRESH, true, true, false).clear_state);
    }

    #[test]
    fn profiles_without_a_nonce_session_are_unchanged() {
        assert_eq!(
            plan(FRESH, true, false, false),
            Plan {
                identify_first: false,
                clear_state: false,
                register_sensor: true
            }
        );
        assert_eq!(
            plan(IDENTIFIED, true, false, false),
            Plan {
                identify_first: false,
                clear_state: false,
                register_sensor: false
            }
        );
        assert_eq!(
            plan(FRESH, false, false, true),
            Plan {
                identify_first: false,
                clear_state: false,
                register_sensor: false
            }
        );
    }

    /// The enclave's sensor registration, as the boots on record show it.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Enclave {
        Empty,
        /// REGISTER_SERIAL was accepted and the session never opened.
        /// CLEAR_STATE does not end this, and REGISTER_SERIAL is refused
        /// with 0x16 while it lasts.
        SerialHeld,
        Established,
    }

    const ATTEMPT_MS: u64 = 100;

    /// A J700 sensor and enclave, scripted per attempt of a round.
    struct Model {
        enclave: Enclave,
        /// Whether the sensor answers the nonce read.
        nonce: [bool; 8],
        /// Whether the session exchange completes.
        session: [bool; 8],
        /// Whether REGISTER_SERIAL is refused with 0x16 whatever the
        /// enclave holds, as by an enclave that is still settling.
        busy: [bool; 8],
        attempt: usize,
        identified: bool,
        unfinished: bool,
        registered: u32,
        sessions: u32,
        resumed: bool,
        stopped: [Option<Stopped>; 8],
        wanted_for: usize,
        elapsed_ms: u64,
        pauses: [u32; 8],
        paused: usize,
        failed: u32,
    }

    impl Model {
        fn new() -> Model {
            Model {
                enclave: Enclave::Empty,
                nonce: [true; 8],
                session: [true; 8],
                busy: [false; 8],
                attempt: 0,
                identified: false,
                unfinished: false,
                registered: 0,
                sessions: 0,
                resumed: false,
                stopped: [None; 8],
                wanted_for: usize::MAX,
                elapsed_ms: 0,
                pauses: [0; 8],
                paused: 0,
                failed: 0,
            }
        }

        /// Starts another round, as the late retry does.
        fn next_round(&mut self) {
            self.attempt = 0;
            self.elapsed_ms = 0;
            self.paused = 0;
            self.stopped = [None; 8];
        }
    }

    impl Authentication for Model {
        type Serial = ();
        type Nonce = ();

        fn read_serial(&mut self) -> Option<()> {
            Some(())
        }

        fn read_nonce(&mut self) -> Option<()> {
            self.nonce[self.attempt].then_some(())
        }

        fn register_serial(&mut self, _serial: &()) -> SerialAnswer {
            self.registered += 1;
            if self.busy[self.attempt] || self.enclave == Enclave::SerialHeld {
                return SerialAnswer::Refused16;
            }
            self.enclave = Enclave::SerialHeld;
            SerialAnswer::Accepted
        }

        fn open_session(&mut self, _nonce: &()) -> bool {
            self.sessions += 1;
            if self.enclave != Enclave::SerialHeld || !self.session[self.attempt] {
                return false;
            }
            self.enclave = Enclave::Established;
            true
        }
    }

    impl Attempts for Model {
        fn attempt(&mut self) -> bool {
            if plan(FRESH, self.identified, true, false).clear_state
                && self.enclave == Enclave::Established
            {
                self.enclave = Enclave::Empty;
            }
            self.identified = true;
            let mut unfinished = self.unfinished;
            let result = authenticate(self, &mut unfinished);
            self.unfinished = unfinished;
            match result {
                Ok(how) => self.resumed |= how == Authenticated::Resumed,
                Err(step) => self.stopped[self.attempt] = Some(step),
            }
            self.attempt += 1;
            self.elapsed_ms += ATTEMPT_MS;
            result.is_ok()
        }

        fn wanted(&self) -> bool {
            self.attempt < self.wanted_for
        }

        fn elapsed_ms(&self) -> u64 {
            self.elapsed_ms
        }

        fn failed(&mut self, _attempt: u32, _of: u32, _pause_ms: u32) {
            self.failed += 1;
        }

        fn pause(&mut self, ms: u32) -> bool {
            self.pauses[self.paused] = ms;
            self.paused += 1;
            self.elapsed_ms += u64::from(ms);
            true
        }
    }

    #[test]
    fn a_silent_first_attempt_is_retried_after_a_pause() {
        // Boot 0ab402d0's first attempt: the sensor fell silent at the
        // nonce read.
        let mut m = Model::new();
        m.nonce[0] = false;
        assert_eq!(bring_up(&ATTACH, &mut m), Outcome::Online(2));
        assert_eq!(m.stopped[0], Some(Stopped::Nonce));
        // The enclave never heard of the serial the silent attempt read, so
        // the next registration was not refused.
        assert_eq!(m.registered, 1);
        assert!(!m.resumed);
        assert_eq!(&m.pauses[..m.paused], &[500]);
        assert_eq!(m.enclave, Enclave::Established);
        assert!(!m.unfinished);
    }

    #[test]
    fn repeated_0x16_refusals_then_success() {
        let mut m = Model::new();
        m.busy[0] = true;
        m.busy[1] = true;
        assert_eq!(bring_up(&ATTACH, &mut m), Outcome::Online(3));
        assert_eq!(m.stopped[0], Some(Stopped::RegisterSerial));
        assert_eq!(m.stopped[1], Some(Stopped::RegisterSerial));
        // A refusal the enclave holds nothing for is not resumed.
        assert_eq!(m.sessions, 1);
        assert!(!m.resumed);
        assert_eq!(&m.pauses[..m.paused], &[500, 1000]);
        assert_eq!(m.failed, 2);
    }

    #[test]
    fn a_0x16_refusal_of_an_unfinished_registration_resumes_it() {
        // The session of the first attempt failed after REGISTER_SERIAL was
        // accepted. Clearing does not end that registration, so the second
        // REGISTER_SERIAL is refused; the session opens on the held one.
        let mut m = Model::new();
        m.session[0] = false;
        assert_eq!(bring_up(&ATTACH, &mut m), Outcome::Online(2));
        assert_eq!(m.stopped[0], Some(Stopped::Session));
        assert!(m.resumed);
        assert_eq!(m.enclave, Enclave::Established);
        assert!(!m.unfinished);
    }

    #[test]
    fn a_registration_without_a_reply_counts_as_unfinished() {
        struct Lost {
            opened: bool,
        }
        impl Authentication for Lost {
            type Serial = ();
            type Nonce = ();
            fn read_serial(&mut self) -> Option<()> {
                Some(())
            }
            fn read_nonce(&mut self) -> Option<()> {
                Some(())
            }
            fn register_serial(&mut self, _serial: &()) -> SerialAnswer {
                SerialAnswer::Failed
            }
            fn open_session(&mut self, _nonce: &()) -> bool {
                self.opened = true;
                true
            }
        }
        let mut lost = Lost { opened: false };
        let mut unfinished = false;
        assert_eq!(
            authenticate(&mut lost, &mut unfinished),
            Err(Stopped::RegisterSerial)
        );
        assert!(unfinished);
        assert!(!lost.opened);
    }

    #[test]
    fn a_bring_up_that_never_succeeds_gives_up_and_reports_not_ready() {
        let mut m = Model::new();
        m.nonce = [false; 8];
        let outcome = bring_up(&ATTACH, &mut m);
        assert_eq!(outcome, Outcome::GaveUp(5));
        assert_eq!(&m.pauses[..m.paused], &[500, 1000, 2000, 4000]);
        assert!(m.elapsed_ms <= ATTACH.budget_ms + ATTEMPT_MS);
        // A late round is still to come.
        assert_eq!(after_round(outcome.online(), true), AttachState::Retrying);

        m.next_round();
        let outcome = bring_up(&ATTACH, &mut m);
        assert_eq!(outcome, Outcome::GaveUp(5));
        assert_eq!(after_round(outcome.online(), false), AttachState::NotReady);
        // Nothing reached the enclave, so nothing is left half open.
        assert_eq!(m.registered, 0);
        assert_eq!(m.enclave, Enclave::Empty);
        assert!(!m.unfinished);
    }

    #[test]
    fn a_completed_attach_is_reported_attached() {
        assert_eq!(after_round(true, true), AttachState::Attached);
        assert_eq!(after_round(true, false), AttachState::Attached);
    }

    #[test]
    fn no_attempt_starts_past_the_budget() {
        assert_eq!(ATTACH.pause_before(1, 0), None);
        assert_eq!(ATTACH.pause_before(2, 0), Some(500));
        assert_eq!(ATTACH.pause_before(5, 6_000), Some(4000));
        assert_eq!(ATTACH.pause_before(5, 6_001), None);
        assert_eq!(ATTACH.pause_before(6, 0), None);

        // Attempts that each take as long as an enclave timeout end early.
        let mut m = Model::new();
        m.nonce = [false; 8];
        struct Slow(Model);
        impl Attempts for Slow {
            fn attempt(&mut self) -> bool {
                self.0.elapsed_ms += 5_000;
                self.0.attempt()
            }
            fn wanted(&self) -> bool {
                self.0.wanted()
            }
            fn elapsed_ms(&self) -> u64 {
                self.0.elapsed_ms()
            }
            fn failed(&mut self, attempt: u32, of: u32, pause_ms: u32) {
                self.0.failed(attempt, of, pause_ms)
            }
            fn pause(&mut self, ms: u32) -> bool {
                self.0.pause(ms)
            }
        }
        let mut slow = Slow(m);
        assert_eq!(bring_up(&ATTACH, &mut slow), Outcome::GaveUp(2));
    }

    #[test]
    fn verify_attempts_follow_each_other_at_once() {
        assert_eq!(VERIFY.pause_before(2, 0), Some(0));
        assert_eq!(VERIFY.pause_before(3, u64::MAX - 1), Some(0));
        assert_eq!(VERIFY.pause_before(4, 0), None);

        let mut m = Model::new();
        m.nonce = [false; 8];
        assert_eq!(bring_up(&VERIFY, &mut m), Outcome::GaveUp(3));
        assert_eq!(m.paused, 0);
        assert_eq!(m.failed, 2);
    }

    #[test]
    fn an_unwanted_bring_up_stops_without_another_attempt() {
        let mut m = Model::new();
        m.nonce = [false; 8];
        m.wanted_for = 1;
        assert_eq!(bring_up(&ATTACH, &mut m), Outcome::Abandoned(1));
        assert_eq!(m.attempt, 1);
        assert_eq!(m.paused, 0);
    }
}
