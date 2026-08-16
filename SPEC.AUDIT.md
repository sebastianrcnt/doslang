# Ferro specification audit log

`SPEC.md`를 항상 최신 규범 문서로 유지하고, 최초 `AUDIT.md` 반영 이후 설계 판단으로
바뀐 사항은 이 파일에 누적한다.

## 2026-08-16 — v0.1.3

### `char`와 `u8` 사이의 변환

- 문제: §4.1은 `char`를 `u8`과 별개 타입으로 규정하고 암묵 변환을 금지하지만,
  §6.4의 줄 수 계산 예제는 `[]u8`에서 얻은 값을 문자 리터럴과 직접 비교했다.
- 결정: 별개 타입과 암묵 변환 금지 원칙을 유지한다. 저장, 대입, 비교 모두 명시적인
  `as`가 필요하며 문자 리터럴도 문맥에 따라 자동으로 `u8`이 되지 않는다.
- 명세 반영: 예제의 비교를 `c.^ == '\n' as u8`로 수정하고 §4.1에 규칙을 명시했다.
- 구현 영향: 타입 검사기는 `char`를 독립 기본 타입으로 취급해야 하고, C 방출 시 같은
  크기의 정수 표현을 사용할 수 있어도 Ferro 단계에서는 `char`/`u8` 혼용을 거부해야 한다.

## 2026-08-16 — v0.1.4

### `match` scrutinee와 구조체 초기화의 중괄호 모호성

- 문제: `match expr { arms }`와 `Type{ fields }`가 모두 식별자 뒤에 `{`를 사용하므로
  `match value { ... }`의 arm 블록을 구조체 초기화로 잘못 소비할 수 있었다.
- 결정: `match` scrutinee 바로 뒤의 `{`는 항상 arm 블록으로 해석한다. 구조체 초기화식
  자체를 scrutinee로 쓸 때는 `match (Type{ ... }) { ... }`처럼 괄호가 필수다.
- 구현 영향: match 문맥의 식 파서는 최상위 `{` 앞에서 scrutinee 파싱을 멈춰야 하며,
  괄호 안에서는 일반 구조체 초기화 규칙을 그대로 적용한다. 오류 복구는 모든 반복에서
  적어도 한 토큰을 소비해 같은 진단을 무한 반복하지 않아야 한다.

## 2026-08-16 — v0.1.5

### 제어 흐름 헤더와 구조체 초기화의 중괄호 모호성 일반화

- 문제: v0.1.4의 모호성은 `match`뿐 아니라 `if flag {}`, `while flag {}` 및
  `for x in values {}`처럼 식 직후 본문이 시작되는 모든 제어 흐름에 동일하게 발생한다.
- 결정: `if`, `while`, `for`, `match`, `comptime if` 헤더 바로 뒤의 최상위 `{`는 항상
  제어 흐름 블록을 시작한다. 헤더 최상위에 구조체 초기화식을 쓰려면 괄호가 필수다.
- 구현 영향: 제어 흐름 헤더의 최상위 식 파싱에서 구조체 초기화를 금지하되 괄호 안에서는
  일반 식 파싱 상태를 복원한다. 단순 식별자 조건과 배열·슬라이스 반복은 본문 `{` 앞에서
  정상적으로 종료되어야 한다.

## 2026-08-16 — v0.1.6

M6(borrow checker) 착수 전에 확정해야 하는 소유권·참조 규칙 결정과, M5까지 누적된
문법·일관성 결함 정리를 함께 반영했다.

### R8 — 파생 반환 규칙으로 전면 교체

- 문제: R4가 `[]T`를 함수 반환 타입에서 금지하고 기존 R8의 예외는 `&T`/`&mut T`만
  다뤘다. 그 결과 §10이 요구하는 `str.trim`, `str.split_at`, `str.find`를 표현할
  방법이 없었다. 기존 R8의 "결과를 지역 변수에 바인딩 불가" 제약도 `trim` 계열을
  무의미하게 만들었다.
- 결정: R8을 파생 반환 규칙으로 교체한다. (a) 참조성 파라미터가 정확히 하나이고
  반환값이 그것에서 파생될 때, (b) 문자열 리터럴이나 `static`에서 파생될 때
  `&T`/`&mut T`/`[]T`/`str`을 반환할 수 있다. 호출 지점에서 (a)의 결과는 그 인자를
  대여한 것으로 취급하며, 바인딩 금지 제약은 삭제한다.
- 근거: 표기 없는 lifetime elision이며 정의와 호출 양쪽 모두 함수 하나만 보고
  검증되므로 §1.2를 깨지 않는다. 바인딩 금지는 대여 추적을 피하려던 제약인데,
  own.c가 R6를 위해 같은 상태 기계를 이미 돌리므로 추가 비용이 거의 없다.
- 구현 영향: own.c는 호출 결과에 "인자로부터의 대여" 상태를 전파해야 한다.
  check.c는 시그니처만 보고 참조성 파라미터 개수와 가변성 관계를 검증한다.
  참조성 파라미터가 둘 이상이면 참조성 반환을 거부한다.

### R6 — 대여 구간을 마지막 사용 지점까지로 축소

- 문제: 대여가 참조 변수의 스코프 끝까지 유지되고 블록 표현식도 없어서
  `let r = &mut x; r.^ = 1; x += 1;`이 에러였다. 회피 수단은 명시적 `{ }`뿐이며,
  M11에서 컴파일러 B를 이 언어로 작성할 때 마찰이 누적된다.
