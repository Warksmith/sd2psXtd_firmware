"""Check USB session setup in an actual ARM ELF (Unicorn + pyelftools).

Usage: python tests/msc/test_usb_init.py path/to/sd2psx-debug.elf
Protocol initialization is stubbed; USB session setup itself executes. GPIO and
UART entry points are recorded. This detects unwanted example-board setup; it
does not emulate USB enumeration or the console's electrical interface.
"""
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    symbols = {s.name: s['st_value'] & ~1 for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_value']}
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, size in [(0x10000000, 0x1000000), (0x20000000, 0x50000), (0x30000000, 0x1000), (0xd0000000, 0x1000)]:
        cpu.mem_map(address, size)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
            cpu.mem_write(segment['p_vaddr'], segment.data())

events = []
returned = False
registers = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
watched = {symbols[name]: name for name in ['mca_protocol_init', 'gpio_init', 'stdio_uart_init_full'] if name in symbols}

def step(cpu, address, size, unused):
    global returned
    if address == 0x30000000:
        returned = True
        cpu.emu_stop()
    elif address in watched:
        name = watched[address]
        args = [cpu.reg_read(reg) for reg in registers] if name == 'stdio_uart_init_full' else []
        events.append((name, args))
        cpu.reg_write(UC_ARM_REG_R0, 0)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

cpu.hook_add(UC_HOOK_CODE, step)
cpu.reg_write(UC_ARM_REG_SP, 0x20041000)
cpu.reg_write(UC_ARM_REG_LR, 0x30000001)
cpu.emu_start(symbols['usb_session_init'] | 1, 0, count=100000)
print('USB initialization calls:', events)
assert returned, 'USB session setup did not return'
assert events == [('mca_protocol_init', [])], 'USB setup changed board GPIO/UART ownership'
print('PASS: USB session setup preserves firmware GPIO and UART ownership')
