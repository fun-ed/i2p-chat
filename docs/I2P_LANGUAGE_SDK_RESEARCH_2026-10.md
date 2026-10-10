# I2P／i2pd 程式語言實作與 SDK／API 研究

研究截點為 2026-10-10。本文比較完整 router、嵌入式 router、原生 client SDK、SAM client、I2CP 及管理 API，並評估目前 I2PChat 的採用路線。

後續決策：2026-10-10 使用者決定移除專案的 Rust 整合，保留 Qt／C++ 客戶端，對齊 Java I2P 2.13.0 與最新 JDK 25 LTS。本文保留調查當時的比較及架構描述；目前實作、驗證結果與回復方式以根目錄 `plan.md` 為準。

## 1. 結論先行

「支持最完整」必須分開看功能範圍、維護證據、介面穩定性和生產適用性。

| 需求 | 本次研究的優先選擇 | 判斷理由與限制 |
| --- | --- | --- |
| 最完整的官方原生 client SDK | Java I2P 的 I2CP、Streaming、Datagram API | 與參考 router 同步發展，有正式 Maven artifacts、原始碼與文件；JVM 語言最直接 |
| 成熟、不依賴 JVM 的 router | C++ i2pd | 已有穩定發行、跨平台封裝、SAM／I2CP；不代表與 Java 的每個進階 API 選項完全一致 |
| Rust 應用程式接入 I2P | yosemite SAM client，加上成熟的外部 router | 有近期 release、同步及 Tokio／smol 非同步介面、stream／datagram；不是 router 或完整 I2CP SDK |
| 純 Rust 可嵌入 router | Emissary／emissary-core | 真正的 Rust router，支援 NTCP2、SSU2、SAM、I2CP；仍應作實驗性採用，不作既有產品的預設替代 |
| Go 應用程式接入 I2P | go-sam-go，加上成熟的外部 router | 新版 SAM client 功能廣；與實驗性的 go-i2p router 必須分開判斷 |
| C／C++ 應用程式透過 SAM 接入 | 現有 SAM 實作，或評估 libsam3 | libsam3 是官方清單明示由 I2P 維護的 C client；仍需驗證 async 介面與宿主 event loop 的整合 |
| 跨語言最大相容性 | SAM 的共同功能，加上精確的 router／feature 測試 | 官方推薦非 Java 應用使用 SAM；不能只依 HELLO 回傳版本判定全部功能 |

本文沒有把「有 GitHub repository」、「有 package」、「README 宣稱 production-ready」當作獨立安全驗證。下文的採用排序是工程判斷，不是 benchmark 或匿名性稽核結果。[S1][S2][S3][S8][S12][S15]

## 2. 研究方法與完整度邊界

- 三個平行研究方向分別調查 router 移植、原生 SDK／嵌入介面、多語言 client 與 package registry。
- 優先查官方規格、release-tag 原始碼、README、release notes，以及 Maven／Go module proxy／npm／PyPI／docs.rs 的版本紀錄。
- 「最新核驗版本」指本次查到且日期不晚於截點的版本，不保證每個發行渠道或 fork 都完全同步。
- 區分正式 release 與開發分支；i2pd 的功能比較以 `2.61.0` tag 為主。HEAD 新增功能不算成該 release 的功能。
- Commit 日期只能證明有更新，不能單獨證明有人持續處理 bug、支援使用者或完成安全維護。
- 查閱範圍包含 GitHub、GitLab、官方自架 forge、Codeberg、NotABug 和語言 registries。這是廣泛且可追溯的生態調查，不宣稱找遍所有 repository。
- 本次沒有安裝、編譯或實際連線測試外部 SDK／router，沒有跨 router 效能或匿名性量測。功能列為「未核驗」時，不代表功能一定不存在。

## 3. 先分清楚 API 層次

| 類型 | 實際責任 | 是否自行提供 router | 適用情境 |
| --- | --- | --- | --- |
| 完整 router | 網路 transport、NetDB、tunnels、routing、router 加密 | 是 | Java I2P、i2pd，以及實驗性的 Emissary／go-i2p |
| 嵌入式 router library | 將 router 放入宿主程式，宿主負責啟停、持久化、更新和資源設定 | 是 | Java Router、libi2pd、emissary-core、Go embedded API |
| Java Streaming SDK | 在 I2CP 上提供可靠、有序的 socket-like streams | 否，另接或嵌入 router | JVM 聊天、P2P、檔案與 HTTP 應用 |
| Java Datagram SDK | 在 I2CP 上建立 datagram 格式與處理介面 | 否 | DHT、訊息及不需要 stream 的協定 |
| I2CP client | 低階 session、destination、LeaseSet、message 與 lookup | 否 | 需要更深控制，願意承擔 Streaming／Datagram 上層實作的 client |
| SAM client | 以 TCP／UDP 指令請 router bridge 處理 session、stream、datagram、naming、keys | 否 | 非 JVM 應用的主要整合路線 |
| SAM bridge／server | 把 SAM 指令轉成 router client 功能 | 通常否 | Java／i2pd 內建 SAM，或 Go SAM bridge |
| SOCKS／HTTP proxy | 將既有應用流量代理進 I2P | 否 | 瀏覽、既有 outgoing client；不是完整 P2P SDK |
| I2PControl | 認證後的 JSON-RPC router 狀態、設定及管理 | 否 | 管理面，不承載聊天 payload |

I2CP 是 I2P Client Protocol。它不是名稱相近的 I2PControl。直接實作 I2CP 也不等於已完成 I2P Streaming；後者還包含可靠傳輸、重送、流量控制和封包排序。[S1][S4][S5][S6]

## 4. 完整 router 與各語言移植

| 專案 | 語言／類型 | 本次核驗的版本或更新 | 功能與成熟度判斷 | 授權及來源 |
| --- | --- | --- | --- | --- |
| Java I2P | Java，官方完整 router／參考實作 | 2.13.0，官方公告 2026-07-20；Maven metadata 更新 07-22；GitHub release 發布 07-24。可見 10-09 commit | 官方 stable；完整 router 加上官方 client SDK、console、應用與工具。2.13.0 公告列出 NTCP2 post-quantum 支援及移除 SSU1 | Core／Router 的預設授權為 Public Domain，但發行包包含各自授權的元件，不能把全部打包內容簡化為單一授權。[S2][S7][S9] |
| i2pd | C++，獨立完整 router | 2.61.0，2026-07-20；可見 10-10 commit | 官方列 stable，支援 NTCP2／SSU2、SAM／I2CP、HTTP／SOCKS；進階 API 仍有差異 | BSD-3-Clause。[S2][S10][S11] |
| I2P+ | Java I2P soft-fork，不是另一語言移植 | 網站顯示 2.13.0+；可見 10-10 commit。Continuous build 不等於當日新的穩定版 | 官方列 stable fork；主要是 Java 技術線的 UI、設定、診斷及效能調整，仍需要 JVM | 專案宣告 AGPL-3.0。[S2][S13] |
| Emissary | Rust，router 與可嵌入 core | v0.4.0，2026-04-12；最新核驗 commit 07-26 | 真正實作 NTCP2／SSU2、SAMv3／I2CP、HTTP／SOCKSv5；0.4.0 加入 IPv6、SSU2 改善及 PQ hybrid。官方替代實作頁仍列 experimental，該頁標示為 2025 資料；未找到更新的 production-ready 宣告 | MIT；跨語言 bindings 仍在 roadmap。[S2][S8][S14] |
| go-i2p | Go，純 Go router | v0.1.70001，2026-09-08；可見 10-09 commit | 有 tunnels、NetDB、NTCP2、I2CP、SAM；SSU2 標 experimental。README 明說 early development、有 bugs、可能在網路上非常 distinct，建議一般使用者使用成熟 router；不是目前產品預設候選 | MIT；低階 API 仍不穩定。[S15][S16] |
| Ire | Rust，部分 router prototype | 最新核驗 commit 2023-02-04 | README 標 pre-alpha；不是功能完整的可用 router。缺 SSU、部分 I2NP 處理及 NetDB 功能，不適合作產品 SDK | MIT。[S17] |
| i2p-cs | C#，歷史 router 實作 | 最新核驗 commit 2022-01-20 | 官方 2025 新 router 公告稱 dormant；未核驗可靠的現代相容性與 feature checklist | MIT；不建議作新產品基礎。[S18] |
| i2pcpp | C++，歷史 router 路線 | 官方 embedding 文件明說開發已停止 | 不應與仍在維護的 i2pd 混淆 | 本研究不建議採用。[S3] |

### Rust／Go 不再是「沒有 router」

2026 年已有值得研究的 Emissary 和 go-i2p。不能再用幾年前的結論說只有 Java／C++ 實作。

但「能跑」、「已實作很多協定」和「適合匿名性敏感的正式使用」是不同標準。Go 的現行 README 明確警告，並指出 Streaming 是 prototype、I2CP client mostly complete、底層 API 正在改動。它也列出 ECIES-only destinations 和 `messageReliability=Guaranteed` 退回 BestEffort 的限制。Emissary 0.4.0 功能進展明確，仍不能從 Rust 語言、release notes 或 fuzzing 的存在推導匿名性與生產安全已獲驗證。[S8][S14][S15]

## 5. Java 原生 SDK 為什麼仍然最完整

Java I2P 對應用程式提供的是原生 API，不只是 SAM 包裝。

- I2CP API 處理 session、message、destination／keys、lookup 及生命週期。
- Streaming API 以 `I2PSocketManagerFactory` 建立 manager，提供 `I2PSocket`／`I2PServerSocket`，隱藏多數低階 I2CP 工作。
- Datagram API 在 I2CP 上處理 datagram 格式；raw message 與 repliable datagram 的責任必須分清楚。
- Router 可在同一 JVM 內啟動，也可透過 I2CP 連到外部相容 router。使用 Java client SDK 不強制只能連 Java router。

Maven Central 已核驗到下列 2.13.0 artifacts：

```text
net.i2p:i2p:2.13.0
net.i2p.client:streaming:2.13.0
net.i2p.client:mstreaming:2.13.0
```

`streaming` 的 POM 依賴相同版本的 `i2p` 和 `mstreaming`。這是 package 版本證據，不是所有 source、plugin、third-party jar 的授權審查。[S4][S5][S9]

Kotlin、Scala 等 JVM 語言可以透過 Java interoperability 使用這組 SDK，不需要重新實作 I2P 協定；這不等於另有官方 Kotlin／Scala 專用 SDK。Android 有官方 Java router 專案，但 Android 打包、背景服務及平台生命週期需要另行評估。[S3][S19]

目前 Java 主庫 README 的完整建置要求是 JDK 17+；它對部分嵌入子模組另保留較低 Java 版本的相容聲明。不要照舊文件的 JDK 6／7 建置文字，推論目前完整 router 可以使用舊 JDK，也不要未檢查 artifact 就承諾 client 的最低 runtime。[S7]

## 6. C／C++ 原生整合與 i2pd embedding

### libi2pd 確實存在，但不能稱為穩定的跨語言 SDK

i2pd 可以建置 library；其 `libi2pd/api.h` 有實際的 C++ 應用介面：

```text
InitI2P / StartI2P / StopI2P / TerminateI2P
CreateLocalDestination / DestroyLocalDestination
RequestLeaseSet
CreateStream / AcceptStream / DestroyStream
```

介面暴露 `std::shared_ptr`、`PrivateKeys`、`ClientDestination`、`Stream` 和 `Mapping` 等 C++ 型別，代表宿主需與特定 source／build 整合。本研究沒有找到穩定 ABI、跨版本相容或長期 public SDK 的承諾。`api.h` 也沒有直接列出與 Java Datagram API 等價的完整高階 datagram／raw 介面；不能把內部類別存在當作完整公開 SDK。[S20]

`libi2pd_wrapper/capi.h` 雖有 `extern "C"`，但只有 router 啟停、初始化、終止及 peer-test 函式，沒有 destination、stream 或 payload API。因此不能直接把它當成 Rust／C#／Python 可用的完整聊天 SDK。[S21]

**工程判斷：** 若目的只是讓 I2PChat 連 router，SAM 比 source-linked libi2pd 低耦合。只有明確需要 in-process router 時，才值得承擔 router 更新、crash isolation、資源、持久化和 ABI 成本。

### SAM client 函式庫

| 套件 | 可核驗功能 | 狀態與限制 |
| --- | --- | --- |
| libsam3 | C，SAM 3.1；STREAM／DATAGRAM／RAW；同步 `libsam3` 與非同步 `libsam3a` | 官方清單明示由 I2P 維護。最新核驗 GitHub release v0.31.2 為 2024-04-29；不是完整 SAM 3.3 的證據，也不等於現成 Qt adapter。[S1][S22] |
| i2psam | C++ 加 C wrapper，SAM 3.1；官方表列 STREAM／DATAGRAM，不列 RAW | README 有 pre-release／refactoring 警告，MIT；使用前檢查現行原始碼與測試。[S1][S23] |
| Samty | C++，官方表列 SAM 3.1 STREAM | 本次未核驗近期維護或 release，不列為優先採用。[S1] |
| Bitcoin 的 i2p.cpp | 真實應用的 C++ SAM 整合參考 | 是參考程式碼，不是可直接引入的通用 SDK；不可只複製其產品特定的 session／privacy 行為。[S1][S24] |

## 7. Rust client SDK 與 router 必須分開評估

| 專案 | 類型及功能 | 版本／維護證據 | 採用判斷 |
| --- | --- | --- | --- |
| yosemite | SAM client；stream connect／accept／forward、repliable／anonymous datagrams、primary sessions、lookup 與 destination generation；sync／Tokio／smol | 0.7.0，2026-03-31；此前 0.6.3 為 2026-02-01，有多個近期 release；MIT | 本次找到的 Rust SAM client 優先 PoC 候選。仍需 router，不能直接取代 I2PChat 的整個 backend。[S12] |
| i2p-rs | Rust SAM client，不是 router；官方表列 STREAM／DATAGRAM／RAW | Repository 說 early development、API in flux；master Cargo.toml 為 0.1.1，但 crates.io `i2p` 只核驗到 0.0.1，2017-04-30；可見 2025-07-17 commit | 不宜依官方舊清單的存在就優先採用；source／published crate 版本不一致要先處理。[S1][S25] |
| ri2p | SAM client，virtual stream／repliable／anonymous datagram／Unicode | 1.0.0，2021-06-20；zlib；未核驗近期 release | 歷史替代方案，沒有足夠維護證據列為首選。[S26] |
| solitude | SAM client 候選 | 官方表中 STREAM／DATAGRAM／RAW 都標 WIP；本次沒有更強的現行完成證據 | 不列為功能完成的 SDK。[S1] |
| resolvingarchitecture/i2p-rust | 早期 Rust I2P application service；SAM datagram，可選嵌入 Emissary | README 說只以 fake bridge 做 unit tests，未作 live router field-test，未提供完整 streaming／I2CP 路線 | 不是成熟通用 SDK，不與 yosemite 等量齊觀。[S27] |
| emissary-core | 真正 Rust router implementation，可嵌入 | 支援 Tokio／smol Runtime；C／C++／TypeScript bindings 仍在 roadmap | 適合隔離研究，不是 native Rust client socket SDK。[S8] |

### yosemite 的明確優點及未核驗部分

- 非同步 stream 接上 runtime 的 `AsyncRead`／`AsyncWrite`，也有同步 `Read`／`Write` 介面。
- `tokio` 預設啟用；`tokio`、`smol`、`sync` 互斥，只能選一個。
- Emissary CLI 使用 yosemite，提供一個真實整合使用案例；這仍不是對 Java／i2pd 的完整互操作測試證據。
- 本次沒有足夠文件證據確認其 SAM USER／PASSWORD、TLS、Datagram2／3、所有 port／I2CP options 或專用 cancellation API。不要把文件寫「SAMv3」理解為最新規格全部實作。
- Primary session 名稱與 i2pd `MASTER` 的相容方式要專門驗證，不能假設任何 SAM client 對兩者都能透明工作。[S12][S28]

### Emissary 的容易誤解之處

官方 embedding 文件明說 `emissary-core` **不提供 Rust client interfaces**。它提供 router，應用還是透過 SAMv3／I2CP 與它通訊。

因此「嵌入 Emissary」和「用 yosemite 連 I2P」是兩個決定。可以用 yosemite 連外部 Java I2P／i2pd，也可以用它連本機嵌入的 Emissary。不要為了取得 Rust socket SDK，無必要地同時替換 router。[S8]

## 8. Go 的 client 生態已比舊資料完整

### SAM client

`go-i2p/go-sam-go` 是新的一條 client 路線。README 宣告 SAM 3.3、STREAM／DATAGRAM／RAW、primary sessions、Datagram2／3，並把它定位為 `sam3` 的替代品。Go module proxy 核驗版本為 v0.33.1，2026-04-23。這是本次 Go SAM client 的優先 PoC 候選，但 README 的 production-ready 宣告不是獨立稽核。[S29]

較舊的路線仍有：

| 套件 | 官方表／專案功能 | 最新核驗 package 日期 |
| --- | --- | --- |
| sam3 | SAM 3.3，STREAM／DATAGRAM／RAW | v0.33.92，2024-11-16 |
| onramp | 高階 Go SAM 使用路線；官方表列 SAM 3.3 三種模式 | v0.33.91，2024-11-16 |
| goSam | 官方表列 SAM 3.2 STREAM | v0.32.54，2022-11-18 |

新專案應先讀 go-sam-go 的替代關係，再決定是否沿用舊 packages，而不是只照官方表選歷史 repository。[S1][S29][S30]

### I2CP、Streaming、Datagrams 與 SAM server

- `go-i2cp` 是真實的非 Java I2CP client。現行 README 表示 WIP，宣告 22／24 message types、session／lookup、TLS、context cancellation／timeouts、LeaseSet2 等功能。這些是專案宣告，不是通過完整 conformance suite 的證明。
- `go-streaming` 和 `go-datagrams` 是上層 libraries。go-i2p README 仍把 Streaming 稱為 prototype，不應把所有 Go client 層視為成熟完整 SDK。
- `go-sam-bridge` 是 SAM **server／bridge**，不是 client。其 binary 需要既有 I2CP router；library embedding 路線可以啟動 embedded go-i2p。
- go-i2cp 和 go-sam-bridge 的 module proxy 都核驗到 v0.1.70001，2026-09-08。版本新不代表其依賴的 router 已適合匿名性敏感的 production。[S15][S31][S32]

官方 I2CP 規格仍有「沒有已知非 Java I2CP／Streaming client library」的文字。2026 年 Go 原始碼已反證其絕對說法，但不代表 Go 已達到 Java SDK 的成熟度。[S5][S31]

## 9. Python、TypeScript、.NET 與其他語言

此表的 STREAM／DATAGRAM／RAW 是 SAM 模式，不是代表整套 router 協定完成。來源只建立部分功能時，其他欄位不自行補成「支持」。

| 語言與候選 | 已核驗或明示的功能 | 發布／狀態證據 | 採用限制 |
| --- | --- | --- | --- |
| Python：AstralMortem/i2psam | asyncio、retry／backoff、session、STREAM connect／accept／forward、classic DATAGRAM、keys／command helpers；Python 3.10+ | PyPI 1.0.0，2026-03-08；README 有 pytest | 新候選，不等於長期維護已證明。PyPI license／license_expression 為 null，repository 未見 LICENSE、pyproject 無 license，**打包前必須釐清授權**；RAW／DGRAM2／3 未核驗。[S33] |
| Python：i2p.socket | 官方表列 SAM 3.2、STREAM／DATAGRAM／RAW，socket-like API | PyPI 0.4.0，2017-12-12；README 說 probably contains bugs | 功能面較廣，published package 很舊，需自行維護及相容測試。[S1][S34] |
| Python：i2plib | asyncio，SAM 3.1 STREAM；另有 Codeberg i2plib-fork | PyPI 0.0.14，2019-02-05 | 原版 release 很舊；fork 存在不能直接推定維護程度或安全性。txi2p 是另一條 Twisted STREAM 路線。[S1][S35] |
| TypeScript／Node：@diva.exchange/i2p-sam | 官方表列 SAM 3.1 STREAM／DATAGRAM／RAW；README 有 forwarding、key helpers 與整合測試 | npm 5.5.2，2025-10-31；Apache-2.0；package 宣告 Node >=16，有 ESM／型別輸出 | 本次 TS／Node 優先 PoC 候選；不能因 Node API 就承諾瀏覽器或 Bun 完整相容，也不能把 registry 的最低 engines 當作推薦使用已 EOL runtime。[S1][S36] |
| JavaScript：node-i2p | SAM 3.0 STREAM；DATAGRAM／RAW 未核驗 | npm `i2p` 3.0.5，2016-07-15；GPL | 歷史候選，不列為新開發首選。[S37] |
| C#／.NET：I2PSharp | .NET Standard 2.0、async、primary／subsession、destination、stream connect／accept；官方表列 SAM 3.3 STREAM | 最新核驗 commit 2024-10-16；未核驗 release／NuGet 發行 | 社群程式碼；官方表列沒有 DATAGRAM／RAW，需 source pinning、補測試及確認 MASTER 相容。[S1][S38] |
| C#／.NET：i2pdotnet | SAM 3.0、async stream | 僅少量 commits，未核驗現代正式 package | 歷史備選；不是原生 .NET router SDK。[S39] |
| Ruby：i2p.rb | SAM 3.0 STREAM；另有 BOB 與舊 JRuby Java jar 路線 | Repository retired，2018-09-12 archived；Unlicense | 不列為新 Ruby 產品首選。BOB 已 deprecated。[S1][S40] |
| PHP：php-sam | 基本 SAMv3；README 範例 MIN=3.0／MAX=3.1；PHP 7.2+ | Packagist v0.5，2019-10-20；MIT | README 明說 very basic；未核驗完整模式、async 或近期 package 更新。[S41] |
| Haskell：haskell-network-anonymous-i2p | 官方表列 SAM 3.1 STREAM／DATAGRAM／RAW | 未核驗近期 release 或維護 | 只作存在性與官方清單功能紀錄，不作當前 conformance 保證。[S1] |
| Lua：mooni2p | 官方表列 SAM 3.1 STREAM／DATAGRAM／RAW | 未核驗近期 release 或維護 | 同上，需另作 source／package／license 審查。[S1] |
| Swift／Dart | 本次範圍內未核驗到足以優先推薦的原生 SAM／I2CP SDK | 不是宣稱所有 repositories 都不存在 | 可評估 SAM client 實作或已審查的 FFI／sidecar；不要引入 router rewrite 來解決 client SDK 缺口 |

同名套件容易混淆：C++ `i2p/i2psam` 與 Python `AstralMortem/i2psam` 是不同專案；Go `go-sam-go`、`sam3`、`go-sam-bridge` 也不是同一個角色。

## 10. 2026 年最重要的相容性陷阱

### SAM 3.3 版本號不足以判斷功能完整度

官方 SAM 文件的概括段落仍說 i2pd 缺乏大部分 3.2／3.3 功能，但穩定 `2.61.0` 原始碼已顯示：

- `MAX_SAM_VERSION=3.3`。
- 接受 STREAM、RAW、DATAGRAM／DATAGRAM1／DATAGRAM2／DATAGRAM3 session。
- 有 `MASTER` session 和 `SESSION ADD`／`REMOVE`。
- **穩定版 `SESSION ADD` 只支持 STREAM**，並有其他 style 的 TODO，不是 Java 那種完整 mixed STREAM／DATAGRAM／RAW subsessions。
- 2.61.0 release notes 加入 SAM datagram destination 的 b32／B33 支援。

因此「i2pd 不支持 3.3」過度概括，「i2pd 完整支持 3.3」也不成立。開發分支已出現更廣的 subsession 處理，必須等正式版本或明確 pin commit，不能計入 2.61.0。[S10][S11][S28]

### PRIMARY 與 MASTER

官方 SAM 規格目前用 `STYLE=PRIMARY`。Java 保留 `MASTER` 作向後相容，規格特別指出 i2pd／I2P+ 仍使用 `MASTER`。Client 的 primary-session 實作需要確認目標 router 接受的名稱及 subsession 功能。[S28]

### 規格新增功能不一定提高版本號

2025-04 的 Datagram2／3 和 `NAMING LOOKUP OPTIONS=true` 是沒有 SAM minor-version bump 的規格變更。看見 `VERSION=3.3` 不能推論已支持這些新增能力，也不能推論 SDK 會正確處理它們。[S1]

### API 預設與文件可能衝突

以 i2pd 2.61.0 `Config.cpp` 為準：

| 介面 | 預設 | 主要用途 |
| --- | --- | --- |
| SAM TCP | 開啟，loopback 7656 | 應用程式資料面 |
| I2CP | 關閉，配置後使用 loopback 7654 | 低階 client 資料面 |
| I2PControl | 關閉，配置後使用 loopback 7650 | 管理面 |

官方 I2PControl 頁表格說 i2pd 預設開啟，部分 i2pd 文件也錯列 I2CP 預設；都與 tag 原始碼不符。Java SAM 通常須另外啟用，不能套用 i2pd 的預設。[S1][S6][S42]

### 新版加密由 router 與 options 共同決定

SAM 規格建議明確設定 `SIGNATURE_TYPE=7`。ECIES 或 PQ hybrid 的 `i2cp.leaseSetEncType` 必須配合 router 能力；規格對 API 0.9.67+ 說明 `6,4`。不能因 SDK 換成 Rust，就宣稱已自動取得所有新版加密保護；也不能未作能力測試就將 options 強加到舊 router。[S1][S5]

## 11. GitHub／GitLab／自架 forge 的來源關係

