from __future__ import annotations

import os
from pathlib import Path

from thewave32.errors import IdfNotFoundError

DEFAULT_IDF_PATH = Path.home() / "ESP32S3"

_NVS_REL = Path("components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py")
_SPIFFS_REL = Path("components/spiffs/spiffsgen.py")


def locate() -> Path:
    """Return the IDF root directory or raise IdfNotFoundError."""
    env = os.environ.get("IDF_PATH")
    if env:
        root = Path(env)
        if root.is_dir() and (root / _NVS_REL).is_file() and (root / _SPIFFS_REL).is_file():
            return root
    else:
        root = DEFAULT_IDF_PATH
        if root.is_dir() and (root / _NVS_REL).is_file() and (root / _SPIFFS_REL).is_file():
            return root
    raise IdfNotFoundError()


def nvs_partition_gen() -> Path:
    return locate() / _NVS_REL


def spiffsgen() -> Path:
    return locate() / _SPIFFS_REL
