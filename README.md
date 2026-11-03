![sd2psXtd Logo](doc/Logo.png)
*Logo by Berion ❤️*

# sd2psXtd Firmware

sd2psXtd is an extended firmware for the popular *Multipurpose MemoryCard Emulator* sd2psx by developer @xyzz (see [here](https://github.com/sd2psx)). It combines cutting-edge extended functionality (like game ID switching, file system access, and dynamic mode selection) with the rock-solid performance of the original sd2psx firmware.

### [!!] **Quick Note: This fork introduces 2 new features**
- **PS2:** Folder-Backed Memory Cards **(experimental)** (works with "GameID" and "In Game Reset")
- **General:** USB mass storage mode if Button1 is held while a USB cable is plugged in.

## PS2: Folder-Backed Memory Cards (experimental)

<img width="383" height="452" alt="image" src="https://github.com/user-attachments/assets/31e6a75b-fc50-46e2-8375-3c577bee93d1" />


On devices with PSRAM (PSxMemCard Gen2), an 8 MB PS2 card can keep its saves as ordinary files in a folder instead of a `.mcd` image. It works with launchers that support GameID and In Game Reset (IGR), including switching back to the FMCB card when IGR is initiated.

The console sees a normal formatted card, and changes are written back to the files in the background.

#### Settings to enable folder mode

**Per memory card:** `F:\MemoryCards\PS2\SLUS-12345\SLUS-12345.ini`
```
[Settings]
StorageBackend=FOLDER

```
If you cant get a game to work, or have trouble booting FMCB in folder mode, you can also add `FolderSnapshot=ON` to keep an 8 MB copy on the SD card. When the console requests a card read it'll serve up the snapshot first, then continue using the filesystem on the backend.

**Global:** Set the following within `F:\.sd2psx/settings.ini` to create new cards as folder cards, including those created automatically via GameID.
```
[PS2]
NewCardBackend=FOLDER

```

### PS2: GameID feature, Defaults and Wildcards

Within `.sd2psx/Game2Folder.ini` you can define the use of **Default** cards and/or **Wildcard** matches.

If you want to define only a few GameID groups, but have all other games just use a single card, you can set `UseDefaultCard=ON` and define a card with `DefaultCard=Shared`. (if you dont define `UseDefaultCard` it defaults to OFF)

```ini
[PS2]
UseDefaultCard=ON
DefaultCard=Shared

```

If you want to define broad wildcards, such as "all Japanese games", you can use **wildcards** with the `*` character. (Exact IDs take priority over wildcards, regardless of order. Longest matching prefix wins. For equally specific matches, the last entry wins.)

```ini
[PS2]
SLUS-*=USGames
SLUS-123**=Series123
SLUS-12345=Dedicated

```

These features have not been *extensively tested* on hardware yet, and saves that have not reached the SD card are lost in a power cut. See [doc/folder-cards.md](doc/folder-cards.md) before using it and always back up your saves.

## General: USB Storage Mode

<img width="611" height="517" alt="image" src="https://github.com/user-attachments/assets/c20cf346-c95e-4d40-85de-db28d60d0ad7" />

Hold a button while connecting the device to a computer over USB to choose how it starts:

| Held while connecting | Result |
| --- | --- |
| Nothing | Normal startup |
| Button 1 | The microSD card appears on the computer as a removable drive |
| Button 2 | Firmware update mode. If both buttons are held, this one wins |

(Previously the firmware update mode would start from either button 1 or 2. The dedicated BOOTSEL button on the board still works as before.)

USB storage mode gives the computer the whole card, while plugged in via USB the memory card emulation feature does not run. 

**Connect only USB (Do not connect to the PS2 at the same time). Eject the drive on the computer before unplugging so that the computer finishes its own cached writes. The display shows `Safe to disconnect` a few seconds after the computer has released it.**

USB on the RP2040 is limited to 12 Mbit/s, so expect under 1 MB/s. It's meant for saves and settings. If you're transferring ISOs or other large files use a microSD card reader. That said, if its slow to mount you can try formatting with 64K clusters and it should mount faster.

# sd2psXtd features, continued

sd2psXtd is an extended firmware for the popular *Multipurpose MemoryCard Emulator* sd2psx by developer @xyzz (see [here](https://github.com/sd2psx)). It combines cutting-edge extended functionality (like game ID switching, file system access, and dynamic mode selection) with the rock-solid performance of the original sd2psx firmware.

It provides the same functionality as the official stable firmware and extends it with the following features:

- **PS2:** Game ID switching
- **PS2:** PS1 dynamic mode selection
- **PS2:** MMCEMAN and MMCEDRV support
- **PS2:** Instant card availability
- **PS2:** 1-128 MB card size support
- **PS2:** Support for developer (`DTL-H` & `DTL-T`), Arcade (`COH-H`) and Prototype (`EB`?) models is available.
- **PS1:** BootCard mechanics
- **PS1:** PSRAM support
- **PS1:** Card Switch Controller Combo Support
- **PS1:** Super fast FreePSXBoot
- **PS1:** Net Yaroze Support
- **General:** Settings file
- **General:** Support for other RP2040-based MMCE devices
- **General:** Per Card Config
- **General:** Game2Folder mapping
- **General:** Splash Screen
- **General:** Game Image Screen
- **General:** USB-CDC command interface

## PS2: Game ID Switching

Like on PS1, *sd2psXtd* can detect the game ID of a PS2 console and switch to a dedicated card per game. Game ID switching can be turned off in the device settings for PS2.

This is done in two ways:

### History File Tracking

When starting a game, the PS2 writes its game ID to a history file on the current memory card. *sd2psXtd* tracks the write to this file and detects which game ID has just been written. After that, a game card for this game is mounted and exposed to the PS2.

### MMCEMAN Game ID

*MMCEMAN* is a custom IOP module to communicate with Multipurpose Memory Card Emulators. This can be integrated with OPL so OPL can directly send the game ID of a launched game to *sd2psx*.

## PS2: PS1 Dynamic Mode Selection

When launching in PS2 mode, commands sent to *sd2psx* are monitored. Since PS1 sends controller messages on the same bus as memory card messages, if a controller message is detected, the PS2 switches to PS1 mode.

While in general this should be safe behavior, if *sd2psx* is used mainly in PS1, manual mode selection is recommended.

> [!CAUTION]
> **Note 1:** If *sd2psx* is connected to a PS1 in PS2 mode, there is always a risk of damaging your PS1 console. You have been warned!

> [!CAUTION]
> **Note 2:** Do not use *sd2psx* in dynamic mode on a PS1 multitap, as this **WILL** damage your PS1 multitap device.

## PS2: MMCEMAN and MMCEDRV Support

*MMCEMAN* is a PS2 module for interacting with *Multipurpose Memory Card Emulators*. Its main use cases include:

- **Card Switching:** MMCEMMAN can request a card change on *MMCEs*, such as setting a channel or selecting a specific card.
- **Game ID Communication:** MMCEMAN can send a game ID to the *MMCE*, which may in turn switch to a dedicated card for this ID if activated.
- **File System Access:** MMCEMAN allows access to *MMCEs* filesystem through standard POSIX file I/O calls
- **Game loading:** MMCEDRV allows for loading games off of *MMCEs* with performance equal to, or in most cases, better than MX4SIO.

## PS2: Instant Card Availability

If using 8MB cards, *sd2psXtd* firmware exposes the card to the PS2 while it is still being transferred to PSRAM. This enables using FMCB/PS2BBL at boot time without additional waiting scripts.
Very helpful for PlayStation 2 models with simpler OSDSYS programs, that result on faster boot times (like PSX DESR and Arcade PS2)

## PS2: 1-128 MB Card Size Support

Support for card sizes between 1 and 128 MB has been added. Cards larger than 8 MB rely heavily on quick SD card access, so on older or lower-quality SD cards, these larger cards may become corrupt.

> [!NOTE]
>  While the feature has been extensively tested, it is still recommended to use 8MB cards, as this is the official specification for memory cards.

## PS2: Support for Developer, Arcade and Prototype PS2s

PS2 memory cards have been used in variations of PS2 like: *DevKits*, *TestKits*, *Arcades* and *Prototypes*.

*sd2psXtd* firmware supports these devices by configuring the variant within the PS2 settings.

These PlayStation 2 variations use different magicgate keysets to ensure their memory cards are inaccessible in other devices (e.g., opening a developer memory card on a normal PS2). This is why SD2PSX must actively support them.

> [!NOTE]
> **Devkit/DTL-H owners**:
> as you may notice, SD2PSX has no `DEVELOPER` mode, this is because sd2psxtd is mimicking the behavior of licensed retail card. To use the device on developer hardware, set the card to `RETAIL` mode [^1]

[^1]: Devkits: official retail memory cards use developer magicgate by default until the console actively requests to use retail magicgate with a dedicated command

## PS1: BootCard Mechanics

If BootCard functionality is activated, the PS1 starts with BootCards at startup. If BootCard is not activated, the card index and channel from the previous session are restored automatically.

## PS1: PSRAM Support

*sd2psXtd* firmware allows PS1 cards to be served from PSRAM. While this is mainly an under-the-hood change, it provides more flexibility in RAM usage.

## PS1: Card Switch Controller Combo Support


Controller Button Mapping for Card and Channel Switching

The following button combinations are used to perform card and channel switches:

- L1 + R1 + L2 + R2 + Up: Switch to the Next Card
- L1 + R1 + L2 + R2 + Down: Switch to the Previous Card
- L1 + R1 + L2 + R2 + Right: Switch to the Next Channel
- L1 + R1 + L2 + R2 + Left: Switch to the Previous Channel
- L1 + R1 + L2 + R2 + SELECT: Switch to Boot Card

These mappings require that all four buttons (L1, R1, L2, R2) are held down in combination with one of the directional inputs.

The Controller Combos can be activated by a setting in the PS1 Settings menu.

## PS1: Super fast FreePSXBoot

*sd2psXtd* allows super fast booting of FreePSXBoot by using some non standard card communication.
Please note: This is only possible using a special FreePSXBoot Version provided at https://sd2psXtd.github.io

## PS1: Net Yaroze Support

*sd2psXtd* will act as a Net Yaroze Access Card, if used with the Net Yaroze Software.

## General: USB-CDC Command Interface

When the firmware is running, the device exposes a USB-CDC serial interface that accepts simple text commands. This can be used to change cards, channels, modes, PS2 variants, or to reboot the device without using the on-device controls.

Commands are lowercase and submitted with Enter:

```text
help
reset bl
reset dev
channel up
channel down
set channel <idx>
card up
card down
set card <idx> [channel <idx>]
set game <id> [channel <idx>]
set mode <ps1|ps2>
set variant <retail|proto|conquest|arcade>
```

`reset bl` resets to the bootloader. `reset dev` restarts the firmware. Card, channel, and game commands affect the currently active mode. Setting a PS2 variant also switches the device to PS2 mode.

## General: Settings File

*sd2psXtd* generates a settings file (`.sd2psx/settings.ini`) that allows you to edit some settings through your computer. This is useful when using one SD card with multiple *sd2psx* devices or *MMCE* devices without a display to change settings.

A settings file has the following format:

```ini
[General]
Mode=PS2
FlippedScreen=OFF
[PS1]
Autoboot=ON
GameID=ON
EnableControllerCombo=ON
MaxCardIdx=0
[PS2]
Autoboot=ON
GameID=ON
CardSize=16
Variant=RETAIL
MaxCardIdx=0
```

Possible values are:

| Setting       | Values                                |
|---------------|---------------------------------------|
| Mode          | `PS1`, `PS2`                          |
| AutoBoot      | `OFF`, `ON`                           |
| GameID        | `OFF`, `ON`                           |
| CardSize      | `1`, `2`, `4`, `8`, `16`, `32`, `64`, `128` |
| Variant       | `RETAIL`, `PROTO`, `ARCADE`, `CONQUEST`     |
| FlippedScreen | `ON`, `OFF`                           |
| EnableControllerCombo | `ON`, `OFF`                   |
| MaxCardIdx    | UINT8                                 |

*Note: Make sure there is an empty line at the end of the ini file.*

## General: Support for Other RP2040-Based MMCE Devices

Support for different MMCE devices that share the same MCU has been added:

- **PicoMemcard+/PicoMemcardZero:** DIY devices by dangiu (see [here](https://github.com/dangiu/PicoMemcard?tab=readme-ov-file#picomemcard-using-memory-card)) without PSRAM. Use *PMC+* or *PMCZero* firmware variant.
- **PSXMemCard:** A commercial device by BitFunX sharing the same architecture as *PMC+*. Use *psxmemcard* firmware variant.
- **PSXMemCard Gen2:** A commercial device by BitFunX, sharing the same architecture as *sd2psx*. Use *sd2psx* firmware variant.

For each device, follow the flashing instructions provided by the creator, using the corresponding *sd2psXtd* firmware file.

*New in 1.2*:
PMC+ and PMCZero now support using the onboard buttons. They are assigned in the following way (according to the markings ons their board):

- **Button 1**: Load BootCard
- **Button 2**:
    - Short Press: Previous Channel
    - Long Press: Previous Card
- **Button 3**:
    - Short Press: Next Channel
    - Long Press: Next Card

## General: Per Card Config

There are some configuration values that can be modified on a per card base within a config file named  `CardX.ini` in a card folder, where `X` is the card index.

*Note 1: The `CardSize` setting is only used for PS2 cards and can only be either of `1`, `2`, `4`, `8`, `16`, `32`, `64`.*
*Note 2: The BOOT folder should contain a file named `BootCard.ini`*
*Note 3: Make sure there is an empty line at the end of the ini file.*

```ini
[ChannelName]
1=Channel 1 Name
2=Channel 2 Name
3=Channel 3 Name
4=Channel 4 Name
5=Channel 5 Name
6=Channel 6 Name
7=Channel 7 Name
8=Channel 8 Name
[Settings]
MaxChannels=8
CardSize=8
```


## General: Game2Folder mapping

There are some games, that share save data for multiple game ids (like the Singstar series etc). For these cases, a custom game to folder mapping can be created.

If a game with a mapped id is loaded, instead of using the game id based folder, the mapped folder is used for storing the card.

The mapping needs to be defined in ```.sd2psx/Game2Folder.ini``` in the following way:

```ini
[PS1]
SCXS-12345=FolderName1
[PS2]
SCXS-23456=FolderName2
```

*Note: Be aware: Long folder names may not be displayed correctly and may result in stuttering of MMCE games due to scrolling.*
Trailing `*` wildcards match any suffix (including an empty suffix). For example,
`SLUS-*` matches every ID starting with `SLUS-`, and `SLUS-123**` is equivalent to
`SLUS-123*`. Only trailing stars are supported; `?` and patterns such as `SLUS-*45`
are not supported. Matching is case-sensitive and uses the database's parent Game ID,
as existing exact mappings do.

Exact IDs take priority over wildcards, regardless of line order. Otherwise the
longest matching prefix wins. For equally specific matches, the last entry wins.
An empty mapping keeps the usual Game ID folder and suppresses the default fallback.

To use a shared card when no mapping matches, set both `UseDefaultCard=ON` and
`DefaultCard` in the relevant console section. This fallback is **off by default**;
`DefaultCard` alone does not enable it. `OFF` or an unrecognized value disables it.

```ini
[PS2]
UseDefaultCard=ON
DefaultCard=Shared
SLUS-*=USGames
SLUS-123**=Series123
SLUS-12345=Dedicated
```

Here `SLUS-12345` uses `Dedicated`, `SLUS-12399` uses `Series123`, other `SLUS-`
IDs use `USGames`, and unmatched IDs use `Shared`. These are card folder names,
not image filenames; for example, channel 1 of an image card uses
`MemoryCards/PS2/Shared/Shared-1.mcd`. Existing per-card settings and storage backends
still apply. Use short folder names (at most 31 characters).

The settings are independent in `[PS1]`, `[PS2]`, `[PROT]`, `[COH]`, and `[CONQUEST]`.
They apply when Game ID card selection runs; they do not enable Game ID switching
or replace the boot card. Without a match or an enabled, nonempty default, the usual
Game ID folder is used. A bare `*` explicitly matches all IDs, independently of the
default option.

Wildcard matching uses a single prefix comparison per entry, with no heap allocation,
regex engine, or backtracking. It uses the existing single INI scan during card selection
and adds no work to memory-card reads or writes.

## General: Splash Screen (1.3)

By default, sd2psXtd comes with a special splash screen resembling the project's logo.

You can customize this by going to the splashgen page on the project website!

From there, you can customize a splash screen to your needs. Once you are happy with the result displayed in the preview, press the **Download UF2** button. This will generate a UF2 file containing your splash screen.

You can flash this splash screen just like any other firmware update.

Alternatively, place a generated `splash.bin` in the root of the SD card. On startup, the firmware imports it into flash and removes `splash.bin` from the SD card.

The flashed splash screen is maintained after a firmware update, so you probably only need to upload it once.

*Note: When combining the splash screen with a firmware UF2 (such as for mass production or flashing multiple sd2psXtd with the same splash and firmware combination), it's strongly recommended to flash the combined image with `picotool`, since uploading using the usual firmware update procedure often does not work. To do so, please install `picotool` and run:*

```sh
picotool load <combined_file_name>.uf2
```

*while having the sd2psx to be updated connected in bootloader mode.*

## General: Game Image Screen (1.3)

Add a splash image that the device shows in the card main view. Use the splashgen page on the project website to convert a source image to the device `.bin` format, then place the generated file in the card folder on your SD card.

Naming and behavior:

- Folder-level splash (default for the folder):
    - Path: `MemoryCards/<Variant>/<card_folder>/<card_folder>.bin`
    - Example: `MemoryCards/<Variant>/SuperGame/SuperGame.bin` — used when no channel-specific image exists.
- Channel-specific splash (overrides folder-level for that channel):
    - Path: `MemoryCards/<Variant>/<card_folder>/<card_folder>-<channel_number>.bin`
    - Example: `MemoryCards/<Variant>/SuperGame/SuperGame-1.bin` — shown only for channel 1 of that card folder.
- Fallback rules:
    - If a channel-specific file exists it is used.
    - Otherwise the folder-level `<card_folder>.bin` is used.
    - If neither exists, no splash is shown.

Practical notes
- `<channel_number>` matches the on-device channel index (1..N).
- Filenames must match exactly; FAT SD cards are usually case-insensitive but keep names consistent.
- Use `misc/splashgen/splashgen.html` to produce correctly-sized and packed `.bin` files for the OLED.
- Keep names short — very long filenames may cause display or performance issues.
- Store splash `.bin` files alongside that card's `CardX.ini` and save data in the same folder.


## Special Thanks to...

- **@xyz**: for sd2psx ❤️
- **sd2psXtd Team**: (you know who you are 😉 )
- **@El_isra**: for so much different stuff ❤️
- **@Berion**: Our new beautiful logo ❤️
- **8BitMods Team**: for helping out with card formatting and providing lots of other useful information ❤️
- **@Mena / PhenomMods**: for providing hardware to some team members ❤️
- **BitFunX**: for providing PSXMemcard and PSXMemcard Gen2 Hardware for dev ❤️
- **All Testers**: ripto, Vapor, seewood, john3d, rippenbiest, ... ❤️
- **@niemasd**: For his [*GameDB-PSX*](https://github.com/niemasd/GameDB-PSX) database - Game naming and ID on PS1 would not be possible without it! ❤️