- 결정: 대여 구간을 참조 변수의 마지막 사용 지점까지로 한다. 조건부 흐름에서는
  모든 경로의 마지막 사용 중 가장 나중 지점을 취한다. 임시 참조는 문장 끝까지로
  유지한다.
- 근거: 함수 지역 liveness 분석이므로 §1.2를 위반하지 않는다.
- 시점: M6 착수 전에 결정해야 한다. 나중에 좁히면 진단 메시지와 `fail/` 기대값을
  전부 다시 써야 한다.
- 구현 영향: own.c에 역방향 liveness 스캔 한 번을 추가한다.

### R10 — 전역에 대한 대여 금지

- 문제: R5가 참조 대상으로 전역을 허용하므로 `fn f(r: &mut i32) { G = 5; }`를
  `f(&mut G)`로 호출하면 R6의 배타성이 호출 경계에서 깨진다. 호출자는 `&mut G`가
  배타적이라고 보고, 피호출자는 자기 파라미터가 `G`를 가리키는지 알 수 없다.
  함수 단위 지역 검사로는 원리적으로 검출 불가능하다.
- 결정: `static`(불변)만 `&`로 대여할 수 있다. 일반 전역 `var`는 `&`·`&mut` 모두
  대여 불가이며 직접 읽기/쓰기만 허용한다. `shared var`는 `critical` 안의 직접
  접근만 허용한다. 전역을 참조로 넘겨야 하면 지역 변수로 복사한다.
- 구현 영향: R5의 대여 대상에서 가변 전역을 제외한다. §11.4에 방출 단계가
  aliasing을 가정하지 않는다는 규정을 추가했다. 이 규칙이 없으면 `&mut T`에
  `restrict`를 붙이거나 M13/M14 네이티브 백엔드에서 noalias를 가정하는 순간
  불건전해진다.

### `error.Name` 코드 부여 시점

- 문제: 코드를 "최종 링크용 생성 헤더의 심볼"로 참조하도록 규정했는데, 그러면 C에서
  상수식이 아니므로 §11.4의 `switch` 방출을 쓸 수 없고 에러 `match`가 if-else
  체인으로 떨어진다.
- 검토 후 기각한 대안: 이름 문자열의 u16 해시. 유닛별 독립 계산과 캐시 유지라는
  장점이 있으나 생일 문제로 이름 약 300개에서 충돌 확률이 50%에 달해 컴파일러 B의
  에러 이름 규모를 감당하지 못한다.
- 결정: 정렬 기반 번호 부여는 유지하되, 부여 시점을 링크가 아니라 **드라이버의 emit
  이전 단계**로 옮긴다. 코드는 방출 C에서 컴파일타임 정수 상수가 된다.
- 대가: 이름 집합이 바뀌면 방출 `.c`와 오브젝트 캐시가 전부 무효화된다. `.fei`는
  이름만 기록하므로 무효화되지 않는다. 유닛 단위 `--emit-c`는 `--error-table`로
  확정 표를 받아야 한다.
- 구현 영향: driver.c가 전체 `.fei`에서 이름을 수집해 코드를 확정한 뒤 emit을
  시작한다.

### `str`을 `[]u8`과 별개 타입으로

- 문제: §4.2는 `str`을 "`[]u8` 불변 별칭"이라 하고 §4.7은 별칭을 완전 동일 취급이라
  규정했다. 완전 동일이면 `str`을 통해 쓸 수 있는데, 문자열 리터럴은 읽기 전용
  저장 영역에 놓이므로 안전성 구멍이다.
- 결정: `str`을 별개의 내장 타입으로 한다. `[]u8` → `str`은 `as str`로 변환 가능
  (가변성 약화이므로 안전), 역방향은 금지. `str` 원소 쓰기는 컴파일 에러.
- 구현 영향: §11.4에 `fe_str`(`const uint8_t*`) 방출 행을 추가했다. types.c는
  `str`을 `[]u8`과 다른 인터닝 엔트리로 다뤄야 한다.

### `catch` 블록은 값을 만들지 않는다

- 문제: §4.6은 catch 블록이 값을 만들 수 있다고 했으나 문법에 블록 표현식이 없다.
- 결정: 블록 표현식을 도입하는 대신 catch 블록에서 값 생성을 금지한다. 블록은
  `return`/`break`/`continue`로 탈출하거나 `@trap()`으로 끝난다. 값이 필요하면
  짧은 형태 `expr catch <식>`을 쓴다.
- 근거: §1.5. 블록 표현식은 §13에 `편의` 등급으로 등재했다.

### 문법 결함 정리 (§6.1)

- `struct_decl`이 `field* fn_decl*`이라 §4.3 예제의 `pub fn new`가 문법 위반이었고
  §8이 요구하는 필드별 `pub`도 표현 불가였다. `member := ['pub'] (field | fn_decl)`
  으로 교체하면서 필드와 메서드의 순서 강제도 함께 풀었다.
- enum 배리언트 필드가 struct `field`를 재사용해 `pub`을 받을 수 있었다. `vfield`로
  분리했다.
