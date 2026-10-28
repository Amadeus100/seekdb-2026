# 2026-10-28 恢复进展（以下是当前状态）

用户已要求继续，当前状态为开发中。报名仓库、此前入门题 UUID、Fork 和 PR 保持不变；本次内核题已推送至开发分支 wip/ai-file-column-2026，尚未替换报名仓库默认分支。

- 文件 SQL 原先的 1210 问题已定位并修复：Schema 相关路径/选项不得被参数化，动态 VARCHAR 列需要设置隐式排序规则属性。
- 修复 DESCRIBE 标记占用参数计数字段的问题，重新生成本地解析器；COPY table 和 COPY query 都已验证。
- 文件扫描接入原生批量算子，PL/普通集合表函数保留原有标量路径。
- 文件 SQL 官方主流程 15 条 SQL 无执行错误；7 组 Python 集成断言通过，覆盖真实 SQL 过滤/聚合/INNER/LEFT/同文件 JOIN、重扫、Schema、NULL、日期、选项、文件变更、Snappy 多 Row Group/可空/列裁剪、COPY 表/查询/禁止覆盖/失败清理。
- 最新独立扫描器 693 项检查通过。Parquet V2 布尔 RLE、重新打开文件时列裁剪状态、资源上限、glob 无正则匹配和 CSV 选项校验均已补充。
- AI 增强官方主流程 52 条 SQL、原有 AI_PROMPT 101 条 SQL 无执行错误；5 组协议断言通过，覆盖 JSON/嵌套/非法模板、长文本、配置、跨行分组/乱序/NULL/过滤/LIMIT、异常响应、分批稳定重排与 Ollama 原生协议。
- 修复 Ollama ALTER Provider 后空密钥被存为 SQL NULL 导致调用失败的问题，并保持其他 Provider 的密钥校验。
- Windows seekdb 编译通过，专用实例端口 2884 正在运行（PID 以英文目录 kernel-test-3/test-server.pid 为准）。本地 Mock 18000 也在运行。测试和构建命令在仓库外 ../native-build.ps1、../restart-kernel-test.ps1、../test-file-scanner.ps1；普通 mysqltest 语句执行辅助器为 ../run-kernel-sql.py，带完整结果断言的集成测试已在 unittest/ 下。
- AI 列已接入构建，Windows 完整编译通过。增加 CREATE/ADD/MODIFY/SHOW/RETRY、同 DDL 事务保存元数据、隐藏列、原生触发器的事务任务、持久化租约与后台模型调用、分组写回、重试和严格读取。完整语义仍在验证。
- AI 列官方 DDL 主流程 30 条 SQL 无执行错误。实际集成测试已确认隐藏列、SHOW、禁止直接赋值、事务内任务创建/回滚、初次 JSON+整数同组生成、无关更新和依赖更新回滚。已定位并修复 MODIFY 误判重复属性及后台读取整数类型问题，正在验证任务领取竞争和后续流程。不能声称全部 AI 列测试已通过。
- 本地已提交 AI/文件验证检查点 e7b1b7e；AI 列阶段已提交 ab56a5e 并推送至开发分支。最新测试实例为英文目录 kernel-test-4，端口仍 2884，重启脚本 ../restart-ai-column-test.ps1。
- 最终 Windows 二进制上的七组 AI 列集成测试已全部通过，覆盖事务、乱序、重试、严格读取、向量索引、无主键/复合主键、已有数据加列和混合成功失败。AI 函数五组协议测试及文件 SQL 七组集成测试也在该版本上重新通过。后台中断恢复已验证：请求进行中重启实例后，任务在持久化租约到期后重新领取并正确写回。Linux 首次构建在 SQL 解析器生成阶段失败，正在排查；比赛平台评测仍待验证。
- 已准备手动或开发分支推送触发的 Linux 编译与集成测试工作流，尚未运行。

验证输出位于本目录 evidence/。本地通过不代表隐藏测试或比赛平台已通过。

---

以下为暂停时的历史记录，其中的“当前阻点/尚待验证”以以上恢复进展为准。

# 内核三题暂停记录

暂停日期：2026-10-27（Asia/Shanghai）。用户要求暂停并保存状态，等待后续继续。

## 当前结论

这三题尚未全部完成，当前快照不能作为已通过评测的最终版本。代码保存在独立的本地工作分支 `wip/ai-file-column-2026`，本次暂停不推送或替换远程提测分支。

报名仓库仍为 `https://github.com/Amadeus100/seekdb-2026`。此前部署 UUID、公开 Fork、PR #1763 及队伍验证行均保留。本次三道内核题与此前的三个入门题是不同的工作。

## 开发基线与要求

