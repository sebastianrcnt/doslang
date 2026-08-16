# 옵션/에러유니온

이 디렉터리는 `?T`/`null`/`try`/`catch`/`orelse`/명목 에러 유니온 동작을 검증합니다.

실행 방법:

`uv run python tests/run.py -k optional`

- `ok*.fe`는 통과해야 합니다.
- `bad*.fe`는 실패해야 하며, 첫 줄 `// ERROR:...` 마커가 있으면 그 패턴을 따른다.

실행은 프런트엔드(`--check`) 기준이며, 코드 생성 단계는 포함하지 않습니다.
