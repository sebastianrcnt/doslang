"""Shared types and selection for the explicit milestone registries."""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    id: str
    milestone: int
    command: str
    expect_success: bool


def all_cases(*, through: int = 6, only: int | None = None) -> list[Case]:
    from .registry import all_cases as _all_cases
    return _all_cases(through=through, only=only)
