.text
    # Two read-only passes over every 32-byte block of the 1 MB data segment.
    # Consecutive blocks map to consecutive DRAM banks of the same row, so the
    # stream enjoys many row buffer hits. Each refresh closes the row buffer
    # of the bank it refreshes, which turns some of these hits into misses.
    lui   $s0, 0x1000        # data memory: 0x10000000
    addiu $t3, $0, 2         # number of passes
Pass:
    lui   $t0, 0x0010        # 1 MB
    addu  $t1, $s0, $t0      # end of the data segment
Loop:
    addiu $t1, $t1, -32      # next block
    lw    $t2, 0($t1)
    addu  $t4, $t4, $t2
    bne   $t1, $s0, Loop
    addiu $t3, $t3, -1
    bne   $t3, $0, Pass

    addiu $v0, $0, 10
    syscall
