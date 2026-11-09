#!/bin/sh
L=/usr/lib/libretro; R=/root/roms
run() { # tag core rom frames
  for vd in vulkan glcore; do
    env -u VK_ICD_FILENAMES sh /root/mx.sh old_$1_$vd $L/$2 "$3" $vd $4
    VK_ICD_FILENAMES=/root/new_icd.json sh /root/mx.sh new_$1_$vd $L/$2 "$3" $vd $4
  done
}
{
run aw mgba_libretro.so "$R/gba/Advance Wars (Europe) (En,Fr,De,Es).gba" 900
run gs mgba_libretro.so "$R/gba/Golden Sun (USA, Europe).gba" 900
run mk mgba_libretro.so "$R/gba/Mario Kart - Super Circuit (Europe).gba" 900
run fz mgba_libretro.so "$R/gba/F-Zero - Maximum Velocity (USA, Europe).gba" 900
run mf mgba_libretro.so "$R/gba/Metroid Fusion (Europe) (En,Fr,De,Es,It).gba" 900
run kdl2 gambatte_libretro.so "$R/gb/Kirby's Dream Land 2 (USA, Europe) (SGB Enhanced).gb" 900
run ladx gambatte_libretro.so "$R/gbc/Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (SGB Enhanced) (GB Compatible).gbc" 900
run pkc gambatte_libretro.so "$R/gbc/Pokemon - Crystal Version (USA, Europe) (Rev 1).gbc" 900
run ka fceumm_libretro.so "$R/nes/Kirby's Adventure (Europe).nes" 900
run cv3 fceumm_libretro.so "$R/nes/Castlevania III - Dracula's Curse (Europe).nes" 900
run sh mednafen_psx_hw_libretro.so "$R/psx/Silent Hill (Europe) (En,Fr,De,Es,It).cue" 2400
run mrb mrboom_libretro.so "" 900
echo ALLDONE
} > /root/mx_summary.txt 2>&1
