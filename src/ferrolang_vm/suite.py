"""The one type shared by the case registry and the DOSBox-X runner.

Kept separate from ``registry`` so that ``dosboxx`` can depend on the type
without importing the case data.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    id: str
    milestone: int
    command: str
    expect_success: bool
