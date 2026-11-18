# DiskScope

<img src="resources/diskscope.svg" alt="DiskScope logo" width="72" height="72">

[한국어](#한국어) · [English](#english)

## 한국어

DiskScope는 Windows와 Linux에서 어떤 폴더와 파일이 디스크 공간을 많이 쓰는지 살펴보는 읽기 전용 분석기입니다. 한국어 GUI와 명령줄 도구가 같은 스캔 엔진을 사용합니다.

### 다운로드 및 실행

Windows에서 기본 스캔·저장 속도를 개선한 빌드는 [Windows 성능 프리뷰 1](https://github.com/mk0000001/DiskScope/releases/tag/v0.1.3-perf.1)에서 받을 수 있습니다. ZIP 전체를 풀고 `bin\diskscope.exe`를 실행하세요. [반복 측정과 검증 범위](docs/VALIDATION.md#windows-performance-preview)를 확인할 수 있습니다.

[DiskScope 0.1.3 프리뷰 릴리스](https://github.com/mk0000001/DiskScope/releases/tag/v0.1.3-preview)에서 운영체제에 맞는 파일을 받으세요. 패키지의 SHA-256은 릴리스의 `DiskScope-0.1.3-SHA256SUMS`와 대조할 수 있습니다.

- **Windows 11 x64** — `DiskScope-0.1.3-windows-x64-portable.zip` 전체를 풀고 `bin\diskscope.exe`를 실행합니다. 압축을 푼 디렉터리 구조를 유지하세요.
- **Ubuntu 24.04 amd64** — `diskscope_0.1.3_amd64.deb`를 받은 디렉터리에서 설치한 뒤 앱 메뉴에서 DiskScope를 열거나 터미널에서 `diskscope`를 실행합니다.

  ```sh
  sudo apt install ./diskscope_0.1.3_amd64.deb
  ```

### GUI에서 분석하기

1. **드라이브·폴더 스캔**에서 로컬 드라이브나 폴더를 선택합니다. 첫 결과가 저장되면 폴더 트리의 루트가 자동으로 열리고, 이후 발견된 항목이 저장되는 순서대로 표시됩니다. 스캔 도중 트리를 펼치거나 제한된 트리맵 미리보기를 볼 수 있습니다.
2. 파일 크기는 스캔에서 관측한 값부터 표시됩니다. 폴더 합계와 하드링크 중복 제외 할당량은 집계 후에 확정되며, 집계가 끝나면 트리가 선택한 크기 기준으로 정렬됩니다. **큰 파일**, **큰 폴더**, 트리맵, 확장자·수정일 통계로 결과를 살펴보세요. 이름·경로, 확장자, 크기, 수정일로 필터링할 수 있습니다.
3. **저장**으로 `.dscan` 스냅샷을 남기고 **결과 열기**로 다시 열거나 **비교**로 두 스냅샷의 변경 사항을 확인할 수 있습니다. **내보내기**에서는 전체 결과, 선택한 항목, 필터에 일치하는 모든 항목을 CSV, JSON 또는 JSONL로 저장할 수 있습니다.

스캔은 일시정지·재개하거나 취소할 수 있습니다. 취소 후 집계되지 않은 행이 남으면 워커가 종료된 뒤 **부분 결과 집계**를 사용할 수 있습니다. Windows에서 GUI가 스캔 워커를 시작할 때 별도의 콘솔 창이 뜨지 않습니다. CLI는 터미널에서 그대로 동작합니다.

Windows의 **오류·제외**에서 접근 거부 항목을 선택하고 **관리자 재확인 명령 복사**를 누를 수 있습니다. 관리자 PowerShell을 직접 열어 붙여넣으면 해당 파일의 메타데이터를 확인하거나 해당 폴더를 스캔합니다.

### 크기 지표

| 지표 | 의미 |
| --- | --- |
| 논리 크기 | 각 경로에서 관측한 파일 데이터의 길이입니다. |
| OS 보고 할당량 | 운영체제가 경로별로 보고한 할당 바이트입니다. 압축 파일이나 희소 파일은 논리 크기와 다를 수 있습니다. |
| 중복 제외 할당량 | 동일한 파일을 가리키는 하드링크가 여러 개라면 해당 할당량을 한 경로에만 귀속합니다. |

스캔은 선택한 로컬 루트와 해당 볼륨 또는 마운트 안에서 일반 파일의 메타데이터를 읽습니다. 심볼릭 링크, Windows 정션, 관리형 클라우드 자리 표시자는 건너뜁니다. 합계는 일반 파일의 기본 데이터 스트림을 기준으로 하며, 디렉터리 자체 블록, 파일시스템 메타데이터, 대체 데이터 스트림과 공유 블록의 물리적 점유량은 집계 범위 밖입니다. 실제 삭제 후 확보되는 공간은 파일시스템의 동작에 따라 달라집니다.

### 캐시와 진단

스캔 결과는 로컬 캐시에 저장되며, 사용 가능한 결과는 **최근 캐시 열기**로 복구할 수 있습니다. 정리 대상인 캐시는 보존 정책에 따라 삭제될 수 있으므로 보관할 결과는 **저장**으로 캐시 밖에 저장하세요.

**진단 내보내기**는 버전, 워커·스캔 상태와 오류 사유별 건수를 로컬 JSON으로 저장합니다. 기본 보고서는 경로와 파일명을 숨깁니다. **스캔 루트 경로 포함**을 선택하면 루트 경로만 추가됩니다. 스냅샷과 일반 결과 내보내기에는 이름과 경로가 포함될 수 있습니다.

### 명령줄 사용

Linux에서는 `diskscope-cli`를, Windows 휴대용 패키지에서는 `.\bin\diskscope-cli.exe`를 실행합니다. 다음 예시는 Linux 기준입니다. 비교하려면 앞서 저장한 `before.dscan`이 필요합니다.

```sh
diskscope-cli scan --root ./fixture --output result.dscan
diskscope-cli export --input result.dscan --format jsonl --output report.jsonl
diskscope-cli compare --before before.dscan --after result.dscan --output changes.jsonl
```

### 개발 문서와 라이선스

- [빌드 및 CLI 옵션](docs/BUILD.md) · [Windows 휴대용 패키지 빌드](packaging/windows/README.md)
- [검증 및 성능 측정](docs/VALIDATION.md) · [알려진 한계](docs/LIMITATIONS.md). 문서의 합성 데이터 측정값은 명시된 테스트 환경과 작업에 해당합니다.
- [의존성 및 라이선스](docs/DEPENDENCIES.md) · [타사 고지](THIRD_PARTY_NOTICES). DiskScope 소스는 [MIT 라이선스](LICENSE)로 배포합니다.

## English

DiskScope is a read-only disk usage analyzer for Windows and Linux. Use it to see which folders and files take up the most space. Its Korean-language GUI and command-line tool use the same scanning engine.

### Download and run

The [Windows Performance Preview 1](https://github.com/mk0000001/DiskScope/releases/tag/v0.1.3-perf.1) improves default scan and save speed. Extract the whole ZIP and run `bin\diskscope.exe`. See the [repeated measurements and validation coverage](docs/VALIDATION.md#windows-performance-preview).

Get the package for your operating system from the [DiskScope 0.1.3 preview release](https://github.com/mk0000001/DiskScope/releases/tag/v0.1.3-preview). You can verify the package SHA-256 against `DiskScope-0.1.3-SHA256SUMS` on the release page.

- **Windows 11 x64** — Extract the entire `DiskScope-0.1.3-windows-x64-portable.zip` and run `bin\diskscope.exe`. Keep the extracted directory structure intact.
- **Ubuntu 24.04 amd64** — Run the following command from the directory containing `diskscope_0.1.3_amd64.deb`. Then open DiskScope from the app menu or run `diskscope` in a terminal.

  ```sh
  sudo apt install ./diskscope_0.1.3_amd64.deb
  ```

### Explore results in the GUI

1. Choose a local drive or folder using **드라이브·폴더 스캔** (Scan drive or folder). The folder tree opens its root when the first results are committed and shows new entries in discovery order as batches are saved. You can expand folders and view a limited treemap preview during the scan.
2. File sizes appear as observed. Folder totals and hardlink-deduplicated allocation are available after aggregation, and the tree then sorts by the selected size metric. Explore the large-file and large-folder views, treemap, and statistics by extension or modification date. Filter by name or path, extension, size, or modification date.
3. Use **저장** (Save) to keep a `.dscan` snapshot, **결과 열기** (Open result) to reopen it, or **비교** (Compare) to inspect changes between two snapshots. **내보내기** (Export) writes the full result, the selection, or all filter matches as CSV, JSON, or JSONL.

You can pause, resume, or cancel a scan. If cancellation leaves committed rows to aggregate, use **부분 결과 집계** (Aggregate partial results) after the worker exits. On Windows, starting a scan from the GUI launches the worker without opening a separate console window. The CLI still runs in a terminal.

On Windows, select an access-denied item under **오류·제외** (Issues and exclusions) and choose **관리자 재확인 명령 복사** (Copy administrator retry command). Open PowerShell as administrator and paste the command to inspect the selected file's metadata or scan the selected folder.

### Size metrics

| Metric | Meaning |
| --- | --- |
| Logical size | The file data length observed at each path. |
| OS-reported allocation | The bytes the operating system reports as allocated at each path. This can differ from logical size for compressed or sparse files. |
| Deduplicated allocation | When several hardlinks refer to the same file, DiskScope attributes its allocation to one path. |

A scan reads regular-file metadata within the selected local root and its volume or mount. It skips symlinks, Windows junctions, and managed cloud placeholders. Totals cover the default data stream of regular files. The blocks used by directories, filesystem metadata, alternate data streams, and physical accounting for shared blocks fall outside these totals. Space reclaimed after deletion depends on the filesystem.

### Cache and diagnostics

Scan results are stored in a local cache and can be recovered using **최근 캐시 열기** (Open recent cache) when available. The retention policy can remove eligible caches. Use **저장** (Save) to keep a snapshot outside the cache.

**진단 내보내기** (Export diagnostics) saves a local JSON report with versions, worker and scan status, and issue counts by reason. Paths and filenames are hidden by default. Selecting **스캔 루트 경로 포함** (Include scan root path) adds only the root path. Snapshots and ordinary result exports can contain names and paths.

### Use the command line

Run `diskscope-cli` on Linux or `.\bin\diskscope-cli.exe` from the extracted Windows package. The following examples use Linux. The comparison assumes you already saved `before.dscan`.

```sh
diskscope-cli scan --root ./fixture --output result.dscan
diskscope-cli export --input result.dscan --format jsonl --output report.jsonl
diskscope-cli compare --before before.dscan --after result.dscan --output changes.jsonl
```

### Developer documentation and license

- [Build instructions and CLI options](docs/BUILD.md) · [Build the Windows portable package](packaging/windows/README.md)
- [Validation and performance measurements](docs/VALIDATION.md) · [Known limitations](docs/LIMITATIONS.md). Measurements on synthetic data apply to the documented test environments and workloads.
- [Dependencies and licenses](docs/DEPENDENCIES.md) · [Third-party notices](THIRD_PARTY_NOTICES). DiskScope source is distributed under the [MIT license](LICENSE).
