# Ferro 언어 명세 v0.1.5

DOS용 시스템 프로그래밍 언어. C만큼 빠르고, 메모리 안전성을 함수 단위 지역 검사만으로 보장한다.
파일 확장자 `.fe`, 컴파일러 이름 `fec`, 심볼 파일 `.fei`.

이 문서는 언어 명세 + 컴파일러 구현 지시서를 겸한다. 구현 중 애매한 부분은 §1 철학과 §5 소유권 규칙을 기준으로 결정한다.

---

## 1. 설계 철학

1. **안전은 기본, 위험은 명시.** 기본 코드는 메모리 안전(널 역참조, 버퍼 오버런, use-after-free, 이중 해제 불가). 위험한 연산은 `unsafe {}` 블록 안에서만.
2. **전역 분석 금지.** 모든 검사(타입, 소유권, 참조)는 함수 하나만 보고 완결되어야 한다. 이 제약이 라이프타임 표기를 없애고, 640KB 머신에서 셀프호스팅을 가능하게 한다.
3. **숨은 비용 없음.** 힙 할당, 복사, 소멸자 호출, 형변환이 전부 소스에 보인다. GC 없음, 예외 없음, 암묵 변환 없음.
4. **읽히는 문법.** `이름: 타입` 순서, 좌→우 파싱, LL(1) 재귀하강으로 처리 가능.
5. **작게 시작.** 기능을 넣기 전에 뺄 이유를 먼저 찾는다. 뺀 것과 그 대체 수단은 §13에 기록한다.
6. **기존 도구체인 재사용.** 링커, `.OBJ`/`.LIB`/`.EXE` 포맷, DPMI 익스텐더를 새로 만들지 않는다.

---

## 2. 타깃

| | `bits16` | `bits32` |
|---|---|---|
| CPU/모드 | 8086 리얼모드 | 386 보호모드 플랫 (DPMI) |
| `usize`/`isize` | 16비트 | 32비트 |
| 포인터 크기 | near 2B / far 4B | 4B (far 없음) |
| 메모리 모델 | small, large | flat |
| 시스템 호출 | INT 21h 직접 | DPMI 서비스 + 실모드 콜백 |

- CLI: `fec main.fe --target=bits16|bits32 [--model=small|large] [--strip-error-names]`
- 소스 분기: `comptime if @bits == 16 { ... } else { ... }`
- `bits32`에서 `far` 키워드를 쓰면 컴파일 에러.
- 표준 라이브러리는 코어 공용, `sys` 유닛만 타깃별 구현.

---

## 3. 어휘 구조

- 식별자: `[A-Za-z_][A-Za-z0-9_]*`. 대소문자 구분.
- 주석: `//` 줄 끝까지, `/* */` **중첩 허용**.
- 정수 리터럴: `123`, `0xFF`, `0b1010`, `0o17`, 자릿수 구분 `1_000_000`.
- 문자 리터럴: `'a'`, `'\n'`, `'\x41'` → 타입 `char`.
- 문자열 리터럴: `"abc"` → 타입 `str`. NUL 종료 아님. 이스케이프는 문자 리터럴과 동일. 인접 리터럴 자동 연결 없음.
- 불린: `true`, `false`. 옵셔널 널: `null`.
- 세미콜론 필수. 블록 중괄호 필수(단문 `if`도 `{}` 필요).

**예약어:**
```
unit import pub fn struct packed enum error const static var let
if else while for in match return break continue defer
unsafe critical shared atomic comptime asm try catch as extern interrupt interrupt_safe far
true false null undefined self Self type
and or not orelse
```

---

## 4. 타입 시스템

### 4.1 기본 타입

- 정수: `i8 i16 i32 u8 u16 u32 usize isize`
- `bool` (1바이트, 정수와 상호 변환 없음)
- `char` (`u8`과 크기 같지만 별개 타입). `char`와 `u8` 사이의 저장·대입·비교에는
  반드시 명시적인 `as` 변환이 필요하며, 리터럴에도 문맥 기반 암묵 변환을 적용하지 않는다.
- `void` (반환 타입으로만)
- `type` (comptime 파라미터에서만, §9)

**정수 규칙:**
- 서로 다른 정수 타입 간 암묵 변환 없음. `as`로 명시.
- `as`는 절단/부호확장을 수행하며 값 손실을 검사하지 않는다.
- `+ - * / %`는 검사 빌드에서 오버플로 시 트랩. `+% -% *%`는 랩어라운드(항상 무검사).
- `/`, `%`의 0 나눗셈은 항상 트랩(검사 빌드 여부 무관, CPU가 트랩함).
- 시프트 `<< >>`: 우변은 `u8`. 시프트 양이 비트폭 이상이면 검사 빌드에서 트랩.
- 비트 연산 `& | ^ ~`는 같은 타입끼리만.

### 4.2 복합 타입

| 문법 | 의미 | 표현 |
|---|---|---|
| `[N]T` | 배열, 값 타입, N은 컴파일타임 상수 | `N * sizeof(T)` |
| `[]T` | 슬라이스 (참조성, §5 R4 적용) | `(ptr, len)` |
| `str` | `[]u8` 불변 별칭 | `(ptr, len)` |
| `^T` | 소유 포인터 (힙, 단일 소유자) | 포인터 |
| `&T` | 공유 참조 | 포인터 |
| `&mut T` | 배타 참조 | 포인터 |
| `*T` | raw 포인터 (`unsafe`에서만 역참조) | 포인터 |
| `far ^T`, `far *T`, `far &T` | far 포인터 (`bits16` 전용) | 4바이트 |
| `?T` | 옵셔널 | 널 표현 가능 타입은 크기 동일, 아니면 `(bool, T)` |
| `E!T` / `!T` | 에러 유니온 (`!T`는 기본 에러 집합) | `(u16 err, T val)` |
| `fn(A, B) -> R` | 함수 포인터 | 포인터 |

- **배열은 포인터로 붕괴하지 않는다.** 함수에 넘기려면 `arr[..]`로 슬라이스를 만들거나 `&arr` / `^[N]T`를 쓴다.
- 슬라이싱: `arr[..]`, `arr[a..b]`(반개구간, 경계 검사), `arr[a..]`, `slice[a..b]`.
- 소유 슬라이스가 필요하면 `^[]T`(길이 있는 힙 버퍼)를 쓴다. `mem.alloc_slice(T, n)`가 반환.

### 4.3 구조체

```fe
pub struct Point {
    x: i32,
    y: i32,

    pub fn new(x: i32, y: i32) -> Point { return Point{ x: x, y: y }; }
    pub fn len2(self: &Self) -> i32 { return self.x*self.x + self.y*self.y; }
    pub fn shift(self: &mut Self, dx: i32) { self.x += dx; }
}
```

