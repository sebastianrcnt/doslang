# 제네릭

이 디렉터리는 제네릭 타입/함수/인스턴스화 관련 고정 fixture입니다.

실행 방법:

`uv run python tests/run.py -k generic`

현재 수록 케이스:

- `okalias.fe`, `okbox.fe`, `okdedup.fe`, `okid.fe`, `okisint.fe`
- `okmulti.fe`, `oknested.fe`, `okpair.fe`, `okscope/main.fe`, `okscope/lib.fe`
- `oksamrec.fe`, `okskip.fe`, `oktypeeq.fe`
- `badarity.fe`, `badarg.fe`, `badbody.fe`, `baddepth.fe`, `baddist.fe`
- `badfew.fe`, `badinfer.fe`, `badop.fe`, `badtype.fe`, `badvalue.fe`
- `defscope/main.fe`, `defscope/lib.fe`

미구현/미완료 구간:

현재 다수의 제네릭 케이스가 통과하지 않습니다.
