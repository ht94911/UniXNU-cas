# UniXNU-cas 设备仓库

工作规则、路线和进度以本地管理仓库为准：`I:\Code\UniXNU-cas\AGENTS.md`（WSL：`/mnt/i/Code/UniXNU-cas/AGENTS.md`）。先读那里，再改这里的代码。

本仓库负责 cas 的 `board.toml`、`dts/`、`configs/`、板级 `package/`（构建树里是 `kits/cas/`）、`patches/` 和板级构建脚本 `scripts/`。工作分支是 `dev`；`legacy/*` 分支是旧流程遗留，不要在上面工作。

构建入口是框架仓库的 `./unixnu`（`/home/hua/cas-work/UniXNU`）。启动镜像用 `scripts/cas_bootimg.py` 打包，命令见管理仓库的 `docs/BUILD.md`。