- 리터럴: `Point{ x: 1, y: 2 }`. 모든 필드 명시 필수(기본값 없음).
- 메서드는 struct 블록 안에 정의. 첫 파라미터가 `self: Self | &Self | &mut Self`면 메서드.
- `x.f(y)`는 `Point.f(x, y)`의 설탕. 자동 참조 취함(`x.shift(1)`은 `Point.shift(&mut x, 1)`).
- `Self`는 자기 타입의 별칭.
- 필드 레이아웃은 선언 순서. 정렬은 타깃 규칙(`bits16`은 1바이트 정렬, `bits32`는 자연 정렬). `packed struct`로 정렬 강제 해제.
- 소멸자: `fn drop(self: &mut Self)`를 정의하면 스코프 종료 시 자동 호출(§5 R3).

### 4.4 열거형 (태그드 유니온)

```fe
pub enum Shape {
    Empty,
    Circle(i32),
    Rect{ w: i32, h: i32 },
}
```

- 표현: `struct { u8 tag; union {...} payload; }`. 배리언트 256개 초과 시 `u16` 태그.
- 페이로드 없는 배리언트만 있는 열거형은 정수처럼 취급되며 `as u8` 가능.
- 생성: `Shape.Circle(5)`, `Shape.Rect{ w: 3, h: 4 }`, `Shape.Empty`.
- 해체는 `match` 또는 `if let`으로만. 직접 필드 접근 불가.

### 4.5 옵셔널

```fe
var p: ?^Node = null;
if let Some(node) = p { node.value = 1; }   // node: &mut Node (p가 mut일 때)
let v = p.?;      // null이면 트랩
let v = p orelse default_node;              // null이면 우변
```

- `?T`에서 T가 `^T`, `&T`, `*T`, `fn`이면 널 포인터를 널 표현으로 사용(크기 증가 없음).
- 검사 없이 역참조 불가. `p.^`는 컴파일 에러, `p.?.^`가 필요.

### 4.6 에러

```fe
pub error IoError {
    NotFound = 1,
    Denied = 2,
    Eof = 3,
}

fn read_all(path: str) -> IoError!^[]u8 {
    let f = try io.open(path);        // 에러면 즉시 반환
    defer f.close();
    let n = f.size() catch |e| { return e; };
    ...
}
```

- `error` 선언은 `u16` 코드 집합. 코드 0은 "성공" 예약, 사용 불가.
- `E!T` 함수만 `try`/`catch` 사용 가능.
- `try e`: 에러면 현재 함수에서 즉시 반환(현재 함수도 에러 유니온을 반환해야 함).
- `e catch |x| { ... }`: 블록은 값을 만들거나 `return`/`break`로 탈출.
- `e catch default_value`: 짧은 형태.
- 서로 다른 error 타입 간 자동 변환 없음. `!T`(기본 에러 집합 `core.Error`)로 통일하거나 명시 매핑.
- 에러는 값이다. 언와인딩, 스택 추적, 소멸자 이외의 자동 정리 없음.
- 실패를 복구하지 않고 트랩으로 바꾸려면 `expr catch @trap()`을 쓴다. v0.1.2에는 별도 `must` 키워드를 두지 않는다.

`error.Name`은 선언된 error 타입을 만들지 않고 기본 `core.Error`의 이름 있는
멤버를 참조하는 익명 에러 값이다. 각 유닛은 사용한 이름을 `.fei`에 기록한다.
최종 빌드에서 모든 유닛의 이름을 합치고 중복을 제거한 뒤 이름의 바이트순으로
정렬하여 1부터 안정적인 `u16` 코드를 부여한다. 따라서 서로 다른 유닛의
`error.Name`은 같은 값이고 빌드 순서와 병렬 컴파일에도 결과가 결정적이다.
코드는 최종 링크용 생성 헤더의 심볼로 참조하므로 분리 컴파일에서도 일관된다.
이름이 65,535개를 넘으면 컴파일 에러다. 명시적인 `error` 선언은 여전히 nominal
타입이며, 같은 멤버 이름이나 숫자 코드를 가진 다른 선언 및 `core.Error`와 자동
변환되지 않는다. `error.Name`의 타입은 `core.Error`이며 `core.Error!T` 또는
축약형 `!T`를 반환하는 함수에서만 직접 반환할 수 있다.
`--strip-error-names`를 사용하면 실행 파일과 런타임 오류 문자열에서 이름을
제거하지만 숫자 코드와 `.fei`의 타입/코드 일관성 정보는 유지한다.
`fmt.write_error`는 이 정책에 따라 `core.Error` 값을 이름 또는 코드로 출력한다.

### 4.7 타입 동등성

이름 기반(nominal). 필드가 같아도 다른 이름이면 다른 타입. 별칭은 `const Alias = Type;`으로 만들며 완전 동일 취급.

---

## 5. 소유권과 참조 — 핵심 규칙

이 절이 언어의 핵심이다. 모든 규칙은 **함수 하나만 보고** 검사된다.

**R1 (단일 소유자).** 모든 값의 소유자는 정확히 하나. 변수 대입, 함수 인자 전달, 반환은 **이동(move)**이다. 이동된 변수는 이후 사용 시 컴파일 에러.

**R2 (Copy 타입).** 다음은 이동 대신 복사된다: 정수, `bool`, `char`, raw 포인터 `*T`, 참조 `&T`, 함수 포인터, 그리고 모든 필드가 Copy이면서 `drop`이 없는 struct/enum/배열. `^T`와 `&mut T`는 Copy가 아니다.

**R3 (소멸자, RAII).** `^T`는 소유자 스코프 종료 또는 재대입 시 `drop` 호출 후 해제. struct에 `fn drop(self: &mut Self)`가 있으면 그 값의 스코프 종료 시 자동 호출되며, 이어서 필드들의 drop이 선언 역순으로 호출된다. `drop`을 직접 호출하는 것은 컴파일 에러(`mem.destroy(x)` 사용). `^Self` 또는 `?^Self`를 재귀적으로 포함한 타입은 기본 필드 drop이 스택 깊이에 비례할 수 있으므로 컴파일러가 경고한다. 이런 연결 구조는 `mem.replace(&mut link, null)`로 소유 링크를 하나씩 꺼내 반복 해제하고 필드를 빈 값으로 남기는 사용자 `drop`을 정의해야 하며, `--deny-recursive-drop`으로 경고를 에러로 바꿀 수 있다.

**R4 (참조는 2급 값).** `&T`, `&mut T`, `[]T`는 다음 위치에만 존재할 수 있다:
- 함수 파라미터
- 지역 변수 (`let`/`var`)
- 표현식 안의 임시값

