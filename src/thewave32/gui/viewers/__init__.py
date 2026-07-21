"""Viewer registry - maps a module slug to a QWidget class.

Each module either gets a hand-written viewer (when its data is rich
enough to warrant one) or a slim subclass of one of the reusable base
classes (``TableViewer``, ``CounterViewer``).
"""

from __future__ import annotations

from typing import Type

from .base import BaseViewer
from .counter import CounterViewer
from .csi_heatmap import CSIHeatmapViewer
from .evil_twin import EvilTwinClientsViewer
from .generic import GenericViewer
from .mac_tracker import MacTrackerViewer
from .port_scanner_grid import PortScannerGridViewer
from .spectrum import SpectrumViewer
from .table import TableViewer
from .wifi_beacon_spam import WifiBeaconSpamViewer
from .wifi_bssid_clone import WifiBssidCloneViewer
from .wifi_deauth import WifiDeauthViewer
from .wifi_handshake_capture import WifiHandshakeCaptureViewer
from .wifi_sniffer import WifiSnifferViewer


# --- Subclasses with module-specific config ---------------------------


class WifiScannerViewer(TableViewer):
    EVENT = "ap"
    COLUMNS = ("ts", "bssid", "ssid", "ch", "rssi", "auth")
    KEY_COLUMN = "bssid"
    HELP = "AP scan results - rows update in place as the same BSSID is seen again."


class WifiEapWatcherViewer(TableViewer):
    EVENT = "eap_id"
    COLUMNS = ("ts", "src", "bssid", "ch", "rssi", "eap_method", "imsi", "eap_id")
    KEY_COLUMN = "imsi"
    ACTIONS = ("start", "stop", "inject_test", "stats")
    HELP = "EAP-Identity observations (IMSI/SIM/AKA). inject_test feeds a synthetic event."


class WifiProbeLoggerViewer(TableViewer):
    EVENT = "probe"
    COLUMNS = ("ts", "src", "ch", "rssi", "ssid", "fp")
    KEY_COLUMN = "fp"
    ACTIONS = ("start", "stop", "peers", "stats")

    def _build_extra_controls(self):
        from PySide6.QtWidgets import (
            QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox,
        )
        row = QHBoxLayout()
        row.setSpacing(8)
        # Source-MAC filter
        row.addWidget(QLabel("filter MAC"))
        self._mac_in = QLineEdit()
        self._mac_in.setPlaceholderText("aa:bb:cc:dd:ee:ff or 'any'")
        row.addWidget(self._mac_in, 2)
        b1 = QPushButton("apply")
        b1.clicked.connect(
            lambda: self.send_command.emit(
                f"filter {self._mac_in.text().strip() or 'any'}"
            )
        )
        self.register_action_button(b1)
        row.addWidget(b1)
        # OUI prefix filter
        row.addWidget(QLabel("OUI"))
        self._oui_in = QLineEdit()
        self._oui_in.setPlaceholderText("aa:bb:cc, aabbcc, or 'any'")
        row.addWidget(self._oui_in, 1)
        b2 = QPushButton("apply")
        b2.clicked.connect(
            lambda: self.send_command.emit(
                f"oui {self._oui_in.text().strip() or 'any'}"
            )
        )
        self.register_action_button(b2)
        row.addWidget(b2)
        # RSSI floor
        row.addWidget(QLabel("RSSI ≥"))
        self._rssi_in = QSpinBox()
        self._rssi_in.setRange(-127, 0)
        self._rssi_in.setSuffix(" dBm")
        self._rssi_in.setValue(-127)
        row.addWidget(self._rssi_in)
        b3 = QPushButton("apply")
        b3.clicked.connect(
            lambda: self.send_command.emit(f"rssi_min {self._rssi_in.value()}")
        )
        self.register_action_button(b3)
        row.addWidget(b3)
        return row


