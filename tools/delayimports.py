"""Print the delay-load imports of a PE32+ file as DLL<TAB>name lines.

usage: python3 delayimports.py claude.exe > delay.tsv
"""
import struct
import sys


def main(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        sys.exit("not PE32+")
    dirs = opt + 112
    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, sec + i * 40 + 8)
        sections.append((va, max(vsize, rsize), rptr))

    def off(rva):
        for va, size, rptr in sections:
            if va <= rva < va + size:
                return rva - va + rptr
        raise ValueError(hex(rva))

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\0", o)].decode()

    delay_rva, _ = struct.unpack_from("<II", data, dirs + 13 * 8)
    o = off(delay_rva)
    while True:
        _, name, _, _, int_rva, _, _, _ = struct.unpack_from("<8I", data, o)
        if name == 0:
            break
        dll = cstr(name)
        t = off(int_rva)
        while True:
            thunk = struct.unpack_from("<Q", data, t)[0]
            if thunk == 0:
                break
            if thunk & (1 << 63):
                print(f"{dll}\t#{thunk & 0xFFFF}")
            else:
                print(f"{dll}\t{cstr(thunk + 2)}")
            t += 8
        o += 32


if __name__ == "__main__":
    main(sys.argv[1])