- `catch`/`orelse`는 §6.2 우선순위 표에 이름만 있고 프로덕션이 없었다. 추가했다.
- `error_decl`이 빈 에러 집합을 허용하고 마지막 쉼표를 강제했다. 최소 1개 + 선택적
  후행 쉼표로 수정했다.
- §6.2에 `expr` 프로덕션이 없어 `try`, `as`, `@builtin`, 구조체 초기화가 EBNF
  어디에도 나오지 않았다. 우선순위 표가 표현식 문법의 규범임을 명시했다.
- `Some`/`None`이 패턴에 하드코딩돼 있으나 §3 예약어가 아니었다. 패턴 위치 전용
  문맥 키워드임을 §4.5에 명시했다.

### 일관성과 문서 정합

- §4.4의 `u16` 태그 승격 규정이 §11.4 방출 표의 `uint8_t` 고정과 어긋났다. 표를
  수정했다.
- CLI 플래그가 §2, §7.4, §8, R3에 흩어져 있었다. §8.1로 통합했다.
- §11.3의 목표 디렉터리에 `own.c`/`lower.c`/`resolve.c`/`generic.c`/`rt/`가 있으나
  실제로는 없다. 목표 구조는 유지하고, M5까지 `check.c`/`emit_c.c` 통합 상태이며
  M6 착수 시 `own.c/h`를 분리한다는 단서를 달았다.
- §12의 테스트 구조가 실제 `tests/m<N>/`와 달랐다. 두 축을 모두 인정하도록 했다.
  R8 변경에 따라 `fail/` 목록의 "참조 반환 바인딩" 항목을 교체하고 R10 전역 대여,
  `str` 변환 케이스를 추가했으며 R6·R8 `pass/` 케이스를 신설했다.

### 삭제

- `HANDOFF.md`를 제거했다. 여기에만 있던 빌드 함정(컴파일러 A는 16비트 large model,
  링크는 `*.obj`, M4 Watcom 테스트는 `-wx -wcd=202`, 8.3 파일명 제약, `D:`에서 빌드
  금지)은 별도 문서로 옮겨야 한다.

## 2026-08-16 — v0.1.7

외부 전면 audit에서 발견된 안전성 모순과 M6~M10 구현 전 미결정 사항을 통합했다.
이 절의 결정은 v0.1.6의 `str` nominal 타입, 함수 포인터 Writer, R8 단일 파라미터
규칙 및 own.c 스코프 끝 해제 결정을 명시적으로 대체한다.

### 슬라이스·문자열·소유 버퍼

- 문제: R4는 일반 `^T`의 대상에 `[]T`를 금지하면서 `^[]T`, `List.items`,
  `mem.alloc_slice`를 요구했다. 또한 하나뿐인 `[]T`가 읽기/쓰기를 모두 나타내어
  R6의 공유·배타 대여를 표현할 수 없었다.
- 결정: `[]T`는 공유·읽기 전용, `[]mut T`는 배타·쓰기 가능 slice다. var place만
  mutable slice를 만들 수 있다. 호출 인자 위치의 `[]mut T → []T`, `&mut T → &T`는
  원래 Exclusive 상태를 유지하는 암묵 재대여로 한정한다.
- 결정: `^[]T`는 일반 포인터 합성이 아닌 `(ptr,len)` 독립 소유 타입이다.
  `^[]T`/`?^[]T`만 R4의 대상 제한에서 예외이고 `*[]T`/`*[]mut T`는 금지한다.
- 재검토: v0.1.6은 문자열 리터럴의 불변성을 위해 `str`을 nominal 타입으로 만들었지만,
  `[]T` 자체가 불변이 되면서 근거가 사라졌다. `str`을 미리 선언된 `[]u8` 완전 동일
  alias로 내렸다. UTF-8 검증은 없으며 별도 cast·C 표현·쓰기 금지 규칙이 필요 없다.
- 결과: `str`도 R4를 그대로 적용받아 field/element에 저장할 수 없다. 문자열을
  소유하려면 `String{ bytes: ^[]u8 }`를 쓰며 `as_str()`은 파생 shared slice를 반환한다.

### 안전한 Writer/Reader와 포매팅 분리

- 문제: 안전한 `File.writer() -> Writer`가 대여 대상을 `*void`에 숨겨 반환하여
  `make() -> Writer`만으로 safe-code dangling을 만들 수 있었다. 문서의 "사용자 책임"은
  §1의 memory-safety 보장과 충돌하며 Reader도 동일하게 불건전했다.
- 원칙: 안전한 표준 라이브러리 API는 대여 대상을 가리키는 raw pointer를 값에 숨겨
  반환할 수 없다. R9 내부에서 unsafe 변환을 한 번 감쌌다는 사실은 safe API를
  건전하게 만들지 않는다.
- 결정: v0.1.2 Writer/Reader는 함수 포인터 struct 대신 정수 payload만 가진 Copy handle
  enum이다. `Writer{Stdout,Stderr,File(u16),Null}`, `Reader{Stdin,File(u16)}`와
  `io.write(Writer, []u8)`, `io.read(Reader, []mut u8)`를 쓴다. fd 재사용은 논리적 I/O
  오류일 수 있으나 memory dangling은 아니다. buffer Writer는 두지 않는다.
