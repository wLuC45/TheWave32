from __future__ import annotations


class Tw32Error(Exception):
    """Classe base para todos os erros do thewave32."""


class ManifestError(Tw32Error):
    def __init__(self, path: str, reason: str) -> None:
        self.path = path
        self.reason = reason
        super().__init__(f"erro no manifesto em {path}: {reason}")


class ModuleNotFoundError(Tw32Error):
    def __init__(self, name: str) -> None:
        self.name = name
        super().__init__(f"módulo não encontrado: {name}")


class DeviceNotFoundError(Tw32Error):
    def __init__(self) -> None:
        super().__init__(
            "nenhum dispositivo ESP32-S3 encontrado em nenhuma porta serial "
            "(segure BOOT e pressione RESET para entrar em modo de gravação)"
        )


class WrongChipError(Tw32Error):
    def __init__(self, detected: str, expected: str) -> None:
        self.detected = detected
        self.expected = expected
        super().__init__(f"chip incompatível: detectado {detected}, esperado {expected}")


class BuildError(Tw32Error):
    def __init__(self, stage: str, stderr: str) -> None:
        self.stage = stage
        self.stderr = stderr
        super().__init__(f"falha na compilação em {stage}: {stderr}")


class FlashError(Tw32Error):
    def __init__(self, returncode: int, stderr: str, log_path: str) -> None:
        self.returncode = returncode
        self.stderr = stderr
        self.log_path = log_path
        super().__init__(
            f"esptool falhou (rc={returncode}); log completo: {log_path}"
        )


class IdfNotFoundError(Tw32Error):
    def __init__(self) -> None:
        # Imported lazily so errors.py stays free of project imports — many
        # modules import errors.py during their own initialisation.
        from thewave32.idf import DEFAULT_IDF_PATH
        super().__init__(
            f"ESP-IDF não encontrado: defina $IDF_PATH ou coloque o IDF em {DEFAULT_IDF_PATH}"
        )
