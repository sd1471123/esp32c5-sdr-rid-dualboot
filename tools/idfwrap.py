"""Portable idf.py launcher for Git Bash / MSYS shells.

ESP-IDF 6.x refuses to start when MSYSTEM is set; the native Windows toolchain
does not need those variables.  This wrapper strips them and forwards every
argument to $IDF_PATH/tools/idf.py using the IDF Python interpreter.

Usage:  python tools/idfwrap.py -C <project> set-target esp32c5
        python tools/idfwrap.py -C <project> build
"""
import os
import runpy
import sys

for _var in ("MSYSTEM", "MINGW_PREFIX", "MINGW_CHOST"):
    os.environ.pop(_var, None)

idf_path = os.environ.get("IDF_PATH")
if not idf_path:
    sys.exit("IDF_PATH is not set. Source ESP-IDF's export.sh / export.ps1 first.")

idf_py = os.path.join(idf_path, "tools", "idf.py")
if not os.path.isfile(idf_py):
    sys.exit("Cannot find %s" % idf_py)

sys.path.insert(0, os.path.dirname(idf_py))
sys.argv = [idf_py] + sys.argv[1:]
runpy.run_path(idf_py, run_name="__main__")
