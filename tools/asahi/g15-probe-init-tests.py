#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual G15 probe with the vendored pin-init crate and proc macro.

Requires a kernel build directory containing the host pin_init_internal proc macro.
The G15 probe, platform callback's dispatch block, Device::set_drvdata and
try_pin_init macro are extracted from production. GPU startup and the allocator
are fault-injection models. KBox's actual source is checked to allocate before
write_pin_init; the host uses the same vendored write_pin_init implementation.
This checks allocation/init/binding order, not a GPU or a kernel probe on hardware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OLD_REF = "564f61b8189e"
PROBE_PATH = "drivers/gpu/drm/asahi/g15/mod.rs"
# Verbatim inherited eager probe before the lazy initializer fix.
OLD_PROBE = r'''    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let cfg = *info.ok_or(ENODEV)?;
        dev_info!(pdev.as_ref(), "G15: probing ({})\n", cfg.name);
        match crate::driver::AsahiPlatformDriver::probe_g15(pdev, cfg) {
            Ok(driver) => Ok(Driver { _driver: Some(driver) }),
            Err(error) if boot::glue::retained() => {
                dev_err!(
                    pdev.as_ref(),
                    "G15: probe failed after the firmware start ({:?}); device kept bound, firmware retained until reboot\n",
                    error
                );
                Ok(Driver { _driver: None })
            }
            Err(error) => Err(error),
        }
    }'''


def item(text, marker):
    start = text.index(marker)
    at = text.index("{", start)
    depth = 1
    end = at + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREFIX = r'''
#![allow(dead_code, unused_variables)]
extern crate self as kernel;
use std::{cell::Cell, marker::PhantomData, pin::Pin};
use pin_init::{pin_data, PinInit, InPlaceWrite};
pub mod error { pub type Error = i32; }
use error::Error;
type Result<T = ()> = std::result::Result<T, Error>;
const ENODEV: i32 = -19;
const GFP_KERNEL: u32 = 0;
macro_rules! dev_info { ($($x:tt)*) => {{}}; }
macro_rules! dev_err { ($($x:tt)*) => {{}}; }
thread_local! {
    static FAIL_ALLOC: Cell<bool> = const { Cell::new(false) };
    static ALLOCS: Cell<u32> = const { Cell::new(0) };
    static PROBES: Cell<u32> = const { Cell::new(0) };
    static TOUCH: Cell<bool> = const { Cell::new(false) };
    static RETAINED: Cell<bool> = const { Cell::new(false) };
    static PROBE_ERROR: Cell<i32> = const { Cell::new(0) };
}
struct Core<'a>(PhantomData<&'a ()>);
struct Device { data: Cell<*mut core::ffi::c_void> }
impl Device {
    fn as_raw(&self) -> *mut Device { self as *const Device as *mut Device }
    // SET_DRVDATA
}
mod bindings {
    pub unsafe fn dev_set_drvdata(dev: *mut crate::Device, data: *mut core::ffi::c_void) {
        unsafe { (*dev).data.set(data); }
    }
}
struct Owned<T>(Pin<Box<T>>);
impl<T> Owned<T> {
    fn into_foreign(self) -> *mut T {
        // Test ownership transfer mirrors the kernel's retained pinned KBox.
        unsafe { Box::into_raw(Pin::into_inner_unchecked(self.0)) }
    }
}
struct KBox;
impl KBox {
    fn pin_init<T>(init: impl PinInit<T, Error>, _flags: u32) -> Result<Owned<T>> {
        ALLOCS.with(|v| v.set(v.get() + 1));
        if FAIL_ALLOC.with(Cell::get) { return Err(-12); }
        // Kernel KBox::try_pin_init has this same ordering. Allocation is the only
        // mocked operation here; write_pin_init is the actual vendored implementation.
        let slot = Box::<T>::new_uninit();
        Ok(Owned(slot.write_pin_init(init)?))
    }
}
mod platform {
    use super::*;
    pub struct Device<Ctx> { pub base: super::Device, pub ctx: PhantomData<Ctx> }
    impl<Ctx> AsRef<super::Device> for Device<Ctx> {
        fn as_ref(&self) -> &super::Device { &self.base }
    }
    pub trait Driver {
        type IdInfo;
        type Data<'bound>;
        fn probe<'bound>(
            pdev: &'bound Device<Core<'_>>,
            info: Option<&'bound Self::IdInfo>,
        ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound;
    }
}
struct Config { name: &'static str }
mod driver {
    use super::*;
    pub struct AsahiPlatformDriverData<'bound> { value: u8, bound: PhantomData<&'bound ()> }
    pub struct AsahiPlatformDriver;
    impl AsahiPlatformDriver {
        pub fn probe_g15<'bound>(
            _pdev: &'bound platform::Device<Core<'_>>, _cfg: &'static Config,
        ) -> Result<AsahiPlatformDriverData<'bound>> {
            PROBES.with(|v| v.set(v.get() + 1));
            RETAINED.with(|v| v.set(TOUCH.with(Cell::get)));
            match PROBE_ERROR.with(Cell::get) {
                0 => Ok(AsahiPlatformDriverData { value: 1, bound: PhantomData }),
                e => Err(e),
            }
        }
    }
}
mod boot { pub mod glue {
    pub fn retained() -> bool { crate::RETAINED.with(core::cell::Cell::get) }
}}
'''

