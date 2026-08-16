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
    from .registry_m1_m3 import M1_M3_CASES
    from .registry_m4_m6 import M4_M6_CASES

    cases = [*M1_M3_CASES, *M4_M6_CASES]
    if only is not None:
        return [case for case in cases if case.milestone == only]
    return [case for case in cases if case.milestone <= through]
