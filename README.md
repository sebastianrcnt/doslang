# Ferro

DOS/Windows용 시스템 프로그래밍 언어와 그 컴파일러 `fec`. C만큼 빠르고, 메모리
안전성을 **함수 단위 지역 검사만으로** 보장한다 — 라이프타임 표기도, 전역 분석도
없다. 파일 확장자는 `.fe`, 타깃은 i386 보호모드 플랫 하나다.

```fe
unit hello;

import std.io;

fn main() -> i32 {
    @print("hello, {}\n", "world");
    return 0;
}
```

## 문서

**`SPEC.md`가 언어의 유일한 규범 문서다.** 구현과 다르면 둘 중 하나가 틀린 것이다.

| 파일 | 무엇에 답하나 |
|---|---|
| [`SPEC.md`](SPEC.md) | 언어가 무엇인가 |
| [`IR.md`](IR.md) | 프론트엔드와 기계 사이가 어떻게 생겼나 |
| [`TODO.md`](TODO.md) | 무엇이 남았나 |
| [`AGENTS.md`](AGENTS.md) | 여기서 어떻게 작업하나 |
| [`audits/`](audits/) | 그때 조사해보니 어땠나 (불변 기록) |

## 검증

```powershell
uv run python tests/run.py                  # 컴파일러가 프로그램에 대해 뭐라고 하는가
uv run python tests/exec.py                 # 컴파일된 프로그램이 실제로 무엇을 하는가
uv run python tests/build.py <프로그램.fe>   # 하나만 빌드해서 돌려보기
```

두 스위트를 모두 통과해야 한다. `run.py`만 보면 진단은 옳은데 코드가 안 나오는
상태를 놓친다. **두 스위트의 통과 수가 현재 상태다.**

## 파이프라인

```
.fe  →  fec  →  i386 asm  →  wasm  →  wlink  →  .exe
        └ lexer parser resolve types own check  (프론트엔드)
        └ lower                                 (IR)
        └ x86                                   (백엔드)
```

`wasm`과 `wlink`는 고정된 Open Watcom의 어셈블러와 링커다 (WebAssembly와 무관).
`.dosboxx/watcom`에 고정되어 있고, 없으면 오류로 멈춘다. 링커와 오브젝트 포맷을
새로 만들지 않는다는 것이 `SPEC.md` §1 철학 6이다.

## 브랜치

primary 브랜치는 `master`다.
