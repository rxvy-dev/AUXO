//! Kernel heap: address-ordered first-fit free list with split + coalesce.
use crate::irq;
use core::mem::size_of;
use core::ptr::null_mut;

#[repr(C, align(16))]
struct Hdr {
    size: usize, // payload bytes
    free: bool,
    next: *mut Hdr,
}

const HDR: usize = size_of::<Hdr>();
const MIN_SPLIT: usize = 16;

static mut HEAD: *mut Hdr = null_mut();
static mut TOTAL: usize = 0;
static mut USED: usize = 0;

pub unsafe fn init(base: usize, size: usize) {
    let h = base as *mut Hdr;
    (*h).size = size - HDR;
    (*h).free = true;
    (*h).next = null_mut();
    HEAD = h;
    TOTAL = size;
    USED = 0;
}

pub unsafe fn alloc(n: usize) -> *mut u8 {
    if n == 0 {
        return null_mut();
    }
    let n = (n + 15) & !15;
    let flags = irq::save();
    let mut out = null_mut();
    let mut cur = HEAD;
    while !cur.is_null() {
        if (*cur).free && (*cur).size >= n {
            if (*cur).size >= n + HDR + MIN_SPLIT {
                let rest = (cur as *mut u8).add(HDR + n) as *mut Hdr;
                (*rest).size = (*cur).size - n - HDR;
                (*rest).free = true;
                (*rest).next = (*cur).next;
                (*cur).next = rest;
                (*cur).size = n;
            }
            (*cur).free = false;
            USED += (*cur).size + HDR;
            out = (cur as *mut u8).add(HDR);
            break;
        }
        cur = (*cur).next;
    }
    irq::restore(flags);
    out
}

pub unsafe fn free(p: *mut u8) {
    if p.is_null() {
        return;
    }
    let flags = irq::save();
    let h = p.sub(HDR) as *mut Hdr;
    if !(*h).free {
        USED -= (*h).size + HDR;
        (*h).free = true;
    }
    // Coalesce adjacent free blocks (list is address-ordered).
    let mut cur = HEAD;
    while !cur.is_null() {
        let nx = (*cur).next;
        if (*cur).free && !nx.is_null() && (*nx).free {
            (*cur).size += HDR + (*nx).size;
            (*cur).next = (*nx).next;
        } else {
            cur = nx;
        }
    }
    irq::restore(flags);
}

pub unsafe fn stats() -> (usize, usize) {
    (TOTAL, USED)
}
