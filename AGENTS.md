# doslang 작업 규칙

DOS용 시스템 프로그래밍 언어 Ferro와 그 컴파일러 `fec`. 규범 문서는 `SPEC.md`이며
이 파일은 그것을 구현할 때의 작업 규칙만 다룬다.

## 문서 지도

| 파일 | 역할 |
|---|---|
| `SPEC.md` | 언어 명세 + 구현 지시서. 유일한 규범 문서 |
| `SPEC.AUDIT.md` | 명세 변경의 문제·결정·근거·구현 영향 누적 로그 |
| `tools/README.md` | 호스트 요구사항, 최초 셋업, 자동화 구조 |
| `tools/tcpagent/README.md` | DOS 내부 TCP 에이전트 프로토콜과 빌드 |

VM 자동화 **명령 목록과 플래그는 문서가 아니라 CLI가 규범**이다. 문서에 복제하면
반드시 드리프트하므로 아래로 확인한다.

```powershell
uv run ferro-vm --help
uv run ferro-vm <command> --help
```

## 검증 규칙

- **실행 검증은 QEMU FreeDOS 내부에서만 한다.** DOSBox는 쓰지 않는다.
- 컴파일러 A와 생성 C 모두 VM 안의 Open Watcom으로 컴파일한다.
  컴파일러 A와 bits16은 `WCL`, bits32 생성 C는 `WCL386`.
- **호스트에서 컴파일하지 않는다.** WSL이나 호스트 C 컴파일러 결과는 정식 검증으로
  인정하지 않는다. 호스트는 편집, diff, Git, 파일 전송에만 쓴다.
- authoritative workspace는 VM의 `C:\FEC`다.
- 마일스톤 완료 기준은 `C:\FEC\BUILD.OK`, `C:\FEC\TEST.OK`, `TEST-DOS.BAT` exit 0
  세 가지를 모두 확인하는 것이다. 테스트가 증명하지 않는 기능은 완료로 처리하지
  않는다.
- VGA 데모처럼 멀티모달 수동 검증이 필요한 항목은 완료 게이트에서 제외한다.

## 빌드 함정

VM 안에서 반복해서 물렸던 것들. 어기면 원인 찾기 어려운 실패가 난다.

- **컴파일러 A는 16비트 large model로 빌드한다.** small model은 메모리 부족으로
  실패한다.
- **링크는 `*.obj` 와일드카드로 한다.** DOS 명령줄 길이 제한 때문에 오브젝트를
  나열할 수 없다. 그래서 `fec/build-dos.bat`는 먼저 `C:\FEC\*.obj`를 지워
  stale 32비트 test object가 섞이지 않게 한다.
- **M4 Watcom 테스트는 `-wx -wcd=202`를 쓴다.** 생성 C의 보수적 미사용 helper 때문에
  W202만 끄고 나머지 경고는 오류로 유지한다.
- **fixture는 짧은 이름으로 전송한다.** DOS 8.3 파일명 때문에 긴 이름은
  `BAD-ARI.FE`, `TRY-FPR.FE`처럼 명시적으로 줄여야 한다.
- **`D:`에서 빌드하지 않는다.** QEMU의 vvfat 뷰는 교환용이며, 과거 `D:`에서 빌드하다
  rename 처리 오류로 QEMU가 종료된 적이 있다. 호스트에서 편집한 파일은 `put`으로
  `C:`에 올린 뒤 컴파일한다.
- 긴 DOS 배치가 멈추면 QEMU를 재시작하기 전에 Ctrl+C 주입을 먼저 시도한다.
  `Terminate batch file ... (Yes/No/All)?`가 뜨면 `y`, `ret`을 보내고 `ping` 복구를
  확인한다.

## 작업 흐름

- 명세 판단이 바뀌면 `SPEC.md`를 즉시 갱신하고 `SPEC.AUDIT.md`에 사유를 누적한다.
  구현이 명세와 다르면 둘 중 하나가 틀린 것이므로 그 자리에서 결론을 낸다.
- 검증된 마일스톤마다 커밋하고 항상 `origin`에 푸시한다.
- primary 브랜치는 `master`다.
- `.qemu/*.png`, `.qemu/*.ppm`은 진단용이며 커밋하지 않는다.

## 현재 상태

- M1~M4 완료 및 QEMU/Open Watcom 검증됨.
- M5(`^T`, drop, defer, 이동 검사) 진행 중. 남은 것: 모든 경로에서 정확히 1회
  cleanup, defer와 drop의 선언 역순 병합, `try` 전파 경로 cleanup, `MaybeMoved`
  런타임 live flag, struct drop과 필드 역순 drop, 분기/루프 상태 합류,
  누수·이중해제 카운터 harness.
- `own.c/h`가 아직 없고 소유권 로직이 `check.c`/`emit_c.c`에 들어가 있다. R1~R8
  전체를 다루는 **M6 착수 시점에 분리한다** (`SPEC.md` §11.3).
- v0.1.6에서 R8(파생 반환), R6(마지막 사용까지 대여), R10(전역 대여 금지)이
  바뀌었다. 셋 다 own.c의 상태 기계를 건드리므로 분리 이후에 함께 구현한다.
