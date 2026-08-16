# 소유권과 대여

이 디렉터리는 `fec/tests`에서 소유권(Ownership)과 대여(Borrow) 동작을 검증하는 고정 fixture 입니다.

실행 방법:

`uv run python tests/run.py -k own`

- `ok*.fe`는 통과해야 합니다.
- `bad*.fe`는 실패해야 하며, 첫 줄 `// ERROR:...` 마커가 있으면 그 패턴을 따른다.

현재 구현 범위는 타입 검사/의미 검사를 통과한 소스 기반만 포함하고, 별도 코드 생성/런타임 동작은 포함하지 않습니다.
