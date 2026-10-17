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
}
