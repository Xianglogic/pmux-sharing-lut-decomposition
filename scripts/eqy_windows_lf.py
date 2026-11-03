"""Run the installed EQY with LF text output inside its new work directory."""
import builtins
import os
from pathlib import Path
import runpy
import sys

script = Path(sys.argv[1]).resolve()
arguments = sys.argv[2:]
work = Path(arguments[arguments.index('-d') + 1]).resolve()
original_open = builtins.open

def open_with_lf(file, mode='r', *args, **kwargs):
    if isinstance(file, (str, bytes, os.PathLike)) and 'b' not in mode and any(letter in mode for letter in 'wax'):
        try:
            inside_work = Path(file).resolve().is_relative_to(work)
        except (TypeError, ValueError):
            inside_work = False
        if inside_work and len(args) < 4 and 'newline' not in kwargs:
            kwargs['newline'] = '\n'
    return original_open(file, mode, *args, **kwargs)

builtins.open = open_with_lf
sys.argv = [str(script)] + arguments
runpy.run_path(str(script), run_name='__main__')
