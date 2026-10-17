# SPDX-License-Identifier: MIT
"""Execute the actual verify and attach sensor bring-up code with mocks.

The first verify after a system suspend re-registers the sensor, and on a
nonce-session profile that registration can fail in a way a new attempt from
a power-cycled sensor recovers. Attach makes up to three attempts; a verify
must too, but stop as soon as the verify is no longer wanted.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

APPLE = Path(__file__).resolve().parents[3] / 'drivers/soc/apple'


def method(source, name):
    """The text of method `name` in `source`, or '' if there is none."""
    match = re.search(r'\n    (pub\(crate\) )?fn ' + name + r'\b', source)
    if not match:
        return ''
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#![allow(dead_code, unused_variables, unused_imports)]
use std::cell::RefCell;
use std::ffi::CStr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
macro_rules! dev_err { ($($args:tt)*) => {} }
macro_rules! dev_warn { ($($args:tt)*) => {} }
macro_rules! dev_info { ($($args:tt)*) => {} }

const ENROL_STATUS_SENSOR: u32 = 1;
const ENROL_STATUS_ENCLAVE: u32 = 2;
const SBIO_PROBE_USER_ID: u32 = 1;

#[derive(Default)]
struct Mock {
    /// Outcomes of successive bring-up attempts; missing ones fail.
    script: Vec<bool>,
    attempts: usize,
    /// The verify stops being wanted once this many attempts have run.
    cancel_after: Option<usize>,
    captured: bool,
    finished: Option<bio::VerifyOutcome>,
}
thread_local! { static MOCK: RefCell<Mock> = RefCell::new(Mock::default()); }

mod bio {
    pub const TOKEN_LEN: usize = 4;
    #[derive(Clone, Copy, Debug, PartialEq)]
    pub enum VerifyOutcome { Matched(u8), NoMatch, Failed(u32) }
    pub struct Session;
    pub fn verify_is_live(_: &Session, _: u64) -> bool {
        super::MOCK.with(|m| {
            let m = m.borrow();
            m.cancel_after.map_or(true, |n| m.attempts < n)
        })
    }
}
mod sbio {
    pub struct UserId;
    impl UserId { pub fn new(_: u32) -> Option<Self> { Some(UserId) } }
}
mod sensor {
    pub fn idle() -> Result<(), ()> { Ok(()) }
    pub fn power(_: bool) -> bool { true }
    pub fn is_bound() -> bool { true }
}
mod diag {
    pub fn sensor_bind() -> u32 { 0 }
    pub fn sensor_brought_up(_: u32, _: bool) {}
}
struct PatchLoaded(());
struct Profile { nonce_session: bool }
struct SessionLock(bio::Session);
impl SessionLock { fn lock(&self) -> &bio::Session { &self.0 } }

struct Driver {
    dev: (),
    profile: Profile,
    templates_restored: AtomicBool,
    shutting_down: AtomicBool,
    capture_epoch: AtomicU64,
    bio_session: SessionLock,
}

impl Driver {
    fn new(nonce_session: bool) -> Self {
        Driver {
            dev: (),
            profile: Profile { nonce_session },
            templates_restored: AtomicBool::new(true),
            shutting_down: AtomicBool::new(false),
            capture_epoch: AtomicU64::new(1),
            bio_session: SessionLock(bio::Session),
        }
    }
    fn bring_sensor_online(&self) -> bool {
        MOCK.with(|m| {
            let mut m = m.borrow_mut();
            let ok = m.script.get(m.attempts).copied().unwrap_or(false);
            m.attempts += 1;
            ok
        })
    }
    fn retry_restore_proof(&self) {}
    fn mint_token_bytes(&self) -> Option<[u8; bio::TOKEN_LEN]> { Some([1; bio::TOKEN_LEN]) }
    fn settle_before_capture(&self) {}
    fn verify_one_image(&self) -> bio::VerifyOutcome {
        MOCK.with(|m| m.borrow_mut().captured = true);
        bio::VerifyOutcome::Matched(1)
    }
    fn note_capture_end(&self) {}
    fn save_after_match(&self, _: sbio::UserId) -> bool { true }
    fn finish_verify(&self, outcome: bio::VerifyOutcome, _: [u8; bio::TOKEN_LEN]) {
        MOCK.with(|m| {
            let mut m = m.borrow_mut();
            assert!(m.finished.is_none(), "the verify reported twice");
            m.finished = Some(outcome);
        });
    }
    fn wake_sensor(&self) -> Option<PatchLoaded> { Some(PatchLoaded(())) }
    fn complete_bringup(&self, _: PatchLoaded) -> bool { true }
'''

MAIN = r'''
}

fn run(script: &[bool], cancel_after: Option<usize>, nonce: bool, verify: bool) -> Mock {
    MOCK.with(|m| *m.borrow_mut() = Mock { script: script.to_vec(), cancel_after, ..Mock::default() });
    let driver = Driver::new(nonce);
    if verify { driver.run_verify() } else { driver.attach_bringup() }
    MOCK.with(|m| std::mem::take(&mut *m.borrow_mut()))
}

fn main() {
    use bio::VerifyOutcome::*;

    // The case seen after a suspend: the first attempt fails, the next works.
    let m = run(&[false, true], None, true, true);
    assert!(m.captured && m.finished == Some(Matched(1)) && m.attempts == 2,
            "a verify gave up after one failed sensor bring-up (attempts {}, finished {:?})",
            m.attempts, m.finished);

    // A sensor that never comes up: three attempts, then a sensor failure.
    let m = run(&[], None, true, true);
    assert_eq!((m.attempts, m.captured, m.finished), (3, false, Some(Failed(ENROL_STATUS_SENSOR))));

    // Cancelled or suspending during the first attempt: no second one.
    let m = run(&[false, true], Some(1), true, true);
    assert_eq!((m.attempts, m.captured, m.finished), (1, false, Some(Failed(ENROL_STATUS_SENSOR))));

    // A first attempt that works is the only one.
    let m = run(&[true], None, true, true);
    assert_eq!((m.attempts, m.captured, m.finished), (1, true, Some(Matched(1))));

    // Profiles without a nonce session keep their single attempt.
    let m = run(&[false, true], None, false, true);
    assert_eq!((m.attempts, m.captured, m.finished), (1, false, Some(Failed(ENROL_STATUS_SENSOR))));

    // Attach keeps its three attempts.
    assert_eq!(run(&[false, false, true], None, true, false).attempts, 3);
    assert_eq!(run(&[], None, true, false).attempts, 3);
    assert_eq!(run(&[true], None, true, false).attempts, 1);
    assert_eq!(run(&[], None, false, false).attempts, 0);
}
'''


class VerifyBringupTest(unittest.TestCase):
    def test_verify_retries_a_failed_bring_up_like_attach(self):
        source = (APPLE / 'sbio.rs').read_text()
        methods = [method(source, name) for name in (
            'run_verify', 'attach_bringup',
            'bring_sensor_online_retrying', 'bring_sensor_online_for_verify',
            'verify_wanted')]
        self.assertTrue(methods[0] and methods[1])
        program = PRELUDE + '\n'.join(m for m in methods if m) + MAIN
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.rs').write_text(program)
            subprocess.run(['rustc', '--edition=2021', str(root / 'test.rs'),
                            '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