- 原本地 `2026-competition` 分支在 `dcae32b85421ebd491b7757d6cc99947753ee7d7`。
- 已合入官方比赛测试提交 `5bce9e06d1281347ad61344e4b01fab9158c58a2`；合并提交为 `77f2d746a2df0cca445d7d5f5c77457b8b898c43`。
- 已取消稀疏检出，完整源码在本地。
- 已读取三份官方完整要求，原始下载文件位于仓库外的 `../requirements-2026/`。
- AI 函数要求：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/psu9fks8dexldoum
- 文件 SQL 要求：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/gy2a66vag0028msk
- AI 列要求：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qenx4sy5s09h2t07

## AI 函数增强：已写入的实现

- AI_PROMPT：TEXT/LOB 读取，保留 JSON 参数结构，支持嵌套 Prompt；替换逻辑改为可增长缓冲区，检查下标溢出，支持重复占位符，避免重复解析替换内容。
- AI_COMPLETE：处理 SQL NULL 配置、JSON 配置与长文本；检查响应为非空字符串；单次模型调用返回原始文本。
- Ollama：注册 Provider，大小写兼容，允许空 API Key，接入原生生成和 Embedding，生成请求带 `stream=false`。
- AI_EMBED：传播 NULL，读取长文本，验证维度和有限数值；OpenAI 响应按 index 恢复顺序并拒绝重复/越界 index。
- AI_EMBED 批量：按模型和 dimension 分组，单请求最多 128 条，排除 NULL/过滤行，保留行映射，全部请求成功后发布结果。
- AI_RERANK：统一字符串/对象输入路径，分批请求，校验数量、index、score，恢复全局 index，按分数及原始顺序稳定排序，对象保留原 ID。
- 新增测试 Mock：`tools/deploy/mysql_test/test_suite/ai_function/include/enhancement_mock.py`，尚未启动或执行对应完整端到端测试。

已验证：实际数据库 SQL `AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1,true), JSON_OBJECT('k',2))` 返回保留结构的正确 JSON。

尚待验证：六小题的完整 Mock SQL 主流程、异常响应、跨行请求次数及过滤/LIMIT/多批次行为。不能声称六小题已通过。

## 文件即数据库：已写入的实现

- SQL 解析入口：read_csv、read_json/read_ndjson、read_parquet、glob、命名参数与别名。
- `enable_file_sql` 默认关闭；读取及 COPY 路径依赖 `secure_file_priv`。
- 动态列 Schema 接入 seekdb 原生 FUNCTION TABLE，过滤、聚合、排序和 JOIN 继续由原生执行计划处理。
- CSV/JSONL：独立的全量 Schema 推断和流式扫描两遍读取，不保存全文件数据；支持重扫、文件变化检测、字段并集和类型宽化。
- CSV：表头去重、引号转义、字段换行、LF/CRLF、NULL、UTF-8 检查及记录/列数限制。
- Parquet：Footer/Thrift Compact、平坦 Schema、Row Group、PLAIN、字典、RLE/bit-packed、Snappy、可空列及列裁剪；含日期/时间与部分无符号整数处理。
- glob：目录发现和路径检查，返回 file 列。
- DESCRIBE SELECT：通过已解析查询的输出列生成元数据结果，不扫描执行查询数据。
- COPY table/query TO：原生 SELECT INTO 接入，CSV 选项、表头、NULL，排他临时文件，成功后原子发布；失败清理临时文件。

已验证：独立 C++ 文件扫描测试曾通过 77 项检查，包含官方 simple.parquet 样本、CSV、JSONL、重扫与边界场景。之后还修改过 UTF-8、类型宽化、Parquet 和 COPY，需重新运行最新版本测试；不能把 77 项当作当前全部代码的回归结果。

当前阻点：测试实例上的 read_csv/read_json 查询（连 EXPLAIN、LIMIT 0）返回 1210 Invalid argument，尚未定位。该实例运行的是较早编译快照。最新代码已加入 `file table schema resolution failed` 日志并成功重新编译，但最新二进制尚未复制到运行目录验证。下次应首先部署最新二进制，回读 reader_error/path_type/format 等诊断，修复后再跑文件 SQL 主流程。

尚待验证：最新扫描器测试、Snappy 多 Row Group/可空列专项、真实 SQL 的过滤/聚合/JOIN、DESCRIBE、COPY 读回及错误清理、路径逃逸和 Linux 构建。

## AI 列：仅草稿，未接入构建

新增文件：

- `src/share/ai_service/ai_column_definition.h`
- `src/sql/engine/ai/ai_column.h`
- `src/sql/engine/ai/ai_column.cpp`