다음은 **컴파일 에러**다:
- struct/enum 필드의 타입
- 배열/슬라이스의 원소 타입
- 함수 반환 타입 (예외: R8)
- `^T`, `*T`의 대상 타입
- 전역 변수의 타입

이 한 줄이 라이프타임 표기 전체를 불필요하게 만든다.

**R5 (참조 수명).** 지역 참조 변수는 대상보다 오래 살 수 없다. R4 덕분에 대상은 항상 같은 함수의 지역 변수, 파라미터, 또는 전역이므로 스코프 중첩 확인만으로 검사된다.

**R6 (배타성).** `&mut x`가 살아있는 동안 `x`에 대한 다른 참조 생성, 직접 읽기/쓰기, 이동이 금지된다. `&x`(공유)는 여러 개 동시 가능하지만 그동안 `x`에 쓰기/이동 금지. 참조의 생존 구간은 **참조 변수의 스코프 끝까지**(NLL 아님). 임시 참조(`f(&x)`)는 그 문장 끝까지.

**R7 (참조 무효화).** 참조 대상이 이동되거나 재대입되면 그 참조는 이후 사용 시 에러.

**R8 (반환값 예외).** 메서드는 `-> &T` / `-> &mut T`를 반환할 수 있다. 단, 첫 파라미터가 `&Self`/`&mut Self`이고 반환 참조는 그 self에서 파생된 것이어야 하며(컴파일러가 확인), **호출 결과는 그 문장 안에서만 사용 가능**하고 지역 변수에 바인딩할 수 없다.

```fe
list.at(0).x = 5;          // OK
let r = list.at(0);        // 에러: 반환 참조를 바인딩 불가
```
장수하는 접근이 필요하면 인덱스(`usize`)나 핸들을 쓴다.

**R9 (unsafe).** `unsafe {}` 안에서만 허용: raw 포인터 역참조, `*T` ↔ `^T`/`&T` 변환, `@ptr_cast`, `@seg_ptr`, `@volatile_*`, `@port_*`, `@as_far_fn`, `@call_far`, `asm`, `*_unchecked` 함수. R1~R8은 `unsafe` 안에서도 그대로 유지된다. 특히 `unsafe`가 참조 반환·저장이나 대여 검사를 끄지 않으며, 프로그래머가 명시적으로 raw 포인터를 경유한 부분만 컴파일러의 메모리 안전 보장 밖에 놓인다.

**R10 (전역과 인터럽트 공유).** `static`은 불변이며 컴파일타임 상수 초기화만 가능하다. 일반 전역 `var`의 읽기와 쓰기는 안전하며 `unsafe`가 필요 없다. 인터럽트 핸들러와 메인 흐름이 함께 접근하는 값은 반드시 `shared var`로 선언하고 다음 규칙을 적용한다.
- 메인 흐름은 `critical {}` 안에서만 `shared var`에 접근할 수 있다. 진입 시 플래그를 저장하고 인터럽트를 막으며, 정상 종료·`return`·`break`·`continue`·에러 전파를 포함한 모든 이탈 경로에서 원래 플래그를 복원한다.
- `interrupt fn` 안에서는 `shared var`에 직접 접근할 수 있다. 일반 함수와 `interrupt_safe fn`은 호출 문맥을 알 수 없으므로 명시적 `critical` 밖의 비원자 공유 접근이 금지된다.
- `shared atomic var`는 타깃에서 한 명령으로 읽고 쓸 수 있는 정수/불린/포인터 스칼라에만 허용한다. 메인 흐름의 단일 읽기·쓰기에는 컴파일러가 필요한 최소 임계 구역을 생성한다. 복합 read-modify-write는 여전히 명시적 `critical {}`이 필요하다. 지원되지 않는 크기나 타입은 컴파일 에러다.
- `interrupt fn`은 `interrupt_safe fn`만 호출할 수 있다. `interrupt_safe fn`은 힙 할당, DOS/DPMI 서비스, 블로킹 I/O, 부동소수점, `critical` 및 안전하지 않은 함수 호출을 사용할 수 없으며 컴파일러가 함수 본문만 보고 검증한다. 이 효과는 `.fei` 시그니처에 기록한다.
- 전역에는 `^T`나 `drop` 있는 타입을 둘 수 없다. `shared`, `atomic`, `critical`, `interrupt fn`은 v0.1.2에서 `bits16` 전용이며 `bits32`에서 사용하면 컴파일 에러다.

**R11 (재귀·그래프 구조).** `^T`는 R4의 2급 참조가 아니므로 소유가 한 방향인 단방향 리스트와 트리는 필드에 저장할 수 있다. 반면 양방향 리스트·순환·일반 그래프는 역방향 필드에 `^T`를 두면 R1의 단일 소유권을 위반하고 `&T`를 두면 R4를 위반한다. 이런 구조는 아레나/배열이 값을 소유하고 `u16`/`u32` 인덱스 핸들이 간선을 나타내도록 구현한다. 표준 라이브러리 `mem.Arena`를 사용할 수 있으며, 핸들 역참조 때 세대 번호 또는 경계 검사를 사용해 해제된 항목 접근을 막아야 한다.

---

## 6. 문법

### 6.1 EBNF

