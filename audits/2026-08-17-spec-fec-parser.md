# SPEC–fec parser audit

Status: resolved
Resolved-By: 730bcac

일곱 전부. 구현 셋(PARSE-04a·05·06), SPEC 다섯(01·02·03·04b·07). 판정과 근거는
아래 표에 덧붙였다. 본문은 조사 시점 그대로다.

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

## 해결

| ID | 어느 쪽이 틀렸나 | 무엇을 했나 |
|---|---|---|
| PARSE-01 | SPEC | comptime 조건은 타입 술어뿐이라(§7.5) 유닛 바깥에는 물어볼 것이 없다. `comptime_decl` 을 `decl` 에서 빼고 §11 v0.2 로 |
| PARSE-02 | SPEC | `import` 는 unit path 의 마지막 segment 를 바인딩하므로 점 둘 이상인 타입 이름은 만들어질 수 없다. 문법을 `type_name := [ident '.'] ident` 로 |
| PARSE-03 | SPEC | §4.6 과 §11(블록 표현식 배제)이 실제 규칙이고 EBNF 가 넓었다. 두 형태로 나눠 적었다 |
| PARSE-04a | 구현 | `~` 를 넣었다. 렉서·파서·검사·lowering(`xor` with all ones) |
| PARSE-04b | SPEC | `\|` 와 `^` 를 한 단계로 두면 `a \| b ^ c` 가 `(a\|b)^c` 가 되어 C 에서 온 사람을 속인다. 구현(C 순서)이 옳아서 표를 쪼갰다 |
| PARSE-05 | 구현 | 전역 `static`/`var` 는 타입 필수. `const` 는 그대로 추론 |
| PARSE-06 | 구현 | error code 는 정수 리터럴 하나만 받는다 |
| PARSE-07 | SPEC | 마지막 쉼표 생략은 흔하고 `enum` 은 이미 허용하고 있었다. 명세에 적었다 |

fixture: `parse/badgtype.fe`, `parse/badecode.fe`, `parse/okglobal.fe`,
`exec/bitnot.fe`. 그리고 `tests/run.py` 가 마커를 진단 스트림에만 맞춘다 --
`--dump-ast` 모드에서 AST 덤프가 먼저 나와 마커가 못 쓰이고 있었다.

## 검증

- `uv run python tests/run.py`: `240/240` 통과
- 위 항목의 최소 입력을 현재 `fec`에 직접 넣어 파싱 및 `--check` 결과를 확인함
- 기존 fixture에는 위 괴리를 직접 고정하는 사례가 없음

이 문서는 조사 시점의 구현 상태를 기록한다. 언어 규칙의 기준은 `SPEC.md`다.