SUFFIX = r'''
fn callback<'bound, T: platform::Driver>(
    pdev: &'bound platform::Device<Core<'_>>,
    info: Option<&'bound T::IdInfo>,
) -> Result<i32> {
    // CALLBACK
}
fn reset(fail_alloc: bool, touch: bool, error: i32) {
    FAIL_ALLOC.set(fail_alloc); TOUCH.set(touch); PROBE_ERROR.set(error);
    RETAINED.set(false); ALLOCS.set(0); PROBES.set(0);
}
fn main() {
    static CFG: Config = Config { name: "test" };
    let cfg = &CFG;
    for (name, allocation_fails, touched, error, expected, bound, calls) in [
        ("allocation-before-start", true, true, 0, Err(-12), false, 0),
        ("allocation-before-retained-failure", true, true, -5, Err(-12), false, 0),
        ("pre-start-failure", false, false, -19, Err(-19), false, 1),
        ("retained-start-failure-binds", false, true, -5, Ok(0), true, 1),
        ("retained-registration-failure-binds", false, true, -12, Ok(0), true, 1),
        ("successful-registration-binds", false, true, 0, Ok(0), true, 1),
    ] {
        reset(allocation_fails, touched, error);
        let pdev = platform::Device { base: Device { data: Cell::new(core::ptr::null_mut()) }, ctx: PhantomData };
        let result = callback::<Driver<'static>>(&pdev, Some(&cfg));
        assert_eq!(result, expected, "{name}: callback result");
        assert_eq!(PROBES.get(), calls, "{name}: firmware probe before drvdata allocation");
        assert_eq!(ALLOCS.get(), 1, "{name}: driver-data allocations");
        assert_eq!(!pdev.base.data.get().is_null(), bound, "{name}: bound data");
        if bound {
            // Fake kernel teardown after asserting installation. Production suppresses manual unbind.
            let data = unsafe { Box::from_raw(pdev.base.data.get().cast::<Driver<'_>>()) };
            assert_eq!(data._driver.is_some(), error == 0, "{name}: retained failure state");
        }
        println!("{name}: PASS");
    }
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel-build", type=Path, required=True)
    p.add_argument("--output", type=Path)
    p.add_argument("--mutations", action="store_true")
    p.add_argument("--rustc", default=os.environ.get("HOSTRUSTC", "rustc"))
    a = p.parse_args()
    out = (a.output or Path(tempfile.mkdtemp(prefix="g15-probe-init-"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    sources = {name: (ROOT / name).read_text() for name in [PROBE_PATH, "rust/kernel/platform.rs", "rust/kernel/device.rs", "rust/kernel/init.rs", "rust/kernel/alloc/kbox.rs"]}
    probe = item(sources[PROBE_PATH], "    fn probe<'bound>(")
    driver = item(sources[PROBE_PATH], "#[pin_data]\npub(crate) struct Driver<'bound>").replace("pub(crate) ", "")
    macro = item(sources["rust/kernel/init.rs"], "macro_rules! try_pin_init")
    set_data = item(sources["rust/kernel/device.rs"], "    pub fn set_drvdata<T>")
    callback = sources["rust/kernel/platform.rs"]
    callback = callback[callback.index("            let data = T::probe(pdev, info);"):]
    callback = callback[:callback.index("            Ok(0)") + len("            Ok(0)")]
    kbox = item(sources["rust/kernel/alloc/kbox.rs"], "    fn try_pin_init<E>(")
    assert "Box::<_, A>::new_uninit(flags)?.write_pin_init(init)" in kbox
    library = out / "libpin_init.rlib"
    subprocess.run([a.rustc, "--edition=2021", "--crate-type=rlib", "--crate-name=pin_init", "--cfg", 'feature="std"', str(ROOT / "rust/pin-init/src/lib.rs"), "--extern", f"pin_init_internal={a.kernel_build / 'rust/libpin_init_internal.so'}", "-L", f"dependency={a.kernel_build / 'rust'}", "-o", str(library)], check=True)
    receipt = {"rustc": subprocess.check_output([a.rustc, "--version"], text=True).strip(), "pin_init_source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT / "rust/pin-init").rglob("*.rs")}, "proc_macro_sha256": hashlib.sha256((a.kernel_build / "rust/libpin_init_internal.so").read_bytes()).hexdigest(), "source_sha256": {n: hashlib.sha256(s.encode()).hexdigest() for n, s in sources.items()}, "cases": 6, "mutants": {}, "scope": __doc__}
    variants = {"actual": probe}
    if a.mutations:
        try:
            old = subprocess.check_output(["git", "show", f"{OLD_REF}:{PROBE_PATH}"], cwd=ROOT, text=True, stderr=subprocess.DEVNULL)
            assert item(old, "    fn probe<'bound>(") == OLD_PROBE
            receipt["old_reference_checked_against_git"] = True
        except subprocess.CalledProcessError:
            receipt["old_reference_checked_against_git"] = False
        variants["eager-probe-before-allocation"] = OLD_PROBE
        needle = "                        None\n"
        assert probe.count(needle) == 1
        variants["retained-error-unbinds"] = probe.replace(needle, "                        return Err(error);\n")
    for name, method in variants.items():
        source = PREFIX.replace("    // SET_DRVDATA", set_data) + macro + "\n" + driver + "\nimpl platform::Driver for Driver<'_> {\n    type IdInfo = &'static Config;\n    type Data<'bound> = Driver<'bound>;\n" + method + "\n}\n" + SUFFIX.replace("    // CALLBACK", callback)
        src = out / (name + ".rs"); src.write_text(source)
        binary = out / name
        subprocess.run([a.rustc, "--edition=2021", str(src), "--extern", f"pin_init={library}", "-L", f"dependency={a.kernel_build / 'rust'}", "-o", str(binary)], check=True)
        r = subprocess.run([str(binary)], capture_output=True, text=True)
        receipt["mutants" if name != "actual" else "control"] = receipt.get("mutants" if name != "actual" else "control", {})
        result = {"exit": r.returncode, "stdout": r.stdout, "stderr": r.stderr}
        if name == "actual":
            receipt["control"] = result
            if r.returncode: raise SystemExit(r.stderr)
        else:
            receipt["mutants"][name] = result
            if r.returncode == 0: raise SystemExit("SURVIVED: " + name)
    (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