```
unit        := 'unit' ident ';' import* decl*
import      := 'import' ident ';'

decl        := ['pub'] (fn_decl | struct_decl | enum_decl | error_decl
                       | const_decl | global_decl)

fn_decl     := ['extern' string] [('interrupt' | 'interrupt_safe')] 'fn' ident
               '(' [param (',' param)*] ')' ['->' type] (block | ';')
param       := ['comptime'] ident ':' type
struct_decl := ['packed'] 'struct' ident '{' field* fn_decl* '}'
field       := ident ':' type ','
enum_decl   := 'enum' ident '{' variant (',' variant)* [','] '}'
variant     := ident | ident '(' type ')' | ident '{' field* '}'
error_decl  := 'error' ident '{' (ident '=' int ',')* '}'
const_decl  := 'const' ident [':' type] '=' expr ';'
global_decl := 'static' ident ':' type '=' expr ';'
             | 'var' ident ':' type '=' expr ';'
             | 'shared' ['atomic'] 'var' ident ':' type '=' expr ';'

block       := '{' stmt* '}'
stmt        := 'let' ident [':' type] '=' expr ';'
             | 'var' ident [':' type] ['=' expr] ';'
             | 'const' ident [':' type] '=' expr ';'
             | lvalue ('=' | '+=' | '-=' | '*=' | '/=' | '%='
                      | '&=' | '|=' | '^=' | '<<=' | '>>=') expr ';'
             | if_stmt | while_stmt | for_stmt | match_stmt
             | 'return' [expr] ';' | 'break' ';' | 'continue' ';'
             | 'defer' block
             | 'unsafe' block
             | 'critical' block
             | 'comptime' 'if' expr block ['else' (block | 'if' ...)]
             | 'asm' '{' asm_body '}'
             | expr ';'

if_stmt     := 'if' (expr | 'let' pattern '=' expr) block
               ['else' (block | if_stmt)]
while_stmt  := 'while' expr block
for_stmt    := 'for' ident [',' ident] 'in' expr block
match_stmt  := 'match' expr '{' arm+ '}'
arm         := pattern '=>' (expr ';' | block)
pattern     := ident                      // 배리언트, 페이로드 없음
             | ident '(' ident ')'        // 튜플형 배리언트 바인딩
             | ident '{' ident (',' ident)* '}'   // 필드형 배리언트 바인딩
             | 'Some' '(' ident ')' | 'None'
             | int_literal | '_'

type        := ident ['.' ident]
             | '?' type | '!' type | ident '!' type
             | '^' type | '&' ['mut'] type | '*' type
             | 'far' ('^' | '*' | '&' ['mut']) type
             | 'far' 'fn' '(' [type (',' type)*] ')' ['->' type]
             | '[' expr ']' type | '[' ']' type
             | 'fn' '(' [type (',' type)*] ')' ['->' type]
             | ident '(' type (',' type)* ')'      // 제네릭 인스턴스
```

### 6.2 표현식 우선순위 (낮음 → 높음)

```
1  orelse, catch
2  or
3  and
4  == != < <= > >=
5  | ^
6  &
7  << >>
8  + - +% -%
9  * / % *%
10 단항: - not ~ & &mut ^(주소아님) try
11 후위: .field  .?  .^  [i]  [a..b]  (args)  as T
12 기본: literal, ident, '(' expr ')', struct_literal, @builtin(...)
```

- `and`, `or`는 단축 평가한다. 호스트 C 방출에서는 각각 `&&`, `||`로
  매핑하며, 평가 순서와 단락 규칙은 Ferro 의미론을 그대로 유지한다.
- `as`는 후위 우선순위(단항보다 강함): `-x as i32`는 `-(x as i32)`.
- 비교 연산 체이닝 금지(`a < b < c`는 에러).

### 6.3 빌트인

```
@size_of(T) -> usize          @align_of(T) -> usize
@bits -> comptime int         @target -> comptime str
@ptr_cast(T, p) -> *T                    (unsafe)
@seg_ptr(seg: u16, off: u16) -> far *T   (unsafe, bits16)
@port_in8(p) @port_in16(p) @port_out8(p,v) @port_out16(p,v)  (unsafe)
@volatile_load(p) @volatile_store(p, v)  (unsafe)
@trap() -> never              @unreachable() -> never  (unsafe)
@line() @file()                          // 진단용

@print(fmt, ...) -> void                 // stdout, 쓰기 오류 무시
@fprint(w, fmt, ...) -> !void            // 임의 Writer
@sprint(buf: []u8, fmt, ...) -> usize    // 버퍼에 기록, 쓴 바이트 수 반환
@compile_error(msg)                      // comptime에서 항상 컴파일 에러
@as_far_fn(f) -> far fn()                // bits16 전용 함수 포인터 변환
@call_far(p: far fn())                    // bits16/unsafe 전용 호출
```

### 6.3.1 포매팅 빌트인

가변 인자를 언어에 도입하지 않는다. `@print` 계열은 **컴파일 단계에서 여러 호출로 전개되는 빌트인**이다.

```fe
@print("x={} y={x} name={s}\n", a, b, s);
```
→ lower 단계에서 다음으로 전개:
```
fmt.write_str(out, "x=");   fmt.write_int_i32(out, a);
fmt.write_str(out, " y=");  fmt.write_hex_u16(out, b);
fmt.write_str(out, " name="); fmt.write_str(out, s);
fmt.write_str(out, "\n");
```

규칙:
- 포맷 문자열은 **컴파일타임 문자열 리터럴 또는 `const`만**. 런타임 값이면 에러.
- verb: `{}` 기본(정수/bool/char/str 자동), `{x}` 16진, `{c}` 문자, `{s}` 문자열/슬라이스, `{b}` 불린. `{{`는 `{` 이스케이프.
- `{}` 개수와 인자 개수 불일치 → 컴파일 에러.
- 인자 타입에 대응하는 `fmt.write_*` 함수가 없으면 컴파일 에러(메시지에 타입명 표시).
- 자릿수/폭/정렬 지정자는 v0.1.1에 없음. 필요하면 `fmt.write_int_pad`를 직접 호출.
- `@fprint`의 첫 인자는 `&mut io.Writer`(§10의 함수 포인터 struct).
- `@print`는 stdout에 기록하며 저수준 writer 오류를 삼키고 `void`를 반환한다.
  따라서 `try @print(...)`는 컴파일 에러다.
- `@fprint`는 writer 오류를 전파하여 `!void`를 반환한다.
- `@sprint`는 버퍼가 찬 뒤의 출력이 잘리더라도 트랩하지 않고 기록된 바이트 수를
  `usize`로 반환한다.
- 전개된 `fmt.write_*` 호출은 위 반환 규칙에 맞게 lower 단계에서 오류를
  전파하거나 무시한다. `fmt.write_error`는 `core.Error`의 이름/코드를 출력한다.

`@compile_error(msg)`의 `msg`는 comptime 문자열이어야 하며, 평가되는 분기에서
항상 진단을 발생시킨다. `comptime if`의 제거되는 분기에서는 진단하지 않는다.
`@as_far_fn(f)`와 `@call_far`는 `bits16`에서만 허용된다. 전자는 함수 포인터를
`far fn()`으로 변환하고 후자는 `far fn()`을 호출한다. 둘 다 `unsafe { }` 안에서만
사용할 수 있으며, `bits32`에서는 컴파일 에러다.

### 6.4 예제

```fe
unit vga;
import sys;

const WIDTH: u16 = 320;
const HEIGHT: u16 = 200;

pub fn set_mode13() {
    unsafe { asm { mov ax, 0x0013; int 0x10; } }
}

pub fn put_pixel(x: u16, y: u16, c: u8) {
    if x >= WIDTH or y >= HEIGHT { return; }
    unsafe {
        let vram: far *u8 = @seg_ptr(0xA000, 0);
        @volatile_store(vram + (y * WIDTH + x) as usize, c);
    }
}
```

