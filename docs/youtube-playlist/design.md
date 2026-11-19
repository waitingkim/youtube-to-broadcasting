# YouTube → Broadcasting 설계 문서 (v0.1)

> OBS Studio 소스 기반의 개인용 빌드. YouTube 링크/재생목록/개인 플레이리스트를 큐로 만들어
> OBS 안에서 Media Source 방식(ffmpeg 직접 재생)으로 끊김 없이 송출한다.

## 0. 확정된 결정

| 항목 | 결정 |
|---|---|
| 기반 | [obs-studio](https://github.com/obsproject/obs-studio) 소스 (기준: 33.0.1 고정) |
| 용도 | 개인용. 배포/외부 게시 없음 (GPL-2.0 배포 의무 해당 없음) |
| 재생 방식 | Media Source 계열 (ffmpeg 디코딩, 광고 없음) |
| 송출 | OBS 기존 출력 기능 그대로 사용 (RTMP/SRT/녹화) |
| 개발 플랫폼 | macOS (우선) |
| 레포 | `waitingkim/youtube-to-broadcasting` (obsproject/obs-studio 포크) |
| 기준 버전 | `33.0.1` 태그 고정 — `main` = 33.0.1 + 본 프로젝트 변경분 |

### 레포 운영
- `origin`  : git@github.com:waitingkim/youtube-to-broadcasting.git (작업 브랜치 `main`)
- `upstream`: https://github.com/obsproject/obs-studio.git (fetch 전용, push 비활성화)
- 버전 업그레이드 시: `git fetch upstream tag <ver>` → `git rebase --onto <ver> 33.0.1 main`
- 서브모듈(obs-browser, obs-websocket 등)은 33.0.1이 가리키는 커밋 그대로 사용

## 1. 전체 구조

```
┌──────────────────────────── OBS Studio (포크) ────────────────────────────┐
│                                                                            │
│  [frontend] YouTube Playlist Dock (Qt)                                     │
│     ├ URL 추가 / 재생목록 가져오기 / 내 플레이리스트 가져오기               │
│     ├ 큐 편집 (순서, 삭제, 셔플, 반복) · 상태 표시 · Skip                   │
│     └ YoutubeApiWrapper 확장 (기존 OAuth 재사용) ──▶ YouTube Data API v3   │
│                         │ (proc handler / settings)                        │
│                         ▼                                                  │
│  [plugins/youtube-playlist] youtube_playlist_source (async video source)  │
│     ├ Queue        : 항목 목록 + 현재 인덱스 (obs_data로 씬 컬렉션에 저장)  │
│     ├ Fetcher      : yt-dlp 서브프로세스 → 로컬 캐시 파일 (선행 다운로드)    │
│     ├ Player A / B : shared/media-playback (mp_media_t) 2개, 교대 재생      │
│     └ Output       : obs_source_output_video / obs_source_output_audio     │
│                         │                                                  │
│                         ▼                                                  │
│  Scene → 오버레이/전환 → Encoder → RTMP/SRT 출력 (OBS 기존 기능)            │
└────────────────────────────────────────────────────────────────────────────┘
```

### 왜 이 구조인가
- **송출·인코딩·장면 구성은 OBS 그대로**: 우리가 만들 부분은 "YouTube → 연속 재생 소스" 하나로 한정.
- **신규 플러그인으로 분리**: `plugins/` 아래 독립 모듈로 두면 업스트림 OBS 업데이트 시 rebase 충돌이 최소화된다.
  코어(libobs) 수정은 원칙적으로 하지 않는다.
- **Media Source(`ffmpeg_source`)를 직접 조작하지 않고 같은 엔진(`shared/media-playback`)을 직접 사용**:
  플레이어를 2개 두고 다음 항목을 미리 열어두어야 항목 전환 시 검은 화면/끊김이 없기 때문.
  (기존 Media Source는 입력 교체 시 재오픈 지연이 생김)

## 2. 핵심 이슈: YouTube 스트림 형식

| 문제 | 내용 | 대응 |
|---|---|---|
| 영상/오디오 분리 (DASH) | 720p 이상은 영상·오디오가 별도 스트림. 단일 muxed 포맷은 사실상 360p까지만 존재 | **yt-dlp로 다운로드 + merge 후 로컬 파일 재생** (기본 전략) |
| URL 만료 | 추출한 googlevideo URL은 약 6시간 후 만료 | 다운로드 방식이면 무관 |
| 라이브 영상 | 라이브는 HLS(m3u8, muxed) 제공 | `yt-dlp -g` 로 m3u8 추출 → 스트리밍 재생 (다운로드 안 함) |
| 재생 불가 영상 | 비공개/삭제/연령제한/멤버십/지역제한 | 해당 항목 `error` 표시 후 자동 skip. 연령제한은 `--cookies-from-browser` 옵션 |
| yt-dlp 파손 | YouTube 변경으로 주기적으로 깨짐 | Dock에 "yt-dlp 업데이트" 버튼 (`yt-dlp -U`), 경로 설정 가능 |

### 다운로드 명령 (기본값)
```
yt-dlp -f "bv*[height<=1080][vcodec^=avc1]+ba[ext=m4a]/b[height<=1080]" \
       --merge-output-format mp4 --no-playlist \
       -o "<cache>/%(id)s.%(ext)s" <URL>
```
- H.264+AAC 우선 → 디코딩 부하 적고 macOS 하드웨어 디코딩 활용 가능.
- 최대 해상도는 설정값 (기본 1080p, 출력 해상도에 맞춤).

## 3. 컴포넌트 상세

### 3.1 Queue
- 항목 구조: `{ id, url, video_id, title, duration, kind: vod|live, state, local_path, error }`
- `state`: `pending → fetching → ready → playing → done | error`
- 재생 모드: 순차 / 전체 반복 / 한 곡 반복 / 셔플
- 저장: 소스 settings(obs_data)에 직렬화 → 씬 컬렉션과 함께 저장·복원

### 3.2 Fetcher (선행 다운로드)
- 백그라운드 스레드 1개, 현재 항목 기준 **앞으로 N개(기본 2개)** 미리 다운로드.
- `libobs/util/pipe.h`(os_process_pipe)로 yt-dlp 실행, 진행률(stdout) 파싱 → 상태 표시.
- 캐시 디렉터리: OBS 설정 폴더 하위 `youtube-cache/`, 용량 상한(기본 20GB) LRU 삭제.
- 이미 캐시에 있으면 재다운로드 안 함 (`video_id` 기준).

### 3.3 Player (A/B 교대)
- `mp_media_t` 2개. 현재 재생 중 플레이어가 끝나기 전에 다음 항목을 반대편 플레이어에 open + 일시정지 상태로 대기.
- `stop_cb`(재생 종료) 수신 → 즉시 대기 플레이어 play, 출력 전환 → 끝난 플레이어 해제 후 그다음 항목 준비.
- 다음 항목이 아직 `ready`가 아니면: 대기 화면(slate 이미지/텍스트) 출력 후 준비되는 즉시 재생.
- 해상도/fps가 달라도 OBS 소스 레벨에서 스케일되므로 송출 스트림은 끊기지 않음.

### 3.4 Dock UI (frontend)
- 큐 리스트 (썸네일, 제목, 길이, 상태), 드래그 정렬, 삭제
- 입력: 단일 URL / 재생목록 URL / "내 플레이리스트" 선택
- 컨트롤: 재생·일시정지·Skip·이전, 재생 모드
- 소스와의 통신: 소스의 proc handler (`add_items`, `skip`, `get_queue` 등) + signal (`queue_changed`, `item_state_changed`)

### 3.5 재생목록 가져오기
| 대상 | 방법 |
|---|---|
| 공개 재생목록 URL | `yt-dlp --flat-playlist -J <URL>` (API 키 불필요) |
| 내 플레이리스트 목록 | Data API `playlists.list?mine=true` |
| 내 플레이리스트 항목 | Data API `playlistItems.list?playlistId=...` (페이지네이션) |
| 나중에 볼 동영상 / 기록 | API 미지원 → 지원 안 함 |

- OAuth: OBS에 이미 있는 YouTube 연동(`frontend/oauth/YoutubeAuth.cpp`, scope `auth/youtube`)을 재사용하고,
  `frontend/utility/YoutubeApiWrappers.*`에 `GetMyPlaylists`, `GetPlaylistItems` 추가.
- 전제: 본인 Google Cloud 프로젝트에서 OAuth 클라이언트 생성 후 빌드 시
  `YOUTUBE_CLIENTID`, `YOUTUBE_SECRET`(+ `_HASH`) 지정, browser-panels(CEF) 활성화 필요
  (`frontend/cmake/feature-youtube.cmake` 조건).

## 4. 디렉터리 계획 (포크 내)

```
plugins/youtube-playlist/
  CMakeLists.txt              # OBS::libobs, OBS::media-playback 링크
  youtube-playlist.c          # 모듈 진입점 (obs_module_load)
  yt-source.c                 # youtube_playlist_source 정의
  yt-queue.c/.h               # 큐 / 재생모드
  yt-fetcher.c/.h             # yt-dlp 실행, 캐시 관리
  data/locale/en-US.ini, ko-KR.ini
frontend/docks/YouTubePlaylistDock.cpp/.hpp
frontend/utility/YoutubeApiWrappers.*   # 플레이리스트 API 추가
```

## 5. 마일스톤

| 단계 | 목표 | 완료 기준 |
|---|---|---|
| M0 | OBS 소스 macOS 빌드 | 직접 빌드한 OBS로 RTMP 송출 성공 |
| M1 ✅ | 플러그인 뼈대 | 로컬 파일 목록을 A/B 플레이어로 끊김 없이 연속 재생 |
| M2 ✅ | yt-dlp 연동 | 단일 YouTube URL 다운로드 → 재생 |
| M3 ✅ | 큐 + 선행 다운로드 | 여러 URL 연속 재생, 다음 항목 사전 준비, 오류 항목 skip |
| M4 ✅ | 공개 재생목록 | 재생목록 URL → 큐 일괄 등록 |
| M5 ✅ | Dock UI | 큐 편집/컨트롤/상태 표시 |
| M6 🧪 | 개인 플레이리스트 | OAuth 로그인 → 내 플레이리스트 가져오기 |
| M7 ✅ | 안정화 | 라이브 URL 직접 재생, yt-dlp 자동 업데이트, 실패 항목 재시도·오류 자동 복구, 장시간 연속 송출 테스트 |
| M8 ✅ | 중계 입력 + 자동 대체 | SRT/RTMP 라이브 입력을 중계하다 끊기면 재생목록으로 자동 전환, 복구되면 라이브로 복귀 |

## 5.1 M2 구현 메모
- **yt-dlp는 플러그인 전용 실행 파일**: 첫 사용 시 `plugin_config/youtube-playlist/bin/`에 자동 설치.
  GitHub 릴리스의 `SHA2-256SUMS`로 SHA-256 검증 후 설치. 시스템 PATH의 yt-dlp는 사용하지 않음.
- **macOS는 one-dir 빌드(`yt-dlp_macos.zip`) 사용**: one-file 빌드는 실행마다 자체 압축 해제 +
  macOS 보안 검사로 매회 ~9초 지연. one-dir은 최초 1회만 검사, 이후 실행 ~0.2초.
- **ffmpeg CLI 불필요**: 영상/오디오를 따로 받은 뒤(`-f "bv*...,ba..."`) 플러그인 안에서
  libavformat으로 MKV remux (`yt-remux.c`, 재인코딩 없음).
- **취소 가능한 프로세스 실행**(`yt-proc.c`): OBS 종료 시 실행 중인 yt-dlp를 SIGTERM으로 즉시 종료.
- 캐시: `plugin_config/youtube-playlist/cache/<key>.mkv` (key = `yt-<videoId>` 또는 URL 해시).
- 라이브 영상은 현재 `--match-filter !is_live`로 제외 → 실패 처리 (M7에서 HLS 직접 재생 예정).
- JS 런타임(deno) 미설치 경고가 나오지만 현재 1080p H.264까지 정상 다운로드됨.

## 5.2 M3 구현 메모
- **이어서 재생**: 씬 컬렉션 저장 시(`save` 콜백) 현재 항목·인덱스·재생 위치(ms)를 소스 설정에 기록
  (`resume_item`, `resume_index`, `resume_ms`). 다음 실행의 첫 재생에서 같은 항목을 찾아 그 위치로 seek.
  재생목록이 끝난 상태로 저장되면 기록을 지워 처음부터 시작. 속성에서 끌 수 있음.
- **대기 화면**: 다운로드 대기 중에는 소스를 비움(투명) → 장면에서 소스 아래 둔 이미지/텍스트가 대기 화면 역할.
  미디어 상태는 `BUFFERING`으로 보고.
- **캐시 관리**: 용량 상한(기본 10GB, 속성에서 1~2000GB) 초과 시 LRU 삭제. 재생 요청 시 파일 mtime을 갱신해
  "최근 사용"을 표시. 가장 최근 3개와 다운로드 중인 항목은 삭제하지 않음. 재생 중인 파일이 삭제돼도
  열린 파일은 계속 재생되며(POSIX), 다음에 필요하면 자동 재다운로드. 시작 시 미완성 파일(.part 등) 정리.
  상한은 플러그인 전역 값(마지막으로 설정된 소스의 값 적용).

## 5.3 M4 구현 메모
- 재생목록 항목에 **YouTube 재생목록 URL**(`list=`가 있고 `v=`가 없는 주소)과 **채널 URL**(`/@이름`, `/channel/`,
  `/c/`, `/user/`)을 넣으면 재생 시점에 영상 목록으로 펼침. 설정에는 원래 URL만 남으므로 재생목록에 추가된
  영상이 자동 반영됨. `watch?v=...&list=...`는 해당 영상 하나로 취급.
- 채널 주소만 넣으면(`/@이름`) 탭 목록이 나오므로 자동으로 `/videos` 탭을 사용.
- 펼친 목록은 `cache/list-<hash>.list`에 저장 → 다음 실행 시 즉시 사용 + 백그라운드 새로고침.
  1시간마다 자동 새로고침, 속성의 "재생목록 새로고침" 버튼으로 즉시 새로고침. 내용이 같으면 무시.
- 첫 재생은 모든 재생목록이 펼쳐질 때까지 대기(순서 보장). 재생 중 목록이 바뀌면 현재 영상은 계속 재생.
- 다운로드 큐에서 재생목록 펼치기를 영상 다운로드보다 먼저 처리.

## 5.4 M5 구현 메모
- 독은 `frontend/` 수정 없이 **별도 UI 플러그인 `plugins/youtube-playlist-ui`**(C++/Qt)로 구현
  (OBS의 aja / aja-output-ui처럼 기능 모듈과 UI 모듈 분리). 소스 플러그인은 Qt 의존성 없음.
- 독 ↔ 소스 통신: OBS 공개 API(미디어 컨트롤, 설정)와 소스의 proc handler
  `get_queue(out string json)`, `play_index(in int index)`, `refresh_lists()`. 독은 0.5초마다 폴링(숨겨져 있으면 중지).
- 독 기능: 소스 선택, 현재 재생 제목·시간·상태, 이전/재생·일시정지/정지/다음, 큐(번호·제목·길이·상태,
  더블클릭으로 바로 재생), URL/경로 추가, 재생목록 새로고침. 처음 설치 시 한 번 메인 창에 도킹된 상태로 표시.
- 제목·길이: yt-dlp 출력에서 수집해 `cache/meta.tsv`에 저장(재생목록 펼칠 때, 다운로드할 때).
  제목이 없는 항목은 낮은 우선순위로 제목만 조회(`--skip-download`, 세션당 1회).
- 다운로드 진행률: yt-dlp `--progress-template` 출력을 실시간 파싱(영상 85% + 오디오 15% 가중).
- 큐 편집(위로/아래로/삭제, 우클릭 메뉴, Delete 키): 소스 설정 `playlist`의 원래 항목 단위로 동작.
  재생목록·채널에서 펼쳐진 영상은 그 재생목록 항목 전체가 이동/삭제됨(삭제 시 확인 창).
  `get_queue`가 항목별 `setting_index`, `from_list`, `list`를 제공. 편집 후에도 현재 재생과 선택은 유지.
- 독에 "반복" 체크박스(소스 `loop` 설정과 동기화). 큐 헤더(#, 제목, 길이, 상태) 가운데 정렬.

## 5.5 M6 구현 메모 (사용자 로그인 검증 대기)
- 인증 방식: **사용자 본인의 Google OAuth 클라이언트**(데스크톱 앱), 권한 `youtube.readonly`.
  (브라우저 쿠키 방식은 계정 제한 위험 때문에 채택하지 않음)
- 역할 분담:
  - `youtube-playlist`(C): 자격 증명 저장(`plugin_config/youtube-playlist/oauth.json`, 0600), 토큰 갱신,
    YouTube Data API 호출(시스템 curl, 비밀값은 0600 임시 파일로 전달해 `ps`에 노출 안 됨).
    전역 프로시저 `youtube_playlist_oauth_status / _set_client / _exchange / _logout`, `youtube_playlist_my_playlists`.
  - `youtube-playlist-ui`(Qt): "내 재생목록…" 대화상자 — 브라우저 로그인(루프백 127.0.0.1 + PKCE S256 + state),
    재생목록 선택(좋아요 표시한 동영상 포함) → `youtube.com/playlist?list=ID` 항목으로 추가.
- 로그인 상태에서는 재생목록 펼치기를 **API 우선**(비공개 재생목록도 펼쳐지고 자동 갱신), 실패 시 yt-dlp.
  공개·일부 공개 영상만 큐에 넣음(비공개/삭제 영상 제외). 다운로드는 계속 익명(yt-dlp).
- 로그아웃 시 Google 쪽 토큰도 폐기(revoke). 토큰이 취소/만료(invalid_grant)되면 자동으로 로그아웃 상태로 전환.

## 5.6 M7 계획 (안정화)
1. **YouTube 라이브 URL 직접 재생**: 라이브는 다운로드하지 않고 yt-dlp로 HLS 주소만 얻어 스트리밍 재생.
   HLS 주소는 몇 시간 뒤 만료되므로 약 5시간마다 다시 얻음. 라이브는 미리 열어 두지 않음(지연·세그먼트 만료 방지).
2. **yt-dlp 자동 업데이트**: 하루 1회 GitHub 최신 버전 확인 → 새 버전이면 체크섬 검증 후 교체.
   소스 속성에 "yt-dlp 지금 업데이트" 버튼.
3. **실패 항목 재시도**: 일시적 네트워크 오류로 실패한 항목은 10분 뒤 다시 시도(세션 내 영구 실패 방지).
4. **오류 자동 복구**: 전체 항목 실패로 정지(ERROR)되면 30초 뒤 자동으로 처음부터 재시도.
5. **장시간 송출 테스트**: 짧은 클립 + YouTube 항목 반복 재생을 녹화/송출하며 메모리·파일 핸들·끊김을 주기적으로 측정.
   macOS `leaks`로 메모리 누수 확인. 24시간 테스트는 사용자 환경에서 돌릴 수 있도록 측정 스크립트 제공.

### M7 결과
- **라이브**: 라이브는 영상/오디오가 분리된 HLS로만 제공됨 → 두 HLS를 묶는 **로컬 마스터 재생목록**
  (`cache/<key>.live.m3u8`)을 만들어 FFmpeg HLS 디먹서로 하나처럼 재생. `.../@채널/live` 주소는
  "현재 라이브 1개"로 처리(채널 목록으로 펼치지 않음). 다운로드 실패 시 라이브 여부를 확인해 전환.
- **yt-dlp 업데이트**: 시작 시 + 다운로드 실패 시 하루 1회 최신 버전 확인(GitHub releases/latest 리다이렉트),
  새 버전이면 체크섬 검증 후 재설치. 소스 속성에 "yt-dlp 지금 업데이트" 버튼.
- **재시도·복구**: 실패 항목·재생목록은 10분 뒤 재시도, 전체 실패(ERROR) 시 30초마다 재시작.
- **장시간 테스트(24분 녹화, 106회 재생 시작)**: 재생 중 프레임 누락 0건, 메모리 306~529MB 범위에서 증가 없음,
  스레드 63~66·열린 파일 186개로 일정. 테스트 중 실제로 발생한 YouTube 일시 오류가 10분 뒤 재시도로 복구됨.
- **메모리 누수 수정**: `leaks`로 영상 끝날 때마다 AVPacket 112바이트 누수 발견 → OBS 공용
  `shared/media-playback`의 EOF 처리 버그(원래 OBS에도 있음)를 수정. 수정 후 해당 누수 0건.
- 사용자 환경 24시간 테스트용 `plugins/youtube-playlist/tools/soak-monitor.sh` 제공.

## 5.7 M8 계획 (중계 입력 + 자동 대체)
**목표**: 평소에는 SRT/RTMP로 받은 라이브를 중계하고, 입력이 끊기면 YouTube 재생목록으로 자동 전환,
입력이 돌아오면 라이브로 복귀 → 방송 중 화면이 끊기는 사고 방지.

- **위치**: 기존 `youtube_playlist_source`에 "중계 입력" 기능 추가(별도 소스가 아니라 같은 소스 안에서 전환 →
  전환 시 장면 구성 변경 불필요).
- **입력**: SRT(listener/caller), RTMP(listen), RIST, 기타 FFmpeg URL. 입력 전용 플레이어(media-playback,
  네트워크 모드)를 하나 더 두고 끊기면 자동으로 다시 대기(재연결).
- **전환 규칙**:
  - 입력 프레임이 들어오고 `복귀 대기`(기본 3초) 동안 안정적이면 → 라이브 출력.
  - 라이브 출력 중 `대체 대기`(기본 2초) 동안 프레임이 없으면 → 재생목록으로 전환.
  - 라이브 중 재생목록은 일시정지 → 돌아오면 멈춘 위치부터 이어서(설정으로 "계속 진행"도 선택 가능).
- **수동 제어**: 독에 입력 상태(대기 / 수신 중 / 라이브 출력 중 / 재생목록 대체 중) 표시,
  "라이브로 전환", "재생목록으로 전환", "자동 전환" 토글.
- **설정**: 입력 URL, FFmpeg 옵션, 대체·복귀 대기 시간, 재생목록 일시정지 여부.
- **검증**: ffmpeg로 테스트 신호를 SRT로 보내고 끊었다 다시 연결하며 녹화본에서 전환 시점과 끊김 측정.

### M8 결과
- 소스 속성의 체크 가능한 그룹 "중계 입력": 입력 URL, FFmpeg 옵션, 전환/복귀 대기 시간, 라이브 중 재생목록 일시정지.
  `rtmp://0.0.0.0:...` 주소는 `listen=1` 자동 추가(FFmpeg 내장 RTMP 수신).
- 입력이 끊기면(연결 종료) 즉시, 연결은 살아 있는데 데이터가 멈추면 `대체 대기` 후 재생목록으로 전환.
  입력이 다시 `복귀 대기` 동안 안정적이면 라이브로 복귀. 재생목록은 멈춘 위치부터 이어서 재생.
- 입력을 열 수 없으면(포트 사용 중 등) 1→2→4→…10초 간격으로 재시도하고 경고는 한 번만 기록.
- 전환 순간 비동기 비디오 큐를 비워(`obs_source_output_video(NULL)`) 두 입력의 시계 차이로 생기던
  최대 2초 화면 멈춤을 제거.
- 독: 중계 상태(입력 대기 / 수신 중 / 라이브 송출 중) 표시, 자동·라이브 고정·재생목록 고정 버튼.
- 검증(녹화 분석):
  - RTMP: 정상 종료 시 약 1초 안에 재생목록 전환, 재연결 시 3초 뒤 라이브 복귀, 2회 반복.
  - 송출 정지(SIGSTOP, 연결 유지) → 3.0초 뒤 재생목록, 재개 → 3.06초 뒤 라이브.
  - SRT(listener, 두 번째 OBS가 SRT로 송출): 연결 3초 뒤 라이브, 송출 종료 0.04초 뒤 재생목록.
  - 전환 시점 화면 전환 지연 1프레임 이내, 재생 중 프레임 누락 없음.
- 참고: 이 Mac에서는 Docker가 1935 포트를 사용 중이라 테스트는 19350 포트로 진행.

## 6. 미결 사항
1. ~~레포 운영 방식~~ → 포크로 결정 (§0)
2. ~~기준 버전~~ → 33.0.1 고정 (§0)
3. 영상 간 전환 효과(페이드 등) 필요 여부 — 필요 시 Player 레벨이 아닌 씬 전환으로 처리할지
4. 송출 대상 플랫폼 (Content ID 이슈: 타인 콘텐츠 송출 시 송출 채널 제재 가능)
