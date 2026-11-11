//! Single-CPU critical sections: disable interrupts, restore previous state.
use core::arch::asm;

const IF: u64 = 1 << 9;

#[inline]
pub unsafe fn save() -> u64 {
    let flags: u64;
    asm!("pushfq", "pop {0}", "cli", out(reg) flags);
    flags
}

#[inline]
pub unsafe fn restore(flags: u64) {
    if flags & IF != 0 {
        asm!("sti");
    }
}