- 결정: fmt는 sink를 모른다. `fmt.fmt_int_i32(tmp: []mut u8, v) -> str`처럼 임시
  buffer에 쓰고 R8(a) 파생 slice를 반환하는 순수 함수 한 벌만 둔다. `@print`/`@fprint`는
  결과를 `io.write`, `@sprint`는 `mem.copy`로 이어 붙인다. v0.2의 `dyn Writer` 전환은
  안전성 수정이 아닌 기능 확장이다.

### 소유권·R4·R8

- `str`/`[]T`/조건부·에러 union/배열의 재귀 Copy 규칙을 완성하고 `[]mut T`와
  `^[]T`는 non-Copy로 정했다.
- field/index/optional projection에서 non-Copy 값을 부분 이동하는 것을 금지했다.
  own.c는 변수 단위 상태를 유지하며 `mem.replace(&mut place, replacement)`만 추출을
  허용한다. `.?`/field/index는 값을 즉시 꺼내는 연산이 아니라 place projection이다.
- mutable borrow·slice와 `&mut Self` 호출은 var place에서만 허용한다. consuming
  `self: Self`는 메서드 내부에서 invalid sentinel을 남길 수 있는 local owner다.
- R8 메서드는 파생 원본을 self로 고정한다. 추가 참조 인자는 받을 수 있지만 반환이
  그 인자에서 파생될 수 없다. 자유 함수만 참조성 파라미터 정확히 하나를 요구한다.
  `?&T`, `?&mut T`, `?[]T`, `?[]mut T` 반환을 포함한다.
- own.c의 오래된 "스코프 끝 해제"와 "R8 결과 바인딩 거부"를 삭제했다. 역방향
  liveness pass로 마지막 사용을 계산하고 defer 사용은 스코프 끝까지 연장한다.

### drop, File, heap 초기화

- `File.close(=drop)` 모순을 제거했다. `close(self: Self) -> !void`는 소비하는 일반
  메서드이며 내부 handle을 invalid로 만든 후 오류를 반환한다. 자동 drop은 열린 handle만
  조용히 닫는다. `drop` 직접 호출 금지는 유지한다.
- `mem.create(T) -> !^T`는 초기화되지 않은 안전 힙을 반환하므로 삭제했다.
  `mem.create(value: T) -> !^T`로 바꾸고 T는 값에서 추론한다.

### error와 결정적 build

- `try`는 operand와 현재 함수의 nominal error 타입이 같을 때만 허용한다. 다른 타입은
  catch에서 명시 매핑한다. `catch`는 error-return 함수 밖에서도 허용하며 void 결과의
  handler block은 정상 fallthrough할 수 있다.
- 정렬 기반 error code 표는 결정성과 fixpoint를 위해 유지한다. build-directory 이력에
  의존하는 append-only 표는 기각했다.
- 드라이버는 단일 `fe_errors.h`에 정렬된 `#define`을 생성한다. 이름 집합 변경 시 유닛
  C를 재방출하지 않고 header 의존 object만 다시 컴파일한다. switch 상수 요건도 유지한다.

### 제네릭·증분 build

- 제네릭 이름 해석은 사용 유닛이 아니라 정의 유닛 scope에서 한다. `.fei`는 본문 token과
  generic 전용 private signature를 함께 기록한다.
- driver가 전체 인스턴스 요청을 합쳐 단일 `fe_generics.c`에 중복 없이 방출한다.
  사용 유닛별 external 중복 심볼과 static 코드 복제를 모두 피한다.
- comptime type 비교와 최소 introspection `@is_int`, `@is_ptr`를 추가했다.
- `.fei` cache key에 source hash뿐 아니라 dependency `.fei` hash를 포함한다.

### interrupt/shared와 panic

- shared C 방출을 `volatile`로 정하고 critical 진입·이탈에 compiler barrier를 둔다.
  bits16의 한 명령 크기 8/16비트 atomic load/store는 interrupt 경계에서 원자적이므로
  자동 critical 없이 volatile 한 명령만 방출한다. far pointer와 RMW는 explicit critical이다.
- `interrupt_safe`에서 critical, port/volatile builtin, asm과 필요한 unsafe를 허용한다.
  금지 목록은 heap, DOS/DPMI, blocking I/O, FPU, non-interrupt-safe 호출로 한정했다.
- panic은 일반 defer unwind를 하지 않지만 interrupt vector 복원용 고정 크기
  `sys.on_exit` callback을 실행한다. bits32 interrupt/shared/critical은 v0.2로 명시했다.

### 문법·표기 정리

- generic struct/enum parameter, declaration-level comptime if, for 전용 range,
  bool/char pattern, `[]mut T`를 EBNF에 추가했다.
- 정의되지 않은 단항 `^`를 삭제했다. field/index/slice/method의 `&`/`&mut`/`^`
  projection과 raw/optional의 비자동 역참조를 명문화했다.
- method가 function-pointer field보다 우선하며 field 호출은 `(x.f)(...)`로 고정했다.
- `@seg_ptr(T, seg, off)`로 타입 인자를 명시하고 type-valued const alias를 허용했다.
- 단항 직후 cast는 `(-x) as T` 또는 `-(x as T)`처럼 괄호를 강제한다.

### 구현 및 milestone 영향

