import os
f = r"G:\Family\Education\nes\English_Word_Blaster.nes"
with open(f, "rb") as fh:
    b = fh.read(16)
print("header:", " ".join("%02X" % x for x in b))
print("magic:", b[0:4])
prg = b[4]
chr_ = b[5]
flags6 = b[6]
flags7 = b[7]
print("PRG banks(16KB):", prg, "->", prg * 16384, "bytes")
print("CHR banks(8KB):", chr_, "->", chr_ * 8192, "bytes")
print("flags6=0x%02X trainer=%d mapper_low=%d" % (flags6, (flags6 >> 2) & 1, flags6 >> 4))
print("flags7=0x%02X mapper_high=%d" % (flags7, flags7 >> 4))
sz = os.path.getsize(f)
print("file size:", sz, "= header16 + data", sz - 16)
print("mapper:", ((flags7 >> 4) << 4) | (flags6 >> 4))
