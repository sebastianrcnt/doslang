# SPEC–fec parser audit

- 날짜: 2026-08-17
- 기준 커밋: `52aaff62e490e37a0995aaaf7cbda47cf98e54a7`
- 범위: `SPEC.md` §6과 `fec/src/lexer.c`, `fec/src/parser.c`

## 현재 문제

| ID | SPEC | 현재 구현 | 재현 결과 |
|---|---|---|---|
| PARSE-01 | 최상위 `comptime if` 선언 허용 | `comptime if`는 문장에서만 처리 | 최상위 사용을 `expected declaration`으로 거부 |
| PARSE-02 | 타입 이름은 `ident ('.' ident)*` | 타입에서 점 하나만 처리 | `alpha.beta.Gamma`를 파싱하지 못함 |
| PARSE-03 | `catch` EBNF가 binding 없는 block과 binding 뒤 expression도 허용 | 짧은 `catch expr`과 `catch \|e\| block`만 처리 | 구현은 §4.6 설명과 맞고 §6.1 EBNF가 지나치게 넓음 |
| PARSE-04 | `\|`와 `^`는 같은 우선순위, 단항 비트 NOT은 `~` | 각각 우선순위 5와 6, `~` 토큰 없음, 단항 `^` 허용 | `~x`를 거부하고 `a \| b ^ c`를 `a \| (b ^ c)`로 파싱 |
| PARSE-05 | 전역 `static`과 `var`의 타입 필수 | 타입 표기를 선택적으로 처리하고 초기값에서 추론 | `static A = 1;`, `var B = 2;` 모두 검사 통과 |
| PARSE-06 | error code는 정수 literal | 일반 expression을 파싱하며 literal이 아니면 code 검증을 건너뜀 | `error E { Bad = 1 + 2, }`가 검사 통과 |
| PARSE-07 | struct field와 enum vfield의 쉼표 필수 | 닫는 `}` 바로 앞에서는 쉼표 생략 허용 | `struct S { x: i32 }`가 검사 통과 |

## 검증

- `uv run python tests/run.py`: `240/240` 통과
- 위 항목의 최소 입력을 현재 `fec`에 직접 넣어 파싱 및 `--check` 결과를 확인함
- 기존 fixture에는 위 괴리를 직접 고정하는 사례가 없음

이 문서는 조사 시점의 구현 상태를 기록한다. 언어 규칙의 기준은 `SPEC.md`다.
