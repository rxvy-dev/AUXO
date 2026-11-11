//! Process table and round-robin scheduler for kernel threads.
//! Slot 0 is the idle thread (the boot context that called `rs_init`).
use crate::{ffi, heap, irq, rprintln};
use core::ptr::addr_of_mut;

pub const MAXPROC: usize = 128;
pub const KSTACK: usize = 64 * 1024;   // Chrono's painting is stack-hungry
const QUANTUM_TICKS: u64 = 10; // 100 ms at 100 Hz

#[derive(Clone, Copy, PartialEq)]
pub enum State {
    Unused,
    Runnable,
    Running,
    Sleeping, // until `sleep_until` ticks
    Zombie,
}

/// FXSAVE area: x87/SSE state of a thread (user programs and the desktop use SSE).
#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct Fpu(pub [u8; 512]);

pub struct Proc {
    pid: i32,
    state: State,
    rsp: u64,
    stack: *mut u8,
    exit_code: i32,
    ticks: u64,
    name: [u8; 16],
    ucr3: u64,        // page tables of the user program this thread runs (0 = kernel only)
    fs_base: u64,     // user TLS pointer (FS base MSR)
    sleep_until: u64,
    fpu: Fpu,
}

const EMPTY: Proc = Proc {
    pid: -1,
    state: State::Unused,
    rsp: 0,
    stack: core::ptr::null_mut(),
    exit_code: 0,
    ticks: 0,
    name: [0; 16],
    ucr3: 0,
    fs_base: 0,
    sleep_until: 0,
    fpu: Fpu([0; 512]),
};

static mut PROCS: [Proc; MAXPROC] = [EMPTY; MAXPROC];
static mut CURRENT: usize = 0;
static mut NEXT_PID: i32 = 1;
static mut TICKS: u64 = 0;

#[inline]
unsafe fn slot(i: usize) -> *mut Proc {
    (addr_of_mut!(PROCS) as *mut Proc).add(i)
}

unsafe fn set_name(p: *mut Proc, name: *const u8) {
    let mut i = 0;
    while i < 15 && !name.is_null() && *name.add(i) != 0 {
        (*p).name[i] = *name.add(i);
        i += 1;
    }
    (*p).name[i] = 0;
}

pub unsafe fn init() {
    let idle = slot(0);
    (*idle).pid = 0;
    (*idle).state = State::Running;
    set_name(idle, b"idle\0".as_ptr());
    CURRENT = 0;
}

pub unsafe fn current_pid() -> i32 {
    (*slot(CURRENT)).pid
}

pub unsafe fn spawn(name: *const u8, entry: extern "C" fn(usize), arg: usize) -> i32 {
    let flags = irq::save();
    let mut found = 0;
    for i in 1..MAXPROC {
        if (*slot(i)).state == State::Unused {
            found = i;
            break;
        }
    }
    if found == 0 {
        irq::restore(flags);
        return -1;
    }
    let stack = heap::alloc(KSTACK);
    if stack.is_null() {
        irq::restore(flags);
        return -1;
    }

    // Initial frame consumed by ctx_switch: r15 r14 r13 r12 rbx rbp ret.
    // 7 qwords placed so the stack is 16-byte aligned after `ret`.
    let top = (stack as usize + KSTACK) & !15;
    let sp = (top - 72) as *mut u64;
    *sp.add(0) = 0; // r15
    *sp.add(1) = 0; // r14
    *sp.add(2) = arg as u64; // r13 -> rdi in thread_trampoline
    *sp.add(3) = entry as usize as u64; // r12 -> called by thread_trampoline
    *sp.add(4) = 0; // rbx
    *sp.add(5) = 0; // rbp
    *sp.add(6) = ffi::thread_trampoline as usize as u64; // ret

    let p = slot(found);
    (*p).pid = NEXT_PID;
    NEXT_PID += 1;
    (*p).state = State::Runnable;
    (*p).rsp = sp as u64;
    (*p).stack = stack;
    (*p).exit_code = 0;
    (*p).ticks = 0;
    (*p).ucr3 = 0;
    (*p).fs_base = 0;
    (*p).sleep_until = 0;
    set_name(p, name);
    let pid = (*p).pid;
    irq::restore(flags);
    pid
}

