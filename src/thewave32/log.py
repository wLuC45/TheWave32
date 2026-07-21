from __future__ import annotations

import logging
import os
from logging.handlers import RotatingFileHandler
from pathlib import Path


def default_log_path() -> Path:
    env = os.environ.get("THEWAVE32_LOG")
    if env:
        return Path(env)
    return Path.home() / ".cache" / "thewave32" / "thewave32.log"


_configured = False


def setup(verbose: bool = False, log_path: Path | None = None) -> Path:
    """Initialise the root 'thewave32' logger.

    File handler: always INFO+ (DEBUG if verbose), rotating 1 MB x 3.
    Console handler: WARNING+ (INFO if verbose) to stderr.
    Idempotent — calling twice is a no-op.
    """
    global _configured
    if _configured:
        return log_path or default_log_path()

    path = log_path or default_log_path()
    path.parent.mkdir(parents=True, exist_ok=True)

    root = logging.getLogger("thewave32")
    root.setLevel(logging.DEBUG)
    root.propagate = False

    fh = RotatingFileHandler(path, maxBytes=1_048_576, backupCount=3, encoding="utf-8")
    fh.setLevel(logging.DEBUG if verbose else logging.INFO)
    fh.setFormatter(logging.Formatter(
        "%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S",
    ))
    root.addHandler(fh)

    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO if verbose else logging.WARNING)
    ch.setFormatter(logging.Formatter("%(levelname)s: %(message)s"))
    root.addHandler(ch)

    _configured = True
    root.info("logger ready (file=%s, verbose=%s)", path, verbose)
    return path


def get(name: str) -> logging.Logger:
    return logging.getLogger(f"thewave32.{name}")
