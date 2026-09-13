# 贡献与协作约定

## 分支策略

- `main`：受保护基线，只接受经 PR 合入的内容；每次合入必须通过 PR 模板中的验证清单。
- `dev`：日常开发分支，所有实现工作在此提交。
- 临时分支（可选）：大特性从 `dev` 切出 `feat/xxx`，完成后 PR 回 `dev`。
- 命名：`feat/<主题>`、`fix/<主题>`、`chore/<主题>`。

## 流程

1. 从 TASKS.md 取一条任务（或先开 Issue 建立任务）。
2. 在 `dev` 上实现，提交信息用中文祈使句；小步提交，每步保持可编译。
3. 推送 `dev` → 开 PR 到 `main`（标题格式：`<类型>(<范围>): <一句话>`）。
4. 按 PR 模板逐项填写验证证据，**空着不放行**。
5. 合并采用 squash，保持 `main` 线性干净；关联 Issue 用 `closes #编号` 自动关闭。

## 规格纪律（重要）

- `DESIGN.md` 是唯一权威规格；实现与文档冲突时改代码或走文档变更流程，不得静默偏离。
- 文档变更：提出 → 确认 → 升版本号并在文首追加版本行（DESIGN §13）。
- `TASKS.md` 完成即勾选并标注日期；验收标准未满足不得勾选。
- 物理内核相关改动必须附带 L1 解析基准结果（DESIGN §4.4）与对应单元测试用例名。

## 提交信息

- 中文、祈使句、不超过 50 字；需要时加正文说明动机与影响面。
- 示例：`实现 Kerr-Schild 度规求值与 r 求根`、`修复盘外半径口径不一致`。

## 开发机限制（务必知晓）

- 开发机无 Vulkan、GL ≤ 4.1，**GPU 渲染无法在本机运行**。
- 可在本机完成：编译、单元测试、CPU 参考实现与 golden 渲染。
- 所有"跑起来看"的验证需在目标机执行，PR 中注明由哪台机器验证。

## 网络受限环境的推送备用通道

本机 git over HTTPS 到 `github.com` 曾被代理阻断（`CONNECT tunnel failed, response 502`），
表现为 `git push` / `git fetch` 静默失败（exit 128、无输出）。备用手段：

1. **换 SSL 后端**：`git config http.sslBackend openssl`（默认 schannel 在代理链路上握手失败）。
2. **gh-proxy 镜像**（已配置为 `mirror` remote，读通道已验证）：
   `git fetch mirror "+refs/heads/main:refs/remotes/mirror/main"`。
3. **GitHub API 直连**（`gh api` 通常不受影响）：必要时用 Git Data API 创建 blob/tree/commit 再
   `PATCH /git/refs/heads/<branch>` 更新分支；注意 Trees API 请求字段名是 `tree` 而非 `items`，
   且空仓库禁止直接创建 blob——先经 Contents API 建根提交。
4. 恢复正常后务必确认本地与远端一致：`git fetch origin && git status`；不一致时以远端为准重置。

