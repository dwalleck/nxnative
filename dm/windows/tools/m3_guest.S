// Synthetic Horizon guest: the test populates the GOT at module +0x3800.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    .macro list_open expected
    add x0, x20, #80
    add x1, x20, #64
    mov w2, #1
    call 13
    cbnz w0, fail
    ldr w9, [x20, #80]
    cmp w9, #\expected
    b.ne fail
    .endm
    .macro nvn offset
    mov w0, #(0x2000 + \offset)
    bl lookup_nvn
    .endm
    .macro desc_init proc, offset, count, expect
    nvn \proc
    add x0, x20, #176
    add x1, x20, #128
    mov x2, #\offset
    mov w3, #\count
    blr x16
    cmp w0, #\expect
    b.ne fail
    .endm
    .macro register_sampler id
    nvn 0x880
    add x0, x20, #176
    mov w1, #\id
    add x2, x20, #288
    blr x16
    .endm
    .macro queue_proc offset
    mov w0, #(0x1000 + \offset)
    bl lookup_nvn
    add x0, x20, #400           // Queue builder in recycled descriptor backing storage
    .endm
    .macro queue_set offset, value
    queue_proc \offset
    mov x1, #\value
    blr x16
    .endm
    .macro queue_size offset, count, isset
    queue_proc \offset
    add x1, x20, #352
    mov w9, #-1
    str w9, [x1]
    blr x16
    cmp w0, #\isset
    b.ne fail
    ldr w9, [x20, #352]
    cmp w9, #\count
    b.ne fail
    .endm
    .macro read_sampler
    add x0, x20, #176
    mov w1, #3
    add x2, x20, #352
    call 10                     // Fixture-only readback of the logical slot
    .endm
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]
    str x21, [sp, #32]
    sub sp, sp, #832
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov w21, #20
    call 11                   // account::Initialize, without SM or control.nacp
    list_open 0
    mov x0, x20
    call 12                   // TryOpenPreselectedUser(&handle1)
    cmp w0, #1
    b.ne fail
    add x0, x20, #24
    call 12                   // TryOpenPreselectedUser(&handle2)
    cmp w0, #1
    b.ne fail
    ldr x9, [x20, #16]
    ldr x10, [x20, #40]
    cbz x9, fail
    cbz x10, fail
    cmp x9, x10
    b.eq fail
    call 11                   // Reinitializing does not reset live users.
    list_open 1
    add x0, x20, #48
    mov x1, x20
    call 14                   // GetUserId
    cbnz w0, fail
    ldp x9, x10, [x20, #48]
    ldp x11, x12, [x20, #64]  // Listed UID is the same launch user.
    cmp x9, x11
    b.ne fail
    cmp x10, x12
    b.ne fail
    movz x11, #1
    movk x11, #0x6c00, lsl #32
    movk x11, #0x7862, lsl #48
    cmp x9, x11
    b.ne fail
    movz x12, #0x5645
    movk x12, #0x5449, lsl #16
    movk x12, #0x4e41, lsl #32
    movk x12, #0x5357, lsl #48
    cmp x10, x12
    b.ne fail
    add x0, x20, #96
    add x1, x20, #48
    call 15                   // GetNickname -> UTF-8 Link-\xc3\xa9
    cbnz w0, fail
    ldr x9, [x20, #96]
    movz x10, #0x694c
    movk x10, #0x6b6e, lsl #16
    movk x10, #0xc32d, lsl #32
    movk x10, #0x00a9, lsl #48
    cmp x9, x10
    b.ne fail
    mov x0, x20
    call 17                   // Network account availability cancels offline.
    cmp w0, #0x27c
    b.ne fail
    mov x0, x20
    call 16                   // CloseUser(&handle1)
    mov x0, x20
    call 16                   // A stale close must not invalidate handle2.
    list_open 1
    add x0, x20, #24
    call 16
    list_open 0

    mov w21, #1
    call 0                   // GetCurrentFocusState()
    cmp w0, #1
    b.ne fail
    mov x0, x20
    mov w21, #2
    call 1                   // QueryMountRomCacheSize(&size)
    cbnz w0, fail
    ldr x9, [x20]
    cmp x9, #0x1000
    b.ne fail
    adrp x0, #0x2000
    add x0, x0, #0xa00
    mov w21, #3
    call 2                   // MountRom(name)
    cbnz w0, fail
    mov w0, #2
    call 18                  // SetPriorityOnCurrentThread (no fsp-srv session).
    add x0, x20, #8
    adrp x1, #0x2000
    add x1, x1, #0xa10
    mov w2, #1
    mov w21, #4
    call 3                   // OpenFile (also checks backslash normalization)
    cbnz w0, fail
    ldr x9, [x20, #8]
    cbz x9, fail
    mov x1, x9
    mov x0, x20
    mov w21, #5
    call 4                   // GetFileSize
    cbnz w0, fail
    ldr x9, [x20]
    cmp x9, #8
    b.ne fail
    mov x0, x20
    ldr x1, [x20, #8]
    mov x2, #4
    add x3, x20, #16
    mov x4, #8
    mov w21, #6
    call 5                   // ReadFile with byte count must stop at the entry boundary
    cbnz w0, fail
    ldr x9, [x20]
    cmp x9, #4
    b.ne fail
    ldr w9, [x20, #16]
    mov w10, #0x3635
    movk w10, #0x3837, lsl #16
    cmp w9, w10
    b.ne fail
    ldr x0, [x20, #8]
    mov x1, #4
    add x2, x20, #16
    mov x3, #8
    mov w21, #7
    call 6                   // Exact-read overload must fail for the same short read
    cbz w0, fail
    ldr x0, [x20, #8]
    call 7                   // CloseFile
    mov x0, x20
    ldr x1, [x20, #8]
    mov w21, #8
    call 4                   // Closed handle must not return the old file's size
    cbz w0, fail
    add x0, x20, #8
    adrp x1, #0x2000
    add x1, x1, #0xa30
    mov w2, #1
    mov w21, #9
    call 3
    cmp w0, #0x202
    b.ne fail
    adrp x0, #0x2000
    add x0, x0, #0xa00
    call 8                   // Unmount
    mov w21, #10
    adrp x0, #0x2000
    add x0, x0, #0xa50
    call 9                   // Known unsupported API must still resolve to a callable diagnostic.
    cbz x0, fail
    adrp x9, #0x2000
    ldr w9, [x9, #0xa80]
    cmp w9, #1
    b.ne query_version
    blr x0                   // Negative control: fail only when the procedure is called
    b fail
query_version:
    mov w21, #11
    nvn 0xac0                // nvnDeviceGetInteger(NULL, item, &out)
    mov x0, xzr
    mov w1, #0
    mov x2, x20
    blr x16
    ldr w9, [x20]
    cmp w9, #231
    b.ne fail
    nvn 0xac0
    mov x0, xzr
    mov w1, #1
    mov x2, x20
    blr x16
    ldr w9, [x20]
    cmp w9, #40
    b.ne fail
    nvn 0xac0
    mov x0, xzr
    mov w1, #2               // constant forwarded by the SDK's default switch
    adrp x9, #0x2000
    ldr w9, [x9, #0xa80]
    cmp w9, #2
    mov w10, #255
    csel w1, w10, w1, eq     // Negative control: unknown query
    cmp w9, #3
    csel w1, w9, w1, eq     // Negative control: device-dependent case
    mov x2, x20
    blr x16
    ldr w9, [x20]
    cmp w9, #256
    b.ne fail
    mov w21, #12
    nvn 0xb00                // nvnDeviceBuilderSetDefaults
    add x0, x20, #64
    blr x16
    nvn 0xb40                // nvnDeviceInitialize
    add x0, x20, #80
    add x1, x20, #64
    blr x16
    cbz w0, fail
    nvn 0xb80                // nvnMemoryPoolBuilderSetDevice
    add x0, x20, #96
    add x1, x20, #80
    blr x16
    nvn 0xbc0                // SetDefaults preserves the device
    add x0, x20, #96
    blr x16
    nvn 0xc00                // nvnMemoryPoolBuilderSetStorage
    add x0, x20, #96
    add x1, x20, #160
    mov x2, #48
    blr x16
    nvn 0xc40                // nvnMemoryPoolInitialize
    add x0, x20, #128
    add x1, x20, #96
    blr x16
    cbz w0, fail
    nvn 0xc80                // Default CPU_NO_ACCESS -> no CPU mapping
    add x0, x20, #128
    blr x16
    cbnz x0, fail
    nvn 0xcc0                // Guest buffer address is still available
    add x0, x20, #128
    blr x16
    add x9, x20, #160
    cmp x0, x9
    b.ne fail
    nvn 0xd00                // nvnMemoryPoolFinalize
    add x0, x20, #128
    blr x16
    nvn 0xd40                // nvnMemoryPoolBuilderSetFlags
    add x0, x20, #96
    mov w1, #0x24            // CPU_CACHED | GPU_CACHED
    blr x16
    nvn 0xc40
    add x0, x20, #128
    add x1, x20, #96
    blr x16
    cbz w0, fail
    nvn 0xc80                // CPU-accessible pool maps its original storage
    add x0, x20, #128
    blr x16
    add x9, x20, #160
    cmp x0, x9
    b.ne fail
    nvn 0xd00
    add x0, x20, #128
    blr x16

    mov w21, #13
    nvn 0xc00                // 12 descriptors in an aligned backing range
    add x0, x20, #96
    add x1, x20, #447
    and x1, x1, #-32
    mov x2, #384
    blr x16
    nvn 0xc40
    add x0, x20, #128
    add x1, x20, #96
    blr x16
    cbz w0, fail
    desc_init 0xd80, 256, 4, 1   // exact end of pool
    desc_init 0xd80, 288, 4, 0   // one descriptor past the end
    desc_init 0xd80, 260, 4, 0   // unaligned offset
    desc_init 0xd80, -32, 4, 0
    desc_init 0xd80, 0, -1, 0
    desc_init 0xd80, 0, 2, 0     // fewer entries than the reserved slots
    desc_init 0xd80, 0, 9, 0     // beyond the SDK limit but fits the backing range
    nvn 0xdc0                   // rejected init calls did not change the object
    add x0, x20, #176
    blr x16
    cmp w0, #4
    b.ne fail
    nvn 0xe00
    add x0, x20, #176
    blr x16
    cmp x0, #256
    b.ne fail
    nvn 0xe40
    add x0, x20, #176
    blr x16
    add x9, x20, #128
    cmp x0, x9
    b.ne fail
    nvn 0xe80
    add x0, x20, #176
    blr x16
    desc_init 0xec0, 352, 1, 1   // texture pool has a different reserved count
    nvn 0xf00
    add x0, x20, #176
    blr x16
    nvn 0xd00
    add x0, x20, #128
    blr x16
    desc_init 0xd80, 0, 4, 0     // finalized backing pool

    mov w21, #14
    nvn 0x700                   // SamplerBuilderSetDevice, then SetDefaults preserves it
    add x0, x20, #240
    add x1, x20, #80
    blr x16
    nvn 0x740
    add x0, x20, #240
    blr x16
    ldr w9, [x20, #252]         // Default max LOD is 1000.0
    mov w10, #0x447a0000
    cmp w9, w10
    b.ne fail
    ldr w9, [x20, #284]         // Default wraps, filters and comparison function
    mov w10, #0x0111
    movk w10, #0x0811, lsl #16
    cmp w9, w10
    b.ne fail
    nvn 0x780                   // ABI passes float arguments through the native-stack thunk
    add x0, x20, #240
    fmov s0, #1.0
    fmov s1, #4.0
    blr x16
    nvn 0x7c0                   // min/mag setter changes only its own field
    add x0, x20, #240
    mov w1, #5
    mov w2, #0
    blr x16
    nvn 0x800                   // Initialize copies parameters; does not retain the builder
    add x0, x20, #288
    add x1, x20, #240
    mov x9, #0xabc
    str x9, [x20, #336]         // Guard the public 48-byte sampler size
    blr x16
    cbz w0, fail
    nvn 0x740
    add x0, x20, #240
    blr x16
    ldr w9, [x20, #296]
    mov w10, #0x3f800000
    cmp w9, w10
    b.ne fail
    ldr w9, [x20, #300]
    mov w10, #0x40800000
    cmp w9, w10
    b.ne fail
    mov w0, #0x1a00             // SamplerGetMinMagFilter
    bl lookup_nvn
    add x0, x20, #288
    add x1, x20, #352
    add x2, x20, #356
    blr x16
    ldr w9, [x20, #352]
    ldr w10, [x20, #356]
    cmp w9, #5
    b.ne fail
    cbnz w10, fail
    nvn 0x840
    add x0, x20, #288
    blr x16
    ldr x9, [x20, #336]
    cmp x9, #0xabc
    b.ne fail

    mov w21, #15
    nvn 0xc40
    add x0, x20, #128
    add x1, x20, #96
    blr x16
    cbz w0, fail
    desc_init 0xd80, 256, 4, 1
    read_sampler
    cbnz w0, fail              // Unregistered slot
    nvn 0x800
    add x0, x20, #288
    add x1, x20, #240
    blr x16
    cbz w0, fail
    adrp x9, #0x2000
    ldr w9, [x9, #0xa80]
    cmp w9, #4
    b.ne register_limit
    register_sampler 2         // Reserved ID
    b fail
register_limit:
    cmp w9, #5
    b.ne register_overlap
    register_sampler 4         // One past the end
    b fail
register_overlap:
    cmp w9, #6
    b.ne register_ok
    nvn 0xd80
    add x0, x20, #16           // Different object, overlapping guest range
    add x1, x20, #128
    mov x2, #224
    mov w3, #4
    blr x16
    b fail
register_ok:
    add x9, x20, #447
    and x9, x9, #-32
    mov x10, #0x123
    stp x10, x10, [x9, #352]   // Backing bytes of slot 3 must remain untouched
    stp x10, x10, [x9, #368]
    register_sampler 3
    nvn 0x840
    add x0, x20, #288
    blr x16
    adrp x9, #0x2000
    ldr w9, [x9, #0xa80]
    cmp w9, #7
    b.ne sampler_snapshot
    register_sampler 3         // Finalized sampler cannot be registered again
    b fail
sampler_snapshot:
    read_sampler
    cbz w0, fail
    ldr w9, [x20, #356]
    mov w10, #0x447a0000       // Copy survives source sampler finalization
    cmp w9, w10
    b.ne fail
    nvn 0x780
    add x0, x20, #240
    fmov s0, #1.0
    fmov s1, #4.0
    blr x16
    nvn 0x800
    add x0, x20, #288
    add x1, x20, #240
    blr x16
    cbz w0, fail
    register_sampler 3
    read_sampler
    cbz w0, fail
    ldr w9, [x20, #356]
    mov w10, #0x40800000       // Replacement updates the same slot
    cmp w9, w10
    b.ne fail
    add x9, x20, #447
    and x9, x9, #-32
    ldp x10, x11, [x9, #352]
    ldp x12, x13, [x9, #368]
    cmp x10, #0x123
    b.ne fail
    cmp x10, x11
    b.ne fail
    cmp x10, x12
    b.ne fail
    cmp x10, x13
    b.ne fail
    nvn 0xe80
    add x0, x20, #176
    blr x16
    read_sampler
    cbnz w0, fail
    desc_init 0xd80, 256, 4, 1
    read_sampler
    cbnz w0, fail              // Same pool object and backing, new lifetime
    register_sampler 3
    nvn 0xd00
    add x0, x20, #128
    blr x16
    read_sampler
    cbnz w0, fail              // Backing finalization invalidates the descriptor table
    adrp x9, #0x2000
    ldr w9, [x9, #0xa80]
    cmp w9, #8
    b.ne ok
    register_sampler 3
    b fail
ok:
    mov w21, #16
    mov x9, #0xabc
    str x9, [x20, #464]        // Guard the public 64-byte builder size
    queue_proc 0x500           // SetDevice comes before SetDefaults
    add x1, x20, #80
    blr x16
    queue_proc 0x540           // Reset a fresh builder
    blr x16
    queue_size 0x600, 0, 0     // Compute size is unset, not encoded as an explicit zero
    queue_set 0x580, 0         // SetComputeMemorySize(0)
    queue_size 0x600, 0, 1     // Explicit zero requests no compute memory
    queue_set 0x5c0, 4096      // SetCommandMemorySize
    queue_size 0x640, 4096, 1
    queue_set 0x7c0, 4096      // SetControlMemorySize
    queue_size 0x800, 4096, 1
    queue_set 0x840, 8192      // SetRayTracingMemorySize
    queue_size 0x880, 8192, 1
    queue_set 0x8c0, 384       // SetCommandFlushThreshold
    queue_size 0x900, 384, 1
    queue_proc 0x940           // SetQueueMemory (records the request; does not use it)
    add x1, x20, #480
    mov x2, #256
    blr x16
    queue_proc 0x980
    blr x16
    add x9, x20, #480
    cmp x0, x9
    b.ne fail
    queue_proc 0x9c0
    blr x16
    cmp x0, #256
    b.ne fail
    queue_set 0x680, 200       // SetQueuePriority
    queue_proc 0x6c0
    blr x16
    cmp w0, #200
    b.ne fail
    queue_set 0x700, 3         // SetFlags
    queue_proc 0x740           // GetFlags
    blr x16
    cmp w0, #3
    b.ne fail
    queue_proc 0x540           // Reset all options, preserve only device
    blr x16
    queue_size 0x600, 0, 0
    queue_size 0x640, 0, 0
    queue_size 0x800, 0, 0
    queue_size 0x880, 0, 0
    queue_size 0x900, 0, 0
    queue_proc 0x980
    blr x16
    cbnz x0, fail
    queue_proc 0x9c0
    blr x16
    cbnz x0, fail
    queue_proc 0x6c0
    blr x16
    cmp w0, #100
    b.ne fail
    queue_proc 0x740
    blr x16
    cbnz w0, fail
    queue_proc 0x780
    blr x16
    add x9, x20, #80
    cmp x0, x9
    b.ne fail
    ldr x9, [x20, #464]
    cmp x9, #0xabc
    b.ne fail
    mov x0, #0
    b done
fail:
    mov w0, w21
done:
    add sp, sp, #832
    ldr x21, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret

// Keep repeated lookups small enough for the fixture's single text page.
lookup_nvn:
    stp x29, x30, [sp, #-16]!
    adrp x9, #0
    add x0, x9, x0
    call 9
    ldp x29, x30, [sp], #16
    cbz x0, fail
    mov x16, x0
    ret
