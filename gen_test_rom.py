#!/usr/bin/env python3
"""Generate a minimal homebrew NES ROM (solid-color screen demo).
Not a commercial game -- used only to verify the partition-flashing
+ firmware-mmap ROM loading pipeline on ESP32-S31.
iNES header + 16KB PRG (NROM-128) + 8KB CHR (all zeros)."""
import struct, sys

# 6502 code at $8000 (PRG offset 0). Fills screen with palette[0]=red.
code = bytes([
    0x78,             # SEI
    0xD8,             # CLD
    0xA2, 0xFF,       # LDX #$FF
    0x9A,             # TXS
    # vwait1: wait for vblank (bit $2002 top bit set)
    0x2C, 0x02, 0x20, # BIT $2002
    0x10, 0xFB,       # BPL vwait1
    # vwait2: one more vblank to be safe
    0x2C, 0x02, 0x20, # BIT $2002
    0x10, 0xFB,       # BPL vwait2
    # set PPU address to palette $3F00
    0xA9, 0x3F,       # LDA #$3F
    0x8D, 0x06, 0x20, # STA $2006
    0xA9, 0x00,       # LDA #$00
    0x8D, 0x06, 0x20, # STA $2006
    # write backdrop color (red-ish) to $3F00
    0xA9, 0x20,       # LDA #$20
    0x8D, 0x07, 0x20, # STA $2007
    # enable background rendering
    0xA9, 0x1E,       # LDA #$1E
    0x8D, 0x01, 0x20, # STA $2001  (PPUMASK)
    0xA9, 0x00,       # LDA #$00
    0x8D, 0x00, 0x20, # STA $2000  (PPUCTRL, no NMI)
    # loop forever
    0x4C, 0x2B, 0x80, # JMP $802B
    # dummy NMI/IRQ handler
    0x40,             # RTI  (at $802E)
])

# Build 16KB PRG bank
prg = bytearray(0x4000)
prg[0:len(code)] = code
# vectors at PRG offset $3FFA..$3FFF (mirrors $FFFA..$FFFF)
# NMI -> $802E, RESET -> $8000, IRQ -> $802E
prg[0x3FFA] = 0x2E; prg[0x3FFB] = 0x80   # NMI
prg[0x3FFC] = 0x00; prg[0x3FFD] = 0x80   # RESET
prg[0x3FFE] = 0x2E; prg[0x3FFF] = 0x80   # IRQ

# 8KB CHR (all zeros -> pattern 0 = color0 everywhere)
chr_ = bytes(0x2000)

# iNES header: NES\x1a, 1 PRG (16KB), 1 CHR (8KB), mapper 0
header = b'NES\x1a' + bytes([0x01, 0x01, 0x00, 0x00]) + bytes(8)

rom = header + bytes(prg) + chr_
out = sys.argv[1] if len(sys.argv) > 1 else "test_demo.nes"
with open(out, "wb") as f:
    f.write(rom)
print("wrote %s (%d bytes)" % (out, len(rom)))
