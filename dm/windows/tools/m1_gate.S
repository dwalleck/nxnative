// Native Windows ARM64 gate. The rewrite block owns guest x29/x30.
// M1 runs on a native stack; M2 switches back to the native stack for C dispatch.
    .text
    .p2align 2

    .macro vectors op, r, s, offset
    \op q\r, q\s, [sp, #\offset]
    .endm

    .macro all_vectors op
    vectors \op, 0, 1, 256
    vectors \op, 2, 3, 288
    vectors \op, 4, 5, 320
    vectors \op, 6, 7, 352
    vectors \op, 8, 9, 384
    vectors \op, 10, 11, 416
    vectors \op, 12, 13, 448
    vectors \op, 14, 15, 480
    vectors \op, 16, 17, 512
    vectors \op, 18, 19, 544
    vectors \op, 20, 21, 576
    vectors \op, 22, 23, 608
    vectors \op, 24, 25, 640
    vectors \op, 26, 27, 672
    vectors \op, 28, 29, 704
    vectors \op, 30, 31, 736
    .endm

#ifdef DM_M2_HOST
    .globl m2_enter
    .seh_proc m2_enter
m2_enter:                    // (entry, x0, x1, guest stack top, &TLS native_sp)
    stp x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    str x19, [sp, #-16]!
    .seh_save_reg_x x19, 16
    .seh_endprologue
    mov x19, x4
    mov x16, x0
    mov x0, x1
    mov x1, x2
    str xzr, [sp, #8]          // Suspended guest SP, populated only while a gate is active.
    mov x9, sp
    str x9, [x19]
    mov sp, x3
    mov x29, xzr
    blr x16
    ldr x9, [x19]
    mov sp, x9
    str xzr, [x19]
    .seh_startepilogue
    ldr x19, [sp], #16
    .seh_save_reg_x x19, 16
    ldp x29, x30, [sp], #16
    .seh_save_fplr_x 16
    .seh_endepilogue
    ret
    .seh_endproc

    .macro native_stack
    ldr x30, [x18, #0x58]
    adrp x29, _tls_index
    ldr w29, [x29, :lo12:_tls_index]
    ldr x30, [x30, x29, lsl #3]
    adrp x29, m2_native_sp_offset
    ldr x29, [x29, :lo12:m2_native_sp_offset]
    ldr x30, [x30, x29]
    mov x29, sp
    mov sp, x30
    str x29, [sp, #8]
    .endm

    // A regular SDK import, unlike an SVC, can clobber volatile registers. The
    // wrapper leaves x0–x8 and the vector arguments alone on entry to the host.
    // The AV1 SDK imports with overflow arguments copy their AAPCS64 stack
    // slots below the native SP. The preference's guest x8 is a C out-pointer.
    .macro import_thunk name, stack=0, preference=0
    .globl m3_\name
m3_\name:
    stp x29, x30, [sp, #-16]!
    native_stack
    .if \stack
    sub sp, sp, #((\stack + 1) / 2 * 16)
    .set stack_slot, 0
    .rept \stack
    ldr x9, [x29, #(16 + 8 * stack_slot)]
    str x9, [sp, #(8 * stack_slot)]
    .set stack_slot, stack_slot + 1
    .endr
    .endif
    .if \preference
    mov x4, x8
    .endif
    bl \name
    .if \stack
    add sp, sp, #((\stack + 1) / 2 * 16)
    .endif
    str xzr, [sp, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .endm
#define M3_IMPORT(symbol, name) import_thunk name
#define M3_ALIAS(symbol, name)
#define M3_VIDEO_STACK(symbol, name, count) import_thunk name, count
#define M3_VIDEO_PREFERENCE(symbol, name) import_thunk name, 0, 1
#include "m3_imports.inc"
#undef M3_ALIAS
#undef M3_IMPORT
    import_thunk fixture_sampler_snapshot
    import_thunk fixture_guest_call
    import_thunk audio_worker

    // prepo::PlayReport::Send returns a nontrivial AsyncContext in x8.
    // No upload is queued: the SDK's empty context is already complete.
    .globl m3_prepo_offline_send
m3_prepo_offline_send:
    str xzr, [x8]
    ret

#include "m2_guest_call.inc"

    // Static call targets, not runtime-generated instructions. Unsupported
    // operations are reported on call, so bulk NVN lookups can finish.
    .p2align 3
    .globl m3_nvn_stubs
m3_nvn_stubs:
    .set nvn_index, 0
    .rept 1024
    mov w9, #nvn_index
    b m3_missing_nvn_gate
    .set nvn_index, nvn_index + 1
    .endr
    .globl m3_nvn_stubs_end
m3_nvn_stubs_end:
m3_missing_nvn_gate:
    stp x29, x30, [sp, #-16]!
    native_stack
    mov x4, x2
    mov x3, x1
    mov x2, x0
    ldr x1, [x29, #8]        // Guest return address before the native stack switch.
    mov w0, w9
    bl m3_missing_nvn
    brk #0

    .section .rdata,"dr"
    .globl m3_nvn_names
m3_nvn_names:
    .incbin "names.txt"
    .globl m3_nvn_names_end
m3_nvn_names_end:
    .text

    .globl m2_gate
m2_gate:
    stp x29, x30, [sp, #-16]!
    native_stack
    sub sp, sp, #800
#else
    .globl m1_gate
    .seh_proc m1_gate
m1_gate:
    stp x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    sub sp, sp, #800
    .seh_stackalloc 800
    .seh_endprologue
#endif
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp xzr, x19, [sp, #144]  // x18 is the host TEB, never a guest register here.
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    str x28, [sp, #224]
    all_vectors stp
    mrs x9, nzcv
    mrs x10, fpsr
    mrs x11, fpcr
    str x9, [sp, #768]
    str x10, [sp, #776]
    str x11, [sp, #784]
    // The smoke host uses the default FP environment, not the guest's rounding mode.
    msr fpsr, xzr
    msr fpcr, xzr
    mov x1, sp               // Mutable x0..x28 GPR array; x18 is not returned.
#ifdef DM_M2_HOST
    ldr x0, [x29, #8]        // Return address in the rewrite block, gates.tsv key.
    add x2, x29, #32         // Guest SP before the two x29/x30 saves.
    bl m2_dispatch
#else
    mov x0, x30
    bl m1_dispatch
#endif
    ldr x9, [sp, #768]
    ldr x10, [sp, #776]
    ldr x11, [sp, #784]
    msr nzcv, x9
    msr fpsr, x10
    msr fpcr, x11
    all_vectors ldp
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldr x19, [sp, #152]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldr x28, [sp, #224]
#ifdef DM_M2_HOST
    ldr x29, [sp, #808]
    str xzr, [sp, #808]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
#else
    .seh_startepilogue
    add sp, sp, #800
    .seh_stackalloc 800
    ldp x29, x30, [sp], #16
    .seh_save_fplr_x 16
    .seh_endepilogue
    ret
    .seh_endproc

    .globl m1_clobber_volatile
m1_clobber_volatile:
    mov x9, #0
    movi v0.16b, #0
    msr fpsr, xzr
    cmp x9, #1
    ret
#endif
