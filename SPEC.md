# Ferro 언어 명세 v0.1

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

- CLI: `fec main.fe --target=bits16|bits32 [--model=small|large]`
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
unit import pub fn struct enum error const static var let mut
if else while for in match return break continue defer
unsafe comptime asm try catch as extern interrupt far
true false null self Self type
```

---

## 4. 타입 시스템

### 4.1 기본 타입

- 정수: `i8 i16 i32 u8 u16 u32 usize isize`
- `bool` (1바이트, 정수와 상호 변환 없음)
- `char` (u8과 크기 같지만 별개 타입)
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

### 4.7 타입 동등성

이름 기반(nominal). 필드가 같아도 다른 이름이면 다른 타입. 별칭은 `const Alias = Type;`으로 만들며 완전 동일 취급.

---

## 5. 소유권과 참조 — 핵심 규칙

이 절이 언어의 핵심이다. 모든 규칙은 **함수 하나만 보고** 검사된다.

**R1 (단일 소유자).** 모든 값의 소유자는 정확히 하나. 변수 대입, 함수 인자 전달, 반환은 **이동(move)**이다. 이동된 변수는 이후 사용 시 컴파일 에러.

**R2 (Copy 타입).** 다음은 이동 대신 복사된다: 정수, `bool`, `char`, raw 포인터 `*T`, 참조 `&T`, 함수 포인터, 그리고 모든 필드가 Copy이면서 `drop`이 없는 struct/enum/배열. `^T`와 `&mut T`는 Copy가 아니다.

**R3 (소멸자, RAII).** `^T`는 소유자 스코프 종료 또는 재대입 시 `drop` 호출 후 해제. struct에 `fn drop(self: &mut Self)`가 있으면 그 값의 스코프 종료 시 자동 호출되며, 이어서 필드들의 drop이 선언 역순으로 호출된다. `drop`을 직접 호출하는 것은 컴파일 에러(`mem.destroy(x)` 사용).

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

**R9 (unsafe).** `unsafe {}` 안에서만 허용: raw 포인터 역참조, `*T` ↔ `^T`/`&T` 변환, `@ptr_cast`, `@seg_ptr`, `@volatile_*`, `@port_*`, `asm`, `*_unchecked` 함수, 전역 `var` 접근. R1~R8은 `unsafe` 안에서도 유지되며, raw 포인터를 경유해야만 우회된다.

**R10 (전역).** `static`은 불변이며 컴파일타임 상수 초기화만 가능. `var` 전역은 허용되나 접근이 `unsafe`. 전역에 `^T`나 `drop` 있는 타입 금지.

**R11 (그래프 구조).** 참조 순환이 필요한 자료구조는 아레나 + 인덱스 핸들로 표현한다. 표준 라이브러리 `mem.Arena`와 `u16`/`u32` 인덱스를 쓴다.

---

## 6. 문법

### 6.1 EBNF

```
unit        := 'unit' ident ';' import* decl*
import      := 'import' ident ';'

decl        := ['pub'] (fn_decl | struct_decl | enum_decl | error_decl
                       | const_decl | global_decl)

fn_decl     := ['extern' string] ['interrupt'] 'fn' ident
               '(' [param (',' param)*] ')' ['->' type] (block | ';')
param       := ['comptime'] ident ':' type
struct_decl := ['packed'] 'struct' ident '{' field* fn_decl* '}'
field       := ident ':' type ','
enum_decl   := 'enum' ident '{' variant (',' variant)* [','] '}'
variant     := ident | ident '(' type ')' | ident '{' field* '}'
error_decl  := 'error' ident '{' (ident '=' int ',')* '}'
const_decl  := 'const' ident [':' type] '=' expr ';'
global_decl := ('static' | 'var') ident ':' type '=' expr ';'

block       := '{' stmt* '}'
stmt        := 'let' ['mut'] ident [':' type] '=' expr ';'
             | 'var' ident ':' type ['=' expr] ';'
             | 'const' ident [':' type] '=' expr ';'
             | lvalue ('=' | '+=' | '-=' | '*=' | '/=' | '%='
                      | '&=' | '|=' | '^=' | '<<=' | '>>=') expr ';'
             | if_stmt | while_stmt | for_stmt | match_stmt
             | 'return' [expr] ';' | 'break' ';' | 'continue' ';'
             | 'defer' block
             | 'unsafe' block
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
             | '[' expr ']' type | '[' ']' type
             | 'fn' '(' [type (',' type)*] ')' ['->' type]
             | ident '(' type (',' type)* ')'      // 제네릭 인스턴스
```

### 6.2 표현식 우선순위 (낮음 → 높음)

```
1  orelse, catch
2  ||
3  &&
4  == != < <= > >=
5  | ^
6  &
7  << >>
8  + - +% -%
9  * / % *%
10 단항: - ! ~ & &mut ^(주소아님) try
11 후위: .field  .?  .^  [i]  [a..b]  (args)  as T
12 기본: literal, ident, '(' expr ')', struct_literal, @builtin(...)
```

- `&&`, `||`는 단축 평가.
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
```

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
            if c.^ == '\n' { n += 1; }
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

- `let`은 불변, `let mut`은 가변, `var`는 타입 명시 필수인 가변 선언.
- 모든 변수는 사용 전 초기화 필수(정적 검사). 명시적 미초기화는 `= undefined`(unsafe 아님, 단 읽기 전 쓰기 필수는 여전히 검사).
- 섀도잉 허용(같은 스코프에서 `let` 재선언).

### 7.2 제어 흐름

