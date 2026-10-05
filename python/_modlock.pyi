"""_modlock is the runtime's bridge to the host, which the modlock library
uses. A mod uses the library instead."""

from collections.abc import Callable

def call(request: bytes) -> bytes:
    """call sends one encoded Call to the host and returns its encoded
    Reply."""

def log(line: str) -> None:
    """log writes line to the server log."""

def handle(handler: Callable[[bytes], bytes]) -> None:
    """handle makes handler answer each encoded Call the host sends."""
