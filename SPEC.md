# Ferro 언어 명세 v0.1.8

DOS용 시스템 프로그래밍 언어. C만큼 빠르고, 메모리 안전성을 함수 단위 지역 검사만으로 보장한다.
파일 확장자 `.fe`, 컴파일러 이름 `fec`, 심볼 파일 `.fei`.

이 문서는 Ferro 언어 명세만 다룬다. 컴파일러 구현 지시서와 표준 라이브러리 상세 명세는
별도 문서에서 다룬다. 명세 판단이 애매한 부분은 §1 철학과 §5 소유권 규칙을 기준으로 결정한다.

---

## 1. 설계 철학

1. **안전은 기본, 위험은 명시.** 기본 코드는 메모리 안전(널 역참조, 버퍼 오버런, use-after-free, 이중 해제 불가). 위험한 연산은 `unsafe {}` 블록 안에서만.
2. **전역 분석 금지.** 모든 검사(타입, 소유권, 참조)는 함수 하나만 보고 완결되어야 한다. 이 제약이 라이프타임 표기를 없애고, 진단을 위반 지점에 국소적으로 묶으며, 컴파일러를 작게 유지한다.
3. **숨은 비용 없음.** 힙 할당, 복사, 소멸자 호출, 형변환이 전부 소스에 보인다. GC 없음, 예외 없음, 암묵 변환 없음.
4. **읽히는 문법.** `이름: 타입` 순서, 좌→우 파싱, LL(1) 재귀하강으로 처리 가능.
5. **작게 시작.** 기능을 넣기 전에 뺄 이유를 먼저 찾는다. 뺀 것과 그 대체 수단은 §11에 기록한다.
6. **기존 도구체인 재사용.** 링커, `.OBJ`/`.LIB`/`.EXE` 포맷, DPMI 익스텐더를 새로 만들지 않는다.

---

## 2. 타깃

타깃은 하나다.

| | |
|---|---|
| CPU/모드 | i386 보호모드 플랫 |
| 실행 환경 | Windows 11, 그리고 DPMI 익스텐더 위의 DOS |
| 포인터 크기 | 4바이트 |
| 메모리 모델 | flat. 세그먼트 개념 없음 |
| `usize`/`isize` | 타깃의 포인터 폭 |

- 타깃이 하나이므로 타깃에 따라 갈라지는 소스는 v0.1에 없다.
- `usize`/`isize`는 **타깃의 포인터 폭**이며 특정 비트 수를 약속하지 않는다. 오늘
  그것은 32비트지만 `u32`와 자동으로 변환되지 않는다. 폭을 언어 의미론으로
  새어나가게 두지 않는 이 구분이, 나중에 다른 폭의 타깃을 여는 유일한 장치다.
- **세그먼트 주소 지정은 언어에 없다.** far 포인터는 x86 리얼모드에만 있는 개념이고,
  평평한 주소 공간을 가진 다른 32비트 프로세서에는 대응물이 없다.

### 2.1 컴파일러 자신이 도는 곳

컴파일러 `fec`도 32비트 보호모드 플랫에서 돈다 — 호스트에서든, DOS에서든 DPMI
익스텐더 위에서다. 당대의 Open Watcom 컴파일러 자신이 그렇게 돌았다.

**8086 리얼모드는 이 명세의 범위 밖이다.** 640KB는 8086의 한계가 아니라 IBM PC가
1MiB 주소 공간의 위쪽 384KB를 하드웨어에 예약해서 생긴 것이고, 그 안에 컴파일러를
넣으려면 AST를 통째로 들지 않는 스트리밍 구조와 오버레이가 필요하다. 리얼모드
타깃이 필요해지면 그것은 백엔드와 `usize` 폭의 문제이지 언어 설계의 문제가 아니다.

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
unsafe comptime asm try catch as extern
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
- `void` (반환 타입으로만. 단, 역참조 불가능한 `*void`의 대상 타입은 허용, R9)
- `type` (comptime 파라미터와 type alias의 `const` 초기값에서만, §4.7·§9)

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
| `[]T` | 공유·읽기 전용 슬라이스 (참조성, R4 적용) | `(const ptr, len)` |
| `[]mut T` | 배타·쓰기 가능 슬라이스 (참조성, R4 적용) | `(ptr, len)` |
| `str` | 미리 선언된 `[]u8`의 type alias | `[]u8`과 동일 |
| `^[]T` | 소유 버퍼. 일반 `^T`와 구별되는 독립 소유 타입 | `(ptr, len)` |
| `^T` | 일반 소유 포인터 (힙, 단일 소유자) | 포인터 |
| `&T` | 공유 참조 | 포인터 |
| `&mut T` | 배타 참조 | 포인터 |
| `*T` | raw 포인터 (`unsafe`에서만 역참조) | 포인터 |
| `?T` | 옵셔널 | 널 표현 가능 타입은 크기 동일, 아니면 `(bool, T)` |
| `E!T` / `!T` | 에러 유니온 (`!T`는 기본 에러 집합) | `(u16 err, T val)` |
| `fn(A, B) -> R` | 함수 포인터 | 포인터 |