```fe
unit main;
import io;
import list;
import fmt;

fn count_lines(path: str) -> !usize {
    let f = try io.open(path, io.Read);
    defer f.close();

    var buf: [256]u8 = undefined;
    var n: usize = 0;
    while true {
        let got = try f.read(buf[..]);
        if got == 0 { break; }
        for c in buf[0..got] {
            if c.^ == '\n' as u8 { n += 1; }
        }
    }
    return n;
}

pub fn main() -> !void {
    let n = count_lines("data.txt") catch |e| {
        fmt.print_str("failed: ");
        fmt.print_int(e as u16);
        return e;
    };
    fmt.print_int(n);
}
```

---

## 7. 의미론 세부

### 7.1 변수와 초기화

- `let`은 불변, `var`는 가변 선언이다. 두 형태 모두 초기값이 있으면 타입을 추론할 수 있다. `var x: T;`와 `var x: T = undefined;`처럼 초기값이 없거나 `undefined`이면 타입 명시가 필수다.
- 모든 변수는 사용 전 초기화 필수(정적 검사). 명시적 미초기화는 `= undefined`(unsafe 아님, 단 읽기 전 쓰기 필수는 여전히 검사).
- 섀도잉 허용(같은 스코프에서 `let` 재선언).

### 7.2 제어 흐름

- `for x in slice`: `x`는 `&T`(가변 슬라이스면 `&mut T`). 값 접근은 `x.^`.
- `for i, x in slice`: `i: usize`.
- `for i in a..b`: 정수 범위.
- 이 루프 형태들은 경계 검사를 생략한다(컴파일러가 안전을 보장).
- `while`은 `bool` 조건만.
- `match`는 **완전성 검사**. 모든 배리언트를 다루거나 `_` 필요.
- `if`/`while`/`for`/`match`/`comptime if` 헤더 바로 뒤의 `{`는 항상 해당 제어 흐름의
  본문 또는 arm 블록을 시작한다. 따라서 구조체 초기화식을 헤더의 최상위 식으로 직접
  쓸 때는 `match (Point{ x: 1, y: 2 }) { ... }`처럼 괄호로 감싸 구조체 초기화의 `{`를
  명시한다. 괄호 안의 구조체 초기화는 일반 식 규칙을 따른다.
- `break`/`continue`는 가장 안쪽 루프에만 적용(레이블 없음).
- `defer block`은 스코프 종료 시 역순 실행. 소멸자와 함께 선언 역순으로 병합 실행. `return`/`break`/에러 전파 경로에서도 실행.

### 7.3 함수 호출 규약

- 기본: `bits32`는 cdecl, `bits16`은 타깃 C 컴파일러 기본.
- `extern "c" fn name(...) -> T;` — 본문 없이 선언, C 심볼과 링크. 이름 맹글링 없음. 인자/반환에 `^T`, 슬라이스, 에러 유니온 사용 불가(`*T`, `usize`만).
- `interrupt fn name()` — 모든 레지스터 보존 + `iret`. 파라미터/반환 없음. 주소는 `@as_far_fn(name)`으로 획득. 호출 제한과 공유 상태 규칙은 R10을 따른다.
- `interrupt_safe fn name(...)` — 인터럽트 문맥에서 호출 가능한 함수. ABI는 일반 함수와 같고 R10의 제한을 본문 검사로 만족해야 한다.
- 큰 struct(> 4바이트)는 숨은 포인터로 반환(C ABI 따름).

### 7.4 검사와 트랩

트랩 발생 조건: 배열/슬라이스 경계 초과, 정수 오버플로, 0 나눗셈, `?T`의 `.?` 실패, `@trap()`.

동작: `core.panic(msg: str, file: str, line: u32)` 호출 → 메시지 출력 → `sys.exit(3)`. 사용자가 `core.set_panic_handler`로 교체 가능.

`--no-checks` 빌드에서 제거되는 것: 경계 검사, 오버플로 검사, `.?` 검사.
**절대 제거되지 않는 것:** 소유권/참조 검사, 옵셔널 타입 검사, `match` 완전성 — 전부 컴파일타임이므로.

### 7.5 comptime

- `const` 선언의 초기값은 컴파일타임 평가(정수 연산, `@size_of`, `@bits`, 다른 const).
- `comptime if`는 평가되지 않는 분기를 **파싱은 하되 타입 검사/코드 생성하지 않는다**(타깃별 분기용).
- 함수의 `comptime` 파라미터는 §9 제네릭.
- 재귀 평가 깊이 제한 256, 초과 시 에러.

---

## 8. 유닛과 빌드

- 파일 하나 = 유닛 하나. 첫 줄은 `unit <이름>;`이며 파일명과 일치해야 함.
- `import bar;` → 같은 검색 경로의 `bar.fe`. 접근은 `bar.name`.
- `pub` 붙은 선언만 외부 노출. 구조체 필드도 개별 `pub` 필요.
- 순환 import 금지(에러).
- 유닛 컴파일 시 `.fei` 생성: pub 선언 시그니처, 타입 레이아웃, 제네릭 본문 토큰. 소스 해시가 같으면 재컴파일 생략.
- 검색 경로: `-I <dir>`, 기본은 현재 디렉터리 + `<fec>/std`.

```
fec main.fe --target=bits32 -o game.exe
fec main.fe --target=bits16 --model=large --no-checks -o game.exe
fec main.fe --emit-c -o out/          # 트랜스파일 결과만
fec --dump-ast main.fe
```

---

## 9. 제네릭

`comptime` 파라미터 기반 모노모피제이션.

```fe
pub struct List(T) {
    items: ^[]T,
    len: usize,

    pub fn new() -> List(T) { ... }
    pub fn push(self: &mut Self, v: T) -> !void { ... }
    pub fn at(self: &Self, i: usize) -> &T { ... }   // R8 적용
    pub fn drop(self: &mut Self) { ... }
}

fn max(comptime T: type, a: T, b: T) -> T {
    if a > b { return a; }
    return b;
}

let m = max(i32, 3, 7);
var xs: List(u8) = List(u8).new();
```

- 인스턴스화 시 타입 인자를 대입해 본문을 재검사하고 코드를 생성한다. 인스턴스 캐시 키는 `(선언, 타입 인자 목록)`.
- 제약(trait bound) 없음. 본문에서 쓰는 연산이 그 타입에 없으면 **인스턴스화 시점에** 에러(에러 메시지에 인스턴스화 위치를 표시할 것).
- `.fei`에 제네릭 본문을 토큰 스트림으로 저장, 사용처에서 재파싱.
- 재귀적 인스턴스화 깊이 제한 32.

---

## 10. 표준 라이브러리 (최소 집합)

