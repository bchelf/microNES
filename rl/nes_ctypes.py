"""
ctypes bindings for libmicrones_rl.{so,dylib}.

Usage:
    from nes_ctypes import NesLib
    lib = NesLib()                      # auto-discovers library in build-host/
    h = lib.create()
    lib.load_rom(h, b"/path/to/smb1.nes")
    lib.reset(h)
    lib.set_buttons(h, 0x08)            # START
    lib.step(h)
    ram = lib.ram_view(h)               # numpy array, shape=(2048,), no-copy
"""

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# NES button bitmasks (mirrors input.h)
# ---------------------------------------------------------------------------
NES_BUTTON_A      = 1 << 0
NES_BUTTON_B      = 1 << 1
NES_BUTTON_SELECT = 1 << 2
NES_BUTTON_START  = 1 << 3
NES_BUTTON_UP     = 1 << 4
NES_BUTTON_DOWN   = 1 << 5
NES_BUTTON_LEFT   = 1 << 6
NES_BUTTON_RIGHT  = 1 << 7

_u8p = ctypes.POINTER(ctypes.c_uint8)


def _find_lib() -> str:
    """Locate libmicrones_rl relative to this file's repo root."""
    repo_root = Path(__file__).parent.parent
    suffixes = [".dylib", ".so"]
    search = [repo_root / "build-host", repo_root / "build"]
    for d in search:
        for suf in suffixes:
            p = d / f"libmicrones_rl{suf}"
            if p.exists():
                return str(p)
    raise FileNotFoundError(
        "libmicrones_rl not found. Build with:\n"
        "  cmake -S . -B build-host -DMICRONES_PLATFORM=host\n"
        "  cmake --build build-host -j"
    )


class NesLib:
    """Thin ctypes wrapper around the micrones_rl shared library."""

    def __init__(self, lib_path: str | None = None):
        path = lib_path or _find_lib()
        self._lib = ctypes.CDLL(path)
        self._configure()

    def _configure(self):
        lib = self._lib
        vp = ctypes.c_void_p

        lib.micrones_rl_create.restype  = vp
        lib.micrones_rl_create.argtypes = []

        lib.micrones_rl_destroy.restype  = None
        lib.micrones_rl_destroy.argtypes = [vp]

        lib.micrones_rl_load_rom.restype  = ctypes.c_int
        lib.micrones_rl_load_rom.argtypes = [vp, ctypes.c_char_p]

        lib.micrones_rl_reset.restype  = None
        lib.micrones_rl_reset.argtypes = [vp]

        lib.micrones_rl_step.restype  = ctypes.c_int
        lib.micrones_rl_step.argtypes = [vp]

        lib.micrones_rl_set_buttons.restype  = None
        lib.micrones_rl_set_buttons.argtypes = [vp, ctypes.c_uint8]

        lib.micrones_rl_ram.restype  = _u8p
        lib.micrones_rl_ram.argtypes = [vp]

        lib.micrones_rl_nametables.restype  = _u8p
        lib.micrones_rl_nametables.argtypes = [vp]

        lib.micrones_rl_oam.restype  = _u8p
        lib.micrones_rl_oam.argtypes = [vp]

        lib.micrones_rl_framebuffer.restype  = _u8p
        lib.micrones_rl_framebuffer.argtypes = [vp]

        lib.micrones_rl_frame_count.restype  = ctypes.c_uint64
        lib.micrones_rl_frame_count.argtypes = [vp]

        lib.micrones_rl_last_error.restype  = ctypes.c_char_p
        lib.micrones_rl_last_error.argtypes = [vp]

        lib.micrones_rl_write_ram.restype  = None
        lib.micrones_rl_write_ram.argtypes = [vp, ctypes.c_uint16, ctypes.c_uint8]

        lib.micrones_rl_state_size.restype  = ctypes.c_size_t
        lib.micrones_rl_state_size.argtypes = []

        lib.micrones_rl_save_state.restype  = None
        lib.micrones_rl_save_state.argtypes = [vp, vp]

        lib.micrones_rl_load_state.restype  = None
        lib.micrones_rl_load_state.argtypes = [vp, vp]

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------
    def create(self) -> int:
        h = self._lib.micrones_rl_create()
        if not h:
            raise RuntimeError("micrones_rl_create returned NULL")
        return h

    def destroy(self, h: int):
        self._lib.micrones_rl_destroy(h)

    def load_rom(self, h: int, path: bytes | str):
        if isinstance(path, str):
            path = path.encode()
        if not self._lib.micrones_rl_load_rom(h, path):
            err = self._lib.micrones_rl_last_error(h)
            raise RuntimeError(f"load_rom failed: {err.decode()}")

    def reset(self, h: int):
        self._lib.micrones_rl_reset(h)

    def step(self, h: int) -> bool:
        return bool(self._lib.micrones_rl_step(h))

    def set_buttons(self, h: int, buttons: int):
        self._lib.micrones_rl_set_buttons(h, ctypes.c_uint8(buttons))

    def write_ram(self, h: int, addr: int, value: int):
        self._lib.micrones_rl_write_ram(h, ctypes.c_uint16(addr), ctypes.c_uint8(value))

    def frame_count(self, h: int) -> int:
        return int(self._lib.micrones_rl_frame_count(h))

    # ------------------------------------------------------------------
    # Zero-copy numpy views into emulator memory
    # ------------------------------------------------------------------
    def ram_view(self, h: int) -> np.ndarray:
        ptr = self._lib.micrones_rl_ram(h)
        return np.ctypeslib.as_array(ptr, shape=(2048,))

    def nametables_view(self, h: int) -> np.ndarray:
        ptr = self._lib.micrones_rl_nametables(h)
        return np.ctypeslib.as_array(ptr, shape=(2048,))

    def oam_view(self, h: int) -> np.ndarray:
        ptr = self._lib.micrones_rl_oam(h)
        return np.ctypeslib.as_array(ptr, shape=(256,))

    def framebuffer_view(self, h: int) -> np.ndarray:
        ptr = self._lib.micrones_rl_framebuffer(h)
        return np.ctypeslib.as_array(ptr, shape=(240, 256))

    # ------------------------------------------------------------------
    # Savestate
    # ------------------------------------------------------------------
    def state_size(self) -> int:
        """Return sizeof(MicroNESSaveState) — fixed blob size in bytes."""
        return int(self._lib.micrones_rl_state_size())

    def save_state_bytes(self, h: int) -> bytes:
        """Capture complete emulator state; returns opaque bytes of fixed size."""
        sz  = self.state_size()
        buf = ctypes.create_string_buffer(sz)
        self._lib.micrones_rl_save_state(h, buf)
        return bytes(buf)

    def load_state_bytes(self, h: int, data: bytes) -> None:
        """Restore emulator state from bytes previously returned by save_state_bytes."""
        buf = ctypes.create_string_buffer(data, len(data))
        self._lib.micrones_rl_load_state(h, buf)
