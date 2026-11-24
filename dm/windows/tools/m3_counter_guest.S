// Exercise NVN2 timestamp reports through guest proc lookup and queue submission.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x30, [sp, #-16]!
    sub sp, sp, #576
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    adrp x22, #0x1000
    ldr w22, [x22, #0x780]
    mov x1, #0x200000
    svc #1
    cbnz w0, fail
    mov x21, x1

    mov x0, x20
    call 0                       // DeviceBuilderSetDefaults
    add x0, x20, #16
    mov x1, x20
    call 1                       // DeviceInitialize
    cbz w0, fail
    add x0, x20, #32
    call 2                       // MemoryPoolBuilderSetDefaults
    add x0, x20, #32
    add x1, x20, #16
    call 3                       // MemoryPoolBuilderSetDevice
    add x0, x20, #32
    mov x1, x21
    mov x2, #0x4000
    call 4                       // MemoryPoolBuilderSetStorage
    add x0, x20, #64
    add x1, x20, #32
    call 5                       // MemoryPoolInitialize
    cbz w0, fail
    add x0, x20, #64
    call 6                       // MemoryPoolGetBufferAddress
    cmp x0, x21
    b.ne fail
    add x0, x20, #128
    add x1, x20, #16
    call 7                       // CommandBufferInitialize
    cbz w0, fail
    add x0, x20, #128
    add x1, x20, #64
    mov x2, #0x1000
    mov x3, #0x100
    call 8                       // AddCommandMemory
    add x0, x20, #128
    add x1, x21, #2, lsl #12
    mov x2, #0x100
    call 9                       // AddControlMemory
    add x0, x20, #352
    call 10                      // QueueBuilderSetDefaults
    add x0, x20, #352
    add x1, x20, #16
    call 11                      // QueueBuilderSetDevice
    add x0, x20, #448
    add x1, x20, #352
    call 12                      // QueueInitialize
    cbz w0, fail

    add x0, x20, #16
    adrp x1, #0x1000
    add x1, x1, #0x500
    call 13                      // DeviceGetProcAddress("nvnCommandBufferReportCounter")
    cbz x0, fail
    mov x23, x0
    mov x9, #0x6789
    stp x9, x9, [x21]
    stp x9, x9, [x21, #16]
    stp x9, x9, [x21, #32]
    stp x9, x9, [x21, #48]
    stp x9, x9, [x21, #64]
    add x0, x20, #128
    call 14                      // BeginRecording
    add x0, x20, #128
    mov w1, #0x10               // TIMESTAMP_TOP
    cmp w22, #1
    mov w9, #2                  // INPUT_VERTICES must not pretend to be a timer
    csel w1, w9, w1, eq
    add x2, x21, #16
    cmp w22, #2
    mov x9, #0x10               // Unmapped GPU address, only used at submit
    csel x2, x9, x2, eq
    blr x23
    add x0, x20, #128
    mov w1, wzr                 // TIMESTAMP
    add x2, x21, #48
    blr x23
    // Uniform bytes are consumed while recording, unlike their destination.
    mov w9, #0x4321
    str w9, [x20, #544]
    add x0, x20, #128
    add x1, x21, #256
    mov x2, #256
    mov x3, #12
    mov x4, #4
    add x5, x20, #544
    call 19
    str wzr, [x20, #544]
    ldr w9, [x21, #268]
    cbnz w9, fail
    add x0, x20, #128
    call 15                      // EndRecording
    str x0, [x20, #512]
    mov x9, #0x6789
    ldp x10, x11, [x21, #16]
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne fail
    ldp x10, x11, [x21, #48]
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne fail

    mov x25, xzr
    mov w27, #2                 // Reuse the list; each submit must resample
submit:
    movz x0, #0x4240
    movk x0, #0xf, lsl #16
    svc #0xb                    // SleepThread(1 ms) between record/replay and replays
    svc #0x1e                   // Bracket NVN's current time in guest CPU ticks
    mov x26, x1
    add x0, x20, #16
    call 16                      // DeviceGetCurrentTimestampInNanoseconds
    mov x24, x0
    svc #0x1e
    mov x9, #625
    mov x10, #12                // Default fixture CPU clock: 19.2 MHz
    mul x26, x26, x9
    udiv x26, x26, x10
    cmp x24, x26
    b.lo fail
    add x1, x1, #1              // Account for quantization of the upper-bound tick
    mul x1, x1, x9
    udiv x1, x1, x10
    cmp x24, x1
    b.hi fail

    add x0, x20, #448
    mov w1, #1
    add x2, x20, #512
    call 17                      // QueueSubmitCommands: write both records now
    ldr w9, [x21, #268]
    mov w10, #0x4321
    cmp w9, w10
    b.ne fail
    ldr w9, [x21, #264]
    ldr w10, [x21, #272]
    orr w9, w9, w10
    cbnz w9, fail
    str wzr, [x21, #268]         // The second submission reuses the same snapshot.
    add x0, x20, #16
    call 16
    ldp x9, x10, [x21, #16]
    ldp x11, x12, [x21, #48]
    orr x9, x9, x11            // Both counter words must be zero on return
    cbnz x9, fail
    cmp x10, x24               // pre-submit <= TOP <= BOTTOM <= post-submit
    b.lo fail
    cmp x10, x12
    b.hi fail
    cmp x12, x0
    b.hi fail
    cmp x10, x25
    b.ls fail
    mov x25, x12
    add x0, x20, #16
    add x1, x21, #48
    call 18                      // DeviceGetTimestampInNanoseconds
    cmp x0, x25                // The NVN2 raw timestamp is in ns already
    b.ne fail
    subs w27, w27, #1
    b.ne submit

    mov x9, #0x6789
    ldp x10, x11, [x21]
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne fail
    ldp x10, x11, [x21, #32]
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne fail
    ldp x10, x11, [x21, #64]
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne fail
    mov w0, wzr
    b done
fail:
    mov w0, #1
done:
    add sp, sp, #576
    ldp x27, x30, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret
