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
| M3 | 큐 + 선행 다운로드 | 여러 URL 연속 재생, 다음 항목 사전 준비, 오류 항목 skip |
| M4 | 공개 재생목록 | 재생목록 URL → 큐 일괄 등록 |
| M5 | Dock UI | 큐 편집/컨트롤/상태 표시 |
| M6 | 개인 플레이리스트 | OAuth 로그인 → 내 플레이리스트 가져오기 |
| M7 | 안정화 | 라이브 URL, 캐시 LRU, yt-dlp 업데이트, 24시간 연속 송출 테스트 |

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

## 6. 미결 사항
1. ~~레포 운영 방식~~ → 포크로 결정 (§0)
2. ~~기준 버전~~ → 33.0.1 고정 (§0)
3. 영상 간 전환 효과(페이드 등) 필요 여부 — 필요 시 Player 레벨이 아닌 씬 전환으로 처리할지
4. 송출 대상 플랫폼 (Content ID 이슈: 타인 콘텐츠 송출 시 송출 채널 제재 가능)
