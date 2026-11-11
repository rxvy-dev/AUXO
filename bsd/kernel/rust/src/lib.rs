//! AuxoBSD, Rust half: kernel heap, process table, scheduler, syscalls.
//! The C half (kern/, arch/) calls in through the `rs_*` functions below
//! (declared in include/rs.h) and this crate calls back through `ffi`.
#![no_std]

use core::fmt;

pub mod ffi {
    extern "C" {
        pub fn console_write(buf: *const u8, len: usize);
        pub fn ctx_switch(old_rsp: *mut u64, new_rsp: u64);
        pub fn thread_trampoline();
        pub fn arch_switch_to(ucr3: u64, kstack_top: u64);
    }
}

/// Zero-sized console writer so `write!`/`rprintln!` work in the kernel.
pub struct Con;
impl fmt::Write for Con {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        unsafe { ffi::console_write(s.as_ptr(), s.len()) };
        Ok(())
    }
}

#[macro_export]
macro_rules! rprintln {
    ($($arg:tt)*) => {{
        use core::fmt::Write;
        let _ = writeln!($crate::Con, $($arg)*);
    }};
}

mod heap;
mod irq;
mod proc;
mod syscall;

#[no_mangle]
pub unsafe extern "C" fn rs_init(heap_base: usize, heap_size: usize) {
    heap::init(heap_base, heap_size);
    proc::init();
}

#[no_mangle]
pub unsafe extern "C" fn rs_spawn(name: *const u8, entry: extern "C" fn(usize), arg: usize) -> i32 {
    proc::spawn(name, entry, arg)
}

#[no_mangle]
pub unsafe extern "C" fn rs_kmalloc(n: usize) -> *mut u8 { heap::alloc(n) }

#[no_mangle]
pub unsafe extern "C" fn rs_kfree(p: *mut u8) { heap::free(p) }

#[no_mangle]
pub unsafe extern "C" fn rs_syscall(num: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    syscall::dispatch(num, a0, a1, a2)
}

#[no_mangle]
pub unsafe extern "C" fn rs_tick() { proc::tick() }

#[no_mangle]
pub unsafe extern "C" fn rs_reap() { proc::reap() }

#[no_mangle]
pub unsafe extern "C" fn rs_ps() {
    proc::ps();
    let (t, u) = heap::stats();
    rprintln!("heap: {} KiB used of {} KiB", u / 1024, t / 1024);
}

#[no_mangle]
pub unsafe extern "C" fn rs_proc_count() -> usize { proc::count() }

#[no_mangle]
pub unsafe extern "C" fn rs_heap_stats(total: *mut usize, used: *mut usize) {
    let (t, u) = heap::stats();
    *total = t;
    *used = u;
}

/// Sleep for `n` timer ticks (10 ms each); 0 = until the next tick.
#[no_mangle]
pub unsafe extern "C" fn rs_sleep(n: u64) { proc::sleep(n) }

#[no_mangle]
pub unsafe extern "C" fn rs_current_pid() -> i32 { proc::current_pid() }

/// Set the user page tables of the calling thread.
#[no_mangle]
pub unsafe extern "C" fn rs_set_ucr3(cr3: u64) { proc::set_ucr3(cr3) }

#[no_mangle]
pub unsafe extern "C" fn rs_kstack_top() -> u64 { proc::kstack_top() }

/// Give the CPU to the next runnable thread (cooperative wait loops).
#[no_mangle]
pub unsafe extern "C" fn rs_yield() { proc::yield_cpu() }

/// Called by `thread_trampoline` when a thread's entry function returns.
#[no_mangle]
pub unsafe extern "C" fn rs_thread_exit() -> ! { proc::exit_current(0) }

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write;
    let _ = write!(Con, "\n*** AuxoBSD PANIC (Rust): {}\nSystem halted.\n", info);
    loop {
        unsafe { core::arch::asm!("cli", "hlt") };
    }
}
