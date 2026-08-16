# doslang 작업 규칙

DOS용 시스템 프로그래밍 언어 Ferro와 그 컴파일러 `fec`. 규범 문서는 `SPEC.md`이며
이 파일은 그것을 구현할 때의 작업 규칙만 다룬다.

## 문서 지도

| 파일 | 역할 |
|---|---|
| `SPEC.md` | 언어 명세. 유일한 규범 문서. 구현 지시서와 표준 라이브러리 명세는 별도 문서 |
| `tools/README.md` | 호스트 요구사항, 최초 셋업, 자동화 구조 |

개발 환경과 테스트 명령 목록·플래그는 CLI로 확인한다.

```powershell
uv run ferro-dos --help
uv run ferro-dos <command> --help
uv run ferro-test --help
```

## 검증 규칙

- 컴파일러와 생성 C는 DOSBox-X 내부의 고정된 Open Watcom으로 컴파일한다.
  컴파일러와 bits16은 `WCL`, bits32 생성 C는 `WCL386`을 쓴다.
- 호스트 C 컴파일러 결과는 검증으로 인정하지 않는다. 호스트는 편집, Git, 다운로드,
  격리 작업공간 준비에만 쓴다.
- 실행마다 만들어지는 authoritative workspace는 `C:\FEC`다. 호스트에서는
  `.dosboxx/runs/<run>/FEC`에 대응한다.
- 완료하려는 기능을 직접 검사하는 pytest case가 통과해야 한다. 테스트가 증명하지
  않는 기능은 완료로 처리하지 않는다.
- VGA 데모처럼 수동 검증이 필요한 항목은 자동 완료 게이트에서 제외한다.

## 빌드 함정

- 컴파일러는 16비트 large model로 빌드한다. small model은 메모리 부족으로 실패한다.
- 링크는 `*.obj` 와일드카드로 한다. DOS 명령줄 길이 제한 때문에 오브젝트를 나열할
  수 없다. `fec/build-dos.bat`는 먼저 stale object를 지운다.
- M4 Watcom 테스트는 `-wx -wcd=202`를 쓴다. 생성 C의 보수적 미사용 helper 때문에
  W202만 끄고 나머지 경고는 오류로 유지한다.
- fixture는 DOS 8.3 이름으로 실행한다. 긴 이름은 registry에서 명시적으로 줄인다.
- `R:`은 읽기 전용 저장소, `W:`은 읽기 전용 Watcom이다. 빌드 산출물은 반드시
  임시 `C:\FEC`에 쓴다.
- 실패 분석이 필요하면 `ferro-test --keep-failed` 또는 `ferro-dos --keep`으로
  임시 작업공간을 보존한다.

## 작업 흐름

- 명세 판단이 바뀌면 `SPEC.md`를 즉시 갱신하고 `SPEC.AUDIT.md`에 사유를 누적한다.
  구현이 명세와 다르면 둘 중 하나가 틀린 것이므로 그 자리에서 결론을 낸다.
- 검증된 마일스톤마다 커밋하고 항상 `origin`에 푸시한다.
- primary 브랜치는 `master`다.
- `.dosboxx/`의 다운로드, 실행 작업공간, 로그는 커밋하지 않는다.

## 현재 상태

현재 구현 상태와 다음 마일스톤은 `SPEC.md`와 테스트 registry를 기준으로 판단한다.
과거 VM 이미지나 호스트에 남은 바이너리를 근거로 완료 처리하지 않는다.