/// Pick the next runnable thread (round-robin over 1..MAXPROC, idle as fallback)
/// and switch to it. Safe to call from thread or interrupt context.
pub unsafe fn yield_cpu() {
    let flags = irq::save();
    let cur = CURRENT;
    if (*slot(cur)).state == State::Running {
        (*slot(cur)).state = State::Runnable;
    }

    let mut next = 0;
    for k in 0..(MAXPROC - 1) {
        let i = 1 + (cur + k) % (MAXPROC - 1);
        let p = slot(i);
        if (*p).state == State::Sleeping && TICKS >= (*p).sleep_until {
            (*p).state = State::Runnable;
        }
        if (*p).state == State::Runnable {
            next = i;
            break;
        }
    }

    (*slot(next)).state = State::Running;
    if next != cur {
        let c = slot(cur);
        let n = slot(next);
        // FPU/SSE registers and the TLS base belong to the thread; page tables and the kernel
        // stack used on entry from user mode are handed to the entry code.
        core::arch::asm!("fxsave64 [{}]", in(reg) (*c).fpu.0.as_mut_ptr());
        (*c).fs_base = rdmsr(0xC000_0100);
        wrmsr(0xC000_0100, (*n).fs_base);
        let ktop = if (*n).stack.is_null() { 0 } else { ((*n).stack as usize + KSTACK) & !15 };
        ffi::arch_switch_to((*n).ucr3, ktop as u64);
        CURRENT = next;
        ffi::ctx_switch(&mut (*c).rsp, (*n).rsp);
        // back on this thread
        core::arch::asm!("fxrstor64 [{}]", in(reg) (*slot(CURRENT)).fpu.0.as_ptr());
    }
    irq::restore(flags);
}

#[inline]
unsafe fn rdmsr(msr: u32) -> u64 {
    let (lo, hi): (u32, u32);
    core::arch::asm!("rdmsr", in("ecx") msr, out("eax") lo, out("edx") hi);
    ((hi as u64) << 32) | lo as u64
}

#[inline]
unsafe fn wrmsr(msr: u32, v: u64) {
    core::arch::asm!("wrmsr", in("ecx") msr, in("eax") v as u32, in("edx") (v >> 32) as u32);
}

/// Sleep the current thread for `n` timer ticks (at least until the next tick).
pub unsafe fn sleep(n: u64) {
    let flags = irq::save();
    let p = slot(CURRENT);
    (*p).sleep_until = TICKS + if n == 0 { 1 } else { n };
    (*p).state = State::Sleeping;
    yield_cpu();
    irq::restore(flags);
}

/// The user page tables of the current thread (exec and clone set them).
pub unsafe fn set_ucr3(cr3: u64) {
    let flags = irq::save();
    (*slot(CURRENT)).ucr3 = cr3;
    let p = slot(CURRENT);
    let ktop = if (*p).stack.is_null() { 0 } else { ((*p).stack as usize + KSTACK) & !15 };
    ffi::arch_switch_to(cr3, ktop as u64);
    irq::restore(flags);
}

pub unsafe fn kstack_top() -> u64 {
    let p = slot(CURRENT);
    if (*p).stack.is_null() { 0 } else { (((*p).stack as usize + KSTACK) & !15) as u64 }
}

pub unsafe fn tick() {
    TICKS += 1;
    (*slot(CURRENT)).ticks += 1;
    // the idle thread gives way every tick, so sleepers wake on time
    if TICKS % QUANTUM_TICKS == 0 || CURRENT == 0 {
        yield_cpu();
    }
}

pub unsafe fn exit_current(code: i32) -> ! {
    irq::save(); // stay uninterruptible until we've switched away
    let p = slot(CURRENT);
    (*p).exit_code = code;
    (*p).state = State::Zombie;
    yield_cpu();
    loop {
        core::arch::asm!("hlt");
    }
}

/// Free the stacks of finished threads. Called from the idle loop, i.e. never
/// while running on the stack being freed.
pub unsafe fn reap() {
    let flags = irq::save();
    for i in 1..MAXPROC {
        let p = slot(i);
        if (*p).state == State::Zombie {
            heap::free((*p).stack);
            (*p).stack = core::ptr::null_mut();
            (*p).state = State::Unused;
            (*p).pid = -1;
        }
    }
    irq::restore(flags);
}

/// Number of live threads (idle included), for `auxo-fetch`.
pub unsafe fn count() -> usize {
    let mut n = 0;
    for i in 0..MAXPROC {
        let st = (*slot(i)).state;
        if st == State::Runnable || st == State::Running || st == State::Sleeping {
            n += 1;
        }
    }
    n
}

pub unsafe fn ps() {
    rprintln!("PID  STATE     TICKS  NAME");
    for i in 0..MAXPROC {
        let p = slot(i);
        let st = match (*p).state {
            State::Unused => continue,
            State::Runnable => "runnable",
            State::Running => "running ",
            State::Sleeping => "sleeping",
            State::Zombie => "zombie  ",
        };
        let len = (*p).name.iter().position(|&c| c == 0).unwrap_or(16);
        let name = core::str::from_utf8(&(&(*p).name)[..len]).unwrap_or("?");
        rprintln!("{:<4} {} {:<6} {}", (*p).pid, st, (*p).ticks, name);
    }
}
