// The importing fixture exercises the real native socket thunks: a guest
// thread wakes poll with an EventFd; an IPv6 loopback datagram arrives in the
// same descriptor namespace. No SDK or external network is required.
    .text
    .macro call n
    ldr x16, [x19, #(8 * \n)]
    blr x16
    .endm
    .macro expect value
    mov w9, #\value
    cmp w0, w9
    b.ne fail
    .endm
    stp x19, x20, [sp, #-64]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x30, [sp, #48]
    adrp x19, #0x3000
    add x19, x19, #0x800

    mov w25, #1
    call 0                         // IsInitialized
    expect 0
    mov x1, #0x200000
    svc #1                         // SetHeapSize
    expect 0
    mov x20, x1
    add x8, x20, #0x1000
    mov x9, #0x1000
    str x8, [x20, #8]
    stp x9, x9, [x20, #16]
    mov x0, x20
    call 1                         // Initialize(Config)
    expect 0
    mov x0, x20
    call 1
    expect 0x41b
    call 0
    expect 1
    mov x0, xzr
    mov w1, #6
    call 2                         // EventFd(nonblocking | close-on-exec)
    tbnz w0, #31, fail
    mov w22, w0
    str w22, [x20, #0x150]
    add x1, x20, #0x140
    call 3                         // EventFdRead
    expect -1
    call 6                         // GetLastErrno
    expect 11                     // Horizon EAGAIN
    add x21, x20, #0x100
    mov w8, #1                     // POLLIN
    stp w22, w8, [x21]
    mov x0, x21
    mov w1, #1
    mov w2, #0
    call 5                         // Poll(empty, no wait)
    expect 0

    mov w25, #2
    mov x1, #14
    svc #0x29                      // GetInfo: stack alias region
    expect 0
    add x23, x1, #0x800, lsl #12
    mov x0, x23
    add x1, x20, #0x3000
    mov x2, #0x1000
    svc #4                         // MapMemory: guest worker stack
    expect 0
    adr x1, writer
    mov x2, x20
    add x3, x23, #0x1000
    mov w4, #42
    mov w5, #1
    svc #8                         // CreateThread
    expect 0
    str w1, [x20, #0x170]
    mov w0, w1
    svc #9                         // StartThread
    expect 0
    mov x0, x21
    mov w1, #1
    mov w2, #-1
    call 5                         // Poll wakes on the worker's write.
    expect 1
    ldrh w0, [x21, #6]
    expect 1
    mov w0, w22
    add x1, x20, #0x140
    call 3
    expect 0
    ldr x8, [x20, #0x140]
    cmp x8, #19
    b.ne fail
    add x1, x20, #0x170
    mov w2, #1
    mov x3, #-1
    svc #0x18                      // WaitSynchronization: worker exited
    expect 0
    ldr w0, [x20, #0x158]
    expect 0
    ldr w0, [x20, #0x170]
    svc #0x16                      // CloseHandle
    expect 0
    mov x0, x23
    add x1, x20, #0x3000
    mov x2, #0x1000
    svc #5                         // UnmapMemory
    expect 0

    mov w25, #3
    mov w0, #28                    // AF_INET6
    mov w1, #2                     // SOCK_DGRAM
    mov w2, #0
    call 7                         // Socket
    tbnz w0, #31, fail
    mov w23, w0
    add x1, x20, #0x180
    mov w8, #0x1c1c                // sin6_len and sin6_family, ephemeral port
    str w8, [x1]
    mov w8, #1
    strb w8, [x1, #23]             // ::1
    mov w2, #28
    call 8                         // Bind
    expect 0
    mov w8, #28
    str w8, [x20, #0x1a0]
    mov w0, w23
    add x1, x20, #0x180
    add x2, x20, #0x1a0
    call 9                         // GetSockName supplies the ephemeral port.
    expect 0
    ldrh w8, [x20, #0x182]
    cbz w8, fail
    mov w0, #28
    mov w1, #2
    mov w2, #0
    call 7
    tbnz w0, #31, fail
    mov w24, w0
    add x1, x20, #0x180
    mov w2, #28
    call 10                        // Connect to the local receiver.
    expect 0
    mov w8, #0x1234
    str w8, [x20, #0x140]
    mov w0, w24
    add x1, x20, #0x140
    mov x2, #4
    mov w3, #0
    call 11                        // Send
    expect 4
    mov w8, #1
    stp w22, w8, [x21]
    stp w23, w8, [x21, #8]
    mov x0, x21
    mov w1, #2
    mov w2, #0
    call 5
    expect 1
    ldrh w0, [x21, #6]
    expect 0
    ldrh w0, [x21, #14]
    expect 1
    mov w0, w23
    add x1, x20, #0x148
    mov x2, #4
    mov w3, #0x80                  // MSG_DONTWAIT
    call 12                        // Recv
    expect 4
    ldr w0, [x20, #0x148]
    expect 0x1234
    mov w8, #0x20
    strb w8, [x20, #0x188]         // This is no longer a loopback address.
    mov w0, w24
    add x1, x20, #0x180
    mov w2, #28
    call 10
    expect -1
    call 6
    expect 100                    // Horizon ENETDOWN
    mov w0, w24
    call 13                       // Close
    expect 0

    mov w25, #4
    call 14                       // Finalize closes both remaining descriptors.
    expect 0
    call 0
    expect 0
    call 14
    expect 0x41b
    mov x0, x20
    call 1
    expect 0
    mov x0, x21
    mov w1, #2
    mov w2, #0
    call 5
    expect 2
    ldrh w0, [x21, #6]
    expect 0x20                   // POLLNVAL, not a silently ignored socket.
    ldrh w0, [x21, #14]
    expect 0x20
    call 14
    expect 0
    mov w25, #0
fail:
    mov w0, w25
    ldp x25, x30, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #64
    ret

writer:
    mov x20, x0
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov w0, #0x2d00
    movk w0, #0x131, lsl #16       // 20 ms lets the main guest reach Poll.
    svc #0xb
    ldr w0, [x20, #0x150]
    mov x1, #19
    call 4                        // EventFdWrite
    str w0, [x20, #0x158]
    svc #0xa                      // ExitThread