- **core**: `panic`, `set_panic_handler`, `Error`(기본 에러 집합), `assert`.
- **mem**: `create(T) -> !^T`, `destroy(p)`, `alloc_slice(T, n) -> !^[]T`, `replace(dst: &mut T, value: T) -> T`, `copy(dst, src)`, `set(dst, v)`, `Arena{ init, alloc, reset, drop }`. `replace`는 이전 값을 이동해 반환하고 새 값으로 자리를 초기화하며 재귀 구조의 반복 drop에도 사용한다.
- **str**: `eq`, `find`, `starts_with`, `split_at`, `parse_int`, `trim`, `to_cstr(buf, s)`, `from_cstr(p)`.
- **list**: `List(T)`.
- **map**: `Map(K, V)` (오픈 어드레싱, `K`는 정수/str).
- **fmt**: `@print` 계열이 전개해 호출하는 저수준 함수 모음.
  `write_str(w, s)`, `write_int_i8/i16/i32/u8/u16/u32(w, v)`, `write_hex_u8/u16/u32(w, v)`,
  `write_char(w, c)`, `write_bool(w, b)`, `write_error(w, e)`,
  `write_int_pad(w, v, width, pad)`. `write_error`는 `--strip-error-names` 설정을
  따르며, 모든 함수는 `!void`를 반환한다.
  전부 `fn(w: &mut io.Writer, ...) -> !void` 시그니처. 사용자가 직접 호출해도 된다.
- **io**:
  - `File{ open, create, read, write, seek, size, close(=drop) }`, `stdin`, `stdout`, `stderr`.
  - `Writer` — 함수 포인터 struct (인터페이스 도입 전까지의 동적 디스패치 수단):
    ```fe
    pub struct Writer {
        ctx: *void,
        write_fn: fn(*void, []u8) -> !usize,
    }
    ```
    `File.writer(&mut self) -> Writer`, `buf_writer(buf: &mut []u8) -> Writer`, `null_writer()` 제공.
  - `Reader` — 같은 형태, `read_fn: fn(*void, []u8) -> !usize`.
  - `Writer`/`Reader`는 `*void`를 담으므로 필드 저장이 가능(2급 참조 아님). 대신 대상보다 오래 살면
    dangling이므로 **`Writer`를 만든 지역 스코프 밖으로 내보내지 않는 것**이 사용자 책임이며,
    `writer()` 메서드는 R8(참조 반환) 대상이 아니라 값 반환이라 컴파일러가 막지 않는다.
    v0.2에서 `dyn Writer`로 대체되면 이 구멍이 닫힌다.
- **sys**: `exit`, `args`, `env`, `ticks`, `int21(regs)`, `dpmi_*`(bits32), `port_in/out`, `far_copy`(bits16).

`io.File`은 `drop`에서 핸들을 닫는다. 이중 닫기는 소유권 규칙이 막는다.

---

## 11. 컴파일러 구현

### 11.1 부트스트랩 전략

1. **컴파일러 A** — C89로 작성. Ferro → C 트랜스파일러. 호스트는 현대 PC 또는 DOS. 출력 C는 DJGPP(gcc, bits32) / Open Watcom(bits16, bits32) / Borland C(bits16)로 컴파일.
2. **컴파일러 B** — Ferro로 A와 동일 구조를 재작성. A로 빌드.
3. **셀프호스팅** — B로 B를 빌드. 그 결과로 다시 B를 빌드해 출력이 바이트 동일(fixpoint)하면 완료. A 폐기.
4. **네이티브 백엔드** — B에 386 코드 생성기 추가, 이후 8086 코드 생성기.

A는 버릴 코드다. 최적화하지 말고 B를 컴파일할 수 있는 최소 언어 부분집합만 지원한다.

### 11.2 파이프라인

```
소스 → lexer → parser(AST) → resolve(이름/import) → check(타입)
     → own(소유권·참조) → lower(소멸자/defer/try/for 전개 → LIR)
     → emit_c(C 소스)  [또는 emit_x86]
```

각 단계는 실패해도 가능한 한 진행해 에러를 모아 보고한다(문장 단위 복구).

### 11.3 디렉터리

```
fec/
  src/
    lexer.c/h      토큰화. 위치(파일, 줄, 열) 보존.
    ast.c/h        노드 정의, 아레나 할당자.
    parser.c/h     LL(1) 재귀하강. 에러 복구는 다음 ';' 또는 '}'까지 스킵.
    types.c/h      타입 인터닝(포인터 비교로 동등성), 레이아웃 계산(타깃별).
    resolve.c/h    스코프 체인, 심볼 테이블, import, .fei 읽기/쓰기.
    check.c/h      타입 검사, 리터럴 타입 결정, match 완전성, R4 위치 검사.
    own.c/h        §11.5 알고리즘.
    lower.c/h      AST → LIR. 소멸자/defer 삽입, try/catch/for/메서드 호출 전개.
    emit_c.c/h     LIR → C. §11.4 규칙.
    generic.c/h    인스턴스 캐시, 토큰 재파싱.
    driver.c       CLI, 유닛 의존 순서, .fei 캐시, 외부 C 컴파일러 호출.
  rt/              런타임 (C): trap, 힙, 슬라이스 헬퍼, DPMI/INT21 shim
  std/             표준 라이브러리 (.fe)
  tests/           §12
```

### 11.4 C 방출 규칙

| Ferro | C |
|---|---|
| `i16`, `u32` 등 | `int16_t`, `uint32_t` (`<stdint.h>` 없으면 자체 typedef) |
| `usize` | `uint16_t`(bits16) / `uint32_t`(bits32) |
| `bool` | `unsigned char` |
| `^T`, `*T` | `T*` |
| `&T` | `const T*` |
| `&mut T` | `T*` |
| `far X` | `__far X` (Watcom/Borland), bits32는 무시 |
| `[N]T` | `struct { T a[N]; }` (값 의미론 유지, 붕괴 방지) |
| `[]T` | `typedef struct { T* p; fe_usize n; } fe_slice_T;` |
| `?T` (포인터류) | 원래 포인터, null 사용 |
| `?T` (그 외) | `struct { unsigned char has; T v; }` |
| `E!T` | `struct { uint16_t e; T v; }`, `!void`는 `uint16_t` |
| struct | `struct fe_<unit>_<Name>` |
| enum | `struct { uint8_t tag; union { ... } u; }` |
| 함수 | `fe_<unit>_<name>`, 메서드는 `fe_<unit>_<Type>_<name>` |
| 제네릭 인스턴스 | `fe_<unit>_<Name>__<타입인자맹글>` |

