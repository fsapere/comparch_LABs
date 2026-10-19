.text
    # Stores one word into every 32-byte block of the 1 MB data segment,
    # walking downward. Every store misses in the L1 data cache and makes the
    # filled block dirty. Once the 64 KB L1 data cache is full, every fill
    # evicts a dirty block into the L2 cache; once the 256 KB L2 cache is
    # full, those insertions evict dirty blocks to DRAM.
    lui   $s0, 0x1000        # data memory: 0x10000000
    lui   $t0, 0x0010        # 1 MB
    addu  $t1, $s0, $t0      # end of the data segment
Loop:
    addiu $t1, $t1, -32      # next block (one store per block)
    sw    $t1, 0($t1)
    bne   $t1, $s0, Loop

    addiu $v0, $0, 10
    syscall
