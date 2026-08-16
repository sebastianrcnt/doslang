# 단위 모듈

이 디렉터리는 `unit` 선언, `import` 경로/별칭, 다중 파일 단위 경계를 다룹니다.

실행 방법:

`uv run python tests/run.py -k units`

현재 수록 케이스:

- `alias/`
- `basic/`
- `badlong/`
- `badupper/`
- `bindconf/`
- `cycle/`
- `dotpriv/`
- `dotted/`
- `errdet/`
- `errnom/`
- `errsame/`
- `missing/`
- `privfld/`
- `privfn/`
- `pubfld/`
- `pubpriv/`
- `unitbad/`

미구현/미완료 구간:
현재 다수 케이스가 완전 통과하지 않고, `badlong`, `badupper`, `unitbad`는 규칙 고의 위반 케이스입니다.