- **배열은 포인터로 붕괴하지 않는다.** 함수에 넘기려면 `arr[..]`로 슬라이스를 만들거나 `&arr` / `^[N]T`를 쓴다.
- 슬라이싱: `arr[..]`, `arr[a..b]`(반개구간, 경계 검사), `arr[a..]`, `slice[a..b]`. `let` 배열·공유 슬라이스에서는 `[]T`, `var` 배열·배타 슬라이스에서는 `[]mut T`가 생긴다.
- `[]mut T`는 `[]T`로, `&mut T`는 `&T`로 **호출 인자 위치에서만** 암묵 재대여할 수 있다. 이것은 호출 동안의 read-only view이며 원래 배타 대여는 원래 마지막 사용까지 유지된다. 일반 `let`/대입에는 이 암묵 약화를 적용하지 않는다. 장기 shared borrow가 필요하면 root/place에서 명시적으로 새 `&` 또는 shared slice를 만들고 R6 검사를 받는다.
- `^[]T`는 "슬라이스를 가리키는 포인터"가 아니라 길이를 함께 소유하는 독립 타입이다. R4의 일반 `^T` 대상 제한의 예외이며 `?^[]T`도 허용한다. `*[]T`/`*[]mut T`는 계속 금지한다. `mem.alloc_slice(T, n)`가 반환하고 drop 시 버퍼를 해제한다.
- `str`은 nominal 타입이 아니라 미리 선언된 `const str = []u8;` type alias다. UTF-8 검증을 보장하지 않으며 문자열 리터럴은 정적 읽기 전용 `[]u8`이다. 따라서 별도 변환 규칙이나 별도 C 표현은 없다.

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
- 필드 레이아웃은 선언 순서. 정렬은 자연 정렬. `packed struct`로 정렬 강제 해제.
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
p.?.value = 1;                              // projection chain, 소유권 이동 없음
let v = mem.replace(&mut p, null).?;         // 소유값을 실제로 꺼냄
```

- `?T`에서 T가 일반 `^T`, `&T`, `*T`, `fn`이면 널 포인터를 널 표현으로 사용한다. `?^[]T`는 빈 소유 버퍼와 null을 구별해야 하므로 `(bool, ^[]T)` 표현을 사용한다.
- `null`은 독립 runtime 타입이 없다. expected type으로 정확한 optional 또는 pointer-like 타입을 하나 결정할 수 있는 위치에서만 허용한다. `let p: ?^Node = null;`과 `takes_optional(null);`은 허용하지만 `let p = null;`처럼 문맥이 없거나 둘 이상의 타입으로 해석 가능한 경우는 컴파일 에러다.
- 검사 없이 역참조 불가. `p.^`는 컴파일 에러, `p.?.^`가 필요.
- `.?`, `.field`, `[i]`는 place projection이다. projection chain은 값을 이동하지 않는다. Copy 값은 읽기에서 복사되며 비-Copy 값을 projection에서 꺼내는 것은 R7에 따라 금지한다. 실제 추출은 `mem.replace`를 사용한다.
- `orelse`는 Copy payload를 복사한다. 비-Copy optional 변수 자체에 적용하면 optional 전체를 이동하며, field/index projection의 비-Copy optional에는 직접 적용할 수 없다.
- `orelse`는 optional이 `Some`이면 우변을 평가하지 않고, `None`일 때만 우변을 평가하는 lazy 연산이다. 부수 효과·이동·대여도 실행되는 경로에만 적용한다.
- `Some`과 `None`은 `if let`과 `match`의 패턴 위치에서만 옵셔널 해체를 의미하는 문맥 키워드다. 다른 위치에서는 일반 식별자이며 §3의 예약어가 아니다. 패턴은 place를 파괴적으로 추출하지 않는다. immutable place의 payload binding은 shared borrow/view, mutable place는 필요한 mutable borrow/view이고 Copy payload만 복사할 수 있다. non-Copy payload의 소유권을 꺼내려면 `mem.replace`가 필요하며 temporary optional만 자동 소유 추출하는 예외도 없다.

### 4.6 에러

```fe
pub error ParseError {                  // 사용자 nominal error 예시
    InvalidDigit = 1,
    Overflow = 2,
}

fn read_all(path: str) -> !^[]u8 {      // 표준 io는 core.Error로 통일
    var f = try io.open(path, io.Read); // 같은 core.Error면 즉시 반환
    defer { f.close() catch @trap(); }
    let n = f.size() catch |e| { return e; };
    ...
}
```

- `error` 선언은 `u16` 코드 집합. 코드 0은 "성공" 예약이라 사용할 수 없고, 한 선언 안에서 member 이름이나 숫자 code가 중복되면 컴파일 에러다. 서로 다른 nominal error 선언은 같은 숫자 code를 사용할 수 있지만 여전히 다른 타입이다.
- expected type이 `E!T`인 위치에서는 `T` 값은 success, `E` 값은 failure를 구성한다. 함수의 `return`도 선언된 반환 타입이 `E!T`이면 같은 규칙을 쓴다. 이는 일반 implicit conversion이 아니라 error-union 전용 contextual construction이며, `E1`과 `E2` 또는 nominal error와 `core.Error` 사이의 자동 변환은 없다.
- `try`는 에러 유니온 반환 함수 안에서만 허용한다. 피연산자의 nominal error 타입은 현재 함수의 error 타입과 정확히 같아야 한다. 다르면 `catch`에서 명시적으로 매핑한다.
- `catch`는 현재 함수의 반환 타입과 무관하게 어디서든 에러를 그 자리에서 처리할 수 있다.
- `try e`: 에러면 현재 함수에서 즉시 반환한다.
- `e catch |x| { ... }`: 블록은 값을 만들 수 없다. 결과 타입이 `void`이면 정상적으로 끝까지 실행할 수 있고, 값 결과가 필요하면 `return`/`break`/`continue`로 탈출하거나 `@trap()`으로 끝나야 한다. 값이 필요하면 아래 짧은 형태를 쓴다. (언어에 블록 표현식을 도입하지 않기 위한 선택. §11 참조.)
- `e catch default_value`: 짧은 형태. 우변은 식이며 그 값이 결과가 된다.
- 짧은 `catch`의 우변과 block `catch`의 handler는 피연산자가 error일 때만 평가·실행한다. success이면 handler의 부수 효과·이동·대여가 발생하지 않는다.
- 서로 다른 error 타입 간 자동 변환 없음. `!T`(기본 에러 집합 `core.Error`)로 통일하거나 명시 매핑.
- `try`와 `catch`도 field/index/optional projection에서 non-Copy payload를 숨게 이동시키지 않는다. projection에서 소유값을 추출해야 하면 먼저 `mem.replace`로 유효한 대체값을 남긴다.
- 에러는 값이다. 언와인딩, 스택 추적, 소멸자 이외의 자동 정리 없음.
- 실패를 복구하지 않고 트랩으로 바꾸려면 `expr catch @trap()`을 쓴다. v0.1에는 별도 `must` 키워드를 두지 않는다.

`error.Name`은 선언된 error 타입을 만들지 않고 기본 `core.Error`의 이름 있는
멤버를 참조하는 익명 에러 값이다. 각 유닛은 사용한 이름을 `.fei`에 기록한다.
최종 빌드에서 **드라이버가 emit 단계 이전에** 모든 유닛의 `.fei`에서 사용된 이름을
합치고 중복을 제거한 뒤 이름의 바이트순으로 정렬하여 1부터 `u16` 코드를 부여한다.
드라이버는 이 표를 단일 생성 헤더 `fe_errors.h`의 `#define` 정수 상수로 방출한다.
따라서 서로 다른 유닛의 `error.Name`은 같은 값이고 빌드 순서와 병렬 컴파일에도
결과가 결정적이며 `switch` case 라벨로 쓸 수 있다.
이름 집합이 바뀌면 `fe_errors.h`와 그 헤더에 의존하는 오브젝트를 무효화하지만,
각 유닛의 `.c`는 재방출하지 않는다. 각 `.fei`는 그 unit이 사용하는 이름 집합만
기록하므로 global 번호 재배정 때문에 다시 쓰지 않으며, source에서 이름 사용 자체가
바뀐 unit의 `.fei` interface만 갱신한다.
빌드 디렉터리 이력에 따라 번호가 달라지는 append-only 표는 금지한다.
유닛 단위 `--emit-c`는 전체 이름 집합을 알 수 없으므로 `--error-table=<파일>`로
확정된 표를 받아야 하며, 없으면 컴파일 에러다.
이름이 65,535개를 넘으면 컴파일 에러다. 명시적인 `error` 선언은 여전히 nominal
타입이며, 같은 멤버 이름이나 숫자 코드를 가진 다른 선언 및 `core.Error`와 자동
변환되지 않는다. `error.Name`의 타입은 `core.Error`이며 `core.Error!T` 또는
축약형 `!T`를 반환하는 함수에서만 직접 반환할 수 있다.
`--strip-error-names`를 사용하면 실행 파일과 런타임 오류 문자열에서 이름을
제거하지만 숫자 코드와 `.fei`의 타입/코드 일관성 정보는 유지한다.
`fmt.fmt_error`는 이 정책에 따라 `core.Error` 값을 이름 또는 코드로 포맷한다.

