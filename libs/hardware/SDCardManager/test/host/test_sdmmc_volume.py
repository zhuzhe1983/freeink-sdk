#!/usr/bin/env python3
"""Compile the production mount helper against an unmodified SdFat checkout."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdfat", type=Path, required=True, help="SdFat 2.3.1 library directory")
    args = parser.parse_args()
    src = args.sdfat.resolve() / "src"
    version = (src / "SdFat.h").read_text()
    if "#define SD_FAT_VERSION 20301" not in version:
        parser.error("This regression test is validated against SdFat 2.3.1")

    here = Path(__file__).resolve().parent
    sources = sorted(path for name in ("common", "FatLib", "ExFatLib", "FsLib") for path in (src / name).glob("*.cpp"))
    with tempfile.TemporaryDirectory(prefix="freeink-sdmmc-volume-") as directory:
        binary = Path(directory) / "test_sdmmc_volume"
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17",
            "-DENABLE_ARDUINO_FEATURES=0",
            "-DENABLE_ARDUINO_SERIAL=0",
            "-DENABLE_ARDUINO_STRING=0",
            "-DUSE_BLOCK_DEVICE_INTERFACE=1",
            "-DUSE_UTF8_LONG_NAMES=1",
            "-DSPI_DRIVER_SELECT=3",
            "-DDESTRUCTOR_CLOSES_FILE=1",
            "-include",
            str(here / "sdfat_host_compat.h"),
            "-I" + str(src),
            "-I" + str(here / "../../src"),
        ]
        command += shlex.split(os.environ.get("CXXFLAGS", ""))
        command += ["-UNDEBUG", str(here / "test_sdmmc_volume.cpp"), *map(str, sources), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
