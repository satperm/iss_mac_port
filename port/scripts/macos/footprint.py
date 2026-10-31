#!/usr/bin/env python3
"""Prints a process's physical footprint in MiB (what Activity Monitor calls Memory), from
proc_pid_rusage: top and footprint round it to whole GB. usage: footprint.py <pid>"""
import ctypes
import sys


class RUsageInfoV0(ctypes.Structure):
    _fields_ = [('uuid', ctypes.c_uint8 * 16), ('user_time', ctypes.c_uint64),
                ('system_time', ctypes.c_uint64), ('pkg_idle_wkups', ctypes.c_uint64),
                ('interrupt_wkups', ctypes.c_uint64), ('pageins', ctypes.c_uint64),
                ('wired_size', ctypes.c_uint64), ('resident_size', ctypes.c_uint64),
                ('phys_footprint', ctypes.c_uint64), ('proc_start_abstime', ctypes.c_uint64),
                ('proc_exit_abstime', ctypes.c_uint64)]


info = RUsageInfoV0()
libproc = ctypes.CDLL('/usr/lib/libproc.dylib')
if libproc.proc_pid_rusage(int(sys.argv[1]), 0, ctypes.byref(info)) != 0:
    sys.exit(1)
print(info.phys_footprint >> 20)