### 4.7 타입 동등성과 alias

이름 기반(nominal). 필드가 같아도 다른 이름이면 다른 타입. `type` 값은 comptime 파라미터뿐 아니라 `const Alias = Type;`의 초기값에 허용하며, 이 선언은 새 nominal 타입이 아닌 완전 동일 alias를 만든다. 런타임 type 값은 없다. `str`은 이 규칙으로 미리 정의된 `[]u8` alias다(§4.2).

---

## 5. 소유권과 참조 — 핵심 규칙

이 절이 언어의 핵심이다. 모든 규칙은 **함수 하나만 보고** 검사된다.

**R1 (단일 소유자).** 모든 값의 소유자는 정확히 하나. 변수 대입, 함수 인자 전달, 반환은 **이동(move)**이다. 이동된 변수는 이후 사용 시 컴파일 에러.

**R2 (Copy 타입).** 다음은 이동 대신 복사된다: 정수, `bool`, `char`, raw 포인터 `*T`, 공유 참조 `&T`, 공유 슬라이스 `[]T`(`str` 포함), 함수 포인터. `?T`, `E!T`, `[N]T`, struct/enum은 모든 포함 값이 Copy이고 `drop`이 없을 때 재귀적으로 Copy다. `^T`, `^[]T`, `&mut T`, `[]mut T`는 Copy가 아니다.

**R3 (소멸자, RAII).** 일반 `^T`와 `^[]T`는 소유자 스코프 종료 또는 재대입 시 `drop` 호출 후 해제. struct에 `fn drop(self: &mut Self)`가 있으면 그 값의 스코프 종료 시 자동 호출되며, 이어서 필드들의 drop이 선언 역순으로 호출된다. `drop`을 직접 호출하는 것은 컴파일 에러(`mem.destroy(x)` 사용). `^Self` 또는 `?^Self`를 재귀적으로 포함한 타입은 기본 필드 drop이 스택 깊이에 비례할 수 있으므로 컴파일러가 경고한다. 이런 연결 구조는 `mem.replace(&mut link, null)`로 소유 링크를 하나씩 꺼내 반복 해제하고 필드를 빈 값으로 남기는 사용자 `drop`을 정의해야 하며, `--deny-recursive-drop`으로 경고를 에러로 바꿀 수 있다.

**R4 (참조는 2급 값).** `&T`, `&mut T`, `[]T`(`str` 포함), `[]mut T`는 다음 위치에만 존재할 수 있다:
- 함수 파라미터
- 지역 변수 (`let`/`var`)
- 표현식 안의 임시값

다음은 **컴파일 에러**다:
- struct/enum 필드의 타입
- 배열/슬라이스의 원소 타입
- 함수 반환 타입 (예외: R8)
- 일반 `^T`, `*T`의 대상 타입
- 전역 변수의 타입

예외는 독립 소유 타입 `^[]T`/`?^[]T`와 문자열 리터럴로 초기화한 `const`/`static str`뿐이다. `^[]T`는 참조를 저장하지 않고 버퍼 자체를 소유한다. 이 제한이 라이프타임 표기 전체를 불필요하게 만든다.

**R5 (참조 수명).** 지역 참조 변수는 대상보다 오래 살 수 없다. R4 덕분에 대상은 항상 같은 함수의 지역 변수, 파라미터, 또는 `static` 전역이므로 스코프 중첩 확인만으로 검사된다. 가변 전역에 대한 대여는 R10이 금지한다.

**R6 (배타성).** `&mut x`가 살아있는 동안 `x`에 대한 다른 참조 생성, 직접 읽기/쓰기, 이동이 금지된다. `&x`(공유)는 여러 개 동시 가능하지만 그동안 `x`에 쓰기/이동 금지.

v0.1의 대여 상태는 **root local/parameter 단위**로 추적한다. projection은 place의 root를 찾는 데만 쓰며 field-sensitive/index-sensitive 독립성을 증명하지 않는다. 따라서 `&mut p.a`는 `p` 전체를 배타 대여하고 그동안 `p.b`의 읽기·쓰기·대여도 금지한다. `&mut xs[0]`과 `&mut xs[1]`도 서로 다른 index라는 이유로 분리하지 않고 같은 root `xs`의 충돌 대여로 본다. 이는 compiler A/B의 함수-local 상태 기계를 작고 결정적으로 유지하기 위한 v0.1의 의도적인 보수성이다.

```fe
var p = Pair{ a: 1, b: 2 };
let r = &mut p.a;
p.b = 3;                    // 에러: root p가 배타 대여 중
r.^ = 4;

let a = &mut xs[0];
let b = &mut xs[1];         // 에러: 둘 다 root xs를 대여
a.^ = 1;
b.^ = 2;
```

참조의 생존 구간은 **참조 변수의 마지막 사용 지점까지**다. 그 이후에는 원본에 대한 접근·이동이 다시 허용된다. 조건부 흐름에서는 모든 경로의 마지막 사용 중 가장 나중 지점을 취한다. 임시 참조(`f(&x)`)는 그 문장 끝까지다. `defer` 블록에서 사용한 참조와 그 원본의 대여는 해당 defer가 실행되는 스코프 끝까지 연장한다.

호출 인자 위치의 `&mut T → &T`, `[]mut T → []T` 약화는 새 장기 공유 대여가 아니라 기존 배타 대여의 읽기 전용 재대여다. callee를 평가한 뒤 해당 인자를 평가하는 시점부터 호출이 끝날 때까지만 임시 재대여가 존재하고, 원래 배타 대여는 원래 마지막 사용까지 유지된다. 일반 `let`/대입에서는 암묵 약화를 허용하지 않으므로 `let s: &i32 = m;`(`m: &mut i32`)은 컴파일 에러다. 별도의 lifetime/coercion 시스템은 두지 않는다.

이 판정은 함수 지역 liveness 분석이며 함수 밖 정보를 쓰지 않으므로 §1.2를 위반하지 않는다.

```fe
var x: i32 = 0;
let r = &mut x;
r.^ = 1;        // r의 마지막 사용
x += 1;         // OK — 여기서 r의 대여는 이미 끝났다
```