- `for x in slice`: `x`는 `&T`(가변 슬라이스면 `&mut T`). 값 접근은 `x.^`.
- `for i, x in slice`: `i: usize`.
- `for i in a..b`: 정수 범위.
- 이 루프 형태들은 경계 검사를 생략한다(컴파일러가 안전을 보장).
- `while`은 `bool` 조건만.
- `match`는 **완전성 검사**. 모든 배리언트를 다루거나 `_` 필요.
- `break`/`continue`는 가장 안쪽 루프에만 적용(레이블 없음).
- `defer block`은 스코프 종료 시 역순 실행. 소멸자와 함께 선언 역순으로 병합 실행. `return`/`break`/에러 전파 경로에서도 실행.

### 7.3 함수 호출 규약

- 기본: `bits32`는 cdecl, `bits16`은 타깃 C 컴파일러 기본.
- `extern "c" fn name(...) -> T;` — 본문 없이 선언, C 심볼과 링크. 이름 맹글링 없음. 인자/반환에 `^T`, 슬라이스, 에러 유니온 사용 불가(`*T`, `usize`만).
- `interrupt fn name()` — 모든 레지스터 보존 + `iret`. 파라미터/반환 없음. 주소는 `@as_far_fn(name)`으로 획득.
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
- **mem**: `create(T) -> !^T`, `destroy(p)`, `alloc_slice(T, n) -> !^[]T`, `copy(dst, src)`, `set(dst, v)`, `Arena{ init, alloc, reset, drop }`.
- **str**: `eq`, `find`, `starts_with`, `split_at`, `parse_int`, `trim`, `to_cstr(buf, s)`, `from_cstr(p)`.
- **list**: `List(T)`.
- **map**: `Map(K, V)` (오픈 어드레싱, `K`는 정수/str).
- **fmt**: `print_str`, `print_int`, `print_hex`, `format(buf, ...)`. 제네릭 도입 후 `print(fmt, args)` 추가.
- **io**: `File{ open, create, read, write, seek, size, close(=drop) }`, `stdin`, `stdout`, `stderr`.
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
| M4 | `^T`, drop, defer, 이동 검사 | 누수/이중해제 테스트 통과 |
| M5 | `&`, `&mut`, 배타성 검사 (own.c 전체) | R1~R8 실패 테스트 통과 |
| M6 | `?T`, `E!T`, try/catch | io 유닛 동작 |
| M7 | 유닛/import/.fei, 분리 컴파일, std 초안 | 다중 유닛 프로그램 빌드 |
| M8 | **제네릭** (모노모피제이션) | `List(T)`, `Map(K,V)`를 Ferro로 재작성 |
| M9 | bits16 타깃: far, `@seg_ptr`, 메모리 모델, asm, interrupt fn | DOSBox에서 VGA 데모 실행 |
| M10 | 컴파일러 B를 Ferro로 작성, A로 빌드 | B가 M1~M9 테스트 통과 |
| M11 | 셀프호스팅 fixpoint | B(B(B)) == B(B) 바이트 동일, A 폐기 |
| M12 | 386 네이티브 백엔드 | gcc 없이 빌드, 컴파일 속도 10배 |
| M13 | 8086 네이티브 백엔드 | Watcom 없이 bits16 빌드 |

M8은 M9보다 앞이다(제네릭 없이 표준 라이브러리를 쓰는 기간을 최소화).

---

## 12. 테스트

```
tests/
  pass/*.fe      + *.expected   컴파일→실행→stdout 비교
  fail/*.fe                     첫 줄 "// ERROR:<line>:<메시지 일부>"
  run16/*.fe                    bits16 빌드 후 DOSBox 실행, 출력 파일 비교
  boot/                         A/B 출력 비교, fixpoint 검증
```

- `fail/`은 규칙별 최소 3개: R1(이동 후 사용), R3(직접 drop 호출), R4(필드에 참조), R5(스코프 초과), R6(배타성 위반), R7(무효화), R8(참조 반환 바인딩), R9(unsafe 밖 raw 역참조), match 완전성, 암묵 변환, 타입 불일치.
- 각 마일스톤은 해당 기능의 pass/fail 테스트와 함께 완료한다.
- 회귀 실행: `make test` — 전 타깃 전 테스트.

---

## 13. 의도적으로 제외한 기능

| 기능 | 제외 이유 | 대체 수단 |
|---|---|---|
| 라이프타임 표기 (`'a`) | 전역 분석 필요 | R4 (2급 참조) |
| 트레잇/인터페이스 | 복잡도 대비 이득 낮음 | 함수 포인터 struct: `struct Writer { ctx: *void, write: fn(*void, []u8) -> !usize }` |
| 클로저 | 캡처 = 참조 저장 = R4 위반 | 콜백에 `ctx: *void` 전달 |
| 매크로 / 전처리기 | 도구 지원과 컴파일 속도 파괴 | `const`, `comptime if`, 제네릭 |
| 예외 | 언와인딩 기반 시설 없음, 비용 큼 | 에러 유니온 |
| GC | 결정적 비용 원칙 위반 | 소유권 + RAII + 아레나 |
| 연산자 오버로딩 | 숨은 비용 | 메서드 |
| 가변 인자 | ABI 복잡, 타입 안전 불가 | 제네릭 `comptime` 파라미터 |
| 튜플 / 다중 반환 | 이름 없는 필드는 가독성 손해 | struct |
| 암묵 형변환 | 버그 원인 1위 | `as` |
| 스레드 | DOS에 없음 | — |
| 상속 | — | 합성 |

