# 마커 없는 fixture 진단 증거 보고서

기준선: `uv run python tests/run.py` → `150/188 passed (58 pin a line and message)`

이 보고서는 마커가 없는 37개 fixture를 직접 읽고, 기준선에서 새로 빌드된
`.build/fec.exe --check`의 실제 진단을 확인한 결과다. 진단 전문의 색상 제어 문자는
가독성을 위해 제거했으며, 텍스트·줄·열·진단 순서는 그대로 기록했다.

## types

### `fec/tests/types/bad_ari.fe`

- 검사 대상: 함수 호출 인자 개수 불일치.
- 근거: `add`는 두 인자를 받지만 8행에서 `add(1)`로 한 인자만 전달한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_ari.fe:8:15: error: wrong number of arguments
    8 |     return add(1);
      |               ^
  ```

- 일치 여부: 예 — 호출의 인자 개수 위반을 직접 진단한다.

### `fec/tests/types/bad_asgn.fe`

- 검사 대상: 불변 `let` 변수에 대입.
- 근거: 4행에서 `let value`로 선언한 뒤 5행에서 `value = 2`로 대입한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_asgn.fe:5:5: error: cannot assign to immutable let
    5 |     value = 2;
      |     ^
  ```

- 일치 여부: 예 — `let`의 불변성 위반을 직접 진단한다.

### `fec/tests/types/bad_cast.fe`

- 검사 대상: 허용되지 않는 타입의 `as` 변환.
- 근거: 4행에서 `bool` 값 `true`를 `i32`로 변환한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_cast.fe:4:23: error: 'as' requires integer or char types
    4 |     let x: i32 = true as i32;
      |                       ^
  ```

- 일치 여부: 예 — 정수/문자가 아닌 피연산자의 캐스트를 직접 진단한다.

### `fec/tests/types/bad_cond.fe`

- 검사 대상: 조건식의 비-`bool` 값 사용.
- 근거: 4행의 `if 1`에서 정수 리터럴을 조건으로 사용한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_cond.fe:4:5: error: if condition must be bool
    4 |     if 1 { return 0; }
      |     ^
  ```

- 일치 여부: 예 — 조건식이 `bool`이어야 한다는 규칙을 직접 진단한다.

### `fec/tests/types/bad_mlet.fe`

- 검사 대상: `let`으로 mutable slice를 바인딩.
- 근거: 4행에서 mutable 배열 slice를 만든 뒤 5행의 `let s: []mut u8`에 바인딩한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_mlet.fe:5:5: error: let cannot bind a mutable slice
    5 |     let s: []mut u8 = raw[..];
      |     ^
  ```

- 일치 여부: 예 — mutable slice의 `let` 바인딩 금지를 직접 진단한다.

### `fec/tests/types/bad_ret.fe`

- 검사 대상: 반환식과 함수 반환 타입의 불일치.
- 근거: `main`은 `i32`를 반환한다고 선언했지만 4행에서 `true`를 반환한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_ret.fe:4:5: error: return type mismatch
    4 |     return true;
      |     ^
  ```

- 일치 여부: 예 — 반환 타입 불일치를 직접 진단한다.

### `fec/tests/types/bad_shwr.fe`

- 검사 대상: shared slice를 통한 쓰기.
- 근거: `s`는 `[]u8` shared slice인데 4행에서 `s[0] = 1`로 쓴다.
- 실제 진단:

  ```text
  fec/tests/types/bad_shwr.fe:4:6: error: cannot write through shared slice
    4 |     s[0] = 1;
      |      ^
  ```

- 일치 여부: 예 — shared slice 쓰기 위반을 직접 진단한다.

### `fec/tests/types/bad_type.fe`

- 검사 대상: 함수 인자 타입 불일치.
- 근거: `add`의 첫 인자는 `i32`인데 8행에서 `true`를 전달한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_type.fe:8:16: error: argument type mismatch
    8 |     return add(true, 1);
      |                ^
  ```

- 일치 여부: 예 — 함수 인자의 타입 불일치를 직접 진단한다.

### `fec/tests/types/bad_unit.fe`

- 검사 대상: 초기화되지 않은 지역 변수 사용.
- 근거: 4행에서 `var value: i32`만 선언하고 값을 넣지 않은 채 5행에서 반환한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_unit.fe:5:12: error: use of uninitialized variable
    5 |     return value;
      |            ^
  ```

- 일치 여부: 예 — 초기화되지 않은 변수 사용을 직접 진단한다.