- M3: shared/mutable slice와 str alias를 재검증한다.
- M4: 함수 포인터 Writer를 handle enum + 순수 fmt 함수로 교체한다.
- M5: `^[]T`, consuming close, projection 부분 이동, 초기화된 create를 반영한다.
- M6: 역방향 liveness와 self-source R8을 구현한다.
- M7: try nominal error 일치와 일반 catch를 구현한다.
- M8/M9: `fe_errors.h`, dependency hash, `fe_generics.c`를 구현한다.
- M10: volatile/barrier, interrupt-safe 허용 목록, on_exit 복원을 검증한다.

## 2026-08-16 — v0.1.8

M6~M9 구현 전에 함수-local 소유권 분석, optional/error 의미, 계층형 unit/import,
`.fei` cache와 제네릭 모노모피제이션을 동결했다. compiler A/B가 같은 작은 상태 기계를
구현하고 DOS와 host에서 같은 source graph를 선택하며 M12 fixpoint에서 byte-identical
출력을 만들 수 있는지가 공통 판단 기준이다.

### Deterministic expression evaluation order

- 문제: C는 일반 호출 인자와 많은 operand의 평가 순서를 보장하지 않는다. Ferro가 이를
  그대로 상속하면 side effect뿐 아니라 move, borrow, `try`, defer/drop cleanup 결과가 C
  compiler와 최적화에 따라 달라진다.
- 결정: Ferro 일반 표현식은 left-to-right다. 호출은 callee 먼저, 이어서 source 순서의
  인자, 이항식은 왼쪽 operand 먼저다. `and`/`or`, `orelse`, `catch`는 필요한 우변만
  평가하는 lazy 연산이다.
- 근거: source만으로 동작과 cleanup 순서를 예측할 수 있고 compiler A/B의 lower 결과가
  동일해진다. 함수-local 분석 원칙도 그대로 유지한다.
- 기각한 대안: target C의 평가 순서에 맡기기. host compiler와 build option에 따라 의미가
  변해 M12 결정성을 깨므로 기각했다.
- 구현 영향: lower는 C에서 순서가 보장되지 않는 식을 ordered temporary statement로
  분해하고 own.c도 같은 순서로 place effect를 처리한다.

### M6 root-granularity borrow tracking

- 문제: field/index별 독립 대여를 허용하려면 projection overlap, 동적 index 동등성,
  union/alias까지 다루는 별도 alias analysis가 필요하다.
- 결정: v0.1 대여 상태는 root local/parameter 단위다. `&mut p.a`는 `p` 전체를 잠그고
  `xs[0]`과 `xs[1]`도 같은 root의 충돌 대여다. projection은 root를 찾는 데만 쓴다.
- 근거: R1~R8을 함수 하나의 작은 상태 기계로 검사할 수 있어 compiler A와 M11의 B가
  단순해진다. 보수적 거부일 뿐 memory safety나 표현 결정성은 약화하지 않는다.
- 기각한 대안: field-sensitive/index-sensitive borrow checking. 편의는 늘지만 compiler A의
  구현량과 진단 상태가 크게 증가하고 동적 index에는 결국 보수성이 남아 기각했다.
- 구현 영향: own.c의 borrow key는 projection이 아니라 root symbol이다. M6에는 disjoint
  field/index도 충돌하는 pass/fail 경계를 고정한다.

### M6 reborrow/coercion 제한

- 문제: `&mut → &`와 `[]mut → []`를 일반 암묵 변환으로 허용하면 새 shared borrow의
  수명과 원래 exclusive borrow의 재활성화를 결정하는 숨은 coercion/lifetime 시스템이
  필요하다.
- 결정: 암묵 약화는 호출 인자 위치의 호출 기간 read-only reborrow만 허용한다. 원래
  exclusive borrow는 원래 last-use까지 유지하며 일반 `let`/대입의 암묵 약화는 에러다.
- 근거: API 호출 편의는 확보하면서 수명 annotation 없이 함수-local R6 분석을 유지한다.
- 기각한 대안: arbitrary implicit reborrow/coercion과 `let s: &T = m` 허용. 대여 종료
  시점이 숨고 compiler A/B가 별도 coercion graph를 가져야 하므로 기각했다.
- 구현 영향: check/own은 call argument에만 임시 shared view 전이를 만들고 assignment
  conversion table에는 추가하지 않는다.

### R8 provenance lattice

- 문제: 여러 return path의 static/parameter 파생 결과, optional null 경로와 method의
  추가 참조 인자를 합칠 명시 규칙이 없으면 caller borrow가 구현 순서에 따라 달라진다.
- 결정: provenance를 `Static`과 `Param(N)`으로 정규화한다. method는 `Param(self)`만,
  자유 함수는 유일한 참조성 parameter만 허용한다. `Static + Param(N)`은 `Param(N)`,
  서로 다른 `Param`의 합류는 에러다. null 경로는 caller borrow가 없는 경로다.
- 근거: provenance가 작은 lattice라 함수 본문만 보고 계산하고 시그니처로 전달할 수 있다.
  lifetime annotation이나 interprocedural inference가 필요 없다.
- 기각한 대안: arbitrary parameter union provenance 또는 lifetime parameter. caller에서
  숨은 alias set/전역 분석이 필요해 Ferro 철학과 맞지 않는다.
- 구현 영향: own.c가 return CFG에서 lattice를 합치고 lowered signature와 `.fei`가
  provenance metadata를 보존한다.

### M6 branch merge와 loop fixed point