- Java I2P 官方開發指南指向 `i2pgit.org/I2P_Developers/i2p.i2p`，GitHub `i2p/i2p.i2p` 是 mirror。
- 指南仍稱主機為 GitLab，但本次實際頁面標題是 **I2P Gitea**，`/api/v1/version` 回傳 1.25.4。應稱「官方自架 forge，目前是 Gitea」，不能把舊產品名稱當現況。
- GitHub 主庫的 sync workflow 從 i2pgit.org 取得原始碼。GitHub 頁面、release 與主 forge 的 issue／contribution 流程有不同角色。
- I2P+ 現行網站／README 指向 GitHub `I2PPlus/i2pplus`；GitLab `i2pplus/I2P.Plus` 的 API 顯示最後活動為 2024-11-06，未取得它仍同步的證據。
- i2pd、Emissary、go-i2p 的現行來源指向各自 GitHub。Emissary 的舊 `altonen/emissary` 連結已轉到 `eepnet/emissary`。
- 官方 SAM 表也列 Codeberg 的 i2plib-fork、NotABug 的 Lua／C++ client。Forge 上有同名專案，不代表可合併其 commits、維護狀態或 package provenance。[S7][S13][S43][S44]

## 12. 對目前 I2PChat 的採用建議

目前客戶端是 Qt／C++，`rust/sam_core` 只接管可選的 SAM parser／command builders；Qt 仍持有 socket，SessionController／I2PStream 的狀態及聊天／檔案協定仍是 C++。這不是 Java 客戶端，也不是完整 Rust backend。

### 優先順序

1. **正式產品 router 保持 Java I2P 與 i2pd 可切換。** 想減少 JVM 部署需求時，優先測試成熟 i2pd，而不是自製或預設切換 Rust router。
2. **目前不為 SDK 而重寫 GUI。** 已有 SAM 路線可沿用，客戶端語言不必與 router 相同。
3. **Rust 客戶端 PoC 應加入 yosemite。** 前次只看自製 `sam_core` 的評估應補上這個現成候選。若要讓 Rust 真正持有 SAM session／streams，它比繼續擴張自製文字 parser 更值得比較。
4. **只搬純解析／狀態機時，保留小型自製 core 仍有理由。** yosemite 會帶入它自己的 client I/O／runtime／session 行為；不應硬套到 Qt 仍持有全部 transport 的架構，造成兩份狀態或兩個 socket owner。
5. **需要 in-process router 時，另開隔離實驗。** C++ 路線比較 libi2pd source integration；Rust 路線比較 Emissary。正式版本保持外部 router 回退選項。
6. **Go 不作本專案重寫的理由。** go-sam-go 可作跨語言 SAM 測試參考；Go router／低階 SDK 的進展不代表應用需要改成 Go。

### 三個值得實測的候選架構

| 架構 | 適合目標 | 主要代價 |
| --- | --- | --- |
| Qt socket + 現有 C++／小型 Rust pure core + 外部 router | 最小改動、先提高 parser／狀態機測試品質 | SAM client 行為仍自行維護 |
| Qt UI + Rust client module 使用 yosemite + 外部 router | Rust 真正接管 session、stream 與錯誤生命週期 | Qt／Rust runtime、FFI ownership、取消／shutdown、跨平台發版 |
| Qt UI + Rust module + Emissary router | 無 JVM、內嵌 Rust router 的研究型產品 | 前項全部成本，加上 router 網路行為、持久化、更新與匿名性驗證 |

如果目標只是「SDK 支援最完整」，Java 官方 SDK 勝出；如果目標是「這個 Qt app 最少風險地可靠運作」，成熟 router 加 SAM 勝出。**功能排名不能直接替代產品架構決策。**

## 13. 採用前的驗收矩陣

至少用 Java I2P 2.13.0、i2pd 2.61.0 做基準；Emissary 0.4.0 和 Go experimental router 放在獨立測試組，不作成功基準的替代。

- SAM HELLO、DEST GENERATE、NAMING LOOKUP、SESSION CREATE、STREAM CONNECT／ACCEPT／FORWARD。
- Destination private keys 持久化、重啟後地址穩定、重複 ID／destination、失敗回覆與未知 options。
- Classic DATAGRAM／RAW；若產品需要，另測 Datagram2／3 和 PRIMARY／MASTER 每種 subsession，不能只測 STREAM。
- `.i2p`、b32、B33 的實際需求及 router／SDK 雙端能力。
- Split／coalesced lines、CRLF、quoted／escaped values、過長輸入與 invalid reply。
- 長時間 session、router restart、代理中斷、高延遲、timeout、cancellation、shutdown、背壓及大檔案傳輸。
- 本機 direct 與遠端受保護的 SAM 通道；確認沒有私鑰或認證資料進 log。
- macOS ARM64、Windows、Linux 的 library／runtime／installer；實際 bundled artifact 的授權與更新機制。

SAM 預設 client-to-bridge 連線不是加密、認證的保護通道。遠端 SAM 需要可信任的受保護路徑，不能把「支援 SOCKS5」當作這段 TCP 已加密。I2PChat 用 SOCKS5 到遠端 SAM，和 router 提供的 I2P SOCKS outgoing proxy 是不同功能。[S1][S6]

Router embedding 也需要持久化 NetDB／設定、更新與 graceful shutdown；不要每條 peer connection 建新 router／session。官方強調 session 應長期存在、限制 tunnel/session 數量並讓使用者適度貢獻 participating traffic。[S1][S3]

## 14. 尚未解決的問題與反方觀點

