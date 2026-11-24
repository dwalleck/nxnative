// SDK imports are the indexed GOT entries at +0x3800. Mode at +0x3c00:
// 0 commits, remounts, rolls back and leaves another transaction uncommitted;
// 1 verifies the committed save survived process exit; 2 requires no usable DM_SAVE.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    .macro ok index
    call \index
    cbnz w0, fail
    .endm
    .macro expect result
    movz w9, #(\result & 0xffff)
    movk w9, #(\result >> 16), lsl #16
    cmp w0, w9
    b.ne fail
    .endm
    .macro path name, index
    adr x0, \name
    ok \index
    .endm
    .macro unmount name
    adr x0, \name
    call 3                     // SDK Unmount returns void.
    .endm
    .macro mount_save
    adr x0, save_name
    adr x1, launch_uid
    ok 0
    .endm
    .macro open_file name, mode
    mov x0, x20
    adr x1, \name
    mov w2, #\mode
    ok 7
    ldr x22, [x20]
    cbz x22, fail
    tst x22, #15
    b.ne fail
    .endm
    .macro close_file
    mov x0, x22
    call 14
    .endm
    .macro file_size size
    add x0, x20, #8
    mov x1, x22
    ok 10
    ldr x9, [x20, #8]
    cmp x9, #\size
    b.ne fail
    .endm
    .macro create name, size
    adr x0, \name
    mov x1, #\size
    ok 5
    .endm
    .macro resize size
    mov x0, x22
    mov x1, #\size
    ok 11
    .endm
    .macro write_bytes offset, result=0
    mov x0, x22
    mov x1, #\offset
    adr x2, payload
    mov x3, #4
    add x4, x20, #16
    call 8
    expect \result
    .endm
    .macro open_dir name, mode
    mov x0, x20
    adr x1, \name
    mov w2, #\mode
    ok 15
    ldr x23, [x20]
    cbz x23, fail
    .endm
    .macro dir_count size
    add x0, x20, #8
    mov x1, x23
    ok 17
    ldr x9, [x20, #8]
    cmp x9, #\size
    b.ne fail
    .endm
    .macro next_dir size
    add x0, x20, #8
    add x1, x20, #0x40
    mov x2, x23
    mov x3, #1
    ok 16
    ldr x9, [x20, #8]
    cmp x9, #\size
    b.ne fail
    .endm
    .macro file_dir_entry size
    ldr w9, [x20, #0x40]
    movz w10, #0x6966
    movk w10, #0x656c, lsl #16
    cmp w9, w10                 // 'file'
    b.ne fail
    ldrb w9, [x20, #0x44]
    cbnz w9, fail
    ldrb w9, [x20, #0x344]       // offsetof(DirectoryEntry, type) = 0x304
    cmp w9, #1
    b.ne fail
    ldr x9, [x20, #0x348]        // offsetof(DirectoryEntry, file_size) = 0x308
    cmp x9, #\size
    b.ne fail
    .endm
    .macro close_dir
    mov x0, x23
    call 18
    .endm
    stp x19, x20, [sp, #-64]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    str x30, [sp, #48]
    sub sp, sp, #0x350
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    adrp x24, #0x3000
    ldr w24, [x24, #0xc00]
    mov w9, #1
    str w9, [x20, #16]           // WriteOption::Flush
    mov w21, #1
    cmp w24, #2
    b.ne valid_config
    adr x0, save_name
    adr x1, launch_uid
    call 0
    expect 0x7d402              // ResultTargetNotFound; never in-memory fallback
    b done

valid_config:
    adr x0, save_name
    stp xzr, xzr, [x20, #32]
    add x1, x20, #32
    call 0
    expect 0x7d402              // An unknown UID must not mount Player's save.
    adr x0, launch_uid
    ok 22                       // EnsureSaveData validates the same external store.
    mount_save
    cbnz w24, verify_committed

    mov w21, #2
    adr x0, save_name
    call 1
    expect 0x7802               // The save name cannot also name temporary storage.
    path rom_name, 2
    path temp_name, 1
    create temp_file, 4
    open_file temp_file, 3
    mov w21, #0x21
    write_bytes 0
    mov w21, #0x22
    mov x0, x22
    mov x1, #0
    add x2, x20, #24
    mov x3, #4
    ok 12
    mov w21, #0x23
    ldr w9, [x20, #24]
    adr x10, payload
    ldr w10, [x10]
    cmp w9, w10
    b.ne fail
    close_file
    unmount temp_name
    mov w21, #0x24
    open_dir rom_root, 2
    mov w21, #0x25
    dir_count 1
    next_dir 1
    file_dir_entry 8
    close_dir
    unmount rom_name

    mov w21, #3
    path save_dir, 4
    create save_file, 4
    adr x0, save_file
    mov x1, #4
    call 5
    expect 0x402                // ResultPathAlreadyExists
    adr x0, save_dir
    call 21
    expect 0x1002               // ResultDirectoryNotEmpty
    add x0, x20, #8
    adr x1, save_dir
    ok 6
    ldr w9, [x20, #8]
    cbnz w9, fail               // DirectoryEntryType::Directory = 0
    open_file save_file, 3
    file_size 4
    write_bytes 1, 0x307202    // ResultFileExtensionWithoutOpenModeAllowAppend
    resize 8
    write_bytes 2
    mov x0, x22
    ok 9
    file_size 8
    close_file

    mov w21, #4
    open_dir save_dir, 3
    add x0, x20, #8
    mov x1, x23
    call 10
    expect 0x2a2                // A live directory token must not be a file token.
    dir_count 1
    next_dir 1
    file_dir_entry 8
    dir_count 1                 // Count does not consume or depend on the cursor.
    next_dir 0
    close_dir
    path save_name, 19
    unmount save_name
    mount_save

verify_committed:
    mov w21, #5
    add x0, x20, #8
    adr x1, marker_file
    call 6
    expect 0x202                // ResultPathNotFound, including after rollback/crash.
    add x0, x20, #8
    adr x1, save_file
    ok 6
    ldr w9, [x20, #8]
    cmp w9, #1
    b.ne fail
    open_file save_file_backslash, 1
    file_size 8
    add x0, x20, #8
    mov x1, x22
    mov x2, #0
    add x3, x20, #24
    mov x4, #16
    ok 13
    ldr x9, [x20, #8]
    cmp x9, #8
    b.ne fail
    ldr x9, [x20, #24]
    movz x10, #0x4241, lsl #16
    movk x10, #0x4443, lsl #32    // 00 00 41 42 43 44 00 00
    cmp x9, x10
    b.ne fail
    mov x0, x22
    mov x1, #0
    add x2, x20, #24
    mov x3, #9
    call 12
    expect 0x177a02             // Exact short reads report ResultOutOfRange.
    close_file
    cmp w24, #1
    b.eq finish_verify
    cbnz w24, leave_pending

    mov w21, #6
    open_file save_file, 3
    resize 2
    file_size 2
    close_file
    path save_file, 20
    path save_dir, 21
    create marker_file, 8
    unmount save_name           // No Commit since the mutations: restores data and names.
    mount_save
    mov w24, #3
    b verify_committed

leave_pending:
    mov w21, #7
    create marker_file, 8
    open_file save_file, 3
    resize 1
    file_size 1
    mov x0, x22
    ok 9                        // Flush staged pages, not the guest transaction.
    close_file
    b done
finish_verify:
    unmount save_name

done:
    mov w0, #0
    b restore
fail:
    mov w0, w21
restore:
    add sp, sp, #0x350
    ldr x30, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #64
    ret

    .p2align 3
launch_uid:
    .quad 0x78626c0000000001, 0x53574e4154495645
payload:
    .ascii "ABCD"
save_name:
    .asciz "save"
save_dir:
    .asciz "save:/dir"
save_file:
    .asciz "save:/dir/file"
save_file_backslash:
    .asciz "save:\\dir\\file"
marker_file:
    .asciz "save:/only-pending"
rom_name:
    .asciz "content"
rom_root:
    .asciz "content:/"
temp_name:
    .asciz "tmp"
temp_file:
    .asciz "tmp:/file"
    .p2align 2
