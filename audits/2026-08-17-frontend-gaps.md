# Frontend gap audit

Status: resolved
Resolved-By: b4f947b

11 건 전부와 `0b`/`0o` 리터럴까지. 모두 구현 쪽이었다. fixture 는 아래 표에
적었다. 본문은 조사 시점 그대로다.

- 날짜: 2026-08-17
- 기준 커밋: `6dc298d828872409fdf6b7d2e85830f18a118d9f`
- 범위: parser, checker, 전역 lowering의 경계

## 재현된 문제

아래 최소 입력은 발견 시점의 `fec --check`를 모두 통과했다.

| ID | 문제 | 필요한 fixture |
|---|---|---|
| FRONT-01 | runtime 호출을 `const` 초기값으로 허용하고 lowering에서 초기값을 방출하지 않음 | `types/badcini.fe` |
| FRONT-02 | runtime 호출을 `static` 초기값으로 허용하고 저장소를 0으로 초기화 | `types/badsini.fe` |
| FRONT-03 | bool 비교 체이닝 허용: `true == false == true` | `types/badchain.fe` |
| FRONT-04 | 괄호 없는 단항식 뒤 cast 허용: `-x as u32` | `types/badunas.fe` |
| FRONT-05 | `unsafe` 밖에서 `asm` 허용 | `types/badasm.fe` |
| FRONT-06 | ABI 문자열 없는 `extern fn f();` 허용 | `types/badexns.fe` |
| FRONT-07 | `extern "c"` 이외 ABI 문자열 허용 | `types/badexab.fe` |
| FRONT-08 | extern 함수 본문 허용 | `types/badexbd.fe` |
| FRONT-09 | `extern`이 아닌 본문 없는 `fn f();`를 외부 심볼로 처리 | `types/badfnsm.fe` |
| FRONT-10 | 빈 enum 선언 허용 | `parse/bademen.fe` |
| FRONT-11 | 빈 error 선언 허용 | `parse/bademer.fe` |

중복 struct field와 중복 enum variant 선언도 통과했지만, 중복 선언 규칙을 SPEC에서 먼저
확정해야 하므로 위 목록에는 넣지 않았다.

## 해결

SPEC 은 FRONT-03·04(§6.2), 05(§5 R9), 06~09(§6.1·§7.3), 10·11(§6.1)을 이미
옳게 적고 있었다. 구현만 따라가지 않았다. FRONT-01·02 는 SPEC 에도 규칙이
없어서 §7.1 에 문장을 넣었다 -- 전역 초기값은 컴파일 시점에 알 수 있어야 한다.

| ID | fixture |
|---|---|
| FRONT-01 | `types/badcini.fe` |
| FRONT-02 | `types/badsini.fe` |
| FRONT-03 | `types/badchain.fe` |
| FRONT-04 | `types/badunas.fe` |
| FRONT-05 | `types/badasm.fe` |
| FRONT-06 | `types/badexns.fe` |
| FRONT-07 | `types/badexab.fe` |
| FRONT-08 | `types/badexbd.fe` |
| FRONT-09 | `types/badfnsm.fe` |
| FRONT-10 | `parse/bademen.fe` |
| FRONT-11 | `parse/bademer.fe` |
| 허용되는 짝 | `types/okglobin.fe` |
| `0b`/`0o` | `exec/radix.fe` |

`-x as T` 와 비교 체이닝을 구별하려면 괄호가 트리에 남아야 해서 노드에
`FE_NODE_PAREN` 을 두었다. 파싱 뒤에는 `-x as T` 와 `-(x as T)` 가 같은
트리다.

남은 것: `parse/` fixture 가 트리 내용을 비교하지 않는다는 지적은 그대로
유효하다. 우선순위는 지금 `exec/bitnot.fe` 처럼 실행 결과로 구별한다.

## 이미 알려진 실행 문제

`0b`와 `0o` 리터럴은 lexer가 받지만 값 계산이 진법을 반영하지 않는다. 실행 결과를
고정하는 `exec/radix.fe`가 필요하다.

## 테스트 기반의 빈틈

`parse/` fixture는 `--dump-ast`의 성공 여부만 검사하고 트리 내용은 비교하지 않는다.
따라서 연산자 우선순위나 postfix 결합 방향은 parse fixture만으로 고정되지 않는다.
이런 항목은 실행 결과로 구별하거나 선택적인 AST 기대값 검사를 추가해야 한다.

## 검증

- `uv run python tests/run.py`: `245/245` 통과
- 각 문제를 독립적인 최소 입력으로 만들어 `--check` 결과를 확인함
- 조사용 임시 입력은 제거함

이 문서는 발견 시점의 상태를 기록한다. 작성 중인 미커밋 수정으로 일부 항목의 상태가
바뀔 수 있으므로 해결 여부는 fixture와 두 테스트 suite로 확인한다.