class WifiGhostViewer(TableViewer):
    EVENT = "learned"
    COLUMNS = ("ssid", "src", "rssi")
    KEY_COLUMN = "ssid"
    ACTIONS = ("start", "stop", "clear", "stats")
    HELP = "SSIDs harvested from probe-requests and beaconed back."


class WifiDeauthDetectorViewer(TableViewer):
    EVENT = "attack_alert"
    COLUMNS = ("ts", "bssid", "count", "ch", "window_us")
    KEY_COLUMN = "bssid"
    ACTIONS = ("start", "stop", "stats")
    HELP = "Frames-per-second flood alerts; per-BSSID rolling counter > threshold."


class WifiPmkidCaptureViewer(TableViewer):
    EVENT = "pmkid"
    COLUMNS = ("ts", "bssid", "sta", "pmkid", "ch", "rssi")
    KEY_COLUMN = "pmkid"
    ACTIONS = ("scan", "start", "stop", "stats")
    HELP = "PMKIDs extracted from EAPOL-Key M1 (clientless)."


## wifi-handshake-capture, wifi-beacon-spam, wifi-bssid-clone all have
## hand-written viewers in their own files (see imports above) - the
## CounterViewer surface couldn't expose `target`/`chan`/`add`/`clones`
## etc., which is what the user actually needs to drive the modules.


class BleScannerViewer(TableViewer):
    EVENT = "adv"
    COLUMNS = ("ts", "mac", "rssi", "type", "addr_type", "name")
    KEY_COLUMN = "mac"
    ACTIONS = ("start", "stop", "stats")

    def _build_extra_controls(self):
        from PySide6.QtWidgets import (
            QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox,
        )
        row = QHBoxLayout()
        row.setSpacing(8)
        row.addWidget(QLabel("filter MAC"))
        self._mac_in = QLineEdit()
        self._mac_in.setPlaceholderText("aa:bb:cc:dd:ee:ff or 'any'")
        row.addWidget(self._mac_in, 2)
        b1 = QPushButton("apply")
        b1.clicked.connect(
            lambda: self.send_command.emit(
                f"filter {self._mac_in.text().strip() or 'any'}"
            )
        )
        self.register_action_button(b1)
        row.addWidget(b1)
        row.addWidget(QLabel("name contains"))
        self._name_in = QLineEdit()
        self._name_in.setPlaceholderText("substring or 'any'")
        row.addWidget(self._name_in, 2)
        b2 = QPushButton("apply")
        b2.clicked.connect(
            lambda: self.send_command.emit(
                f"name {self._name_in.text().strip() or 'any'}"
            )
        )
        self.register_action_button(b2)
        row.addWidget(b2)
        row.addWidget(QLabel("RSSI ≥"))
        self._rssi_in = QSpinBox()
        self._rssi_in.setRange(-127, 0)
        self._rssi_in.setSuffix(" dBm")
        self._rssi_in.setValue(-127)
        row.addWidget(self._rssi_in)
        b3 = QPushButton("apply")
        b3.clicked.connect(
            lambda: self.send_command.emit(f"rssi_min {self._rssi_in.value()}")
        )
        self.register_action_button(b3)
        row.addWidget(b3)
        return row


class BleAirtagFinderViewer(TableViewer):
    EVENT = "airtag"
    COLUMNS = ("ts", "mac", "rssi", "key_hash", "status", "new")
    KEY_COLUMN = "key_hash"
    ACTIONS = ("start", "stop", "peers", "stats")
    HELP = "Apple Find My beacons (Continuity 0x12); deduped by rotating-key hash."


class BleSpamViewer(CounterViewer):
    # frames_sent lives only in the sparkline; running is on the StatusStrip.
    STATS_KEYS = ("mode", "interval_ms", "host_ready")
    SPARKLINE_KEYS = ("frames_sent",)
    ACTIONS = ("start", "stop", "stats")


class BleHidKeyboardViewer(CounterViewer):
    # reports_sent lives only in the sparkline.
    STATS_KEYS = ("connected", "subscribed", "ran", "start_delay_ms")
    SPARKLINE_KEYS = ("reports_sent",)
    ACTIONS = ("replay", "stats")
    HELP = "BLE HID keyboard. Connect from a host then click `replay` to type the payload."