已有草稿：定义编解码、模型/类型/选项/依赖检查、隐藏列生成、表达式引用处理。设计使用已有 Schema extended_type_info 的最后一项保存定义，保留 VECTOR 类型信息第一项；使用当前保留的低位 column flag 标记 AI 列。

这些文件尚未加入 source inventory，尚未编译。AI COLUMN 语法、Schema 存储/恢复、系统元数据表、DML 联动、持久化任务、后台调度、结果写回、重试、Strict Read 和向量索引验证都尚未完成。`ai_column_strict_read` 目前只注册了系统变量，没有实现其读取语义。

已研究但未实施的方案：通过内核维护的事务内触发逻辑创建持久任务，后台只领取已提交任务；以请求 UUID 和定义版本保护旧结果；同一行同次变更的多 AI 列分组原子发布；写回和失效都经过原生 DML 维护索引。此方案仍需实现和验证，不得视为已完成。

草稿的已知待修事项：向量 dimension 不匹配应使用 `OB_ERR_INVALID_VECTOR_DIM`（7600），现草稿错误码尚未修正；VECTOR extended_type_info 附加定义后，现有若干 `count()!=1` 校验及序列化/恢复条件需要有针对性兼容。禁止放宽普通非 AI VECTOR 的校验。

## 本地构建与运行环境

仓库路径：`D:\作业\Oceabase数据库\比赛\seekdb-2026`。

英文工作目录：`C:\Users\R9000P\.codex\visualizations\2026\10\26\01a17623-3399-72a1-b473-73a01a05faa4`。

- `seekdb-source` 是指向仓库的 Junction，供 Windows 工具规避中文路径问题。
- 构建目录：上述英文目录下 `build-kernel`。
- 主程序：`build-kernel/src/observer/seekdb.exe`，构建时自动打包依赖 DLL。
- 仓库外 `../native-build.ps1` 已保存可复用的增量构建命令，日志为 `../native-build.log`。
- 官方 Windows 依赖在 `deps/3rd/`，未纳入 Git。
- 使用官方 LLVM 18.1.8。机器上的 VS 2026 STL 要求更新的编译器，已下载微软官方 `STL-vs-2022-17.13/stl/inc` 到 deps/3rd，并仅为构建进程前置 INCLUDE；未修改系统安装。
- Rust 工具链 1.98.1 已安装到 deps/3rd/cargo 和 deps/3rd/rustup；构建设置 RUSTUP_TOOLCHAIN=1.98.1。
- 系统变量生成脚本原为 Python 2；本次以 Python 3 的 lib2to3 在内存中转换后执行。生成文件只保留必要差异。
- 最新完整 Windows seekdb 编译成功；AI 列草稿不在此次编译范围内。
- 本地 Python 已有 pymysql、pyarrow，可用于连接验证和生成 Parquet 测试数据；产品实现未使用外部 SQL 引擎。

本次测试目录（保留，不删除）：

- 仓库外 `../native-runtime`：较早编译二进制副本及首次失败的测试数据。
- 英文目录 `kernel-runtime`：较早二进制和依赖 DLL。
- `kernel-test-2`：曾在 2883 端口运行。
- `kernel-test-3`：曾在 2884 端口运行，PID 46648，本次暂停时关闭。
- `kernel-fixtures`：CSV、JSONL、Parquet 样本，已作为实例启动时的 secure_file_priv。
- `build-kernel/file-scan-tests.exe`：独立扫描器测试可执行文件。

启动经验：初始数据文件使用 1G、日志 2G；512M 的首次初始化未成功。当前配置参数为 `memory_budget`，不是旧教程中的 memory_limit。Windows 远程连接不允许修改 secure_file_priv，应在新实例首次启动时通过 `--variable=secure_file_priv=<测试目录>` 配置，不能绕过限制。

## 下次继续顺序

1. 读取本文件，确认 WIP 分支及工作树状态。
2. 将最新成功构建的二进制部署到独立运行目录，修复文件表函数的 1210 解析/Schema 问题。
3. 运行 AI Mock 端到端测试、最新扫描器测试及文件 SQL 主流程；修复编译通过但 SQL 未验证的问题。
4. 实现 AI 列剩余的事务、Schema、调度、重试与严格读取；不要只提交草稿或空接口。
5. 验证原生索引更新/回滚、乱序结果保护和恢复；执行官方公开测试及补充异常测试。
6. 整理提交、Linux 检查和可审阅的验证记录，再更新报名仓库的提测分支。不得将 WIP 误报为已通过。

本文件不保存任何令牌、密码或认证 URL。后续 Git 网络操作沿用已有代理配置，仅在进程内临时提供认证。