- 문제: `Owned/Moved`, 초기화 여부와 live borrow가 branch/backedge에서 만날 때 단순히
  한쪽 상태를 고르면 use-after-move를 놓치거나 안전한 borrow를 너무 일찍 푼다.
- 결정: `Owned + Moved → MaybeMoved`, 경로별 초기화 차이는 `MaybeUninit` 동등 상태로
  합친다. live borrow는 합집합을 보수적으로 유지하고 incompatible borrow는 약화하지
  않는다. loop은 진입/종료 상태를 합쳐 두 번째 pass를 돌리고 안정되지 않으면 에러다.
- 근거: 유한한 함수-local lattice와 기존 2-pass만으로 모든 iteration을 보수적으로
  근사한다. 첫 iteration만 검사하는 불건전성을 피한다.
- 기각한 대안: 첫 pass만 검사, 또는 merge에서 borrow를 `Owned`로 되돌리기. loop-carried
  alias와 조건부 move를 놓치므로 기각했다.
- 구현 영향: own.c는 branch exit 전에 last-use를 반영하고 merge table/bit state를
  구현한다. runtime drop에는 `MaybeMoved` live flag가 필요하다.

### M7 contextual null/error-union construction

- 문제: `null`에 독립 타입을 주거나 error union을 일반 implicit conversion으로 다루면
  타입 추론·overload 후보가 늘고 nominal error 경계가 흐려진다.
- 결정: `null`은 expected optional/pointer-like type이 유일할 때만 구성된다. expected
  `E!T` 위치에서는 T가 success, E가 failure를 구성하며 return도 같다. E1/E2 및
  nominal error/`core.Error` 자동 변환은 없다.
- 근거: contextual expected type 한 개만 보면 되어 compiler A/B의 local type checker가
  결정적이고 nominal error 안전성도 유지된다.
- 기각한 대안: polymorphic null, 일반 union injection conversion, error widening. 숨은
  conversion 및 overload resolution을 요구하므로 기각했다.
- 구현 영향: check는 expected-type 전달 위치에서만 null/error construction을 허용하고
  문맥 없는 `let p = null`을 진단한다.

### M7 error declaration uniqueness

- 문제: code 0 외에도 한 nominal error 선언 안의 중복 member 이름이나 숫자 code는
  match/format 결과를 모호하게 만든다.
- 결정: code 0, 중복 이름, 중복 숫자 code를 모두 compile error로 한다. 서로 다른 nominal
  error 선언끼리는 같은 숫자를 사용할 수 있지만 타입은 계속 다르다.
- 근거: 선언 하나의 symbol/code table만 검사하면 되고 runtime representation은 바뀌지
  않는다.
- 구현 영향: error declaration check가 두 deterministic set을 만들고 중복 위치를 note로
  표시한다.

### M7 lazy recovery operators / non-Copy extraction

- 문제: `orelse`/`catch` RHS를 eager 평가하면 불필요한 side effect와 move가 생긴다.
  또한 `Some(x)` pattern이나 projection이 non-Copy payload를 암묵 이동하면 R7과
  조건부 drop이 불명확해진다.
- 결정: recovery RHS/handler는 failure 경로에서만 평가한다. optional pattern은 place의
  borrow/view이고 Copy만 복사한다. non-Copy owned payload 추출은 `mem.replace`로만 하며
  temporary optional 자동 추출 예외도 두지 않는다.
- 근거: 평가와 소유권 효과가 같은 CFG 경로를 따르고 기존 projection/R7 상태 기계를
  재사용한다.
- 기각한 대안: pattern별 destructive move와 temporary 특례. hidden move와 추가 drop
  상태를 만들고 source에서 비용이 보이지 않아 기각했다.
- 구현 영향: lower는 lazy branch를 만들고 own은 실행 경로별 effect를 합친다. pattern
  binding은 place mutability에 따른 shared/mutable borrow다.

### Hierarchical dotted unit namespace

- 문제: 단일 `unit foo` namespace는 외부 source library가 늘 때 `util`, `types`, `parse`
  같은 이름 충돌을 피할 수 없다.
- 결정: `unit_path := ident ('.' ident)*`, `import unit_path [as ident]`의 계층형 canonical
  이름을 도입한다. import binding은 마지막 segment이고 항상 `binding.member`로 접근한다.
- 근거: Go와 비슷한 단순 unit 전체 import를 유지하면서 namespace 충돌만 해결한다.
  resolver에는 relative scope walk나 symbol import가 필요 없다.
- 기각한 대안: relative/glob/selective imports, re-export, friend/package-private visibility.
  이름 해석과 캐시 의존 graph가 복잡해져 v0.1에서 제외했다.
- 구현 영향: lexer keyword 추가는 없고 parser/resolve/diagnostic이 canonical dotted path와
  optional alias를 보존한다.

### DOS-safe unit naming

- 문제: host의 case sensitivity와 FAT 8.3 규칙이 다르면 같은 source tree가 다른 unit을
  찾거나 긴 이름 전송 시 변형될 수 있다.
- 결정: unit segment는 lowercase ASCII, 첫 글자 letter, 이후 letter/digit/underscore,
  최대 8자로 제한한다. dotted path는 root 아래 `segment/.../last.fe`와 정확히 대응하고
  비교는 규범적 ASCII case-insensitive mapping을 쓴다.