- Java SDK 功能最完整，不表示每個 native app 都應引入 JVM。對現有 Qt app，SAM 可避免 Java／C++ FFI 與 router embedding 耦合。
- libi2pd 避免外部 bridge，但帶來 C++ ABI、source pinning 和 router lifecycle；沒有實測資料證明它一定比 SAM 更快或更省記憶體。
- Rust 能降低移入 safe Rust 部分的記憶體錯誤風險，但 FFI、協定狀態、資源耗盡、應用 metadata 與匿名性仍須測試。
- Emissary 比舊 Rust prototype 成熟得多，值得 PoC；目前仍沒有足夠證據用它預設替換成熟 router。
- SDK 宣稱「支持 SAM 3.3」或 router 宣稱「full-featured」不能取代逐功能檢查。官方文件與穩定 source 已出現明確矛盾。
- 新 Python i2psam 的授權、部分語言 SDK 的維護程度、yosemite 進階 SAM features 和 cross-router 行為仍待確認。
- 本報告不聲稱某個 SDK 已經外部安全稽核，也不從沒有找到 audit 推論一定沒有 audit。

## 15. 可追溯來源

以下是主要依據。正文的 [S#] 對應本清單；每個來源只支持其實際涵蓋的功能與日期。

- [S1] [官方 SAM v3 規格、語言函式庫表、2025 新功能及開發建議](https://i2p.net/en/docs/api/samv3/)。表中除 libsam3 明示維護外，官方警告套件可能陳舊，未代為測試或 review。
- [S2] [官方替代 router 比較](https://i2p.net/en/docs/overview/alternative-clients/)。頁面標示更新至 2025，成熟度另以當前 project source 補查。
- [S3] [官方 router embedding 指引](https://i2p.net/en/docs/applications/embedding/)。生命週期、持久化、資源和網路負擔；舊 runtime 文字不可視為當前版本要求。
- [S4] [官方 Streaming API](https://i2p.net/en/docs/api/streaming/)、[Datagram API](https://i2p.net/en/docs/api/datagrams/)。
- [S5] [官方 I2CP 規格](https://i2p.net/en/docs/specs/i2cp/)。協定與版本說明；非 Java client 的概括文字已落後 Go 現況。
- [S6] [官方 I2PControl](https://i2p.net/en/docs/api/i2pcontrol/)、[SOCKS](https://i2p.net/en/docs/api/socks/)。管理面與 outgoing proxy，部分預設值需核對 source。
- [S7] [Java I2P GitHub](https://github.com/i2p/i2p.i2p)、[官方 developer guide](https://i2p.net/en/docs/development/new-developers/)、[mirror sync workflow](https://raw.githubusercontent.com/i2p/i2p.i2p/master/.github/workflows/sync.yaml)。
- [S8] [Emissary repository](https://github.com/eepnet/emissary)、[原生嵌入文件](https://eepnet.github.io/emissary/embedding-overview.html)。
- [S9] [Java 2.13.0 官方公告](https://i2p.net/en/blog/2026/07/20/i2p-2.13.0-release/)、[Maven streaming metadata](https://repo.maven.apache.org/maven2/net/i2p/client/streaming/maven-metadata.xml)、[2.13.0 streaming POM](https://repo.maven.apache.org/maven2/net/i2p/client/streaming/2.13.0/streaming-2.13.0.pom)、[Java LICENSE](https://raw.githubusercontent.com/i2p/i2p.i2p/master/LICENSE.txt)。
- [S10] [i2pd repository](https://github.com/PurpleI2P/i2pd)、[2.61.0 release](https://github.com/PurpleI2P/i2pd/releases/tag/2.61.0)、[release API](https://api.github.com/repos/PurpleI2P/i2pd/releases/latest)。
- [S11] [i2pd 2.61.0 SAM.h](https://raw.githubusercontent.com/PurpleI2P/i2pd/2.61.0/libi2pd_client/SAM.h)、[SAM.cpp](https://raw.githubusercontent.com/PurpleI2P/i2pd/2.61.0/libi2pd_client/SAM.cpp)。穩定版 feature 證據。
- [S12] [yosemite 0.7.0 release history／README](https://docs.rs/crate/yosemite/0.7.0)、[Rust API](https://docs.rs/yosemite/0.7.0/yosemite/)、[repository](https://github.com/eepnet/yosemite)。
- [S13] [I2P+ website](https://i2pplus.github.io/)、[現行 GitHub](https://github.com/I2PPlus/i2pplus)、[GitLab 舊專案 API](https://gitlab.com/api/v4/projects/i2pplus%2FI2P.Plus)。
- [S14] [Emissary 0.4.0 release notes](https://github.com/eepnet/emissary/releases/tag/v0.4.0)。
- [S15] [go-i2p README](https://github.com/go-i2p/go-i2p)、[原始 README](https://raw.githubusercontent.com/go-i2p/go-i2p/main/README.md)。現行功能 checklist 與 early-development 警告。
- [S16] [go-i2p releases](https://github.com/go-i2p/go-i2p/releases)、[release metadata](https://api.github.com/repos/go-i2p/go-i2p/releases?per_page=3)。
- [S17] [Ire prototype](https://github.com/str4d/ire)、[commit metadata](https://api.github.com/repos/str4d/ire/commits?per_page=3)。
- [S18] [i2p-cs](https://github.com/PeterZander/i2p-cs)、[官方新 router 公告](https://i2p.net/en/blog/2025/10/16/new-i2p-routers)。
- [S19] [官方 Android router](https://github.com/i2p/i2p.android.base)。不是 Kotlin SDK 的證據。
- [S20] [i2pd 2.61.0 C++ api.h](https://raw.githubusercontent.com/PurpleI2P/i2pd/2.61.0/libi2pd/api.h)、[library 建置文件](https://docs.i2pd.website/en/latest/devs/building/unix/)。
- [S21] [i2pd C wrapper header](https://raw.githubusercontent.com/PurpleI2P/i2pd/2.61.0/libi2pd_wrapper/capi.h)。只有 router lifecycle／peer test。
- [S22] [libsam3](https://github.com/i2p/libsam3)、[release metadata](https://api.github.com/repos/i2p/libsam3/releases?per_page=5)。
- [S23] [C++ i2psam](https://github.com/i2p/i2psam)。
- [S24] [Bitcoin SAM reference code](https://github.com/bitcoin/bitcoin/blob/master/src/i2p.cpp)。
- [S25] [i2p-rs](https://github.com/i2p/i2p-rs)、[published i2p crate](https://crates.io/crates/i2p)、[master Cargo.toml](https://raw.githubusercontent.com/i2p/i2p-rs/master/Cargo.toml)。
- [S26] [ri2p 版本／功能／repository metadata](https://docs.rs/crate/ri2p/latest)。
- [S27] [早期 i2p-rust service](https://github.com/resolvingarchitecture/i2p-rust)。
- [S28] [SAM PRIMARY／MASTER 規格](https://i2p.net/en/docs/api/samv3/)。
- [S29] [go-sam-go](https://github.com/go-i2p/go-sam-go)、[module proxy 版本與日期](https://proxy.golang.org/github.com/go-i2p/go-sam-go/@latest)。
- [S30] [sam3](https://github.com/go-i2p/sam3)、[onramp](https://github.com/eyedeekay/onramp)、[goSam](https://github.com/eyedeekay/goSam)；版本依各自 Go module proxy 核驗。
- [S31] [go-i2cp README](https://github.com/go-i2p/go-i2cp)、[module proxy](https://proxy.golang.org/github.com/go-i2p/go-i2cp/@latest)。
- [S32] [go-sam-bridge README](https://github.com/go-i2p/go-sam-bridge)、[module proxy](https://proxy.golang.org/github.com/go-i2p/go-sam-bridge/@latest)。
- [S33] [Python i2psam](https://github.com/AstralMortem/i2psam)、[PyPI metadata](https://pypi.org/pypi/i2psam/json)、[pyproject](https://raw.githubusercontent.com/AstralMortem/i2psam/master/pyproject.toml)。
- [S34] [i2p.socket](https://github.com/majestrate/i2p.socket)、[PyPI metadata](https://pypi.org/pypi/i2p.socket/json)。
- [S35] [i2plib](https://github.com/l-n-s/i2plib)、[PyPI metadata](https://pypi.org/pypi/i2plib/json)、[Codeberg fork](https://codeberg.org/weko/i2plib-fork)、[txi2p](https://github.com/str4d/txi2p)。
- [S36] [TypeScript i2p-sam](https://github.com/diva-exchange/i2p-sam)、[npm 5.5.2 metadata](https://registry.npmjs.org/@diva.exchange%2Fi2p-sam/5.5.2)、[npm publication timeline](https://registry.npmjs.org/@diva.exchange%2Fi2p-sam)。
- [S37] [node-i2p](https://github.com/redhog/node-i2p)、[npm i2p metadata](https://registry.npmjs.org/i2p)。
- [S38] [I2PSharp](https://github.com/MohA39/I2PSharp)。
- [S39] [i2pdotnet](https://github.com/SamuelFisher/i2pdotnet)。
- [S40] [retired Ruby i2p.rb](https://github.com/dryruby/i2p.rb)。
- [S41] [php-sam](https://github.com/theimpossibleastronaut/php-sam)、[Packagist package metadata](https://repo.packagist.org/p2/theimpossibleastronaut/php-sam.json)。
- [S42] [i2pd 2.61.0 Config.cpp](https://raw.githubusercontent.com/PurpleI2P/i2pd/2.61.0/libi2pd/Config.cpp)。SAM／I2CP／I2PControl defaults。
- [S43] [官方主 forge](https://i2pgit.org/I2P_Developers/i2p.i2p)、[Gitea version API](https://i2pgit.org/api/v1/version)、[主庫 API](https://i2pgit.org/api/v1/repos/I2P_Developers/i2p.i2p)。
- [S44] [GitLab I2P+ 舊主題專案](https://gitlab.com/i2pplus/I2P.Plus)。不作當前 mirror 證明。

## 16. 重跑研究的輸入

- 題目：I2P／i2pd 的完整 router 語言移植、嵌入式 router、原生 client SDK 與 SAM／I2CP／I2PControl 支援。
- 截點：2026-10-10。
- 深度：廣泛調查，重點候選以第一方文件、package 日期及 release-tag source 交叉驗證。
- 優先來源：官方規格、canonical forge、GitHub／GitLab、Maven Central、Go module proxy、npm、PyPI、docs.rs。
- 下一步：以第 13 節驗收矩陣實際測試選定 SDK／router，而不是再次只比較 README。
