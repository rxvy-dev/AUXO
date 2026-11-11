//! BSD-numbered syscall dispatcher. Return value < 0 means -errno (BSD errno values).
use crate::{ffi, proc};

pub const SYS_EXIT: u64 = 1;
pub const SYS_WRITE: u64 = 4;
pub const SYS_GETPID: u64 = 20;
pub const SYS_SCHED_YIELD: u64 = 331;

pub const EBADF: i64 = 9;
pub const EINVAL: i64 = 22;
pub const ENOSYS: i64 = 78; // BSD value (Linux uses 38)

pub unsafe fn dispatch(num: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    match num {
        SYS_EXIT => proc::exit_current(a0 as i32),
        SYS_WRITE => sys_write(a0, a1 as *const u8, a2 as usize),
        SYS_GETPID => proc::current_pid() as i64,
        SYS_SCHED_YIELD => {
            proc::yield_cpu();
            0
        }
        _ => -ENOSYS,
    }
}

// NOTE: no user-pointer validation yet. Callers are kernel threads. Before
// ring 3 exists, this needs copyin()/access_ok() against the process's map.
unsafe fn sys_write(fd: u64, buf: *const u8, len: usize) -> i64 {
    if fd != 1 && fd != 2 {
        return -EBADF;
    }
    if buf.is_null() || len > (1 << 20) {
        return -EINVAL;
    }
    ffi::console_write(buf, len);
    len as i64
}
