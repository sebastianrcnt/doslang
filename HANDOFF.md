# doslang 작업 인수인계

작성 시각: 2026-08-16 (Asia/Seoul)

## 최종 목표와 사용자 지시

- `SPEC.md`의 최신 명세대로 M14까지 전부 구현한다.
- DOSBox는 사용하지 않는다. 실행과 권위 있는 검증은 **QEMU FreeDOS 내부**에서만 한다.
- 컴파일러 A와 생성 C 모두 VM 내부 Open Watcom으로 컴파일한다.
  - 컴파일러 A / bits16: `WCL`
  - bits32 생성 C: `WCL386`
- 호스트 Windows는 소스 편집, diff, Git, 직렬 전송에만 사용한다.
- WSL이나 호스트 C 컴파일러 결과를 정식 검증으로 인정하지 않는다.
- 코딩은 가능한 한 `gpt-5.6-luna` 서브에이전트에 맡기고, 루트 에이전트는 명세 감사와 QEMU 검증을 담당한다.
- VGA 데모처럼 복잡한 멀티모달 수동 검증은 완료 게이트에서 제외한다.
- 검증된 마일스톤마다 커밋하고 항상 `origin`에 푸시한다.
- 구현 중 명세 판단이 바뀌면 `SPEC.md`를 즉시 최신화하고 `SPEC.AUDIT.md`에 변경 이유를 누적한다.

## 현재 완료 상태

현재 기준 커밋과 원격 `master`는 둘 다 다음 SHA다.

```text
53bca214b82c3abf7882ad41fb177cc488e999af
```

완료 및 QEMU/Open Watcom 검증된 범위:

- M1: lexer, parser, AST dump
- M2: 기본 타입 검사와 C 방출, bits16/bits32 경로
- M3: struct, enum, match, 배열, 슬라이스, str, 경계 검사, 반복 참조
- M4: `@print`, `@fprint`, `@sprint`, 최소 `io.Writer`

마지막 완료 커밋:

```text
53bca21 feat: implement M4 formatting builtins
8e6a409 feat: implement M3 aggregate types and iteration
57b47a5 docs: disambiguate control-flow headers
95de333 docs: disambiguate match scrutinees
3aa7618 docs: clarify char and byte conversions
```

M4 최종 검증 증거:

- VM의 `C:\FEC\BUILD.OK`: `OK`
- VM의 `C:\FEC\TEST.OK`: `OK`
- `C:\FEC\TEST-DOS.BAT`: exit 0
- 오류 코드 7을 반환하는 C89 Writer harness가 `@fprint`의 정확한 오류 보존과 첫 오류 이후 단축 중단을 실행 검증한다.
- 로컬/원격 SHA 일치까지 확인하고 푸시했다.

## 최신 명세

- `SPEC.md`: v0.1.5
- `SPEC.AUDIT.md`: 최초 감사 이후 설계 변경 로그
- 주요 후속 결정:
  - `char`와 `u8`은 별개 타입이며 명시적 `as`만 허용한다.
  - 제어 흐름 헤더 직후 `{`는 본문 시작이다. 헤더 최상위 구조체 초기화식은 괄호로 구분한다.

## 현재 워크트리: 미완성 M5

M5 작업 도중 인수인계를 위해 Luna를 중단했다. **아래 변경은 미검증·미커밋 상태이므로 버리지 말고 먼저 감사할 것.**

수정 파일:

```text
fec/src/check.c
fec/src/emit_c.c
fec/src/emit_c.h
fec/src/types.c
fec/src/types.h
fec/test-dos.bat
fec/tests/run-tests.sh
fec/vm-m1.bat
```

새 파일:

```text
fec/tests/m5/defer.fe
fec/tests/m5/owned.fe
fec/tests/m5/bad-move.fe
fec/tests/m5/bad-destroy.fe
```

현재 diff 규모는 약 251 insertions / 6 deletions이며 `git diff --check`는 통과했다. 아직 DOS로 전송하거나 빌드하지 않았다.

현재 부분 구현에 들어간 것으로 확인된 것:

- `FE_TYPE_OWNED`
- struct의 `has_drop` 메타데이터 일부
- 단순 `moved` 비트 기반 이동 후 사용 진단
- `mem.create` / `mem.destroy` 일부 checker/emitter 분기
- owned drop 및 block cleanup helper
- return/break/continue 정리 경로를 위한 emitter 코드 일부
- 정상 종료 defer 역순 방출 일부

그러나 M5 완료로 간주하면 안 된다. 첫 Luna 결과는 골격뿐이었고 다음 누락 때문에 반려했다.

- 자동 drop/free가 모든 경로에서 정확히 한 번 실행되는지
- `mem.create(T) -> !^T`의 실제 C ABI와 초기화
- 재대입 전에 기존 owned 값 정리
- defer와 drop의 선언 위치 기준 역순 병합
- return / break / continue / try 전파에서 cleanup
- 조건부 이동의 `MaybeMoved` 상태와 런타임 live flag
- struct drop 메서드 및 필드 역순 drop
- 분기/루프 상태 합류
- 누수와 이중 해제를 세는 런타임 harness
- R1/R3 실패 테스트 최소 수량

중단 직전 두 번째 Luna 패스가 cleanup 코드를 더 추가했으므로 위 항목 일부가 코드에 들어갔을 수 있다. 다음 세션은 반드시 `git diff`로 실제 구현을 재감사하고, 테스트가 증명하지 않는 기능은 완료 처리하지 말아야 한다.

