// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounds for the entries of an AFK ring.
//!
//! The ring pointers and the entry headers live in memory the firmware can
//! write. These checks keep every entry inside its ring, independent of the
//! shared allocation that also holds the other ring and the header blocks.

use super::{QEHeader, QEH_SIZE, QE_MAGIC1, QE_MAGIC2};
use kernel::prelude::*;

/// A ring of `size` bytes of entries made of `block`-byte blocks. Entries
/// start on a block boundary.
pub(super) struct Ring {
    size: usize,
    block: usize,
}

impl Ring {
    pub(super) fn new(size: usize, block: usize) -> Result<Self> {
        if size < QEH_SIZE || !block.is_power_of_two() || size % block != 0 {
            return Err(EIO);
        }
        Ok(Self { size, block })
    }

    /// The most entries the ring can hold, one per block. One receive pass
    /// takes at most this many, so that a producer that keeps moving the
    /// write pointer cannot hold the receive worker.
    pub(super) fn capacity(&self) -> usize {
        self.size / self.block
    }

    /// Checks a read or write pointer: it must be a block boundary inside
    /// the ring with room for an entry header.
    pub(super) fn check_ptr(&self, ptr: usize) -> Result {
        if ptr >= self.size || ptr % self.block != 0 || self.size - ptr < QEH_SIZE {
            return Err(EIO);
        }
        Ok(())
    }

    /// Returns the pointer after an entry of `len` bytes at `ptr`, if the
    /// entry fits in the ring.
    fn end(&self, ptr: usize, len: usize) -> Result<usize> {
        self.check_ptr(ptr)?;
        let end = ptr
            .checked_add(QEH_SIZE)
            .and_then(|v| v.checked_add(len))
            .ok_or(EIO)?;
        let aligned = end.checked_add(self.block - 1).ok_or(EIO)? & !(self.block - 1);
        if aligned > self.size {
            return Err(EIO);
        }
        Ok(aligned % self.size)
    }

    /// Finds the entry at read pointer `ptr`, reading entry headers with
    /// `read`. An entry that does not fit before the end of the ring starts
    /// again at the top, where its header is read a second time. Returns the
    /// header, where the entry is and the read pointer after it.
    pub(super) fn read_entry(
        &self,
        mut ptr: usize,
        mut read: impl FnMut(usize) -> Result<QEHeader>,
    ) -> Result<(QEHeader, usize, usize)> {
        self.check_ptr(ptr)?;
        let previous = ptr;
        let mut header = read(ptr)?;
        Self::check_magic(&header)?;
        if header.size as usize > self.size - ptr - QEH_SIZE {
            ptr = 0;
            header = read(ptr)?;
            Self::check_magic(&header)?;
        }
        // The header at the top is firmware-written too: it must fit in the
        // ring before it sizes an allocation or a copy.
        let next = self.end(ptr, header.size as usize)?;
        // An entry that leaves the read pointer where it was would be taken
        // again and again.
        if next == ptr || next == previous {
            return Err(EIO);
        }
        Ok((header, ptr, next))
    }

    fn check_magic(header: &QEHeader) -> Result {
        if header.magic != QE_MAGIC1 && header.magic != QE_MAGIC2 {
            return Err(EIO);
        }
        Ok(())
    }
}

#[kunit_tests(apple_aop_ring)]
mod tests {
    use super::*;

    const BUF: usize = 0x1000;
    const BLOCK: usize = 0x40;

    fn header(size: u32) -> QEHeader {
        QEHeader {
            magic: QE_MAGIC1,
            size,
            ..QEHeader::default()
        }
    }

    #[test]
    fn invalid_layout_and_alignment_overflow_are_rejected() -> Result {
        assert!(Ring::new(0, 64).is_err());
        assert!(Ring::new(256, 0).is_err());
        assert!(Ring::new(256, 3).is_err());
        assert!(Ring::new(255, 64).is_err());
        // no room for an entry header
        assert!(Ring::new(QEH_SIZE - 1, 1).is_err());
        let ring = Ring::new(usize::MAX - 63, 64)?;
        assert!(ring.end(0, usize::MAX - QEH_SIZE).is_err());
        Ok(())
    }