세부:
- **오버플로 검사**: `fe_add_i16(a, b, LINE)` 인라인 함수. `--no-checks`면 매크로가 `((a)+(b))`로 축약.
- **경계 검사**: `fe_idx_T(s, i, LINE)` → `(i < s.n ? s.p[i] : (fe_trap_bounds(LINE), s.p[0]))`. `for` 루프는 직접 인덱스.
- **`try`**: `{ Ttmp t = expr; if (t.e) return (RetT){ t.e }; }` 후 `t.v` 사용. defer/소멸자가 있으면 return 전에 정리 코드 삽입.
- **`catch`**: `t.e`가 참일 때 블록 실행, 바인딩 변수는 `t.e`.
- **`defer`/소멸자**: lower 단계에서 스코프 종료 지점(정상 흐름, `return`, `break`, `continue`, `try` 전파)마다 역순 호출을 명시적으로 삽입. C의 goto 라벨을 써도 되고 복제해도 된다(A는 복제, B는 goto 권장).
- **조건부 이동**: 이동 여부가 분기에 따라 다르면 `unsigned char fe_live_<var> = 1;` 플래그 삽입, drop 전에 검사.
- **`match`**: `switch (x.tag)`. 페이로드 바인딩은 지역 변수로 복사 또는 포인터.
- **`asm`**: Intel 문법으로 고정 저장. Watcom/Borland는 그대로, gcc는 `__asm__(".intel_syntax noprefix\n" ...)`로 감싼다.
- **`@print` 계열**: emit 단계에는 도달하지 않는다. lower 단계에서 이미 `fmt.write_*` 호출 나열로 전개되므로 emit_c는 일반 함수 호출로만 본다. `@print`는 각 호출의 오류 코드를 명시적으로 버리고 `void`가 되며, `@fprint`는 첫 오류를 전파하고, `@sprint`는 남은 버퍼 길이를 추적해 잘라 쓴 뒤 실제 길이를 반환한다. 포맷 문자열 조각은 각각 static const 문자열 리터럴로 방출하고 동일 문자열은 중복 제거.
- **논리 연산**: `and`, `or`, `not`은 각각 C의 `&&`, `||`, `!`로 방출한다. `and`와 `or`는 C의 시퀀스 포인트와 단축 평가를 그대로 사용한다.
- **임계 구역**: bits16의 `critical`은 진입 시 FLAGS를 저장한 뒤 `cli`하고 모든 이탈 경로에서 저장한 FLAGS를 복원한다. `shared atomic var`의 단일 접근도 같은 보존형 시퀀스를 사용하며 무조건 `sti`하지 않는다.
- **방출 순서**: typedef 전방선언 → struct 정의(의존 위상 정렬) → 전역 → 함수 프로토타입 → 함수 본문.
- 유닛 하나당 `.c` 하나, `.fei`에서 필요한 부분은 `.h`로 생성.

### 11.5 own.c 알고리즘

함수 단위. 각 지역 변수/파라미터에 상태:
```
Uninit | Owned | Moved | MaybeMoved | Shared(n) | Exclusive
```

1. AST를 문장 순서로 순회하며 상태 전이.
2. 표현식 평가 시 lvalue 사용을 분류: 읽기 / 이동 / `&` 대여 / `&mut` 대여 / 쓰기.
3. 이동: `Owned → Moved`. `Moved`/`MaybeMoved` 사용 시 에러(최초 이동 위치를 에러에 표시).
4. `&x`: `Owned → Shared(n+1)`. `&mut x`: `Owned → Exclusive`. 해제는 참조 변수의 스코프 끝(임시 참조는 문장 끝).
5. `Shared`/`Exclusive` 상태에서 쓰기/이동/재대여 시 에러(R6).
6. **분기 합류**: `if`/`match`의 각 브랜치를 독립 상태로 계산 후 병합. `Owned` + `Moved` → `MaybeMoved`(사용 에러, drop은 런타임 플래그).
7. **루프**: 본문을 2회 순회. 1회차 종료 상태를 진입 상태와 병합해 2회차 실행, 상태가 수렴하지 않으면(예: 첫 반복에서 이동) 에러.
8. R4 위반(참조를 필드/반환/힙에 저장)은 own이 아니라 check 단계에서 **타입만 보고** 거부한다.
9. R8(메서드 참조 반환)은 호출 결과를 바인딩하려는 시도를 check에서 거부.

에러 메시지 형식: `file:line:col: error: <설명>` + 관련 위치 `file:line:col: note: <최초 이동/대여 위치>`.

### 11.6 마일스톤

| # | 내용 | 완료 기준 |
|---|---|---|
| M1 | lexer, parser, AST 덤프 | `--dump-ast`가 std 소스 전체를 파싱 |
| M2 | 타입 검사 + C 방출: 정수, 함수, if, while | bits32 hello world 실행 |
| M3 | struct, enum, match, 배열, 슬라이스, 경계 검사, str | 문자열 처리 예제 통과 |
| M4 | **`@print`/`@fprint`/`@sprint` 빌트인** (§6.3.1), `io.Writer` 함수 포인터 struct | `@print`의 `void` 오류 삼킴, `@fprint`의 `!void` 전파, `@sprint`의 잘림/길이 동작과 인자 개수·타입 불일치가 컴파일 에러 |
| M5 | `^T`, drop, defer, 이동 검사 | 누수/이중해제 테스트 통과 |
| M6 | `&`, `&mut`, 배타성 검사 (own.c 전체) | R1~R8 실패 테스트 통과 |
| M7 | `?T`, `E!T`, try/catch | io 유닛 동작 |
| M8 | 유닛/import/.fei, 분리 컴파일, std 초안 | 다중 유닛 프로그램 빌드 |
| M9 | **제네릭** (모노모피제이션) | `List(T)`, `Map(K,V)`를 Ferro로 재작성 |
| M10 | bits16 타깃: far, `@seg_ptr`, 메모리 모델, asm, interrupt fn, `shared`/`atomic`/`critical`/`interrupt_safe` | QEMU FreeDOS에서 자동화된 far 포인터·인터럽트 공유 상태 테스트 통과(VGA 데모는 수동/멀티모달 검증 대상이라 완료 게이트에서 제외) |
| M11 | 컴파일러 B를 Ferro로 작성, A로 빌드 | B가 M1~M10 테스트 통과 |
| M12 | 셀프호스팅 fixpoint | B(B(B)) == B(B) 바이트 동일, A 폐기 |
| M13 | 386 네이티브 백엔드 | gcc 없이 빌드, 컴파일 속도 10배 |
| M14 | 8086 네이티브 백엔드 | Watcom 없이 bits16 빌드 |