- 근거: unit identity가 DOS와 host에서 같고 8.3 alias 생성에 기대지 않는다.
- 기각한 대안: 일반 Ferro identifier/임의 길이 허용 후 host별 normalization. case-fold와
  truncation 충돌이 platform-dependent라 기각했다.
- 구현 영향: entry path suffix로 project root를 계산하고 mismatch/case-variant duplicate를
  진단한다. canonical identity는 항상 lowercase dotted form이다.

### Deterministic import-root resolution / ambiguity rejection

- 문제: project root, 여러 `-I`, std root를 first-match-wins로 검색하면 `-I` 순서나 host
  directory 상태가 실제 선택 source를 바꾼다.
- 결정: 모든 candidate root를 조사하고 동일 unit에 서로 다른 canonical file이 둘 이상
  있으면 ambiguous error와 path note를 낸다. 같은 실제 file alias만 dedup한다.
- 근거: build/order/platform과 무관한 source graph를 만들어 `.fei`, generated C와 M12
  fixpoint를 안정시킨다.
- 기각한 대안: first-match-wins. 편하지만 shadowing이 command-line order에 숨어 결정성을
  깨므로 기각했다.
- 구현 영향: driver는 후보를 canonicalize·정렬한 뒤 identity를 비교하고 첫 성공에서
  검색을 중단하지 않는다.

### Reserved std namespace

- 문제: flat `io`, `mem`, `fmt`, `sys`는 user library 이름과 충돌하고 compiler 내장 std
  root를 일반 user root처럼 검색하면 같은 이름이 환경에 따라 shadow된다.
- 결정: `std` top-level을 compiler-reserved로 하고 `std.io`, `std.mem`, `std.fmt`,
  `std.sys`를 canonical unit으로 쓴다. 마지막 segment binding 때문에 사용 표면은
  `io.write`, `mem.replace`로 유지한다. `str`은 import unit이 아니다.
- 근거: std lookup이 명시적이고 deterministic이며 향후 source package와 충돌하지 않는다.
- 구현 영향: M8에서 std source layout/unit 선언을 이동하고 builtin std root는 `std.*`에만
  후보가 된다.

### Source-only external libraries

- 문제: v0.1에서 package manifest/solver나 stable binary ABI까지 정의하면 M8 범위를 넘어
  `.fei` encoding과 target C ABI를 영구 호환 계약으로 굳히게 된다.
- 결정: 외부 library는 source tree를 `-I` root로 제공한다. package manager/registry/version
  solver/manifest 문법과 `.fei + .obj/.lib` binary-only 배포 ABI는 지원하지 않는다.
- 근거: 언어 import 의미는 작게 유지하고 target/model별로 source에서 결정적으로
  재컴파일할 수 있다. DOS 배포 도구도 단순하다.
- 기각한 대안: package manager를 언어 의미론에 결합, binary-only ABI. compiler A와
  M12 전에 해결할 필요가 없고 호환 부담이 커 기각했다.
- 구현 영향: `-I`는 source-only candidate root이며 미래 도구도 root 구성만 담당한다.

### `.fei` interface/cache role과 deterministic schema

- 문제: source hash, public interface hash와 compile cache key가 섞여 있었고 `.fei`의
  최소 논리 정보·결정적 직렬화 조건이 없어 private 변경이 전체 rebuild를 유발하거나
  host path/timestamp가 fixpoint에 섞일 수 있었다.
- 결정: 세 hash 개념을 분리하고 `.fei`에 version/target/model, canonical unit, public
  signatures/layout, anonymous errors, exported generic body/support closure, direct dependency
  names/hashes를 기록한다. canonical key 순으로 직렬화하며 absolute path/time/build dir를
  금지한다.
- 근거: private non-generic 변경은 자기 unit만 재컴파일하고 interface가 같으면 dependent를
  유지할 수 있다. 같은 graph+target의 `.fei`는 host/build order와 무관하게 byte-identical하다.
- 기각한 대안: source hash를 interface hash로 재사용, unordered serializer. 구현은 짧지만
  불필요한 rebuild와 M12 비결정성을 만들어 기각했다.
- 구현 영향: `.fei` encoding은 자유지만 논리 schema와 sorted serialization을 만족하고
  driver cache가 dependency interface hash를 사용해야 한다.

### Ferro visibility vs backend linkage

- 문제: Ferro private를 무조건 C `static`으로 방출하면 통합 `fe_generics.c`의 exported
  instance가 definition unit private helper를 호출할 수 없다.
- 결정: Ferro private는 resolver visibility일 뿐 C linkage와 동일하지 않다. generic
  support에 필요한 private top-level symbol은 deterministic unit-mangled linkage와 internal
  prototype을 가질 수 있다.
- 근거: source 접근 권한은 유지하면서 단일 통합 generic body 방출을 가능하게 한다.
- 기각한 대안: `C static == Ferro private`, 또는 private helper를 instance마다 복제.
  전자는 linkage 실패, 후자는 중복과 비결정적 출력 때문에 기각했다.
- 구현 영향: resolver는 여전히 cross-unit private 참조를 거부하고 backend/internal header만
  `.fei` support metadata를 통해 symbol을 연결한다.

### M9 type-only generics

- 문제: user comptime value generic까지 허용하면 값 canonicalization, mangling, expression
  evaluator와 instance explosion 정책을 M9에서 함께 설계해야 한다.