    #[test]
    fn pointers_inside_the_ring_are_accepted() -> Result {
        let ring = Ring::new(BUF, BLOCK)?;
        for ptr in [0, 0x40, 0xfc0] {
            assert!(ring.check_ptr(ptr).is_ok());
        }
        Ok(())
    }

    #[test]
    fn pointers_at_or_past_the_end_are_refused() -> Result {
        let ring = Ring::new(BUF, BLOCK)?;
        for ptr in [BUF, u32::MAX as usize, 0x10_0000, usize::MAX] {
            assert!(ring.check_ptr(ptr).is_err());
        }
        Ok(())
    }

    #[test]
    fn misaligned_pointers_are_refused() -> Result {
        let ring = Ring::new(BUF, BLOCK)?;
        for ptr in [0x10, 0x44, 0x7f, 0xbf] {
            assert!(ring.check_ptr(ptr).is_err());
        }
        Ok(())
    }

    #[test]
    fn invalid_pointers_are_rejected_before_reading() -> Result {
        let ring = Ring::new(256, 64)?;
        for ptr in [256, 320, usize::MAX, 1, 255] {
            let mut reads = 0;
            assert!(ring
                .read_entry(ptr, |_| {
                    reads += 1;
                    Ok(header(0))
                })
                .is_err());
            assert_eq!(reads, 0);
        }
        Ok(())
    }

    #[test]
    fn oversized_wrapped_header_is_rejected() -> Result {
        let ring = Ring::new(256, 64)?;
        let mut reads = 0;
        let result = ring.read_entry(192, |ptr| {
            reads += 1;
            Ok(header(if ptr == 0 { 256 } else { 64 }))
        });
        assert!(result.is_err());
        assert_eq!(reads, 2);
        assert!(ring
            .read_entry(192, |ptr| Ok(header(if ptr == 0 { u32::MAX } else { 64 })))
            .is_err());
        Ok(())
    }

    #[test]
    fn wrapped_entry_stays_in_ring() -> Result {
        let ring = Ring::new(256, 64)?;
        let (_, ptr, next) =
            ring.read_entry(192, |ptr| Ok(header(if ptr == 0 { 48 } else { 64 })))?;
        assert_eq!((ptr, next), (0, 64));
        assert_eq!(ring.read_entry(192, |_| Ok(header(48)))?.2, 0);
        Ok(())
    }

    #[test]
    fn receive_entry_must_advance_the_read_pointer() -> Result {
        let ring = Ring::new(256, 64)?;
        assert!(ring.read_entry(0, |_| Ok(header(240))).is_err());
        assert!(ring
            .read_entry(192, |ptr| Ok(header(if ptr == 0 { 176 } else { 64 })))
            .is_err());
        Ok(())
    }

    #[test]
    fn one_pass_takes_at_most_a_ring_of_entries() -> Result {
        let ring = Ring::new(BUF, BLOCK)?;
        assert_eq!(ring.capacity(), 64);
        // A producer that moves the write pointer one block ahead of every
        // entry taken never lets the pointers meet; the pass still ends. The
        // test itself gives up after two rings' worth.
        let (mut rptr, mut wptr, mut taken) = (0, BLOCK, 0);
        let mut budget = ring.capacity();
        while wptr != rptr && budget != 0 && taken < 2 * BUF / BLOCK {
            budget -= 1;
            let (_, _, next) = ring.read_entry(rptr, |_| Ok(header(16)))?;
            taken += 1;
            rptr = next;
            wptr = (wptr + BLOCK) % BUF;
        }
        assert_eq!(taken, 64);
        Ok(())
    }
}
