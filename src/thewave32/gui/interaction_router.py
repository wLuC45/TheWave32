"""InteractionRouter - classify firmware JSON into user-facing events.

The MainWindow's `_on_json` previously inlined a ~80-line switch over
event/cmd names that decided which JSON lines were "interactions" vs
just chatter. That logic doesn't belong in the window itself: the
classification is purely behavioural and benefits from being unit-
testable in isolation.

Owns the per-slug "previous HID flag" state so transitions are
deduped (a stats poll where ``connected=True`` only fires the
"BLE host paired" event the first time it flips).

Returns from :meth:`classify` whether the caller should switch the
workspace tab - credential captures get a ``True`` so the UI can
foreground the Interactions tab; chattier transitions return False.
"""

from __future__ import annotations

from typing import Callable, Optional


class InteractionRouter:
    """Stateful classifier. Keep one instance per MainWindow."""

    def __init__(self, sink: Callable[[dict], None]) -> None:
        self._sink = sink
        self._prev_hid: dict[str, dict[str, bool]] = {}

    def classify(self, slug: Optional[str], obj: dict) -> bool:
        """Push relevant interaction records into the sink. Returns True
        when the caller should auto-switch focus to the interactions
        view (credential captures), False otherwise."""
        slug = slug or "-"
        evt = obj.get("event")
        cmd = obj.get("cmd")
        urgent = False

        if evt in ("client_connected", "client"):
            self._sink({
                "module": slug, "kind": "Wi-Fi associate",
                "who": str(obj.get("mac", "?")),
                "detail": f"aid={obj.get('aid','?')}",
            })
        elif evt == "client_disconnected":
            self._sink({
                "module": slug, "kind": "Wi-Fi disassociate",
                "who": str(obj.get("mac", "?")),
                "detail": f"reason={obj.get('reason','?')}",
            })
        elif evt == "eap_id":
            self._sink({
                "module": slug, "kind": "EAP-ID captured",
                "who": str(obj.get("imsi", obj.get("eap_id", "?"))),
                "detail": (f"src={obj.get('src','?')} "
                           f"method={obj.get('eap_method','?')}"),
            })
            urgent = True
        elif evt == "creds":
            self._sink({
                "module": slug, "kind": "Captive-portal creds",
                "who": str(obj.get("ssid", "?")),
                "detail": f"user={obj.get('user','')} pass={obj.get('password','')}",
            })
            urgent = True
        elif cmd == "stats":
            self._diff_hid(slug, obj)
        return urgent

    def _diff_hid(self, slug: str, obj: dict) -> None:
        prev = self._prev_hid.setdefault(slug, {})
        if "ble" in slug:
            for flag, label in (
                ("connected",  "BLE host paired"),
                ("subscribed", "BLE host wrote"),
            ):
                cur = bool(obj.get(flag, False))
                if cur and not prev.get(flag, False):
                    self._sink({
                        "module": slug, "kind": label, "who": "host",
                        "detail": f"reports_sent={obj.get('reports_sent', 0)}",
                    })
                prev[flag] = cur
        elif slug.startswith("usb-"):
            cur = bool(obj.get("mounted", False))
            if cur and not prev.get("mounted", False):
                self._sink({
                    "module": slug, "kind": "USB host enumerated",
                    "who": "host",
                    "detail": (f"ran={obj.get('ran', False)} "
                               f"reports_sent={obj.get('reports_sent', 0)}"),
                })
            prev["mounted"] = cur


__all__ = ["InteractionRouter"]
