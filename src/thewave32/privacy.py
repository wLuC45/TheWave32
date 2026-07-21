"""Privacy policy for inbound device JSON.

Passive modules (ble-airtag-finder, wifi-probe-logger, wifi-mac-tracker)
surface permanent identifiers, and capture modules surface key material
(PMKID, EAPOL/handshake bytes). Even in authorised research, the session
log should not have to persist those in the clear.

This module is pure (no PySide6, no I/O) so it is unit-testable on its
own. ``PrivacyPolicy.apply`` returns a transformed copy of an event:

* MAC addresses become a per-session pseudonym, ``HMAC(salt, mac)`` folded
  into a MAC-shaped string with the locally-administered bit set, so it is
  visibly synthetic yet stable within a session (correlation is kept, the
  real address is not). A fresh random salt per session means the mapping
  does not survive across runs.
* Values under sensitive keys (pmkid, handshake, psk, ...) are replaced
  with ``"[redacted]"``.

Both switches default off, so behaviour is unchanged unless a policy is
explicitly enabled.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import re
from dataclasses import dataclass, field

# Matches a colon- or hyphen-separated 6-octet MAC anywhere in a string.
_MAC_RE = re.compile(r"\b([0-9a-fA-F]{2}([:-])(?:[0-9a-fA-F]{2}\2){4}[0-9a-fA-F]{2})\b")

# Keys whose values are identifiers or secrets we never want persisted raw.
_SENSITIVE_KEYS = frozenset({
    "pmkid", "handshake", "eapol", "psk", "pmk", "ptk", "gtk",
    "kck", "kek", "key", "password", "passphrase", "pwd", "secret",
})

_REDACTED = "[redacted]"


def _anonymize_mac(mac: str, salt: bytes) -> str:
    """Map a MAC to a stable, synthetic MAC-shaped pseudonym."""
    sep = ":" if ":" in mac else "-"
    norm = mac.lower().replace("-", ":").encode("ascii")
    digest = hmac.new(salt, norm, hashlib.sha256).digest()
    octets = bytearray(digest[:6])
    # Mark it locally-administered (bit 1) and unicast (clear bit 0) so it
    # reads as deliberately synthetic, not a real OUI.
    octets[0] = (octets[0] | 0x02) & 0xFE
    return sep.join(f"{b:02x}" for b in octets)


@dataclass
class PrivacyPolicy:
    """Transform inbound JSON before it is logged or displayed."""

    anonymize_macs: bool = False
    redact_secrets: bool = False
    # Random per-instance salt unless one is supplied (tests pin it).
    salt: bytes = field(default_factory=lambda: os.urandom(16))

    @classmethod
    def from_env(cls, env: dict | None = None) -> "PrivacyPolicy":
        """Build a policy from ``THEWAVE32_PRIVACY``.

        Comma-separated tokens: ``macs`` (anonymize MACs), ``secrets``
        (redact key material), or ``all`` for both. Unset/empty yields a
        disabled policy, so the default behaviour is unchanged.
        """
        raw = (env or os.environ).get("THEWAVE32_PRIVACY", "")
        tokens = {t.strip().lower() for t in raw.split(",") if t.strip()}
        all_on = "all" in tokens
        return cls(
            anonymize_macs=all_on or "macs" in tokens,
            redact_secrets=all_on or "secrets" in tokens,
        )

    @property
    def active(self) -> bool:
        return self.anonymize_macs or self.redact_secrets

    def _scrub_str(self, value: str) -> str:
        if not self.anonymize_macs:
            return value
        return _MAC_RE.sub(lambda m: _anonymize_mac(m.group(1), self.salt), value)

    def apply(self, obj):
        """Return a privacy-transformed copy of ``obj`` (dict/list/scalar)."""
        if not self.active:
            return obj
        if isinstance(obj, dict):
            out = {}
            for k, v in obj.items():
                if self.redact_secrets and isinstance(k, str) and k.lower() in _SENSITIVE_KEYS:
                    out[k] = _REDACTED
                else:
                    out[k] = self.apply(v)
            return out
        if isinstance(obj, list):
            return [self.apply(v) for v in obj]
        if isinstance(obj, str):
            return self._scrub_str(obj)
        return obj


__all__ = ["PrivacyPolicy"]