**R7 (참조 무효화와 부분 이동).** 참조 대상이 이동되거나 재대입되면 그 참조는 이후 사용 시 에러. own.c는 변수 단위 상태만 추적하므로 field/index/`.?` projection에서 비-Copy 소유값을 이동해 꺼내는 것은 금지한다. `mem.replace(&mut place, replacement)`로 유효한 대체값을 남기면서 꺼내야 한다. 배열의 선택적 소유 원소는 `?^T`로 두고 `mem.replace(&mut arr[i], null).?`로 꺼낸다. projection chain 자체(`p.?.^`, `s.field.x`)는 값을 소비하지 않는다.

**R8 (파생 반환).** 함수는 다음 두 경우에 한해 `&T`, `&mut T`, `[]T`, `[]mut T`와 이를 `?`로 감싼 타입을 반환할 수 있다.

**(a) 파라미터 파생.** 메서드는 파생 원본이 항상 참조성 `self`여야 한다. 다른 참조성 인자를 추가로 받을 수 있지만 반환값은 그 인자에서 파생될 수 없고 그 인자의 임시 대여는 문장 끝에 풀린다. 자유 함수는 참조성 파라미터(`&T`, `&mut T`, `[]T`, `[]mut T`)가 **정확히 하나**여야 한다. 두 경우 모두 반환값이 정해진 원본에서 파생됐음을 컴파일러가 함수 본문만 보고 확인한다. 파생은 슬라이싱, 인덱싱, 필드 접근, projection, `&`/`&mut` 취함과 다른 R8(a) 호출의 연쇄다. 반환의 가변성은 원본 이하여야 한다.

**(b) 정적 파생.** 반환값이 문자열 리터럴 또는 `static` 선언에서 파생된 경우. 이때는 참조성 파라미터가 없어도 된다.

참조성 반환의 provenance는 인터페이스에서 다음 둘로 정규화한다.

- `Static`: 문자열 리터럴 또는 `static`에서 파생되어 caller local borrow를 만들지 않는다.
- `Param(N)`: 시그니처로 정해진 하나의 참조성 parameter에서 파생된다. 메서드는 `Param(self)`만 허용하며 다른 참조성 인자에서 파생되면 에러다. 자유 함수는 기존 규칙대로 참조성 parameter가 정확히 하나여야 한다.

control-flow 합류는 `Static + Static → Static`, `Static + Param(N) → Param(N)`, `Param(N) + Param(N) → Param(N)`이다. 서로 다른 `Param` provenance가 합류하면 컴파일 에러다. `?&T`/`?[]T`의 `null` 반환 경로는 caller borrow를 만들지 않는 경로이므로 static/null 경로와 `Param(N)` 경로가 합쳐지면 전체를 보수적으로 `Param(N)`으로 본다. 이 provenance는 함수 시그니처와 lowered `.fei` interface metadata에 기록할 수 있어야 한다.

호출 지점에서 `Param(N)` 결과는 **정해진 파생 원본을 대여한 것으로 취급**한다. 즉 결과를 지역 변수에 바인딩할 수 있으며, 그 대여가 사는 동안 원본에 R6·R7이 그대로 적용된다. `Static` 결과는 caller local borrow를 만들지 않는다.

```fe
let t = line.trim();                         // 내장 alias 메서드 R8(a)                         // OK. line은 t의 대여 구간 동안 잠긴다

list.at_mut(0).x = 5;                       // &mut T, 배타 대여
let r = list.at(0);                         // &T, 공유 대여
list.push(1);                               // 에러: r이 list를 대여 중 (R6)

map.get_str(key);                           // ?&V: self에서만 파생, key는 문장 끝에 해제
pub fn name() -> str { return "main"; }     // R8(b)
```

자유 함수에 참조성 파라미터가 둘 이상이면 어느 쪽에서 파생됐는지 시그니처만으로 결정되지 않으므로 참조성 반환을 할 수 없다. 그런 함수가 필요하면 메서드로 만들어 `self`를 원본으로 고정하거나 인덱스(`usize`)·핸들을 반환한다.

**R9 (unsafe).** `unsafe {}` 안에서만 허용: raw 포인터 역참조, `*T` ↔ `^T`/`&T` 변환, `@ptr_cast`, `@volatile_*`, `@port_*`, `asm`, `*_unchecked` 함수. `*void`는 저장·비교·전달과 `@ptr_cast`에만 쓸 수 있고 직접 역참조할 수 없다. R1~R8은 `unsafe` 안에서도 그대로 유지된다. 특히 `unsafe`가 참조 반환·저장이나 대여 검사를 끄지 않으며, 프로그래머가 명시적으로 raw 포인터를 경유한 부분만 컴파일러의 메모리 안전 보장 밖에 놓인다.

**R10 (전역).** `static`은 불변이며 컴파일타임 상수 초기화만 가능하다. 일반 전역 `var`의 읽기와 쓰기는 안전하며 `unsafe`가 필요 없다.
- 전역에 대한 대여는 다음으로 제한한다. `static`(불변)은 `&`로 대여할 수 있다. 일반 전역 `var`는 `&`·`&mut` 모두 대여할 수 없으며 직접 읽기와 쓰기만 허용한다. 전역 값을 참조로 넘겨야 하면 지역 변수로 복사한 뒤 대여한다.
- 이 제한의 근거는 R6다. 전역에 대한 대여가 살아 있는 동안 호출된 다른 함수가 같은 전역에 직접 접근할 수 있고, 그것은 함수 단위 지역 검사로 검출할 수 없다. 아래는 이 제한이 없으면 통과해 버리는 예다.

```fe
var G: i32 = 0;
fn f(r: &mut i32) { G = 5; }   // r과 G가 같은 곳을 가리키는지 f는 알 수 없다
fn g() { f(&mut G); }          // 제한이 없으면 g의 지역 검사는 통과한다
```

- 전역에는 `^T`나 `drop` 있는 타입을 둘 수 없다. 인터럽트 핸들러와 공유 상태(`shared`, `atomic`, `critical`, `interrupt fn`)는 v0.1에 없다(§11).

**R11 (재귀·그래프 구조).** `^T`는 R4의 2급 참조가 아니므로 소유가 한 방향인 단방향 리스트와 트리는 필드에 저장할 수 있다. 반면 양방향 리스트·순환·일반 그래프는 역방향 필드에 `^T`를 두면 R1의 단일 소유권을 위반하고 `&T`를 두면 R4를 위반한다. 이런 구조는 아레나/배열이 값을 소유하고 `u16`/`u32` 인덱스 핸들이 간선을 나타내도록 구현한다. 표준 라이브러리 `mem.Arena`를 사용할 수 있으며, 핸들 역참조 때 세대 번호 또는 경계 검사를 사용해 해제된 항목 접근을 막아야 한다.

---

## 6. 문법

### 6.1 EBNF