배치 근거:
- **M4(포매팅)를 앞에 두는 이유**: 구현이 작고(check + lower 합쳐 300줄 안팎) 언어 표면에 새 개념을 추가하지 않는다. 이후 모든 마일스톤의 디버깅과 M11의 컴파일러 B 에러 출력이 여기에 의존한다.
- **M9(제네릭)가 M10보다 앞인 이유**: 제네릭 없이 표준 라이브러리를 쓰는 기간을 최소화한다.
- **인터페이스(`dyn`)는 마일스톤에 없다**: 부트스트랩 경로에 불필요하고(컴파일러 B는 인터페이스 없이 작성 가능), 타입 시스템 전반에 영향을 준다. §13의 v0.2 1순위로 미룬다. 그때까지 `io.Writer`/`io.Reader` 함수 포인터 struct로 대체한다.

---

## 12. 테스트

```
tests/
  pass/*.fe      + *.expected   컴파일→실행→stdout 비교
  fail/*.fe                     첫 줄 "// ERROR:<line>:<메시지 일부>"
  run16/*.fe                    bits16 빌드 후 QEMU FreeDOS 실행, 출력 파일 비교
  boot/                         A/B 출력 비교, fixpoint 검증
```

- `fail/`은 규칙별 최소 3개: R1(이동 후 사용), R3(직접 drop 호출), R4(필드에 참조), R5(스코프 초과), R6(배타성 위반), R7(무효화), R8(참조 반환 바인딩), R9(unsafe 밖 raw 역참조), match 완전성, 암묵 변환, 타입 불일치.
- 포매팅(§6.3.1) 전용 `fail/` 케이스: `{}` 개수 > 인자 개수, 인자 개수 > `{}` 개수, 미지원 verb(`{q}`), 런타임 값 포맷 문자열, 대응 `write_*` 없는 타입(예: struct), 닫히지 않은 `{`, `try @print(...)`(void).
- 포매팅 `pass/` 케이스: 각 verb 1개 이상, `{{` 이스케이프, 인자 0개, `@print`의 오류 삼킴, `@sprint` 반환 길이/잘림 검증, `@fprint`를 `io.buf_writer`로 호출.
- `@compile_error`, `@as_far_fn`, `@call_far`의 comptime/타깃/unsafe 제약과 `error.Name`의
  `core.Error` 등록, 결정적 코드, `--strip-error-names`, `fmt.write_error`를 각각 pass/fail로 검증한다.
- 각 마일스톤은 해당 기능의 pass/fail 테스트와 함께 완료한다.
- 회귀 실행: `make test` — 전 타깃 전 테스트.

---

## 13. 의도적으로 제외한 기능

**등급 정의**
- `영구` — §1 철학과 정면 충돌. v2.0에서도 넣지 않는다.
- `구조적 불가` — 넣으면 R4를 풀어야 하고 전역 분석이 생겨 셀프호스팅 목표가 깨진다. 이 언어의 정의상 불가.
- `v0.2` — 넣을 예정. 순서 문제일 뿐 원칙 위반 아님.
- `편의` — 원칙 위반 없음, 구현도 쉬움. 여유 생기면 아무 때나.

| 기능 | 등급 | 제외 이유 | 대체 수단 |
|---|---|---|---|
| 트레잇/인터페이스 (`dyn`) | **v0.2 (1순위)** | 부트스트랩에 불필요, 타입 시스템 전반에 영향 | 함수 포인터 struct (`io.Writer`, §10) |
| 클로저 | v0.2 | 캡처 = 참조 저장 = R4 위반 소지 | 콜백에 `ctx: *void` 전달 |
| 연산자 오버로딩 | v0.2 (인터페이스 이후) | 숨은 비용. 넣더라도 특정 인터페이스 구현으로만 제한 | 메서드 |
| 튜플 / 다중 반환 | 편의 | 이름 없는 필드는 가독성 손해 | struct |
| 레이블 있는 break | 편의 | — | 플래그 변수 |
| 슬라이스 패턴 매칭 | 편의 | — | 인덱스 비교 |
| `inline fn` | 편의 | — | C 방출 시 `static inline` |
| `must` 키워드 | 편의 | 실패를 트랩으로 바꾸는 문법 설탕일 뿐 핵심 의미론이 아님 | `expr catch @trap()` |
| 라이프타임 표기 (`'a`) | **구조적 불가** | 전역 분석 필요, R4를 풀어야 함 | R4 (2급 참조), 인덱스 핸들 |
| 선점형 스레드 | 구조적 불가 | DOS 기본 실행 모델에 없고 함수 단위 소유권 모델을 넘어서는 동기화가 필요 | — |
| 매크로 / 전처리기 | **영구** | 도구 지원과 컴파일 속도 파괴 | `const`, `comptime if`, 제네릭, `@print` |
| 예외 | 영구 | 언와인딩 기반 시설 없음, 숨은 비용 | 에러 유니온 |
| GC | 영구 | 결정적 비용 원칙 위반 | 소유권 + RAII + 아레나 |
| 암묵 형변환 | 영구 | 버그 원인 1위 | `as` |
| 상속 | 영구 | 숨은 vtable, 취약한 기반 클래스 | 합성 |

### 13.1 인터페이스 설계 스케치 (v0.2 예정)

지금 구현하지 않되, 나중에 `io.Writer` 함수 포인터 struct를 무리 없이 대체할 수 있도록 방향만 고정해 둔다.

```fe
pub interface Writer {
    fn write(self: &mut Self, buf: []u8) -> !usize;
}
impl Writer for File { ... }

fn dump(w: &mut dyn Writer, data: []u8) -> !void { ... }
dump(&mut file, buf);          // &mut File → &mut dyn Writer 자동 변환
```

- `dyn I`의 표현은 `(ctx, vtable)` 팻 포인터. vtable은 `(인터페이스, 구현 타입)` 쌍마다 `static const` 하나.
- **동적 디스패치 전용.** 제네릭 타입 제약(trait bound)으로는 쓸 수 없다 — 그걸 허용하면 전역 분석이 생긴다.
- `&dyn I`는 참조이므로 R4가 적용된다(필드 저장 불가). 필드에 담으려면 `^dyn I`(힙 박싱).
- `^dyn I`의 drop은 vtable 경유. 이 때문에 `?^dyn I`, drop 전개, 제네릭 인자로서의 `dyn` 등 타입 시스템 여러 곳에 케이스가 추가되므로 독립 마일스톤으로 다룬다.
- 도입 시 `io.Writer`/`io.Reader`는 `dyn`으로 교체하고, 함수 포인터 struct 버전은 제거한다(`*void`가 만드는 dangling 구멍이 닫힌다).

위 표에 없는 항목(링크타임 최적화, 디버그 정보 포맷, 언어 서버 등)은 도구 영역이며 v0.2 이후 별도 검토.
