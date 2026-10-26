# 赛题一：部署 seekdb

## 本次部署结果

2026-10-26 11:26（北京时间）已在 GitHub Actions 的 Linux 环境部署官方 seekdb 1.4，执行真实 SQL 查询成功，官方遥测接口返回 HTTP 200、业务码 200 和“上报成功”。

**赛题一提测填写以下完整 UUID：**

```text
3c001d6b-dc45-8fd9-aadf-fb0f0db5dbf4
```

- [成功的部署运行记录](https://github.com/Amadeus100/seekdb-2026/actions/runs/40264057609)
- [原始实例遥测](evidence/telemetry.generated.json)
- [SQL 验证结果](evidence/sql-validation.json)
- [官方遥测接收响应](evidence/telemetry-report.json)
- [部署信息](deployment.json)

以上文件中的 UUID 已交叉核对一致。遥测响应里的数字记录 ID 不是部署 UUID。保存记录后，临时容器已释放；大赛平台的同步和首次认领结果仍需实际提测确认。

官方题目要求部署真实 seekdb、确认可以执行 SQL，并提交实例 `run/telemetry.json` 中的 `content.id`。该值必须是完整的 36 位 UUID。2026-10-26 更新的[赛题说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lz6ixow11mar233g)明确部署平台不限，AMD 算力是可选资源。

## 部署与验证

使用安装有 Docker、Python 3 的 Linux 环境：

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r competition/task1/requirements.txt
bash competition/task1/deploy.sh
```

脚本固定使用官方 `oceanbase/seekdb` 1.4 容器镜像的摘要：

```text
sha256:4db4961869d554313520d6081fca29265ba6da6db7313311bd8acbed771a956b
```

启动参数请求 2 核、2GB 内存、4GB 日志磁盘；实际采用值保留在原始遥测的 `content.resource` 中，本次记录为 2 核、约 6.20GB 内存、4GB 日志磁盘。SQL 端口只映射至本机 `127.0.0.1:2881`。如果该端口已占用，可通过 `SEEKDB_PORT=2883 bash competition/task1/deploy.sh` 使用其他端口。

脚本会完成以下操作：

1. 启动官方 seekdb 容器，等待真实 SQL 查询成功。
2. 从该容器复制实例自己生成的 `run/telemetry.json`，拒绝缺少连字符或非 36 位的标识。
3. 保存 `SELECT 1`、版本、当前用户和数据库的实际查询结果。
4. 将同一实例生成的原始 `content`、`component` 值重试上报至官方遥测接口，并保存实际响应。
5. 保存结果后停止并删除本脚本创建的临时容器。官方教程允许保存 UUID 后释放部署实例。

默认输出目录是 `competition/task1/evidence/`，可通过 `EVIDENCE_DIR` 指定其他路径：

| 文件 | 用途 |
| --- | --- |
| `uuid.txt` | 提测时填写的完整部署 UUID；仅在 SQL、UUID 和遥测接收检查成功后生成 |
| `telemetry.generated.json` | 直接从运行实例复制的遥测文件 |
| `sql-validation.json` | 实际 SQL 查询结果、UUID 和原文件摘要 |
| `telemetry-report.json` | 官方遥测接收接口的 HTTP 状态、响应内容及上报时间 |
| `container.log` | 容器启动及关闭诊断记录 |

`verify.py` 也可以检查已有实例：

```bash
python3 competition/task1/verify.py --base-dir /path/to/seekdb --port 2881
```

需要数据库密码时，使用环境变量 `SEEKDB_PASSWORD`，不要将密码写入仓库。

## 提测

在大赛平台的赛题一入口填写 `evidence/uuid.txt` 的完整 UUID。仓库提交本身不能替代该字段。遥测接口接收与大赛平台认领是两个步骤；最终同步、是否为本队首次认领和得分，以大赛平台的提测结果为准。

旧 Windows 1.3 安装包会生成 64 位十六进制遥测标识，不能直接用于本题。不要截断、转换或手动生成 UUID。

官方镜像说明：[OceanBase Docker Images / seekdb](https://github.com/oceanbase/docker-images/blob/main/seekdb/README.md)。
