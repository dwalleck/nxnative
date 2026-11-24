// Headless Npad fixture. The test populates the import GOT at +0x3800.
    .text
    .macro call i
    ldr x16, [x19, #8 * \i]
    blr x16
    .endm
    .macro no_states i, id_offset
    add x0, sp, #16
    mov w1, #1
    add x2, sp, #\id_offset
    call \i
    cbnz w0, fail
    ldr x9, [sp, #16]
    cmp x9, #0xace
    b.ne fail
    .endm
    stp x19, x30, [sp, #-16]!
    sub sp, sp, #64
    adrp x19, #0x3000
    add x19, x19, #0x800
    call 0                     // InitializeNpad
    mov w9, #0x20
    stp wzr, w9, [sp]         // Player 1 and Handheld are supported, not connected.
    mov x0, sp
    mov x1, #2
    call 1                     // SetSupportedNpadIdType
    mov w0, #0x1f
    call 2                     // SetSupportedNpadStyleSet
    call 0                     // Reinitialization keeps application configuration.
    mov w9, #7
    str w9, [sp, #8]
    add x0, sp, #8
    call 18                    // An unsupported, unassigned player slot.
    mov x0, sp
    call 18                    // A supported player slot without a device.
    call 3                     // GetSupportedNpadStyleSet
    cmp w0, #0x1f
    b.ne fail
    mov x0, sp
    call 4                     // GetNpadStyleSet: no active style for player 1.
    cbnz w0, fail
    add x0, sp, #4
    call 4                     // Handheld also has no source.
    cbnz w0, fail
    mov x9, #0xace
    str x9, [sp, #16]
    no_states 5, 0             // FullKey
    no_states 6, 4             // Handheld
    no_states 7, 0             // JoyDual
    no_states 8, 0             // JoyLeft
    no_states 9, 0             // JoyRight
    mov w9, #0x101
    strh w9, [sp, #8]         // ControllerSupportArg requires exactly one player.
    add x0, sp, #16
    add x1, sp, #8
    call 14                    // No eligible device: canceled, not successful.
    mov w9, #0x3aca
    movk w9, #0x18, lsl #16
    cmp w0, w9
    b.ne fail
    ldr x9, [sp, #16]
    cmp x9, #0xace              // Cancellation leaves result info untouched.
    b.ne fail
    mov w3, #1
    no_states 15, 0            // No six-axis handles for FullKey.
    mov w3, #1
    no_states 16, 0            // No sensor-fusion handles.
    mov w3, #1
    no_states 17, 0            // No vibration handles.
    call 10                    // InitializeMouse
    add x0, sp, #16
    mov w1, #1
    call 11                    // Empty mouse history leaves destination alone.
    cbnz w0, fail
    ldr x9, [sp, #16]
    cmp x9, #0xace
    b.ne fail
    call 12                    // InitializeKeyboard
    mov x9, #-1
    stp x9, x9, [sp, #8]
    stp x9, x9, [sp, #24]
    stp x9, x9, [sp, #40]
    str x9, [sp, #56]
    add x0, sp, #8
    call 13                    // Disconnected keyboard: all 0x30 bytes zero.
    mov w10, #8
1:  ldr x9, [sp, x10]
    cbnz x9, fail
    add w10, w10, #8
    cmp w10, #56
    b.lo 1b
    ldr x9, [sp, #56]
    cmn x9, #1
    b.ne fail

    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cbz w9, done
    mov w9, #0xff
    str w9, [sp]
    mov x0, sp
    mov x1, #2
    call 1                     // Negative control: invalid ID must not be accepted.
fail:
    mov w0, #1
    b return
done:
    mov w0, #0
return:
    add sp, sp, #64
    ldp x19, x30, [sp], #16
    ret
