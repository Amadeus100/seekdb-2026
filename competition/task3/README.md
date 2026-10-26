# 赛题三：Bug 修复并向官方提交 PR

提测的“PR 链接”填写：

```text
https://github.com/oceanbase/seekdb/pull/1763
```

## 已核验的 PR

- 目标：`oceanbase/seekdb:master`。
- 来源：报名仓库 `Amadeus100/seekdb-2026`，GitHub 仓库 ID `1434215580`。
- 来源分支：`fix/ddl-identifier-escaping`。
- 修复版本：`c102360f410756cb1df47affaef0732083b5a7cf`。
- PR 状态：`open`、`draft=false`；创建后核验时可合并。
- PR 只包含 1 个提交、5 个源码及测试文件。

## 修复

关联带 `2026-competition` 标签的 [Issue #1469](https://github.com/oceanbase/seekdb/issues/1469)。共享 DDL 代码原来把列名直接插入反引号之间，含反引号的列名会生成无效标识符。

本修复在 OBLib 的 `ObSqlString` 中增加标识符内容转义，将内部定界符加倍，再供共享 DDL 的源列名及别名使用。转义位于底层字符串模块，不引入 `share -> sql` 依赖，并保留 enum/set 的 unsigned cast、AS 和影子列前缀格式。

## 实际测试结果

[Linux C++ 编译及回归运行](https://github.com/Amadeus100/seekdb-2026/actions/runs/40313026432)已成功完成。使用项目默认 Release/O2 配置、官方依赖、生产 OBLib 运行库及 Google Test，实际命令为：

```bash
python3 build.py init
python3 build.py test --jobs=2 --test_output=errors //unittest/oblib:test_sql_string_identifier
```

6 项测试通过，0 失败、0 错误：

1. 反引号及连续、边缘定界符。
2. 不同定界符的选择。
3. UTF-8 和明确字节长度的非 NUL 终止输入。
4. DDL cast 与影子列前缀的组合。
5. 缓冲区扩容及已有文本追加。
6. 空输入及无效定界符。

已执行 `git diff --check`，并确认测试报告中的源码版本与 PR 来源 SHA 一致。没有执行完整 seekdb 服务端构建或端到端 SQL 回归，不将其表述为已通过。

永久保存的原始报告见 [evidence/test.xml](evidence/test.xml)、[evidence/unit.log](evidence/unit.log)，机器可读核验记录见 [pr-proof.json](pr-proof.json)。比赛只要求已提交核验，不要求等待 PR 合并；最后认领及得分仍以大赛平台为准。