class UsbHidKeyboardViewer(CounterViewer):
    # reports_sent lives only in the sparkline.
    STATS_KEYS = ("mounted", "ran", "start_delay_ms")
    SPARKLINE_KEYS = ("reports_sent",)
    ACTIONS = ("replay", "stats")
    HELP = "USB HID via S3 native USB. mounted=true means a host enumerated the device."


class EspnowBridgeViewer(TableViewer):
    EVENT = "rx"
    COLUMNS = ("ts", "from", "rssi", "len", "data")
    KEY_COLUMN = "from"
    ACTIONS = ("start", "stop", "stats")


class WifiEvilTwinDetectorViewer(TableViewer):
    EVENT = "rogue_ap"
    COLUMNS = ("ssid", "expected_bssid", "actual_bssid", "channel", "rssi")
    KEY_COLUMN = "actual_bssid"
    ACTIONS = ("scan", "start", "stop", "list", "stats")
    HELP = "Rogue APs cloning a whitelisted SSID. Use `add` in the console to grow the whitelist."


class WifiClockSkewViewer(TableViewer):
    EVENT = "skew"
    COLUMNS = ("bssid", "ssid", "ch", "skew_mppm", "resid_us", "n")
    KEY_COLUMN = "bssid"
    ACTIONS = ("start", "stop", "stats")
    HELP = ("Per-AP clock-skew fingerprint from beacon TSF. skew_mppm is "
            "the skew in milli-ppm; a high resid_us means one BSSID is "
            "being driven by two transmitters (clone suspect).")


# wifi-sniffer is binary PCAP only - Generic viewer prints state info.

# --- registry ----------------------------------------------------------

_REGISTRY: dict[str, Type[BaseViewer]] = {
    "wifi-scanner":            WifiScannerViewer,
    "wifi-sniffer":            WifiSnifferViewer,
    "wifi-deauth":             WifiDeauthViewer,
    "wifi-eap-watcher":        WifiEapWatcherViewer,
    "wifi-probe-logger":       WifiProbeLoggerViewer,
    "wifi-handshake-capture":  WifiHandshakeCaptureViewer,
    "wifi-pmkid-capture":      WifiPmkidCaptureViewer,
    "wifi-beacon-spam":        WifiBeaconSpamViewer,
    "wifi-ghost":              WifiGhostViewer,
    "wifi-evil-twin":          EvilTwinClientsViewer,
    "wifi-bssid-clone":        WifiBssidCloneViewer,
    "wifi-deauth-detector":    WifiDeauthDetectorViewer,
    "ble-scanner":             BleScannerViewer,
    "ble-airtag-finder":       BleAirtagFinderViewer,
    "ble-spam":                BleSpamViewer,
    "ble-hid-keyboard":        BleHidKeyboardViewer,
    "usb-hid-keyboard":        UsbHidKeyboardViewer,
    "espnow-bridge":           EspnowBridgeViewer,
    "spectrum-analyzer":       SpectrumViewer,
    "wifi-mac-tracker":        MacTrackerViewer,
    "wifi-evil-twin-detector": WifiEvilTwinDetectorViewer,
    "wifi-clock-skew":         WifiClockSkewViewer,
    "wifi-csi-collector":      CSIHeatmapViewer,
    "net-port-scanner":        PortScannerGridViewer,
}


def get_viewer_class(slug: str) -> Type[BaseViewer]:
    return _REGISTRY.get(slug, GenericViewer)


__all__ = [
    "BaseViewer",
    "GenericViewer",
    "TableViewer",
    "CounterViewer",
    "SpectrumViewer",
    "CSIHeatmapViewer",
    "MacTrackerViewer",
    "PortScannerGridViewer",
    "EvilTwinClientsViewer",
    "WifiDeauthViewer",
    "WifiSnifferViewer",
    "get_viewer_class",
]
