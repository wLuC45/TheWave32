from __future__ import annotations

import tomllib
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator, model_validator

from thewave32.errors import ManifestError, Tw32Error


def _parse_offset(v: Any) -> int:
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        return int(v, 0)
    raise ValueError(f"offset must be int or string, got {type(v).__name__}")


class ModuleMeta(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str
    version: str
    description: str
    target: str
    author: str | None = None
    source_url: str | None = None


class Artifact(BaseModel):
    model_config = ConfigDict(extra="forbid")
    path: str
    offset: int

    @field_validator("offset", mode="before")
    @classmethod
    def _offset(cls, v: Any) -> int:
        return _parse_offset(v)


class FlashSection(BaseModel):
    model_config = ConfigDict(extra="forbid")
    artifacts: list[Artifact] = Field(min_length=1)


class Partition(BaseModel):
    model_config = ConfigDict(extra="forbid")
    offset: int
    size: int
    label: str | None = None

    @field_validator("offset", "size", mode="before")
    @classmethod
    def _ints(cls, v: Any) -> int:
        return _parse_offset(v)


InputType = Literal["string", "int", "bool", "file", "choice"]
InputTarget = Literal["nvs", "spiffs"]


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")
    key: str
    prompt: str
    type: InputType
    target: InputTarget
    namespace: str | None = None
    dest: str | None = None
    default: Any | None = None
    required: bool = True
    options: list[str] | None = None

    @model_validator(mode="after")
    def _validate(self) -> "Input":
        if self.type == "choice" and not self.options:
            raise ValueError("inputs of type 'choice' require non-empty 'options'")
        if self.target == "nvs" and not self.namespace:
            raise ValueError(f"input '{self.key}' (target=nvs) requires 'namespace'")
        if self.target == "spiffs":
            if not self.dest:
                raise ValueError(f"input '{self.key}' (target=spiffs) requires 'dest'")
            if not self.dest.startswith("/") or ".." in self.dest:
                raise ValueError(f"input '{self.key}' has invalid 'dest' (must start with '/' and contain no '..')")
        return self


class Manifest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    module: ModuleMeta
    flash: FlashSection
    partitions: dict[str, Partition] = Field(default_factory=dict)
    inputs: list[Input] = Field(default_factory=list)

    @model_validator(mode="after")
    def _check_offsets(self) -> "Manifest":
        seen: list[tuple[int, str]] = []
        for art in self.flash.artifacts:
            seen.append((art.offset, art.path))
        for name, part in self.partitions.items():
            seen.append((part.offset, f"partition:{name}"))
        seen.sort()
        for (o1, n1), (o2, n2) in zip(seen, seen[1:]):
            if o1 == o2:
                raise ValueError(f"overlapping offsets at {hex(o1)}: {n1} and {n2}")
        return self

    @model_validator(mode="after")
    def _check_input_targets(self) -> "Manifest":
        for inp in self.inputs:
            if inp.target not in self.partitions:
                raise ValueError(
                    f"input '{inp.key}' targets partition '{inp.target}' "
                    f"but it is not declared in [partitions]"
                )
        return self


def load(path: Path) -> Manifest:
    """Parse and validate a module.toml. Raises ManifestError on failure."""
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (tomllib.TOMLDecodeError, OSError) as e:
        raise ManifestError(path=str(path), reason=f"parse error: {e}") from e
    try:
        return Manifest.model_validate(data)
    except ValidationError as e:
        first = e.errors()[0]
        loc = ".".join(str(x) for x in first["loc"])
        raise ManifestError(path=str(path), reason=f"{loc}: {first['msg']}") from e


def coerce_value(value: str, type_: InputType) -> object:
    """Convert a CLI/UI string into the typed value the manifest declares."""
    if type_ == "int":
        return int(value, 0)
    if type_ == "bool":
        return value.lower() in ("1", "true", "yes", "on")
    return value


def resolve_inputs(
    inputs: list[Input],
    raw_values: dict[str, str],
) -> list[tuple[Input, object]]:
    """Combine declared inputs with the user's raw values, applying defaults
    and rejecting missing required inputs.

    `raw_values` is a flat ``key -> str`` dict (CLI ``--input`` or TUI form).
    Missing required inputs raise ``Tw32Error`` so the caller surfaces a
    consistent error to the user — this is the contract the CLI and TUI
    must share.
    """
    resolved: list[tuple[Input, object]] = []
    for inp in inputs:
        if inp.key in raw_values:
            resolved.append((inp, coerce_value(raw_values[inp.key], inp.type)))
            continue
        if inp.default is not None:
            resolved.append((inp, inp.default))
            continue
        if inp.required:
            raise Tw32Error(f"input '{inp.key}' is required (use --input {inp.key}=...)")
    return resolved
