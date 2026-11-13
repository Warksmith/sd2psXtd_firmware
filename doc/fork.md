# Fork features and branches

The maintained features are based on upstream tag **1.5.0**, commit `14cf707612e570ddb79c85b12da2fd42584bba7d`. `integration` combines their tips. Implementation changes belong on their owning topic and merge forward into integration.

| Branch | Change | Review base |
| --- | --- | --- |
| `ini-last-line` | Read the last SD INI line without a trailing newline | `1.5.0` |
| `folder-backend` | [PS2 folder cards](folder-cards.md), recovery and snapshots | `ini-last-line` |
| `game2folder-wildcards-default` | [Prefix mappings and opt-in default card](game2folder.md) | `ini-last-line` |
| `usb-startup-shortcut` | Button 1 enters temporary Mass; Button 2 retains bootloader recovery | `1.5.0` |
| `usb-transfer-batching` | 16 KiB MSC transfers and multi-sector SD operations | `1.5.0` |
| `usb-storage-status` | Waiting, connected and explicit-eject status on the OLED | `1.5.0` |
| `build-tooling` | Container helpers and current MCA host regression tests | `1.5.0` |

The INI prerequisite should be reviewed before its two dependent topics. USB topics are independent of each other. Shared documentation and test edits have merge resolutions on integration; no topic includes integration history. No upstream PR is implied by this layout.

## USB behavior

Follow [USB interfaces](usb.md). The upstream storage session owns the SD card exclusively, retains CDC, and returns to the saved console/USB profile after explicit host eject. Startup Mass is temporary and requires a mountable SD filesystem. The previous fork's raw recovery path and suspend-based disconnect heuristic are superseded by this upstream behavior.

The larger transfer buffer costs 15.5 KiB of additional static RAM. The status display acknowledges completed eject for one second; suspension or inactivity does not report a safe eject. Transfer-speed gains still need measurement on hardware.

## Port and validation

Folder cards use 1.5.0's abstract backends and updated card formatting. Transitions to another console mode, variant, transport or Mass mode wait for pending saves. A final flush after stopping core 1 catches late writes; a failed save retains SD ownership and retries. Direct MCA page operations reject cards while folder loading hides them.

Host regression entry points are `test/ini/run.sh`, `test/mapping/run.sh`, `test/folder/run.sh`, `tests/folder_lifecycle/run.sh`, `tests/usb_startup/run.sh`, `tests/msc/run.sh` and `tests/mca_current/run.sh`. The folder suite has 109 cases including interrupted writes and snapshot recovery. The current MCA suite targets the refactored backend; the older upstream MCA harness references removed modules. `tests/msc/test_gui_startup.py` exercises LVGL from an ARM ELF using Unicorn and pyelftools.

Build and simulation results do not establish behavior on a physical device. Hardware checks should cover USB-only startup and bootloader recovery, host eject/remount on supported operating systems, PS1/PS2/MCA operation, and saving folder cards across mode changes and SD errors. Use spare media for fault-injection checks. Build instructions are in [building.md](building.md).