- 결정: v0.1 user generic parameter는 `type`만 지원한다. `struct Box(T)`는
  `comptime T: type` shorthand이며 builtin comptime value와 구별한다.
- 근거: List/Map과 compiler B에 필요한 추상화를 충족하면서 instance key를 canonical type
  list로 제한한다. trait/bound도 추가하지 않는다.
- 기각한 대안: integer/string/bool value generics. M11 필수 기능이 아니고 구현/결정성
  부담이 커 v0.1 이후로 미룬다.
- 구현 영향: parser/check가 user generic parameter type을 제한하고 value generic fixture를
  명시적으로 거부한다.

### M9 no type inference

- 문제: `id(3)`에서 T를 추론하려면 argument constraints, conversion 후보와 향후 overload
  규칙을 정의해야 하고 진단/instance 발견 순서도 복잡해진다.
- 결정: generic type argument는 `id(i32, 3)`처럼 항상 명시한다. compiler-known
  `mem.create(value)` inference는 별도 intrinsic 규칙이다.
- 근거: call syntax만 보고 instance key가 결정되어 compiler A/B가 단순하고 deterministic하다.
- 기각한 대안: generic type inference. 편의보다 숨은 constraint solver 비용이 커 기각했다.
- 구현 영향: generic call arity/type argument check는 명시 목록만 검사하며 inference
  fallback을 시도하지 않는다.

### Definition-site resolution

- 문제: generic body의 non-dependent 이름을 caller scope에서 다시 찾으면 caller의 import와
  shadowing에 따라 같은 generic이 다른 코드를 만든다.
- 결정: 이름은 definition unit에서 고정하고 type-dependent operation만 instantiation 때
  검사한다. `comptime if`의 선택되지 않은 branch는 parse만 하고 semantic 처리하지 않는다.
- 근거: lexical 의미와 private support를 유지하고 caller/build order와 무관한 instance를
  만든다.
- 기각한 대안: use-site lookup과 selected-out branch의 eager type check. 전자는 의미가
  불안정하고 후자는 type-specific branch를 불가능하게 해 기각했다.
- 구현 영향: `.fei`가 body token과 definition-scope symbol/support identity를 전달하고
  generic.c가 그 환경에서 재검사한다.

### Deterministic monomorphization

- 문제: request 발견 순서대로 instance를 방출하면 unit traversal/hash iteration/parallel
  build에 따라 `fe_generics.c`와 symbol 순서가 바뀐다.
- 결정: canonical definition unit + declaration identity + normalized canonical type args를
  key로 dedup하고 byte ordering으로 정렬한다. prototype 전부를 먼저, body 전부를 나중에
  같은 canonical 순서로 방출한다.
- 근거: alias instance가 중복되지 않고 recursion/cross-instance call을 지원하며 M12에서
  byte-identical output을 만든다.
- 기각한 대안: first-request order 또는 pointer/insertion-order key. platform/build order에
  의존해 기각했다.
- 구현 영향: driver/generic.c가 global request set을 정렬하고 alias를 underlying interned
  identity로 normalize한다.

### Generic private-support closure

- 문제: exported generic이 private helper/type/const/private generic을 참조하면 signature만
  담은 `.fei`로 다른 unit에서 안전하게 instantiate할 수 없다.
- 결정: `.fei`는 필요한 private support dependency의 transitive closure를 compiler-only
  metadata로 제공한다. 이는 Ferro visibility를 public으로 바꾸지 않는다.
- 근거: definition-site semantics와 source private API를 동시에 지키며 `fe_generics.c`에서
  정확한 backend symbol/layout을 사용할 수 있다.
- 기각한 대안: 모든 support를 source `pub`으로 강제하거나 generic body를 definition unit마다
  static 복제. API 누출 또는 중복/링크 문제 때문에 기각했다.
- 구현 영향: interface hash는 exported generic이 관찰하는 support 변화에 반응하고
  serializer는 closure를 canonical 순서로 기록한다.

### Recursive instantiation semantics

- 문제: 단순 재귀 호출이 같은 instance를 다시 요청할 때마다 depth를 올리면 정상 generic
  recursion도 limit에 걸리고, 반대로 growing type chain을 dedup만으로 허용하면 무한 생성된다.
- 결정: pending/known 동일 key 재요청은 재사용하고 depth를 소비하지 않는다. 새로운 distinct
  instance chain만 증가시키며 32 초과를 에러로 한다.
- 근거: ordinary recursion은 prototype-first 방출로 처리하고 실제 instance explosion만
  유한한 local driver 상태로 차단한다.
- 구현 영향: generic.c는 pending/known set과 distinct chain stack을 구별하고 최초/현재
  instantiation 위치를 note로 출력한다.

### Canonical C mangling and type identity

- 문제: unit이 계층화되고 generic이 통합 방출되면 host path, pointer address 또는 insertion
  order 기반 이름은 충돌하거나 run마다 달라질 수 있다.
- 결정: mangling은 canonical dotted unit + declaration + normalized canonical type args만
  사용한다. nominal identity는 fully-qualified defining unit+name, alias는 underlying identity다.
- 근거: collision-free backend linkage와 M12 fixpoint를 동시에 보장한다.
- 구현 영향: 정확한 escaping 문자는 구현 세부지만 deterministic separator encoding과
  collision 검사가 필요하며 absolute path/address를 symbol에 포함할 수 없다.