```
unit        := 'unit' unit_path ';' import* decl*
unit_path   := ident ('.' ident)*
import      := 'import' unit_path ['as' ident] ';'

decl        := ['pub'] (fn_decl | struct_decl | enum_decl | error_decl
                       | const_decl | global_decl) | comptime_decl
comptime_decl := 'comptime' 'if' expr '{' decl* '}' ['else' ('{' decl* '}' | comptime_decl)]

fn_decl     := ['extern' string] 'fn' ident
               '(' [param (',' param)*] ')' ['->' type] (block | ';')
param       := ['comptime'] ident ':' type
generic_params := '(' ident (',' ident)* ')'
struct_decl := ['packed'] 'struct' ident [generic_params] '{' member* '}'
member      := ['pub'] (field | fn_decl)
field       := ident ':' type ','
enum_decl   := 'enum' ident [generic_params] '{' variant (',' variant)* [','] '}'
variant     := ident | ident '(' type ')' | ident '{' vfield* '}'
vfield      := ident ':' type ','
error_decl  := 'error' ident '{' ident '=' int (',' ident '=' int)* [','] '}'
const_decl  := 'const' ident [':' type] '=' expr ';'
global_decl := 'static' ident ':' type '=' expr ';'
             | 'var' ident ':' type '=' expr ';'

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
             | 'comptime' 'if' expr block ['else' (block | 'if' ...)]
             | 'asm' '{' asm_body '}'
             | expr ';'

if_stmt     := 'if' (expr | 'let' pattern '=' expr) block
               ['else' (block | if_stmt)]
while_stmt  := 'while' expr block
for_stmt    := 'for' ident [',' ident] 'in' for_source block
for_source  := expr ['..' expr]
match_stmt  := 'match' expr '{' arm+ '}'
arm         := pattern '=>' (expr ';' | block)
pattern     := ident                      // 배리언트, 페이로드 없음
             | ident '(' ident ')'        // 튜플형 배리언트 바인딩
             | ident '{' ident (',' ident)* '}'   // 필드형 배리언트 바인딩
             | 'Some' '(' ident ')' | 'None'
             | int_literal | char_literal | 'true' | 'false' | '_'

qualified_name := ident ('.' ident)*
type        := qualified_name
             | '?' type | '!' type | qualified_name '!' type
             | '^' type | '&' ['mut'] type | '*' type
             | '[' expr ']' type | '[' ']' ['mut'] type
             | 'fn' '(' [type (',' type)*] ')' ['->' type]
             | qualified_name '(' type (',' type)* ')' // 제네릭 인스턴스

catch_expr  := expr 'catch' ['|' ident '|'] (expr | block)
orelse_expr := expr 'orelse' expr
```

`member`의 `pub`은 필드와 메서드 모두에 개별로 붙는다(§8). 필드와 메서드는 순서를
섞어 쓸 수 있다. `catch`의 블록 형태는 값을 만들지 않으며 §4.6의 규칙을 따른다.
`unit_path` segment의 lexical 제한과 source path 대응은 §8.1이 규정한다.

### 6.2 표현식 우선순위 (낮음 → 높음)

표현식 문법은 다음 우선순위 표가 규범이다. 각 단계는 명시가 없으면 좌결합 이항
연산으로 전개하며, 단항·후위·기본 단계에 나열된 형태가 그대로 프로덕션이 된다.
`catch`와 `orelse`의 구체적 형태는 §6.1을 따른다.

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
10 단항: - not ~ & &mut try
11 후위: .field  .?  .^  [i]  [a..b]  (args)  as T
12 기본: literal, ident, '(' expr ')', struct_literal, @builtin(...)
```

- `and`, `or`는 단축 평가한다. 호스트 C 방출에서는 각각 `&&`, `||`로
  매핑하며, 평가 순서와 단락 규칙은 Ferro 의미론을 그대로 유지한다.
- `orelse`와 `catch`도 lazy다. 좌변이 각각 `Some`/success이면 우변 또는 handler를
  평가하지 않는다(§4.5·§4.6).
- `as`는 후위 우선순위(단항보다 강함)지만 단항 연산자 바로 뒤에 `as`가 나타나면 모호한 비용을 숨기지 않도록 괄호를 강제한다. `(-x) as u32`와 `-(x as u32)`는 허용하고 `-x as u32`는 컴파일 에러다.
- `..`는 일반 표현식 연산자가 아니며 `for` 헤더에서만 쓸 수 있다.
- 비교 연산 체이닝 금지(`a < b < c`는 에러).
- `.field`, `[i]`, `[a..b]`, 메서드 호출은 `&`, `&mut`, `^`를 필요한 만큼 자동 projection한다. 값 자체의 역참조는 `.^`가 필요하며 raw `*T`와 optional `?T`는 자동 역참조하지 않는다.
- `x.f(args)`는 메서드를 우선 탐색한다. 함수 포인터 필드를 호출하려면 `(x.f)(args)`로 쓴다.

### 6.3 빌트인

```
@size_of(T) -> usize          @align_of(T) -> usize
@target -> comptime str
@ptr_cast(T, p) -> *T                    (unsafe)
@port_in8(p) @port_in16(p) @port_out8(p,v) @port_out16(p,v)  (unsafe)
@volatile_load(p) @volatile_store(p, v)  (unsafe)
@trap() -> never              @unreachable() -> never  (unsafe)
@line() @file()                          // 진단용

