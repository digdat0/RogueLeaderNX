# Walks the game's MEM1 heap (an SDK OSAlloc heap) in a MEM1 dump and prints its two cell
# chains - the free list (in address order) and the allocated list - with their totals and the
# largest cells. Cells: +0 prev, +4 next, +8 size (header included), contiguous from the heap's
# start to its end.
#   R2_MEM_DUMP=<vblank>,00000000,1800000 -> build/mem_<vblank>_00000000.bin, then:
#   python tools/heapwalk.py build/mem_4200_00000000.bin [start hex] [end hex]
# Defaults: Rogue Leader's MEM1 heap, 0x80250000-0x817FD900 (the arena after GSYS VM init).
import struct
import sys


def main(argv):
    mem = open(argv[1], "rb").read()
    start = int(argv[2], 16) if len(argv) > 2 else 0x80250000
    end = int(argv[3], 16) if len(argv) > 3 else 0x817FD900

    def word(a):
        a &= 0x1FFFFFF
        return struct.unpack(">I", mem[a:a + 4])[0]

    cells = {}
    a = start
    while a < end:
        size = word(a + 8)
        if size < 0x20 or size & 0x1F or a + size > end:
            print(f"bad cell at {a:08X} (size {size:#x}): not a heap start, or a corrupted heap")
            return 1
        cells[a] = (word(a), word(a + 4), size)
        a += size
    for head in (c for c, (prev, _, _) in cells.items() if prev == 0):
        a, total, count, ascending, last, sizes = head, 0, 0, True, 0, []
        while a and a in cells:
            _, nxt, size = cells[a]
            total += size
            count += 1
            sizes.append((size, a))
            ascending &= a > last
            last, a = a, nxt
        sizes.sort(reverse=True)
        kind = "free" if ascending else "allocated"
        print(f"{kind} chain from {head:08X}: {count} cells, {total} bytes; largest: " +
              ", ".join(f"{s}@{c:08X}" for s, c in sizes[:6]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
