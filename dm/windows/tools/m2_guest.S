// Synthetic M2 entry at module +0x100. The test builds its dynamic tables.
    .text
    stp x19, x20, [sp, #-32]!
    stp x21, x22, [sp, #16]
    sub sp, sp, #48           // QueryMemory output
    // Both writeback forms must use the original guest SP, preserving flags.
    mov x18, #0xabcd
    cmp xzr, xzr
    stp x18, xzr, [sp, #-16]!
    mov x18, #0
    ldp x18, x20, [sp], #16
    b.ne fail
    mov x9, #0xabcd
    cmp x18, x9
    b.ne fail
    cbnz x20, fail

    adrp x11, #0x2000
    add x11, x11, #0x280
    ldr w15, [x11, #16]
    and w13, w15, #3
    ldrsw x13, [x11, w13, uxtw #2]
    add x18, x13, x11
    mov x16, #0x123
    mov x17, #0x456
    cmp xzr, xzr
    br x18
jumped:
    b.ne fail
    cmp x16, #0x123
    b.ne fail
    cmp x17, #0x456
    b.ne fail

    adrp x19, #0x3000         // relative to the first module page, not this instruction
    add x19, x19, #0x800
    ldp x9, x10, [x19]      // RELA and RELR should both point at +0x3820
    add x11, x19, #0x20
    cmp x9, x11
    b.ne fail
    cmp x10, x11
    b.ne fail
    ldr x9, [x19, #16]       // resolved cross-module JUMP_SLOT
    stp x29, x30, [sp, #-16]!
    blr x9
    ldp x29, x30, [sp], #16
    cmp x0, #42
    b.ne fail

    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    mov x20, x1
    mov x9, #0xcafe
    str x9, [x20]
    mov x1, #0
    svc #1
    cbnz x0, fail
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    ldr x9, [x20]
    cbnz x9, fail
    mov x0, sp
    mov x2, x20
    svc #6
    cbnz x0, fail
    ldp x9, x10, [sp]
    cmp x9, x20
    b.ne fail
    mov x11, #0x200000
    cmp x10, x11
    b.ne fail
    ldr w9, [sp, #16]
    cmp w9, #5
    b.ne fail
    mov x0, sp
    mov x2, sp
    svc #6
    cbnz x0, fail
    ldr w9, [sp, #16]
    cmp w9, #0xb
    b.ne fail

    mov x1, #2
    svc #0x29
    cbnz x0, fail
    mov x21, x1
    mov x0, x21
    mov x1, #0x1000
    svc #0x2c
    cbnz x0, fail
    mov x9, #0xcafe
    str x9, [x21]
    mov x0, x21
    mov x1, #0x1000
    svc #0x2d
    cbnz x0, fail
    mov x0, x21
    mov x1, #0x1000
    svc #0x2c
    cbnz x0, fail
    ldr x9, [x21]
    cbnz x9, fail

    and x22, x19, #~0xfff
    mov x0, x22
    mov x1, #0x1000
    mov x2, #1
    svc #2
    cbnz x0, fail
    mov x0, x22
    mov x1, #0x1000
    mov x2, #0x10
    mov x3, x2
    svc #3
    cbnz x0, fail
    mov x0, sp
    mov x2, x22
    svc #6
    cbnz x0, fail
    ldp w9, w10, [sp, #20]   // permission-locked attribute and read-only permission
    cmp w9, #0x10
    b.ne fail
    cmp w10, #1
    b.ne fail
    mov x0, x22
    mov x1, #0x1000
    mov x2, #3
    svc #2
    mov x9, #0xd401
    cmp x0, x9
    b.ne fail

    mrs x18, cntpct_el0
    svc #0x1e
    cbnz x0, fail
    cmp x1, x18
    b.lo fail
    b stack_test
fail:
    mov x0, #1
done:
    add sp, sp, #48
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #32
    ret

    // Targets at module +0x400, +0x404, +0x408 and +0x40c.
    .org 0x300
    b fail
    b jumped
    b fail
    b fail

    .macro arb_wait type, addr, value, timeout, result
    add x0, x20, #\addr
    mov w1, #\type
    mov x2, #\value
    mov x3, #\timeout
    svc #0x34
    mov w9, #\result
    cmp w0, w9
    b.ne fail
    .endm
stack_test:
    // The high half distinguishes a 64-bit wait from a 32-bit wait.
    mov x9, #1
    movk x9, #1, lsl #32
    str x9, [x20, #32]
    arb_wait 2, 32, 1, 0, 0xea01    // Equal, zero timeout.
    arb_wait 2, 32, 2, -1, 0xfa01   // Mismatch, even with no deadline.
    arb_wait 3, 32, 1, 0, 0xfa01
    arb_wait 3, 36, 1, 0, 0xcc01    // Misaligned 64-bit address.
    arb_wait 0, 32, 2, 10000, 0xea01 // Finite timeout.
    arb_wait 1, 32, 2, 0, 0xea01    // Decrements even when polling.
    ldr w9, [x20, #32]
    cbnz w9, fail
    add x0, x20, #32
    mov w1, #1
    mov w2, #1
    mov w3, #1
    svc #0x35
    mov w9, #0xfa01
    cmp w0, w9
    b.ne fail
    add x0, x20, #32
    mov w1, #2
    mov w2, #0
    mov w3, #0
    svc #0x35                       // With no waiters, type 2 increments.
    cbnz w0, fail
    ldr w9, [x20, #32]
    cmp w9, #1
    b.ne fail

    mov x1, #14
    svc #0x29
    cbnz x0, fail
    add x22, x1, #0x800, lsl #12  // Stack region, beyond main stack and TLS
    mov x9, #0xcafe
    str x9, [x21]
    mov x0, x22
    mov x1, x21
    mov x2, #0x1000
    svc #4
    cbnz x0, fail
    ldr x9, [x22]
    mov x10, #0xcafe
    cmp x9, x10
    b.ne fail
    mov x0, sp
    mov x2, x21
    svc #6
    cbnz x0, fail
    ldp w9, w10, [sp, #20]       // Borrowed source has no permission
    cmp w9, #1
    b.ne fail
    cbnz w10, fail
    mov x0, sp
    mov x2, x22
    svc #6
    cbnz x0, fail
    ldr w9, [sp, #16]
    cmp w9, #0xb
    b.ne fail
    mov x0, x21
    mov x1, #0x1000
    svc #0x2d                    // Cannot decommit a borrowed source
    mov x9, #0xd401
    cmp x0, x9
    b.ne fail
    mov x0, x22
    add x1, x21, #0x1000
    mov x2, #0x1000
    svc #5                       // Must match the original stack move
    mov x9, #0xd401
    cmp x0, x9
    b.ne fail
    // A dormant thread owns its mapping but has not executed its entry.
    stp xzr, xzr, [x20]
    adr x1, worker
    mov x2, x20
    add x3, x22, #0x1000
    mov w4, #42
    mov w5, #1
    svc #8
    cbnz x0, fail
    mov w19, w1
    str w19, [sp, #40]
    add x1, sp, #40
    mov w2, #1
    mov x3, xzr
    svc #0x18
    mov x9, #0xea01
    cmp x0, x9
    b.ne fail
    ldr x9, [x20]
    cbnz x9, fail
    mov x0, x22
    mov x1, x21
    mov x2, #0x1000
    svc #5
    mov x9, #0xd401
    cmp x0, x9
    b.ne fail
    mov w1, w19
    svc #0xc
    cbnz x0, fail
    cmp w1, #42
    b.ne fail
    mov w1, w19
    svc #0x25
    cbnz x0, fail
    str w1, [sp, #44]            // Compare the actual ID with the worker's TLS
    // Own the lock before starting the worker. First check a stale lock
    // attempt returns for user-mode retry without modifying the word.
    mrs x9, tpidrro_el0
    ldr w9, [x9, #0x110]
    str w9, [x20, #40]
    mov w0, w1
    add x1, x20, #40
    mov w2, w9
    svc #0x1a
    cbnz w0, fail
    ldr w9, [x20, #40]
    cmp w9, w2
    b.ne fail
    mov x18, #0x5555
    mov w0, w19
    svc #9
    cbnz x0, fail
    // The worker decrements under the same lock that queues its wait. Seeing
    // zero and then signaling through the SVC guarantees it has enrolled.
worker_waiting:
    add x9, x20, #32
    ldar w9, [x9]
    cbz w9, wake_worker
    add x1, sp, #40
    mov w2, #1
    mov x3, #0
    svc #0x18
    mov w9, #0xea01
    cmp w0, w9
    b.ne fail
    mov x0, #0
    svc #0xb
    b worker_waiting
wake_worker:
    add x0, x20, #36
    mov w1, #0
    mov w3, #0
    svc #0x35                       // Must not consume the neighboring waiter.
    cbnz w0, fail
    add x0, x20, #32
    mov w1, #2
    mov w2, #0
    mov w3, #0
    svc #0x35                       // With waiters, wake all and subtract two.
    cbnz w0, fail
    ldr w9, [x20, #32]
    cmn w9, #2
    b.ne fail
mutex_contending:
    add x9, x20, #40
    ldar w9, [x9]
    tbnz w9, #30, mutex_release
    add x1, sp, #40
    mov w2, #1
    mov x3, #0
    svc #0x18
    mov w9, #0xea01
    cmp w0, w9
    b.ne fail
    mov x0, #0
    svc #0xb
    b mutex_contending
mutex_release:
    // No guest instruction in the worker's critical section can run until
    // we release it. This wait also lets the contended SVC take kernel_lock.
    add x1, sp, #40
    mov w2, #1
    mov x3, #0x100000
    svc #0x18
    mov w9, #0xea01
    cmp w0, w9
    b.ne fail
    ldr x9, [x20]
    cbnz x9, fail
    add x0, x20, #40
    svc #0x1b
    cbnz w0, fail
    add x12, x20, #44           // Condition key published before worker releases the mutex.
    add x13, x20, #40
    mrs x9, tpidrro_el0
    ldr w14, [x9, #0x110]
condition_enrolled:
    ldar w9, [x12]
    cbz w9, condition_enrolled
condition_take:
    ldaxr w9, [x13]
    cbnz w9, condition_take
    stxr w9, w14, [x13]
    cbnz w9, condition_take
    mov x0, x12
    mov w1, #1
    svc #0x1d
    cbnz w0, fail
    ldr w9, [x12]
    cbnz w9, fail
    ldr w9, [x13]
    orr w10, w14, #0x40000000
    cmp w9, w10                 // Signaled, but still waiting to reacquire our mutex.
    b.ne fail
    mov x0, x13
    svc #0x1b
    cbnz w0, fail
    add x1, sp, #40
    mov w2, #1
    mov x3, #-1
    svc #0x18
    cbnz x0, fail
    cbnz x1, fail
    ldr x9, [x20]
    mov x10, #0xface
    cmp x9, x10
    b.ne fail
    ldr w9, [x20, #40]
    cbnz w9, fail
    ldr w9, [x20, #24]
    ldr w10, [sp, #44]
    cmp w9, w10
    b.ne fail
    mrs x9, tpidr_el0
    ldr x10, [x20, #8]
    cmp x9, x10
    b.eq fail
    mrs x9, tpidrro_el0
    ldr x10, [x20, #16]
    cmp x9, x10
    b.eq fail
    mov x9, #0x5555
    cmp x18, x9
    b.ne fail
    mov w0, w19
    svc #9
    mov x9, #0xfa01
    cmp x0, x9
    b.ne fail
    mov w0, w19
    svc #0x16
    cbnz x0, fail
    mov w0, w19
    svc #9
    mov x9, #0xe401
    cmp x0, x9
    b.ne fail

    // Closing a dormant handle must reclaim its slot without running it.
    adr x1, worker
    mov x2, x20
    add x3, x22, #0x1000
    mov w4, #42
    mov w5, #1
    svc #8
    cbnz x0, fail
    cmp w1, w19
    b.eq fail
    mov w0, w1
    svc #0x16
    cbnz x0, fail
    mov x1, #24
    svc #0x29
    cbnz x0, fail
    cmp x1, #95
    b.ne fail

    mov x9, #0xbeef
    str x9, [x22]
    mov x0, x22
    mov x1, x21
    mov x2, #0x1000
    svc #5
    cbnz x0, fail
    ldr x9, [x21]
    mov x10, #0xbeef
    cmp x9, x10
    b.ne fail
    mov x0, sp
    mov x2, x22
    svc #6
    cbnz x0, fail
    ldr w9, [sp, #16]            // Destination is decommitted again
    cbnz w9, fail
    b ipc_test

worker:
    mov x19, x0
    cbnz x18, worker_fail
    mrs x10, tpidr_el0
    mrs x11, tpidrro_el0
    stp x10, x11, [x19, #8]
    ldr w9, [x11, #0x110]
    str w9, [x19, #24]
    ldr x12, [x11, #0x1f8]
    add x11, x11, #0x200
    cmp x11, x12
    b.ne worker_fail
    mov w1, #0x8000
    movk w1, #0xffff, lsl #16    // Current-thread pseudohandle
    svc #0x25
    cbnz x0, worker_fail
    ldr w9, [x19, #24]
    cmp w9, w1
    b.ne worker_fail
    mov x18, #0x9999
    svc #0x10
    cmp w0, #1
    b.ne worker_fail
    mov x9, #0x9999
    cmp x18, x9
    b.ne worker_fail
    add x0, x19, #32
    mov w1, #1
    mov w2, #2
    mov x3, #-1
    svc #0x34
    cbnz x0, worker_fail
    add x21, x19, #40
    ldar w0, [x21]
    orr w9, w0, #0x40000000
    stlr w9, [x21]              // Only the owner can compete; it waits for this bit.
    mov x1, x21
    ldr w2, [x19, #24]
    svc #0x1a
    cbnz w0, worker_fail
    ldar w9, [x21]
    ldr w10, [x19, #24]
    cmp w9, w10
    b.eq mutex_owned           // Direct handoff, without an unlocked gap.
    cbnz w9, worker_fail
mutex_retry:                  // Owner released before the SVC could enroll us.
    ldaxr w9, [x21]
    cbnz w9, worker_fail
    stxr w11, w10, [x21]
    cbnz w11, mutex_retry
mutex_owned:
    mov x0, x21
    add x1, x19, #44
    ldr w2, [x19, #24]
    mov x3, #-1
    svc #0x1c
    cbnz w0, worker_fail
    ldr w9, [x21]
    ldr w10, [x19, #24]
    cmp w9, w10
    b.ne worker_fail
    mov x9, #0xface
    str x9, [x19]
    mov x0, x21
    svc #0x1b
    cbnz w0, worker_fail
worker_fail:
    svc #0xa
    brk #0

// Exercise service replies through the real TLS buffer and SVC gate.
    .macro sm_get word0, word1
    mov w9, #17
    mov w10, #2
    stp w9, w10, [x20]
    .if \word0 == 0
    str xzr, [x20, #8]
    .else
    mov w9, #(\word0 & 0xffff)
    movk w9, #(\word0 >> 16), lsl #16
    mov w10, #\word1
    stp w9, w10, [x20, #8]
    .endif
    mov w0, w21
    svc #0x21
    cbnz w0, fail
    .endm
ipc_test:
    mrs x20, tpidrro_el0
    mov w9, #0x7a              // An unregistered named port, not a guest handle.
    str w9, [sp]
    mov x1, sp
    svc #0x1f
    mov w9, #0xf201
    cmp w0, w9
    b.ne fail
    mov w9, #0x6d73
    movk w9, #0x3a, lsl #16   // sm:\0
    str w9, [sp]
    mov x1, sp
    svc #0x1f
    cbnz w0, fail
    mov w21, w1
    cbz w21, fail
    sm_get 0x3a636361, 0x3075  // acc:u0 before RegisterClient
    ldr w9, [x20, #8]
    cmp w9, #0x415
    b.ne fail

    mov w9, #16                // RegisterClient without a PID descriptor
    stp w9, wzr, [x20]
    mov w0, w21
    svc #0x21
    mov w9, #0xe801
    cmp w0, w9
    b.ne fail
    ldr w9, [x20]
    cmp w9, #16               // Malformed request is untouched
    b.ne fail
    mov w9, #16
    mov w10, #0x80000000
    stp w9, w10, [x20]
    mov w9, #1
    str w9, [x20, #8]
    mov w0, w21
    svc #0x21
    cbnz w0, fail
    ldr w9, [x20, #8]
    cbnz w9, fail
    sm_get 0, 0
    ldr w9, [x20, #8]
    cmp w9, #0xc15
    b.ne fail
    sm_get 0x3a636361, 0x3075  // Local account imports don't advertise acc:u0 in SM.
    ldp w10, w9, [x20, #4]
    cmp w10, #1               // Result only, no moved handle.
    b.ne fail
    cmp w9, #0xe15
    b.ne fail

    mov w9, #15                // TIPC close; local handle still needs CloseHandle
    stp w9, wzr, [x20]
    mov w0, w21
    svc #0x21
    mov w9, #0xf601
    cmp w0, w9
    b.ne fail
    mov w0, w21
    svc #0x16
    cbnz w0, fail
    mov w0, w21
    svc #0x21
    mov w9, #0xe401
    cmp w0, w9
    b.ne fail
    mov w0, #0
    b done
