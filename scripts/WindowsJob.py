"""Bound one proof launcher and its descendants to a Windows job object."""
import ctypes
from ctypes import wintypes as W
import subprocess

class Basic(ctypes.Structure):
    _fields_ = [('ProcessTime', ctypes.c_int64), ('JobTime', ctypes.c_int64), ('Flags', W.DWORD),
                ('MinWS', ctypes.c_size_t), ('MaxWS', ctypes.c_size_t), ('ActiveLimit', W.DWORD),
                ('Affinity', ctypes.c_size_t), ('Priority', W.DWORD), ('Scheduling', W.DWORD)]

class IO(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in ['ReadOps', 'WriteOps', 'OtherOps', 'ReadBytes', 'WriteBytes', 'OtherBytes']]

class Extended(ctypes.Structure):
    _fields_ = [('Basic', Basic), ('IO', IO), ('ProcessMemory', ctypes.c_size_t),
                ('JobMemory', ctypes.c_size_t), ('PeakProcess', ctypes.c_size_t), ('PeakJob', ctypes.c_size_t)]

def run(arguments, *, timeout, **kwargs):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, W.LPCWSTR]
    kernel.CreateJobObjectW.restype = W.HANDLE
    kernel.SetInformationJobObject.argtypes = [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD]
    kernel.SetInformationJobObject.restype = W.BOOL
    kernel.AssignProcessToJobObject.argtypes = [W.HANDLE, W.HANDLE]
    kernel.AssignProcessToJobObject.restype = W.BOOL
    kernel.TerminateJobObject.argtypes = [W.HANDLE, W.UINT]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    native = ctypes.WinDLL('ntdll')
    native.NtResumeProcess.argtypes = [W.HANDLE]
    native.NtResumeProcess.restype = ctypes.c_long
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    process = None
    try:
        info = Extended()
        info.Basic.Flags = 0x2000  # Kill descendants when this owned job closes.
        if not kernel.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        process = subprocess.Popen(arguments, creationflags=0x4 | 0x08000000, **kwargs)
        if not kernel.AssignProcessToJobObject(job, W.HANDLE(int(process._handle))):
            raise ctypes.WinError(ctypes.get_last_error())
        if native.NtResumeProcess(W.HANDLE(int(process._handle))) < 0:
            raise RuntimeError('Cannot resume proof launcher')
        return subprocess.CompletedProcess(arguments, process.wait(timeout=timeout))
    finally:
        kernel.TerminateJobObject(job, 1)
        kernel.CloseHandle(job)
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=30)