@print(fmt, ...) -> void                 // stdout, 쓰기 오류 무시
@fprint(w, fmt, ...) -> !void            // 임의 Writer
@sprint(buf: []mut u8, fmt, ...) -> usize // 버퍼에 기록, 쓴 바이트 수 반환
@compile_error(msg)                      // comptime에서 항상 컴파일 에러
```

### 6.3.1 포매팅 빌트인

가변 인자를 언어에 도입하지 않는다. `@print` 계열은 **컴파일 단계에서 여러 호출로 전개되는 빌트인**이다.

```fe
@print("x={} y={x} name={s}\n", a, b, s);
```
→ lower 단계에서 개념적으로 다음처럼 전개한다. 아래 `io`와 `fmt`는 각각 canonical
`std.io`, `std.fmt` 유닛을 가리킨다.
```
io.write(out, "x=");
var t1: [12]u8 = undefined; io.write(out, fmt.fmt_int_i32(t1[..], a));
io.write(out, " y=");
var t2: [8]u8 = undefined; io.write(out, fmt.fmt_hex_u16(t2[..], b));
io.write(out, " name="); io.write(out, s); io.write(out, "\n");
```

`fmt.fmt_*`는 `[]mut u8` 임시 버퍼에 쓰고 그 버퍼에서 파생된 `str`을 반환하는 순수 함수다. R8(a)의 원본이 하나이므로 별도 lifetime 표기가 필요 없다. 포매팅과 sink를 분리해 `@print`, `@fprint`, `@sprint`가 같은 변환 함수 한 벌을 사용한다.

규칙:
- 포맷 문자열은 **컴파일타임 문자열 리터럴 또는 `const`만**. 런타임 값이면 에러.
- verb: `{}` 기본(정수/bool/char/str 자동), `{x}` 16진, `{c}` 문자, `{s}` 문자열/슬라이스, `{b}` 불린. `{{`는 `{` 이스케이프.
- `{}` 개수와 인자 개수 불일치 → 컴파일 에러.
- 인자 타입에 대응하는 `fmt.fmt_*` 함수가 없으면 컴파일 에러(메시지에 타입명 표시).
- 자릿수/폭/정렬 지정자는 v0.1에 없음. 필요하면 `fmt.fmt_int_pad`를 직접 호출.
- `@fprint`의 첫 인자는 Copy 핸들 `io.Writer`(§10)다.
- `@print`는 `io.Writer.Stdout`에 기록하며 저수준 writer 오류를 삼키고 `void`를 반환한다.
  따라서 `try @print(...)`는 컴파일 에러다.
- `@fprint`는 writer 오류를 전파하여 `!void`를 반환한다.
- `@sprint`는 같은 `fmt.fmt_*` 결과를 대상 `[]mut u8`에 `mem.copy`로 이어 붙인다. 버퍼가 찬 뒤의 출력이 잘리더라도 트랩하지 않고 기록된 바이트 수를 `usize`로 반환한다.
- 전개된 `io.write` 호출은 위 반환 규칙에 맞게 lower 단계에서 오류를 전파하거나 무시한다. `fmt.fmt_error`는 `core.Error`의 이름/코드를 포맷한다.

`@compile_error(msg)`의 `msg`는 comptime 문자열이어야 하며, 평가되는 분기에서
항상 진단을 발생시킨다. `comptime if`의 제거되는 분기에서는 진단하지 않는다.

### 6.4 예제

```fe
unit frame;

const WIDTH: usize = 320;
const HEIGHT: usize = 200;

pub struct Buffer {
    pixels: []mut u8,

    pub fn put(self: &mut Self, x: usize, y: usize, c: u8) -> void {
        if x >= WIDTH or y >= HEIGHT { return; }
        self.pixels[y * WIDTH + x] = c;
    }
}
```

```fe
unit main;
import std.io;

fn count_lines(path: str) -> !usize {
    var f = try io.open(path, io.Read);
    defer { f.close() catch @trap(); }

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
        @print("failed: {}\n", e);
        return e;
    };
    @print("{}\n", n);
}
```

---

## 7. 의미론 세부

### 7.1 변수와 초기화

- `let`은 불변, `var`는 가변 선언이다. 두 형태 모두 초기값이 있으면 타입을 추론할 수 있다. `var x: T;`와 `var x: T = undefined;`처럼 초기값이 없거나 `undefined`이면 타입 명시가 필수다.
- `&mut x`, mutable slice 생성과 `&mut Self` 메서드 호출은 `var` place에서만 가능하다. `let`이 `^T`를 보유해도 그 대상을 안전 코드에서 변경할 수 없다. by-value `self: Self`는 소비 메서드 안에서 자신의 필드를 무효 상태로 바꿀 수 있는 가변 local owner로 취급한다.
- 모든 변수는 사용 전 초기화 필수(정적 검사). 명시적 미초기화는 `= undefined`(unsafe 아님, 단 읽기 전 쓰기 필수는 여전히 검사).
- 섀도잉 허용(같은 스코프에서 `let` 재선언).

### 7.2 제어 흐름

- `for x in slice`: `x`는 `&T`(`[]mut T`이면 `&mut T`). 값 접근은 `x.^`.
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

- 기본: cdecl.
- `extern "c" fn name(...) -> T;` — 본문 없이 선언, C 심볼과 링크. 이름 맹글링 없음. 인자/반환에 `^T`, 슬라이스, 에러 유니온 사용 불가(`*T`, `usize`만).
- 큰 struct(> 4바이트)는 숨은 포인터로 반환(C ABI 따름).

### 7.4 검사와 트랩

트랩 발생 조건: 배열/슬라이스 경계 초과, 정수 오버플로, 0 나눗셈, `?T`의 `.?` 실패, `@trap()`.

동작: `core.panic(msg: str, file: str, line: u32)` 호출 → 등록된 `sys.on_exit(fn)` 정리 함수를 역순 호출 → 메시지 출력 → `sys.exit(3)`. 사용자가 `core.set_panic_handler`로 교체 가능. 일반 panic unwind나 defer 실행은 없지만 프로세스 종료 전에 반드시 복원해야 하는 자원은 allocation 없는 고정 크기 `on_exit` registry에 등록한다.

`--no-checks` 빌드에서 제거되는 것: 경계 검사, 오버플로 검사, `.?` 검사.
**절대 제거되지 않는 것:** 소유권/참조 검사, 옵셔널 타입 검사, `match` 완전성 — 전부 컴파일타임이므로.

### 7.5 comptime

- `const` 선언의 초기값은 컴파일타임 평가(정수 연산, `@size_of`, 다른 const).
- `comptime if`는 평가되지 않는 분기를 **파싱은 하되 타입 검사/코드 생성하지 않는다**(타깃별 분기용).
- 함수의 `comptime` 파라미터는 §9 제네릭.
- 재귀 평가 깊이 제한 256, 초과 시 에러.

### 7.6 표현식 평가 순서

- 일반 표현식의 평가 순서는 소스의 왼쪽에서 오른쪽이다.
- 함수·메서드 호출은 callee를 먼저 평가하고 인자를 소스 순서대로 왼쪽에서 오른쪽으로 평가한다. 자동 `self` projection도 callee 평가의 일부다.
- 이항 연산자는 왼쪽 operand를 먼저, 오른쪽 operand를 나중에 평가한다.
- `and`와 `or`는 왼쪽 operand로 결과가 정해지면 오른쪽을 평가하지 않는다. `orelse`와 `catch`도 §4.5·§4.6에 따라 우변/handler가 필요한 경로에서만 평가한다.
- 이 순서는 부수 효과뿐 아니라 move, borrow의 시작·마지막 사용, `try` 전파와 defer/drop cleanup 순서를 결정한다.

---

## 8. 유닛

파일 하나가 유닛 하나다. 유닛의 canonical identity는 fully-qualified dotted unit path이며
모든 cross-unit 타입·선언·제네릭 identity에서 같은 이름을 사용한다.

```fe
unit game.main;

import game.render;
import tinyjson.parse;
import net.http as http;
import std.io;
```

import는 항상 유닛 전체를 가져오며 member는 local unit binding으로 한정해 접근한다.
기본 binding은 마지막 segment이므로 `import tinyjson.parse;` 뒤에는
`parse.read(...)`, `import std.io;` 뒤에는 `io.write(...)`를 쓴다. `as`가 있으면 그
alias가 binding이다.

`std` 최상위 namespace는 compiler-reserved이며 user unit은 선언할 수 없다. 표준 유닛은
`import std.io;`, `import std.mem;`, `import std.fmt;`, `import std.sys;`처럼 가져온다.
`str`은 계속 built-in `[]u8` alias와 alias-method namespace이며 import unit이 아니다.

v0.1은 relative import(`.foo`, `..foo`), glob/selective import, `pub import` re-export,
package-private/friend visibility, package manager를 지원하지 않는다.

### 8.1 unit 이름과 source path

unit path의 각 segment는 ASCII lowercase `a`~`z`로 시작하고 이후에는 `a`~`z`,
`0`~`9`, `_`만 쓸 수 있으며 최대 8자다. 일반 Ferro identifier는 계속 case-sensitive이고
이 제한은 unit path에만 적용한다. `game.main`, `tinyjson.parse`는 허용하지만
`TinyJson.Parse`, `very_long_library_name`은 unit path로 허용하지 않는다. 이 규칙은
FAT/DOS 8.3과 case-sensitive host에서 같은 source가 같은 유닛으로 해석되게 한다.

unit path는 import root 아래의 상대 source path와 정확히 대응한다. canonical identity는
항상 dotted path이고 실제 path separator만 host/DOS에 맞게 바꾼다.

```
tinyjson.parse  -> tinyjson/parse.fe
game.world.map  -> game/world/map.fe
```

### 8.2 binding과 visibility

한 unit에서 import binding은 다른 unit-scope declaration/import binding과 충돌할 수 없다.
`import foo.net; import bar.net;`은 둘 다 `net`을 만들므로 에러이며 두 번째를
`import bar.net as bar_net;`처럼 alias해야 한다. alias는 일반 Ferro identifier다.

visibility는 private과 `pub` 두 단계뿐이다. private 선언은 같은 unit에서만 보이고,
`pub` 선언과 개별 `pub` field/method만 import한 모든 caller에서 보인다. dotted prefix는
권한이 아니므로 `game.foo`와 `game.bar`는 서로의 private 선언에 접근할 수 없다. public
function parameter/return, public field 등 외부 signature에 나타나는 nominal type은
importer가 이름을 해석할 수 있어야 하며 private nominal type을 public API에 노출하면
컴파일 에러다.

### 8.3 순환과 canonical identity

순환 import는 컴파일 에러다. fully-qualified dotted unit path와 선언 이름이 nominal
identity의 기준이므로 `tinyjson.value.Value`와 `tinyjson.value.Box`처럼 표시한다. nominal
struct/enum/error는 defining unit + declaration name으로 구별되고 type alias는 새 nominal
identity를 만들지 않는다.

---

## 9. 제네릭

`comptime` type 파라미터 기반 모노모피제이션. v0.1의 user-defined generic parameter는
`type`만 지원한다.

```fe
pub struct List(T) {
    items: ^[]T,
    len: usize,

    pub fn new() -> List(T) { ... }
    pub fn push(self: &mut Self, v: T) -> !void { ... }
    pub fn at(self: &Self, i: usize) -> &T { ... }       // R8 공유
    pub fn at_mut(self: &mut Self, i: usize) -> &mut T { ... }
    pub fn drop(self: &mut Self) { ... }
}

fn max(comptime T: type, a: T, b: T) -> T {
    if a > b { return a; }
    return b;
}

let m = max(i32, 3, 7);
var xs: List(u8) = List(u8).new();
```

- `fn id(comptime T: type, x: T) -> T`를 기본형으로 하며 `struct Box(T)`와
  `enum Maybe(T)`의 `T`는 `comptime T: type`의 shorthand다. `comptime N: usize`,
  comptime string/bool 등 user value generic은 지원하지 않는다. compiler builtin의 기존
  comptime value는 user generic parameter가 아니다.
- generic type argument는 항상 명시한다. `id(i32, 3)`은 허용하지만 `id(3)`에서 T를
  추론하지 않는다. `mem.create(value)`처럼 별도로 정의된 compiler-known intrinsic
  inference는 일반 generic inference가 아니다.
- generic struct/enum의 method는 enclosing type parameter를 사용할 수 있다. 그러나
  method/function이 enclosing type parameter 외에 별도의 새 generic parameter list를
  선언하는 generic-method 기능은 v0.1에 없다.
- 인스턴스화 시 타입 인자를 대입해 type-dependent operation을 재검사하고 코드를
  생성한다. trait/bound와 overload resolution은 없다. 본문 연산이 해당 타입에서 invalid면
  definition/body의 실제 연산 위치를 primary error로 표시하고 각 caller에 `instantiated
  here` note를 붙인다. nested instance는 가능한 범위에서 instantiation chain을 표시한다.
- generic body의 이름은 항상 definition unit scope에서 해석한다. non-dependent name은
  정의 시 그 symbol로 고정되며 caller의 같은 이름은 영향을 주지 않는다. private support
  symbol도 definition unit의 것을 쓴다. `comptime if`의 선택되지 않는 branch는 parse만
  하고 semantic name resolution/type checking/codegen을 하지 않는다.
- generic instance의 canonical key는 **canonical definition unit + canonical declaration
  identity + canonical type argument list**다. type alias는 새 nominal identity가 아니므로
  underlying/interned canonical type identity로 정규화한다. 따라서 `const Word = i32;` 뒤의
  `id(Word, 1)`과 `id(i32, 2)`는 같은 instance다.
- exported generic은 definition unit의 private symbol을 참조할 수 있다. 이는 컴파일러
  수준의 처리이며 Ferro source visibility를 public으로 바꾸지 않는다 — 다른 Ferro source는
  여전히 그 private 심볼을 직접 참조할 수 없다.

```fe
unit lib;

fn helper(x: i32) -> i32 { return x + 1; }

pub fn bump(comptime T: type, x: T) -> T {
    comptime if T == i32 { return helper(x); }
    return x;
}
```

다른 unit이 요청한 `lib.bump(i32)` instance는 내부적으로 `helper`를 호출할 수 있지만,
다른 Ferro source가 `lib.helper`를 직접 참조할 수는 없다.

- 재귀적 인스턴스화의 distinct-instance chain 제한은 32다. 이미 pending/known인 동일
  canonical instance key를 다시 요청하는 recursion은 pending instance를 재사용하고 depth를
  소비하지 않는다. 새로운 distinct instance가 연쇄적으로 생길 때만 depth가 증가하며
  32를 초과하면 최초/현재 위치와 instance chain을 포함한 compile error를 낸다.
- comptime에서 type 값의 `==`/`!=`, `@is_int(T)`, `@is_ptr(T)`를 허용한다. canonical
  interned type identity로 평가하며 런타임 type reflection은 없다.

---

## 10. 표준 라이브러리

표준 라이브러리 상세 명세는 별도 문서에서 다룬다. 표준 라이브러리는 reserved `std`
namespace 아래에 있으며 `import std.io;`처럼 명시적으로 가져온다. import 뒤의 local
binding은 마지막 segment라 `io.write`, `mem.replace` 형태로 사용한다.

아래는 언어 규칙(§4~§7)이 직접 참조하거나 언어 표면(빌트인, 예제)이 전제하는 최소
표면만 남긴 것이다. `std.list`, `std.map`, `std.io.File`의 전체 API, `std.sys`의 OS
접근 함수 등 나머지 모듈의 정확한 시그니처는 표준 라이브러리 명세가 정의한다.

- **`std.core`**: `panic`, `set_panic_handler`, `Error`(기본 에러 집합, §4.6), `assert`. `panic`/`set_panic_handler`는 §7.4 트랩 동작이 참조한다.
- **`std.mem`**: `create(value: T) -> !^T`(T는 값에서 추론), `destroy(p)`, `alloc_slice(T, n) -> !^[]T`, `replace(dst: &mut T, value: T) -> T`, `copy(dst: []mut u8, src: []u8)`, `set(dst: []mut u8, v: u8)`, `Arena{ init, alloc, reset, drop }`. 초기화되지 않은 힙을 안전 코드에 반환하는 `create(T)` 형태는 없다. `replace`는 이전 값을 이동해 반환하고 새 값으로 자리를 초기화하며 부분 이동과 재귀 구조의 반복 drop에 사용한다(§4.5, §5 R3·R7·R11).
- **문자열/바이트**: `str`은 `[]u8` alias다(§4.2). 내장 alias 메서드 `eq`, `find`, `starts_with`, `split_at`, `parse_int`, `trim`, `to_cstr`, `from_cstr`를 `line.trim()`처럼 호출하며(§6.4 예제) `str` 이름의 import 유닛은 두지 않는다.
- **`std.fmt`**: sink를 소유하지 않는 순수 변환 함수 모음이며 `@print`/`@fprint`/`@sprint`(§6.3.1)가 의존한다. `fmt_int_i8/i16/i32/u8/u16/u32(buf: []mut u8, v) -> str`, `fmt_hex_*`, `fmt_char`, `fmt_bool`, `fmt_error`, `fmt_int_pad`를 제공한다. 반환 slice는 buf에서 파생된 R8(a) 결과다. `fmt_error`는 `--strip-error-names`를 따른다.
- **`std.io`**:
  ```fe
  pub enum Writer { Stdout, Stderr, File(u16), Null }
  pub enum Reader { Stdin, File(u16) }
  ```
  둘 다 정수 payload만 가진 Copy handle이며 참조나 raw context pointer를 저장하지 않는다(§5 R8 예제). `io.write(w: Writer, buf: []u8) -> !usize`, `io.read(r: Reader, buf: []mut u8) -> !usize`가 실제 I/O를 수행한다.
- **`std.sys`**: `exit`, `on_exit(f: fn() -> void) -> !void`. `on_exit`은 §7.4 트랩 동작이 참조하는 allocation 없는 고정 크기 callback registry이며 가득 차면 오류를 반환한다.

---

## 11. 의도적으로 제외한 기능

**등급 정의**
- `영구` — §1 철학과 정면 충돌. v2.0에서도 넣지 않는다.
- `구조적 불가` — 넣으면 R4를 풀어야 하고 전역 분석이 생긴다(§1 철학 2). 이 언어의 정의상 불가.
- `v0.2` — 넣을 예정. 순서 문제일 뿐 원칙 위반 아님.
- `편의` — 원칙 위반 없음, 구현도 쉬움. 여유 생기면 아무 때나.

| 기능 | 등급 | 제외 이유 | 대체 수단 |
|---|---|---|---|
| 트레잇/인터페이스 (`dyn`) | **v0.2 (1순위)** | 부트스트랩에 불필요, 타입 시스템 전반에 영향 | Copy handle enum (`io.Writer`, §10) |
| 인터럽트 핸들러와 공유 상태 (`interrupt fn`, `shared`, `atomic`, `critical`) | v0.2 | 벡터 설치·복원과 배리어가 백엔드 지원을 요구하고, 타깃마다 다르다 | polling |
| far 포인터와 세그먼트 주소 지정 | **영구** | x86 리얼모드에만 있는 개념이고 평평한 주소 공간에는 대응물이 없다 (§2) | 없음. 리얼모드 타깃이 생기면 그때 다시 본다 |
| 클로저 | v0.2 | 캡처 = 참조 저장 = R4 위반 소지 | 콜백에 `ctx: *void` 전달 |
| 연산자 오버로딩 | v0.2 (인터페이스 이후) | 숨은 비용. 넣더라도 특정 인터페이스 구현으로만 제한 | 메서드 |
| 튜플 / 다중 반환 | 편의 | 이름 없는 필드는 가독성 손해 | struct |
| 레이블 있는 break | 편의 | — | 플래그 변수 |
| 슬라이스 패턴 매칭 | 편의 | — | 인덱스 비교 |
| `inline fn` | 편의 | — | C 방출 시 `static inline` |
| `must` 키워드 | 편의 | 실패를 트랩으로 바꾸는 문법 설탕일 뿐 핵심 의미론이 아님 | `expr catch @trap()` |
| 블록 표현식 | 편의 | 값을 만드는 블록이 없으면 `catch`가 짧은 형태로 충분하고, 문법 표면이 작아진다 (§4.6) | `catch <식>`, `return`으로 탈출 |
| 라이프타임 표기 (`'a`) | **구조적 불가** | 전역 분석 필요, R4를 풀어야 함 | R4 (2급 참조), R8 파생 반환, 인덱스 핸들 |
| 선점형 스레드 | 구조적 불가 | DOS 기본 실행 모델에 없고 함수 단위 소유권 모델을 넘어서는 동기화가 필요 | — |
| 매크로 / 전처리기 | **영구** | 도구 지원과 컴파일 속도 파괴 | `const`, `comptime if`, 제네릭, `@print` |
| 예외 | 영구 | 언와인딩 기반 시설 없음, 숨은 비용 | 에러 유니온 |
| GC | 영구 | 결정적 비용 원칙 위반 | 소유권 + RAII + 아레나 |
| 암묵 형변환 | 영구 | 버그 원인 1위 | `as` |
| 상속 | 영구 | 숨은 vtable, 취약한 기반 클래스 | 합성 |

### 11.1 인터페이스 설계 스케치 (v0.2 예정)

지금 구현하지 않되, 나중에 `io.Writer` Copy handle enum을 무리 없이 대체할 수 있도록 방향만 고정해 둔다.

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
- `^dyn I`의 drop은 vtable 경유. 이 때문에 `?^dyn I`, drop 전개, 제네릭 인자로서의 `dyn` 등 타입 시스템 여러 곳에 케이스가 추가되므로 독립적으로 다룬다.
- 도입 시 `io.Writer`/`io.Reader` handle enum을 `dyn` 기반 API로 교체한다. v0.1 safe API에는 이미 대여 대상을 숨긴 `*void`가 없으므로 이 전환은 기능 확장이지 안전성 수정이 아니다.

위 표에 없는 항목(링크타임 최적화, 디버그 정보 포맷, 언어 서버 등)은 도구 영역이며 v0.2 이후 별도 검토.
