`fec/tests/pending-backend/`에 남겨둔 fixture는 현재 프론트엔드 전용 상태에서 실행할 수 없습니다.

- `bounds_trap.fe` / `bounds_nocheck.fe`: 경계 검사 실패가 실제로 trap되는지,
  `--no-checks` 플래그가 그 검사를 제거하는지 확인하는 런타임 동작 테스트입니다.
- `ownership_drop.fe` + `ownership-drop.c`: 삽입된 `drop`/`defer`가 실제로 실행되는지 확인하는 테스트입니다.
  `ownership-drop.c`는 해제 횟수/순서/이중 해제를 검증하는 하네스입니다.
- `format-prop.c`: 포맷 프로퍼티 동작을 확인하는 런타임 검사입니다.

이들은 코드 생성기가 없는 현재 단계에서는 실행할 수 없어서 `tests/run.py`가 건너뜁니다.
백엔드(코드 생성기)가 돌아오면 가장 먼저 재활성화할 대상입니다.
