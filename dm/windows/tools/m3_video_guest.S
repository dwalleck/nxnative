// Real SDK 22.2.8 SystemEvents, native AV1 worker, and a lossless AV1 picture.
// The heap holds an Av1DecoderType, one pool, and independent CPU Y/UV outputs.
    .text
    .set pref, 0x400
    .set pool, 0x440
    .set sync, 0x480
    .set properties, 0x4a0
    .set event, 0x500
    .set luma, 0x800
    .set chroma, 0xa00
    .set storage, 0x4000
    .macro call n
    add w21, w21, #1
    ldr x16, [x19, #(8 * \n)]
    blr x16
    .endm
    .macro success
    cbnz w0, fail
    .endm
    .macro props
    add x0, x20, #properties
    add x1, x0, #2
    add x2, x0, #4
    add x3, x0, #6
    add x4, x0, #8
    add x5, x0, #16
    mov x6, x20
    mov w7, wzr
    call 7
    .endm
    sub sp, sp, #64
    // The eight-register calls below also need four overflow arguments.
    // Keep them at SP+0; the saved registers live above that area.
    stp x19, x20, [sp, #32]
    stp x21, x30, [sp, #48]
    mov w21, #1
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x1, #0x200000
    svc #1
    success
    mov x20, x1

    mov w0, #16
    mov w1, #16
    mov w2, #3
    mov w3, #1
    add x8, x20, #pref
    call 0 // MakeDecodeImagePreference: indirect return in guest x8.
    add x0, x20, #sync
    add x1, x0, #8
    add x2, x20, #pref
    mov w3, #1
    call 1 // CalculateDecoderImagePoolBuffer.
    success
    add x10, x20, #sync
    ldp x8, x9, [x10]
    cmp x8, #storage
    b.ne fail
    cmp x9, #storage
    b.ne fail
    add x0, x20, #pool
    add x1, x20, #storage
    mov x2, x8
    add x3, x20, #pref
    mov w4, #1
    call 2 // InitializeDecoderImagePool.
    success

    mov x0, x20
    add x1, x20, #0x8000
    mov x2, #0x4000
    mov x3, #0x10000
    mov w4, #16
    mov w5, #16
    mov w6, #1
    mov w7, #0
    mov x8, #3
    mov x9, #1
    stp x8, x9, [sp]
    call 3 // InitializeAv1DecoderByTransferMemory: output and mode on guest stack.
    success
    mov x0, x20
    mov w1, wzr
    add x2, x20, #pool
    mov w3, wzr
    call 4 // RegisterAv1DecoderBuffer.
    success
    mov x0, x20
    call 5 // GetAv1DecoderSubmissionReadyEvent.
    cbz x0, fail
    call 6 // SDK TryWaitSystemEvent: initially submission-ready.
    cbz w0, fail

    props
    mov w8, #0x286f
    movk w8, #0x1c, lsl #16 // module 111, description 3604.
    cmp w0, w8
    b.ne fail
    add x0, x20, #(sync + 16)
    add x1, x20, #sync
    mov x2, x20
    call 8 // Acquire initially empty output buffer.
    success
    ldr w8, [x20, #(sync + 16)]
    cbnz w8, fail
    add x0, x20, #sync
    call 9 // TryWaitNativeSync: the initial buffer is immediately reusable.
    cbz w0, fail
    mov x0, x20
    mov w1, wzr
    mov x2, xzr
    mov x3, xzr
    call 10 // Release output buffer for the worker to fill.
    add x0, x20, #(sync + 16)
    add x1, x20, #sync
    mov x2, x20
    call 8 // Reacquire the same buffer, initially still pending.
    success
    add x0, x20, #sync
    call 9
    cbnz w0, fail
    add x0, x20, #event
    call 11 // CreateSyncPointEvent.
    success
    add x0, x20, #event
    add x1, x20, #sync
    call 12 // RequestSyncPointEvent before submitting the packet.
    success
    add x0, x20, #(sync + 24)
    mov x1, x20
    // Fixture stores the encoded unit in NRO data, outside rewritten text.
    ldr x3, [x19, #0x100]
    add x2, x19, #0x108
    mov x4, #4321
    mov w5, wzr
    call 13 // SubmitAv1DecoderInputBuffer.
    success
    add x0, x20, #event
    movz x1, #0x9400
    movk x1, #0x7735, lsl #16 // timeout 2,000,000,000 ns.
    call 14 // Guest SDK TimedWaitSystemEvent.
    cbz w0, fail
    add x0, x20, #sync
    call 9
    cbz w0, fail
    props
    success
    ldr x8, [x20, #properties]
    mov w9, #16
    movk x9, #16, lsl #16
    movk x9, #16, lsl #32
    movk x9, #16, lsl #48
    cmp x8, x9
    b.ne fail
    ldr x8, [x20, #(properties + 8)]
    mov x9, #4321
    cmp x8, x9
    b.ne fail
    ldrb w8, [x20, #(properties + 16)]
    cbnz w8, fail

    add x0, x20, #pool
    mov w1, wzr
    mov w2, wzr
    mov w3, wzr
    mov w4, #16
    mov w5, #16
    add x6, x20, #luma
    mov x7, #20
    mov x8, #320
    add x9, x20, #chroma
    stp x8, x9, [sp]
    mov x8, #24
    mov x9, #192
    stp x8, x9, [sp, #16]
    mov w8, #0xa5
    strb w8, [x6, #16]
    call 15 // ReadDecoderOutputImageNv12: four overflow arguments, two strides.
    success
    add x8, x20, #luma
    ldrb w9, [x8, #16]
    cmp w9, #0xa5
    b.ne fail
    mov w9, #16
1:
    ldp x10, x11, [x8]
    add x8, x8, #20
    mov x12, #0x2828
    movk x12, #0x2828, lsl #16
    movk x12, #0x2828, lsl #32
    movk x12, #0x2828, lsl #48
    cmp x10, x12 // Each row's left half is Y=40.
    b.ne fail
    mov x12, #0xb4b4
    movk x12, #0xb4b4, lsl #16
    movk x12, #0xb4b4, lsl #32
    movk x12, #0xb4b4, lsl #48
    cmp x11, x12 // Each row's right half is Y=180.
    b.ne fail
    subs w9, w9, #1
    b.ne 1b
    add x8, x20, #chroma
    mov w9, #8
    mov x12, #0xac5b
    movk x12, #0xac5b, lsl #16
    movk x12, #0xac5b, lsl #32
    movk x12, #0xac5b, lsl #48
2:
    ldp x10, x11, [x8], #24
    cmp x10, x12 // UV pairs are U=91, V=172.
    b.ne fail
    cmp x11, x12
    b.ne fail
    subs w9, w9, #1
    b.ne 2b

    add x0, x20, #event
    call 16 // DestroySyncPointEvent.
    mov x0, x20
    call 17 // FinalizeAv1Decoder.
    success
    add x0, x20, #pool
    call 18 // FinalizeDecoderImagePool.
    mov w0, wzr
    b done
fail:
    mov w0, w21
done:
    ldp x21, x30, [sp, #48]
    ldp x19, x20, [sp, #32]
    add sp, sp, #64
    ret