### `fec/tests/types/bad_unk.fe`

- 검사 대상: 정의되지 않은 이름 사용.
- 근거: 4행에서 선언되지 않은 `missing_name`을 반환한다.
- 실제 진단:

  ```text
  fec/tests/types/bad_unk.fe:4:12: error: unknown name
    4 |     return missing_name;
      |            ^
  ```

- 일치 여부: 예 — 미정의 이름 사용을 직접 진단한다.

### `fec/tests/types/bad_void.fe`

- 검사 대상: `void` 표현식을 값 변수의 초기화식으로 사용.
- 근거: 반환값이 없는 `noop()`의 결과를 8행에서 `i32` 변수에 넣는다.
- 실제 진단:

  ```text
  fec/tests/types/bad_void.fe:8:5: error: initializer type mismatch
    8 |     let value: i32 = noop();
      |     ^
  fec/tests/types/bad_void.fe:8:5: error: void expression cannot initialize a variable
    8 |     let value: i32 = noop();
      |     ^
  ```

- 일치 여부: 예 — void 표현식의 값 초기화 사용을 직접 진단한다.

### `fec/tests/types/badarr.fe`

- 검사 대상: 배열 리터럴의 원소 타입 및 선언된 배열 타입 불일치.
- 근거: `[2]i32`에 세 원소를 쓰고, 둘째 원소로 `bool`인 `true`를 넣는다.
- 실제 진단:

  ```text
  fec/tests/types/badarr.fe:3:25: error: array element type mismatch
    3 |     let a: [2]i32 = [1, true, 3];
      |                         ^
  fec/tests/types/badarr.fe:3:5: error: initializer type mismatch
    3 |     let a: [2]i32 = [1, true, 3];
      |     ^
  ```

- 일치 여부: 예 — 배열 원소 타입 위반을 직접 진단하고, 선언 타입 불일치도 함께 진단한다.

### `fec/tests/types/badchar.fe`

- 검사 대상: 명시적 캐스트 없는 `char`와 `u8`의 대입.
- 근거: 4행에서 `char` 리터럴을 `u8` 변수에 직접 넣는다.
- 실제 진단:

  ```text
  fec/tests/types/badchar.fe:4:5: error: initializer type mismatch
    4 |     let u: u8 = 'A';
      |     ^
  fec/tests/types/badchar.fe:5:5: error: return type mismatch
    5 |     return u;
      |     ^
  ```

- 일치 여부: 예 — 첫 진단이 char/u8 직접 대입의 타입 불일치를 짚는다. 5행 진단은 연쇄 오류다.

### `fec/tests/types/badcycle.fe`

- 검사 대상: 값으로 연결된 재귀 구조체 타입.
- 근거: `A`가 `B`를 값으로 포함하고 `B`가 다시 `A`를 값으로 포함한다.
- 실제 진단:

  ```text
  fec/tests/types/badcycle.fe:1:1: error: by-value recursive type
    1 | unit badcycle;
      | ^
  ```

- 일치 여부: 예 — 값 기반 재귀 타입을 직접 진단한다. 위치가 선언부 첫 줄로 올라가지만 진단 종류는 정확하다.

### `fec/tests/types/badfield.fe`

- 검사 대상: 불변 구조체 값의 필드에 대입.
- 근거: `p`는 `let`으로 선언됐고 6행에서 `p.x = 3`을 수행한다.
- 실제 진단:

  ```text
  fec/tests/types/badfield.fe:6:6: error: cannot assign through immutable value
    6 |     p.x = 3;
      |      ^
  ```

- 일치 여부: 예 — 불변 값의 projection을 통한 쓰기를 직접 진단한다.

### `fec/tests/types/badfld.fe`

- 검사 대상: 구조체 초기화 필드 누락과 존재하지 않는 필드 접근.
- 근거: `Point`는 `x`, `y`를 요구하지만 4행 초기화에는 `x`만 있고, 5행에서 없는 `z`에 접근한다.
- 실제 진단:

  ```text
  fec/tests/types/badfld.fe:4:20: error: missing struct field
    4 |     let p: Point = Point{ x: 1 };
      |                    ^
  fec/tests/types/badfld.fe:5:13: error: unknown struct field
    5 |     return p.z;
      |             ^
  ```

- 일치 여부: 예 — 두 필드 규칙 위반을 모두 직접 진단한다.

### `fec/tests/types/badindex.fe`

- 검사 대상: 불변 배열을 통한 요소 쓰기.
- 근거: `a`는 `let` 배열인데 5행에서 `a[0] = 3`을 수행한다.
- 실제 진단:

  ```text
  fec/tests/types/badindex.fe:5:6: error: cannot assign through immutable value
    5 |     a[0] = 3;
      |      ^
  ```

- 일치 여부: 예 — 불변 배열 index projection을 통한 쓰기를 직접 진단한다.

### `fec/tests/types/badmat.fe`

- 검사 대상: 비-완전 `match`.
- 근거: `Shape`에는 `Empty`, `Circle` 두 variant가 있는데 4행 match에는 `Empty`만 있다.
- 실제 진단:

  ```text
  fec/tests/types/badmat.fe:4:5: error: non-exhaustive match
    4 |     match Shape.Empty { Empty => 0; }
      |     ^
  ```

- 일치 여부: 예 — match의 비-완전성을 직접 진단한다.

### `fec/tests/types/badstr.fe`

- 검사 대상: shared string/slice를 통한 쓰기.
- 근거: `str`인 `text`의 4행에서 인덱스 요소에 대입한다.
- 실제 진단:

  ```text
  fec/tests/types/badstr.fe:4:9: error: cannot write through shared slice
    4 |     text[0] = 'z';
      |         ^
  fec/tests/types/badstr.fe:4:13: error: assignment type mismatch
    4 |     text[0] = 'z';
      |             ^
  ```

- 일치 여부: 예 — 첫 진단이 shared string 쓰기를 직접 짚고, 두 번째는 요소 타입의 연쇄 진단이다.

## format

### `fec/tests/format/bad_ari.fe`

- 검사 대상: format placeholder와 인자 개수 불일치.
- 근거: 4행의 format 문자열에는 `{}`가 두 개지만 인자는 `1` 하나다.
- 실제 진단:

  ```text
  fec/tests/format/bad_ari.fe:4:6: error: format argument count mismatch
    4 |     @print("{} {}", 1);
      |      ^
  fec/tests/format/bad_ari.fe:4:6: error: format argument count mismatch
    4 |     @print("{} {}", 1);
      |      ^
  ```

- 일치 여부: 예 — 인자 개수 불일치를 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/format/bad_bufw.fe`

- 검사 대상: `io.buf_writer(buf)`를 통한 buffer writer 구성.
- 근거: `[]mut u8` 버퍼를 `io.buf_writer`에 전달하지만, 이 호출이 정확히 어떤 금지 규칙을 의도하는지는 파일만으로 확정하기 어렵다. 명세의 `io.Writer`는 enum handle이며 `io.buf_writer` API는 정의되어 있지 않다.
- 실제 진단:

  ```text
  fec/tests/format/bad_bufw.fe:6:13: error: unknown name
    6 |     let w = io.buf_writer(buf);
      |             ^
  fec/tests/format/bad_bufw.fe:6:26: error: invalid enum variant constructor
    6 |     let w = io.buf_writer(buf);
      |                          ^
  ```

- 일치 여부: 애매 — 존재하지 않는 `io.buf_writer`를 거부한다는 점은 맞지만, fixture가 검사하려는 구체적인 buffer-writer 규칙을 진단한 것인지 코드만으로 판정할 수 없다.

### `fec/tests/format/bad_cls.fe`

- 검사 대상: 닫히지 않은 placeholder가 아니라 unmatched `}` 형식 오류.
- 근거: 4행의 format 문자열이 단독 `}`를 포함한다.
- 실제 진단:

  ```text
  fec/tests/format/bad_cls.fe:4:6: error: unmatched '}' in format
    4 |     @print("}", 1);
      |      ^
  fec/tests/format/bad_cls.fe:4:6: error: format argument count mismatch
    4 |     @print("}", 1);
      |      ^
  ```

- 일치 여부: 예 — 첫 진단이 unmatched `}`를 직접 짚는다. 두 번째는 파생된 개수 진단이다.

### `fec/tests/format/bad_many.fe`

- 검사 대상: placeholder보다 많은 format 인자.
- 근거: 문자열에는 `{}` 하나뿐인데 4행에서 `1, 2` 두 인자를 전달한다.
- 실제 진단:

  ```text
  fec/tests/format/bad_many.fe:4:6: error: format argument count mismatch
    4 |     @print("{}", 1, 2);
      |      ^
  ```

- 일치 여부: 예 — format 인자 개수 불일치를 직접 진단한다.

### `fec/tests/format/bad_open.fe`

- 검사 대상: 닫히지 않은 format placeholder.
- 근거: 4행의 문자열에 여는 `{`만 있고 닫는 `}`가 없다.
- 실제 진단:

  ```text
  fec/tests/format/bad_open.fe:4:6: error: unterminated format placeholder
    4 |     @print("{", 1);
      |      ^
  fec/tests/format/bad_open.fe:4:6: error: format argument count mismatch
    4 |     @print("{", 1);
      |      ^
  ```

- 일치 여부: 예 — 첫 진단이 종료되지 않은 placeholder를 직접 짚는다. 두 번째는 파생된 개수 진단이다.

### `fec/tests/format/bad_run.fe`

- 검사 대상: 런타임 문자열을 format 문자열로 사용.
- 근거: 4행에서 `fmt`를 `var str`로 선언하고 5행에서 `@print(fmt, 1)`에 전달한다.
- 실제 진단:

  ```text
  fec/tests/format/bad_run.fe:5:6: error: format must be a comptime string
    5 |     @print(fmt, 1);
      |      ^
  ```

- 일치 여부: 예 — format 문자열의 comptime 제약을 직접 진단한다.

### `fec/tests/format/bad_try.fe`

- 검사 대상: 오류 결과가 아닌 `@print`에 `try` 사용.
- 근거: 명세상 `@print`은 `void`를 반환하는데 4행에서 `try @print(...)`을 쓴다.
- 실제 진단:

  ```text
  fec/tests/format/bad_try.fe:4:5: error: try requires an error result
    4 |     try @print("nope");
      |     ^
  ```

- 일치 여부: 예 — `try`의 오류 결과 요구를 직접 진단한다.

### `fec/tests/format/bad_type.fe`

- 검사 대상: format writer가 없는 타입을 format 인자로 사용.
- 근거: `Point` 구조체 값을 7행에서 `{}` placeholder의 인자로 전달한다.
- 실제 진단:

  ```text
  fec/tests/format/bad_type.fe:7:18: error: no fmt writer for argument type
    7 |     @print("{}", p);
      |                  ^
  ```

- 일치 여부: 예 — 해당 타입을 포맷할 writer가 없음을 직접 진단한다.

### `fec/tests/format/bad_verb.fe`

- 검사 대상: 지원되지 않는 format verb.
- 근거: 4행의 `{q}`에서 `q`는 명세에 없는 verb다.
- 실제 진단:

  ```text
  fec/tests/format/bad_verb.fe:4:6: error: unsupported format verb
    4 |     @print("{q}", 1);
      |      ^
  ```

- 일치 여부: 예 — 지원되지 않는 verb를 직접 진단한다.

### `fec/tests/format/bad_writ.fe`

- 검사 대상: `@fprint` 첫 인자의 `io.Writer` 타입 위반.
- 근거: 5행에서 writer 대신 `i32` 변수 `x`를 첫 인자로 전달한다.
- 실제 진단:

  ```text
  fec/tests/format/bad_writ.fe:5:13: error: @fprint requires io.Writer
    5 |     @fprint(x, "bad");
      |             ^
  ```

- 일치 여부: 예 — `@fprint`의 writer 요구를 직접 진단한다.

## own

### `fec/tests/own/bad_clos.fe`

- 검사 대상: 소유 값을 close 후 다시 사용.
- 근거: 11행의 `try file.close()`가 `file`을 이동시키고 12행에서 다시 `file.close()`를 호출한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_clos.fe:12:5: error: use of moved value
    12 |     file.close();
       |     ^
  fec/tests/own/bad_clos.fe:11:9: note: value was moved here
    11 |     try file.close();
       |         ^
  fec/tests/own/bad_clos.fe:12:5: error: use of moved value
    12 |     file.close();
       |     ^
  fec/tests/own/bad_clos.fe:11:9: note: value was moved here
    11 |     try file.close();
       |         ^
  ```

- 일치 여부: 예 — 이동된 값을 재사용한 위치와 이동 위치를 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/own/bad_cond.fe`

- 검사 대상: 조건부 이동 후 값의 무조건 사용.
- 근거: 6행의 조건 분기 안에서 `take(p)`가 `p`를 이동할 수 있고 7행에서 무조건 `p`를 쓴다.
- 실제 진단:

  ```text
  fec/tests/own/bad_cond.fe:7:5: error: use of possibly moved value
    7 |     p.^ = 3;
      |     ^
  fec/tests/own/bad_cond.fe:7:5: error: use of possibly moved value
    7 |     p.^ = 3;
      |     ^
  ```

- 일치 여부: 예 — 조건부 이동 가능성을 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/own/bad_dbl.fe`

- 검사 대상: 소유 포인터의 이중 destroy.
- 근거: 4행에서 `p`를 destroy한 뒤 5행에서 같은 `p`를 다시 destroy한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_dbl.fe:5:17: error: use of moved value
    5 |     mem.destroy(p);
      |                 ^
  fec/tests/own/bad_dbl.fe:4:17: note: value was moved here
    4 |     mem.destroy(p);
      |                 ^
  fec/tests/own/bad_dbl.fe:5:17: error: use of moved value
    5 |     mem.destroy(p);
      |                 ^
  fec/tests/own/bad_dbl.fe:4:17: note: value was moved here
    4 |     mem.destroy(p);
      |                 ^
  ```

- 일치 여부: 예 — 두 번째 destroy의 이동 후 사용을 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/own/bad_dest.fe`

- 검사 대상: owned pointer가 아닌 값을 `mem.destroy`에 전달.
- 근거: 4행에서 일반 `i32` 값 `x`를 `mem.destroy(x)`에 전달한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_dest.fe:4:16: error: mem.destroy requires exactly one owned pointer
    4 |     mem.destroy(x);
      |                ^
  ```

- 일치 여부: 예 — `mem.destroy`의 owned pointer 요구를 직접 진단한다.

### `fec/tests/own/bad_drop.fe`

- 검사 대상: 사용자 `drop` 메서드의 직접 호출.
- 근거: `Box`에 `drop`을 정의했지만 10행에서 `b.drop()`으로 직접 호출한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_drop.fe:10:11: error: drop may only be invoked by scope cleanup
    10 |     b.drop();
       |           ^
  ```

- 일치 여부: 예 — drop은 scope cleanup에서만 호출된다는 규칙을 직접 진단한다.

### `fec/tests/own/bad_loop.fe`

- 검사 대상: 반복문 안의 이동으로 인한 possibly-moved 값 사용.
- 근거: 6행의 `while` 본문에서 매 반복 `take(p)`가 `p`를 이동할 수 있다.
- 실제 진단:

  ```text
  fec/tests/own/bad_loop.fe:6:24: error: use of possibly moved value
    6 |     while again { take(p); }
      |                        ^
  fec/tests/own/bad_loop.fe:6:24: error: use of possibly moved value
    6 |     while again { take(p); }
      |                        ^
  ```

- 일치 여부: 예 — 반복에 따른 possibly-moved 상태를 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/own/bad_move.fe`

- 검사 대상: 함수 인자의 이중 이동.
- 근거: 6행의 첫 `take(p)`가 `p`를 이동한 뒤 7행에서 다시 `take(p)`를 호출한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_move.fe:7:10: error: use of moved value
    7 |     take(p);
      |          ^
  fec/tests/own/bad_move.fe:6:10: note: value was moved here
    6 |     take(p);
      |          ^
  fec/tests/own/bad_move.fe:7:10: error: use of moved value
    7 |     take(p);
      |          ^
  fec/tests/own/bad_move.fe:6:10: note: value was moved here
    6 |     take(p);
      |          ^
  ```

- 일치 여부: 예 — 이동된 인자의 재사용과 최초 이동 위치를 직접 진단한다. 동일 진단이 중복 출력된다.

### `fec/tests/own/bad_proj.fe`

- 검사 대상: 구조체 projection에서 non-Copy 소유 필드 이동.
- 근거: `Holder.p`는 owned pointer이고 7행에서 `take(h.p)`로 필드 밖으로 직접 이동하려 한다.
- 실제 진단:

  ```text
  fec/tests/own/bad_proj.fe:7:11: error: cannot move a non-Copy value out of a projection; use mem.replace
    7 |     take(h.p);
      |           ^
  ```

- 일치 여부: 예 — projection에서 non-Copy 값을 이동할 수 없다는 규칙과 대안을 직접 진단한다.

## 판정 요약

- `아니오`: 없음.
- `애매`: `fec/tests/format/bad_bufw.fe` — `io.buf_writer` 자체가 명세에 없으므로 fixture의 구체적 의도를 확정할 수 없음.
- 나머지 36개: 실제 진단이 코드가 검사하려는 규칙과 일치.
