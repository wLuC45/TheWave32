from __future__ import annotations

import argparse
import signal
import sys
from pathlib import Path

from thewave32 import builder, compiler, flasher, log as _log, manifest, pipeline, registry
from thewave32.errors import Tw32Error

_logger = _log.get("cli")


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="thewave32")
    p.add_argument("--modules", default="modules", help="caminho para a raiz dos módulos (padrão: ./modules)")
    p.add_argument("--debug", action="store_true", help="mostra stack traces completos e logs verbosos")
    p.add_argument("--no-build", action="store_true", help="pula a recompilação automática de módulos desatualizados")
    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("list", help="lista os módulos disponíveis")

    info = sub.add_parser("info", help="mostra detalhes de um módulo")
    info.add_argument("name")

    flash = sub.add_parser("flash", help="grava um módulo na placa")
    flash.add_argument("name")
    flash.add_argument("--input", "-i", action="append", default=[], help="chave=valor (repetível)")
    flash.add_argument("--port")
    flash.add_argument("--baud", type=int, default=921600)

    return p


def _safe_eprint(msg: str) -> None:
    try:
        print(msg, file=sys.stderr)
    except BrokenPipeError:
        pass


def _auto_build(modules_root: Path) -> list[compiler.BuildResult]:
    repo_root = compiler.repo_root_from_modules(modules_root)
    results = compiler.ensure_fresh(repo_root)
    rebuilt = [r for r in results if r.rebuilt and r.ok]
    failed = [r for r in results if not r.ok]
    if rebuilt:
        names = ", ".join(r.slug for r in rebuilt)
        _safe_eprint(f"recompilados: {names}")
    for r in failed:
        first_line = (r.reason or "").splitlines()[0] if r.reason else ""
        _safe_eprint(f"falha na compilação: {r.slug} ({first_line})")
    return results


def _print_load_failures(fails: list[registry.LoadFailure]) -> None:
    for f in fails:
        _safe_eprint(f"módulo ignorado {f.name}: {f.reason}")


def _cmd_list(args: argparse.Namespace) -> int:
    mods, fails = registry.discover_with_errors(Path(args.modules))
    _print_load_failures(fails)
    if not mods:
        print("(nenhum módulo encontrado)")
        return 0
    width = max(len(m.name) for m in mods)
    for m in mods:
        print(f"  {m.name:<{width}}  {m.description}")
    return 0


def _cmd_info(args: argparse.Namespace) -> int:
    m = registry.get(Path(args.modules), args.name)
    print(f"Nome:        {m.name}")
    print(f"Versão:      {m.version}")
    print(f"Alvo:        {m.target}")
    print(f"Autor:       {m.author or '-'}")
    print(f"Descrição:   {m.description}")
    print("Artefatos:")
    for a in m.manifest.flash.artifacts:
        print(f"  {hex(a.offset):>10}  {a.path}")
    if m.manifest.partitions:
        print("Partições:")
        for name, part in m.manifest.partitions.items():
            print(f"  {name:<8} offset={hex(part.offset)} tamanho={hex(part.size)}")
    if m.manifest.inputs:
        print("Entradas:")
        for inp in m.manifest.inputs:
            req = "obrigatório" if inp.required else "opcional"
            print(f"  {inp.key} ({inp.type}, alvo={inp.target}, {req}): {inp.prompt}")
    return 0


def _cmd_flash(args: argparse.Namespace) -> int:
    mod = registry.get(Path(args.modules), args.name)
    raw_values = pipeline.parse_cli_inputs(args.input)
    resolved = manifest.resolve_inputs(mod.manifest.inputs, raw_values)
    port = flasher.resolve_port(args.port)
    pipeline.execute_flash(mod=mod, port=port, resolved=resolved, baud=args.baud)
    print(f"gravado {mod.name} em {port}")
    return 0


def main(argv: list[str] | None = None) -> int:
    # Redefine SIGPIPE para o padrão — quando a saída é um pipe fechado
    # (ex.: `thewave32 list | head`), o processo termina silenciosamente
    # em vez de lançar BrokenPipeError no meio da recompilação.
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass  # Windows or non-main thread

    parser = _build_parser()
    args = parser.parse_args(argv)
    log_path = _log.setup(verbose=args.debug)
    _logger.info("thewave32 invoked: cmd=%s modules=%s", args.cmd, args.modules)

    if not args.no_build:
        try:
            _auto_build(Path(args.modules))
        except Exception as e:  # noqa: BLE001 — never let auto-build kill the tool
            _logger.exception("auto-build aborted: %s", e)
            print(f"aviso: recompilação automática abortada ({e}); veja {log_path}", file=sys.stderr)

    if not args.cmd:
        # No subcommand → launch the native GUI. Importing PySide6 lazily so
        # `thewave32 list` and friends still run on hosts without Qt installed.
        try:
            from thewave32.gui.app import main as gui_main
        except ImportError:
            parser.error(
                "nenhum comando e PySide6 não instalado; execute `pip install thewave32` "
                "para instalar as dependências da GUI, ou use um subcomando como `list` / `info` / `flash`"
            )
            return 2
        return gui_main(modules_root=Path(args.modules))
    handlers = {"list": _cmd_list, "info": _cmd_info, "flash": _cmd_flash}
    handler = handlers.get(args.cmd)
    if handler is None:
        parser.error(f"comando desconhecido: {args.cmd}")
        return 2
    try:
        return handler(args)
    except Tw32Error as e:
        _logger.error("%s failed: %s", args.cmd, e)
        if args.debug:
            raise
        print(f"erro: {e}", file=sys.stderr)
        return 1
