// Submit consumers first on one CPU. Observations are NVN results and shared backing words.
    .text
    .equ dev, 16
    .equ pool_builder, 32
    .equ pool, 64
    .equ cmd, 128
    .equ queue_builder, 304
    .equ q0, 384
    .equ q1, 512
    .equ q2, 640
    .equ event_builder, 768
    .equ gate, 800
    .equ marker, 832
    .equ consumer, 864
    .equ sync0, 896
    .equ sync1, 960
    .equ handles, 1024
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    .macro signal event, mode, value
    add x0, x20, #cmd
    add x1, x20, #\event
    mov w2, #\mode
    mov w3, #2                  // BOTTOM
    mov w4, wzr
    mov w5, #\value
    call 15
    .endm
    .macro wait_event mode, value
    add x0, x20, #cmd
    add x1, x20, #gate
    mov w2, #\mode
    mov w3, #\value
    call 14
    .endm
    .macro begin
    add x0, x20, #cmd
    call 12
    .endm
    .macro end number
    add x0, x20, #cmd
    call 13
    str x0, [x20, #handles + 8 * \number]
    .endm
    .macro submit queue, number, count=1
    add x0, x20, #\queue
    mov w1, #\count
    add x2, x20, #handles + 8 * \number
    call 18
    .endm
    .macro value_at offset, value
    ldr w9, [x21, #\offset]
    cmp w9, #\value
    b.ne fail
    .endm
    .macro sync_wait sync, expected
    add x0, x20, #\sync
    mov x1, xzr
    call 24
    cmp w0, #\expected
    b.ne fail
    .endm
    .macro fence queue, sync
    add x0, x20, #\queue
    add x1, x20, #\sync
    mov w2, wzr
    mov w3, #1                  // FLUSH_FOR_CPU
    call 22
    .endm
    .macro place_event event, offset
    add x0, x20, #event_builder
    add x1, x20, #pool
    mov x2, #\offset
    call 27
    add x0, x20, #\event
    add x1, x20, #event_builder
    call 28
    cbz w0, fail
    .endm
    .macro init_queue queue
    add x0, x20, #\queue
    add x1, x20, #queue_builder
    call 11
    cbz w0, fail
    .endm

    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x30, [sp, #-16]!
    sub sp, sp, #1152
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x1, #0x200000
    svc #1
    cbnz w0, fail
    mov x21, x1
    mov x0, x20
    call 0
    add x0, x20, #dev
    mov x1, x20
    call 1
    cbz w0, fail
    add x0, x20, #pool_builder
    call 2
    add x0, x20, #pool_builder
    add x1, x20, #dev
    call 3
    add x0, x20, #pool_builder
    mov x1, x21
    mov x2, #0x4000
    call 4
    add x0, x20, #pool
    add x1, x20, #pool_builder
    call 5
    cbz w0, fail
    add x0, x20, #cmd
    add x1, x20, #dev
    call 6
    cbz w0, fail
    add x0, x20, #cmd
    add x1, x20, #pool
    mov x2, #0x1000
    mov x3, #0x1000
    call 7
    add x0, x20, #cmd
    add x1, x21, #2, lsl #12
    mov x2, #0x1000
    call 8
    add x0, x20, #queue_builder
    call 9
    add x0, x20, #queue_builder
    add x1, x20, #dev
    call 10
    init_queue q0
    init_queue q1
    init_queue q2
    add x0, x20, #event_builder
    call 26
    place_event gate, 0
    place_event marker, 4
    place_event consumer, 8
    add x0, x20, #sync0
    add x1, x20, #dev
    call 25
    cbz w0, fail
    add x0, x20, #sync1
    add x1, x20, #dev
    call 25
    cbz w0, fail
    add x0, x20, #q0
    add x1, x20, #sync1
    call 21                     // A queue wait on a never-fenced sync passes.
    cbz w0, fail
    sync_wait sync1, 0

    begin
    add x0, x20, #cmd
    add x1, x20, #sync1
    call 17                     // A command-buffer wait on a never-fenced sync passes.
    signal marker, 1, 1
    wait_event 0, 1             // The actual dependency: EQUAL 1.
    signal marker, 1, 2
    add x0, x20, #cmd
    add x1, x20, #sync0
    mov w2, wzr
    mov w3, wzr
    call 16
    end 0
    begin
    signal marker, 1, 4
    end 1
    begin
    signal consumer, 0, 11
    end 2
    begin
    signal gate, 0, 2            // Wrong value cannot release EQUAL 1.
    end 3
    begin
    signal gate, 0, 1
    signal gate, 0, 2            // A later write must not erase the observed signal.
    end 4
    value_at 0, 0
    value_at 4, 0
    add x0, x20, #gate
    call 29
    place_event gate, 12        // Recorded waits and signals keep the original word.

    submit q0, 0, 2
    add x0, x20, #q0
    call 19                     // Flush must return even with the unresolved wait.
    sync_wait sync0, 2
    value_at 4, 1
    add x0, x20, #q1
    add x1, x20, #sync0
    call 21
    cbz w0, fail
    submit q1, 2
    fence q1, sync1
    sync_wait sync1, 2
    fence q2, sync0             // Refence elsewhere. Queued q1 wait must retain q0's receipt.
    sync_wait sync0, 0
    submit q2, 3
    value_at 0, 2
    value_at 4, 1
    value_at 8, 0
    sync_wait sync1, 2
    add x9, x21, #1, lsl #12
    stp xzr, xzr, [x9, #40]      // The submitted continuation owns its command bytes.
    stp xzr, xzr, [x9, #56]
    add x9, x21, #2, lsl #12
    stp xzr, xzr, [x9]           // It no longer needs the original control handle either.
    submit q2, 4
    add x0, x20, #q1
    call 20                     // Join writes, not merely the unrelated q2 work.
    sync_wait sync1, 0
    value_at 0, 2
    value_at 4, 7
    value_at 8, 11
    value_at 12, 0

    mov w9, #0x7fffffff
    str w9, [x21, #12]
    begin
    wait_event 1, -1             // Exactly half a cycle away: GEQUAL_WRAP is false.
    signal marker, 1, 16
    end 5
    submit q0, 5
    value_at 4, 7
    add x0, x20, #gate
    mov w1, #1
    mov w2, #0x80000001         // CPU ADD wraps this storage word to zero.
    call 30
    add x0, x20, #q0
    call 20
    value_at 4, 23              // Zero satisfies GEQUAL_WRAP against 0xffffffff.
    value_at 12, 0

    begin
    wait_event 0, 9
    signal marker, 1, 32
    end 6
    submit q0, 6
    value_at 4, 23
    mov w9, #9
    add x10, x21, #12
    stlr w9, [x10]               // Write mapped memory without calling EventSignal.
    add x10, x21, #4
    mov w23, #1000
poll:
    ldar w9, [x10]
    cmp w9, #55
    b.eq retired
    // SleepThread can clobber caller-saved registers, so reconstruct x10.
    movz x0, #0x4240
    movk x0, #0xf, lsl #16
    svc #0xb
    add x10, x21, #4
    subs w23, w23, #1
    b.ne poll
    b fail
retired:
    add x0, x20, #q0
    call 23
    init_queue q0                // Reusing queue storage must not inherit deferred work.
    add x0, x20, #q0
    call 20
    value_at 4, 55
    add x0, x20, #q0
    call 23
    add x0, x20, #q1
    call 23
    add x0, x20, #q2
    call 23
    mov w0, wzr
    b done
fail:
    mov w0, #1
done:
    add sp, sp, #1152
    ldp x23, x30, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret
