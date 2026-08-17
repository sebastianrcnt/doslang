# GOAL — 부트스트랩까지 남은 작업

외부 스펙 감사(v0.1.8)와, 그 항목들을 실제로 빌드해서 확인한 결과를 합친 실행
계획이었다. **P0~P4 는 전부 끝났다.** 감사는 문서를 읽었고 여기 적힌 것은
컴파일러에 물어본 답이다.

```
uv run python tests/run.py     245/245   + std 밖 unsafe/*T 예산 검사
uv run python tests/exec.py     38/38
```

---

## 끝난 것

### P0 — 조용히 틀린 것

| # | 무엇이었나 | |
|---|---|---|
| 0-1 | 니치 옵셔널(`?^T`, `?&T`)이 포인터가 아니라 포인터가 든 자리를 넘겼다. `if let` 이 컴파일되고 쓰레기를 반환했다 | `exec/optref.fe` |
| 0-2 | `Handle(Node){ raw: 7 }` 이 파싱되지 않았다. 제네릭 인스턴스는 `Self{...}` 나 생성자로만 지을 수 있었다 | `exec/geninst/` |

### P1 — 스펙의 빈칸

| # | 결정 | |
|---|---|---|
| 1-1 | 정수 리터럴은 문맥이 요구하는 타입, 없으면 `i32`. 범위를 벗어나면 거부 | `types/badlit.fe`, `types/oklit.fe` |
| — | 그 자리에서 나온 것: **store 폭이 목적지가 아니라 값에서 왔다.** `let b: u8 = 200;` 이 4바이트를 1바이트 자리에 써서 옆 지역을 지웠다 | `exec/narrow.fe` |
| 1-2 | 미사용 타입 파라미터는 정상. typed handle 이 그 모양이다 | `generic/okphant.fe`, `badphant.fe` |
| 1-3 | `--no-checks` 에서 오버플로는 랩어라운드로 정의된다 | SPEC §7.4 |
| 1-4 | 작은 규칙 일곱 (SPEC §7.9). 다섯은 이미 옳았고, `defer` 안의 `return` 과 순회 중 원본 변경 둘은 그냥 통과하고 있었다 | `own/bdefret.fe`, `own/badforwr.fe`, `own/okforrd.fe` |

### P2 — SPEC 에서 죽은 C 백엔드 제거

`.fei`, `--emit-c`, `fe_errors.h`, "호스트 C 방출", "C 방출 시 static inline"
열 군데. 감사가 그 문서를 충실히 읽고 존재하지 않는 문제(C 의 부호 있는
오버플로 UB)를 보고했다 — 명세가 거짓말을 하면 그것을 읽는 사람이 틀린 답을
낸다. 오류 코드 절은 실제대로 다시 썼고, `--target=`/`--model=` 은 드라이버에서
없앴다.

### P3 — stdlib

| # | | |
|---|---|---|
| 3-1 | `arena.Handle(T)` — 8바이트 고정. 세대·epoch·arena id 를 담아 놓아준 뒤, reset 뒤, 다른 아레나에 물으면 `null` | `exec/arenat.fe` |
| 3-2 | `arena.Arena(T)` — 짧은 대여만. `get` 은 사본을 준다. `get_mut` 은 두지 않았다 | |
| 3-3 | `list.List(T)` 에 `pop`/`take`/`swap`/`slice`/`slice_mut`/`clear` | `exec/listmor.fe` |
| 3-4 | `intern.Interner` — `str` 을 꺼내는 API 없음. `eq`/`len_of`/`hash_of`/`find`/`copy_into` | `exec/interns.fe` |
| 3-5 | `std.map` 이 `StrId` 를 바이트 키로 받는다. 별도 `IntMap` 불필요 | |

길에서 고친 것 둘:
- 메서드 호출이 시그니처를 **호출자 유닛**에서 풀었다.
- 제네릭 인스턴스를 필드로 담은 구조체가 **크기 0 으로 굳었다** — 짓는 중인
  인스턴스가 배치되어 버려서. `Holder` 가 36 바이트 대신 12 였다.

### P4 — 측정

`--report-unsafe`, `--report-instances`. 그리고 그 숫자를 `run.py` 가 검사한다:
`std.mem`/`std.sys` 밖의 `unsafe` 와 `*T` 는 0 이어야 하고, 늘어나면 빌드가
실패한다.

```
unit                   unsafe       *T  unchecked
std.io                      6        1          0
std.sys                    10       12          0
total                      16       13          0
outside std                 0        0          0
```

---

## 채택하지 않은 것

| 감사 항목 | 판정 | 근거 |
|---|---|---|
| 참조 튜플 `-> (&mut T, &mut T)` | 보류 | struct 는 필드 단위 대여로 풀렸다. 컨테이너의 두 원소만 남는데 렉서·파서에서 필요했던 자리가 0 번 |
| `Arena.get_mut -> ?&mut T` | 거부 | 아레나 하나를 통째로 잠그는 참조를 오래 들고 있게 하는 API |
| `fmt.fmt_strid` | 거부 | `@print` 가 interner 인스턴스에 닿을 수 없다 (R10) |
| 별도 `IntMap(V)` | 불필요 | `std.map` 이 이미 포괄 |
| C 오버플로 방출 규칙 | 대체 | C 백엔드가 없다. 결론만 1-3 으로 흡수 |
| R4 완화 | 영구 제외 | |

---

## 다음

부트스트랩까지 남은 것은 `TODO.md` 에 있다. 도구는 다 갖췄다 — 아레나, 핸들,
맵, interner, 그리고 리졸버가 쓸 `Node.bind` 와 `Map.clear`.
