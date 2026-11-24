// The fixture links the seven logical output device imports at +0x3800.
// The real guest SDK, not this fixture, exercises renderer generation and DSP.
    .text
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    sub sp, sp, #0x210
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x20, sp
    mov x21, #0x90
    strb w21, [sp, #0xff]
    mov x0, x20
    ldr x16, [x19]
    blr x16                       // GetActiveAudioOutputDeviceName(name)
    adr x8, expected_name
    mov x9, x20
1:
    ldrb w10, [x8], #1
    ldrb w11, [x9], #1
    cmp w10, w11
    b.ne fail
    cbnz w10, 1b
    ldrb w9, [sp, #0xff]
    cbnz w9, fail

    strb w21, [sp, #0x100]
    mov x0, x20
    mov w1, #0
    ldr x16, [x19, #8]
    blr x16                       // ListAudioOutputDeviceName(name, 0)
    cbnz w0, fail
    mov x0, x20
    mov w1, #2
    ldr x16, [x19, #8]
    blr x16                       // ListAudioOutputDeviceName(names, 2)
    cmp w0, #1
    b.ne fail
    ldrb w9, [sp, #0x100]
    cmp w9, w21
    b.ne fail
    ldr x16, [x19, #16]
    blr x16                       // GetActiveAudioDeviceChannelCountForOutput()
    cmp w0, #2
    b.ne fail

    mov x0, x20
    fmov s0, #0.5
    ldr x16, [x19, #24]
    blr x16                       // SetAudioDeviceOutputVolume(name, .5f)
    strb wzr, [x20]               // An empty SDK name selects the same default route.
    mov x0, x20
    ldr x16, [x19, #32]
    blr x16                       // GetAudioDeviceOutputVolume(name)
    fmov s1, #0.5
    fcmp s0, s1
    b.ne fail
    mov w0, #1
    ldr x16, [x19, #40]
    blr x16                       // SetDeviceOutputVolumeAutoTuneModeEnabled(true)
    ldr x16, [x19, #48]
    blr x16                       // IsDeviceOutputVolumeAutoTuneModeEnabled()
    cmp w0, #1
    b.ne fail
    mov w0, #0
    ldr x16, [x19, #40]
    blr x16
    ldr x16, [x19, #48]
    blr x16
    b done
fail:
    mov w0, #1
done:
    add sp, sp, #0x210
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
expected_name:
    .asciz "AudioBuiltInSpeakerOutput"
    .p2align 2
