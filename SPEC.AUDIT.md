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
