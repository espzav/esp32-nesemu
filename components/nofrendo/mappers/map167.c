/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
** Subor / 小霸王 learning-computer mapper (NES mapper 167).
** Ported from VirtuaNES Mapper167.cpp (class Mapper167).
**
** The Subor cartridge has:
**   - up to 64x 16 KiB PRG banks (Bank 0x00..0x3F)
**   - 8 KiB of CHR RAM (no CHR ROM) -> handled by nofrendo's default
**     vram mapping when vrom_banks == 0, so the PPU side is a no-op here.
**
** Four write registers are decoded by address bits A13/A14:
**   $8000-$9FFF -> reg0, $A000-$BFFF -> reg1,
**   $C000-$DFFF -> reg2, $E000-$FFFF -> reg3.
** PRG banking is then computed from (reg0^reg1) and (reg2^reg3).
**
** rom_type: 0 = Chinese Subor (fixed upper bank 0x20),
**            1 = Russian Subor (fixed upper bank 0x07).
** English Word Blaster is the Chinese educational variant.
*/

#include <noftypes.h>
#include <nes_mmc.h>

static uint8 regs[4];
static int rom_type;   /* 0 = Chinese, 1 = Russian */

/* VirtuaNES SetPROM_16K_Bank(4, b) == CPU $8000-$BFFF
** VirtuaNES SetPROM_16K_Bank(6, b) == CPU $C000-$FFFF */
static void map167_setbank_cpu(void)
{
   int base, bank;

   base = ((regs[0] ^ regs[1]) & 0x10) << 1;
   bank = (regs[2] ^ regs[3]) & 0x1f;

   if (regs[1] & 0x08)
   {
      /* 32 KiB mode: both 16 KiB halves switchable */
      bank &= 0xfe;
      if (rom_type == 0)
      {
         mmc_bankrom(16, 0x8000, base + bank + 1);
         mmc_bankrom(16, 0xC000, base + bank + 0);
      }
      else
      {
         mmc_bankrom(16, 0xC000, base + bank + 1);
         mmc_bankrom(16, 0x8000, base + bank + 0);
      }
   }
   else
   {
      if (regs[1] & 0x04)
      {
         /* upper fixed to 0x1f, lower switchable */
         mmc_bankrom(16, 0x8000, 0x1f);
         mmc_bankrom(16, 0xC000, base + bank);
      }
      else
      {
         /* lower switchable, upper fixed (region dependent) */
         mmc_bankrom(16, 0x8000, base + bank);
         if (rom_type == 0)
            mmc_bankrom(16, 0xC000, 0x20);
         else
            mmc_bankrom(16, 0xC000, 0x07);
      }
   }
}

static void map167_write(uint32 address, uint8 value)
{
   int idx = (address >> 13) & 0x03;

   regs[idx] = value;
   map167_setbank_cpu();
}

static void map167_init(void)
{
   regs[0] = 0;
   regs[1] = 0;
   regs[2] = 0;
   regs[3] = 0;

   /* Chinese Subor variant (English Word Blaster etc.).
   ** Flip to 1 if a Russian-region ROM fails to boot. */
   rom_type = 0;

   map167_setbank_cpu();
}

static map_memwrite map167_memwrite[] =
{
   { 0x8000, 0xFFFF, map167_write },
   {     -1,     -1, NULL }
};

mapintf_t map167_intf =
{
   167,              /* mapper number */
   "Subor Computer", /* mapper name */
   map167_init,      /* init routine */
   NULL,             /* vblank callback */
   NULL,             /* hblank callback */
   NULL,             /* get state (snss) */
   NULL,             /* set state (snss) */
   NULL,             /* memory read structure */
   map167_memwrite,  /* memory write structure */
   NULL              /* external sound device */
};
