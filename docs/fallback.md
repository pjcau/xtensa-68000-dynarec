# Fallback to the interpreter, and saves

Both engines are always in the app (the flash has no room for a second app): the
dynarec, and gpSP's interpreter (in flash, so it costs no internal RAM). A game
switches to the interpreter automatically, and never back unless the user asks.

## Self-modifying-code storms

gpSP drops and retranslates a RAM block whenever the game writes over its code.
NFS Underground rewrites the same IWRAM block continuously (~50 000
retranslations per frame, 0-2 fps). The dynarec counts RAM translations per frame
(`xt_ram_translations`); after 3 frames above 1000, or one above 20 000,
`xt_give_up` is set and the app runs `execute_arm` from the next frame (the switch
happens between frames, where both engines see the same state in `reg[]` and
CPSR). NFS Underground then runs at 13 fps, the interpreter's speed.

## Crashes and hangs (app side)

The retro-go app keeps a per-game `DynarecState` (`NS_FILE` settings):

| State | Meaning |
|---|---|
| OK | the dynarec ran this game fine (or it never ran) |
| TRYING | set at every launch on the dynarec; two minutes of play or a clean exit set OK |
| OFF | the interpreter, from the next launch on |

A launch that finds TRYING means the previous one crashed, hung or lost power on
the dynarec: the game goes to OFF and the user is told once. A self-modifying-code
storm is remembered the same way. The options menu has "Fast CPU (dynarec)":
Off at once, On from the next start (after a false alarm).

## Saves

- **Save states** hold the guest state (registers, memory, I/O), not translated
  code: a slot saved on the dynarec loads on the interpreter and vice versa.
- **Battery saves** (SRAM / flash / EEPROM, a 128 KB buffer) are written to
  `<saves>/gba/<rom>.gba.sram` one second after the game writes them, before the
  menu opens and on shutdown, and read at start (`gamepak_backup_dirty` flags
  every backup write).
