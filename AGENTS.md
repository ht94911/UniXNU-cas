# UniXNU-cas 设备仓库

工作规则、路线和进度以本地管理仓库为准：`I:\Code\UniXNU-cas\AGENTS.md`（WSL：`/mnt/i/Code/UniXNU-cas/AGENTS.md`）。先读那里，再改这里的代码。

本仓库负责 cas 的 `board.toml`、`dts/`、`configs/`、板级 `package/` 和 `patches/`。工作分支是 `dev`；`legacy/*` 分支是旧流程遗留，不要在上面工作。

构建入口是框架仓库的 `./unixnu`（`/home/hua/cas-work/UniXNU`），命令见管理仓库的 `docs/BUILD.md`。

注意：以下内容属于旧的自制流水线，按计划在 M1 跑通标准流水线后删除，不要再扩展：

- `config/`、`package/CasCpu0`、`package/CasHandoff`、`package/CasTrace`
- `patches/cpu0-menu`、`patches/xnu/cas-*.patch`
- `environment/`
- 大部分 `scripts/` 和 `tests/`

删除清单见管理仓库 `docs/PLAN.md` 的 M1。