현재는 `own.c/h`가 없고 소유권 로직이 주로 `check.c`/`emit_c.c`에 들어가 있다. 복잡도가 계속 커지면 명세 §11.5대로 `own.c/h`와 cleanup/lower 계층을 분리하는 편이 안전하다.

## 다음 세션 권장 순서

1. `git status --short`, `git diff --check`, M5 diff 전체를 읽는다.
2. Luna 읽기 전용 감사 에이전트로 M5와 R1~R5/§11.5를 대조한다.
3. 기존 Luna 구현 에이전트 또는 새 `gpt-5.6-luna`에게 발견된 누락을 수정시킨다.
4. 최소 다음 테스트를 갖춘다.
   - owned 정상 scope 종료 cleanup
   - 조기 return cleanup
   - return 시 defer 실행
   - 중첩 defer/drop 역순
   - owned 재대입 시 기존 값 cleanup
   - 함수 인자 이동 후 사용 실패
   - 조건부 이동 후 사용 실패 및 정확한 drop
   - 직접 `.drop()` 호출 실패
   - `mem.destroy` 후 재사용/이중 destroy 실패
   - 할당/해제 카운터로 누수 0, double free 0
5. 변경 파일과 M5 fixture를 직렬 프로토콜로 VM의 `C:\FEC`에 직접 전송한다.
6. `C:\FEC\TEST-DOS.BAT`를 실행한다. 실패하면 출력/생성 C/정확한 fixture를 좁혀 반복 수정한다.
7. `BUILD.OK`, `TEST.OK`, exit 0을 모두 확인한 뒤에만 M5 커밋 및 `git push origin master`.
8. 이후 M6~M14도 같은 방식으로 진행한다.

## QEMU와 직렬 에이전트 상태

인수인계 작성 시 QEMU는 재시작 없이 살아 있다.

```text
QEMU PID: 11700
monitor: 127.0.0.1:4444
DOS tool/controller: 127.0.0.1:5555
QEMU serial relay: 127.0.0.1:5556
observer: 127.0.0.1:5557
```

5555에 ASCII 한 줄 명령을 보내는 프로토콜:

```text
PING
READ <DOS_PATH_HEX> <OFFSET>
WRITE <DOS_PATH_HEX> T|A <CONTENT_HEX>
EXEC <COMMAND_HEX>
```

- `PING` 정상 응답: `OK 504F4E47`
- `WRITE`는 안전하게 1024바이트 조각으로 보내면 된다. 첫 조각 `T`, 이후 `A`.
- authoritative workspace는 VM의 `C:\FEC`이다.
- QEMU의 vvfat `D:`는 교환용으로만 취급하고 빌드하지 않는다. 과거 D:에서 빌드하다 QEMU가 rename 처리 오류로 종료된 적이 있다.
- DOS의 8.3 파일명 때문에 긴 fixture는 `BAD-ARI.FE`, `TRY-FPR.FE`처럼 명시적으로 짧은 이름으로 전송해야 한다.

QEMU monitor helper:

```powershell
.\.qemu\monitor.ps1 'sendkey ctrl-c'
.\.qemu\screenshot.ps1
```

긴 DOS 배치가 멈추면 QEMU를 재시작하지 말고 먼저 `sendkey ctrl-c`를 사용한다. FreeDOS가 다음 프롬프트를 보이면 monitor로 `y`, `ret`을 보낸다.

```text
Terminate batch file ... (Yes/No/All)?
```

그 뒤 `PING` 복구를 확인한다.

## 빌드 관련 함정

- 컴파일러 A는 16비트 large model로 빌드한다. small model은 메모리 부족이 났다.
- DOS 명령줄 길이 제한 때문에 compiler object link는 `*.obj`를 사용한다.
- 따라서 `fec/build-dos.bat`가 먼저 `C:\FEC\*.obj`를 지워 stale 32-bit test object가 섞이지 않게 한다.
- 생성 C의 보수적 미사용 helper 때문에 M4 Watcom 테스트는 `-wx -wcd=202`를 사용한다. W202만 끄고 다른 경고는 오류로 유지한다.
- 호스트에서 컴파일하지 말 것. 정적 text/diff 검사만 허용한다.
- `.qemu/qemu-screen.png`는 진단용이며 커밋하지 않는다.

## Git과 저장소

```text
origin: https://github.com/sebastianrcnt/doslang.git
branch: master
repository visibility: public
```

사용자는 모든 검증 커밋을 항상 원격에 푸시하길 원한다.

이 인수인계 문서는 별도 문서 커밋으로 푸시하되, 현재 미완성 M5 변경은 그 커밋에 포함하지 않는다.

## 전체 남은 목표

- M5: owned/drop/defer/move — 현재 미완성
- M6: `&`/`&mut`, R1~R8 전체 borrow checker
- M7: `?T`, `E!T`, try/catch
- M8: unit/import/.fei/분리 컴파일/std 초안
- M9: 제네릭 및 Ferro `List`/`Map`
- M10: bits16 far/asm/interrupt/shared/atomic/critical (VGA 수동 데모 제외)
- M11: Compiler B를 Ferro로 작성하고 A로 빌드
- M12: self-host fixpoint `B(B(B)) == B(B)` 후 A 폐기
- M13: 386 네이티브 백엔드
- M14: 8086 네이티브 백엔드, Watcom 없이 bits16 빌드

M14 전체 요구를 증명하기 전에는 goal을 complete로 표시하지 않는다.
