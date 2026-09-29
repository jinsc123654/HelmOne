# openvela 与参赛说明

## 这是一个 openvela 作品

Helm One 是**基于 openvela 的作品**。

openvela 是面向 AI 硬件与智能设备场景的开源操作系统（内核为 NuttX）。
本作品没有另起炉灶，而是**把 openvela 当作产品底座**，在这上面长出整车能力：

| 用到的 openvela 能力 | 在本作品里干什么 |
|---|---|
| **图形框架（LVGL）** | 五个骑行页面的整套界面——外壳、调色板、点阵字库与回退链、图标、菜单 |
| **矢量字体渲染** | 通知与文件名等需要任意汉字的场合，走 LFS 里的子集 TTF（启动后加载） |
| **蓝牙框架 + zblue 主机栈** | 单射频双角色：对手机是 GATT Server，对心率/踏频/功率计是 GATT Client，可同时工作 |
| **KVDB 键值框架** | 配置与配对信息的持久化（`/mnt/kv/db`） |
| **runtime Skill 规范（`packages/ai_agent`）** | 板级 Skill 的加载约定：`/data/agent/skills/*.md` 扁平文件，首行标题进索引、正文按需读取 |

**板级差异不打上游补丁，走抽换机制。** 新平台适配的全部改动放在
`vendor/my_vendor/boards/sf32lb52/my_vendor/vela_override/`（当前 18 个上游 `.c` 的替换件，
覆盖 `sched/`、`fs/`、`mtd/`、`usbdev/`、`bluetooth/`、`zblue/`、`freetype/`），
`repo sync` 把上游刷回干净树也不影响本作品。对上游确实有价值的改进另以补丁形式单独提交。

本作品落地的是大赛要求的三选一里的 **【图形】**（另两项是 AI 与多媒体）。

## 本仓库不是参赛提交仓库

**这一条要说清楚**，避免混淆：

| | 仓库 | 作用 |
|---|---|---|
| **产品开发仓库** | **本仓库**（HelmOne） | 产品文档、硬件设计、固件 submodule、手机 App、PC 工具链 |
| **参赛提交仓库** | `contest2026_048_dijiugexiaxianyue` | 提交材料：技术报告、自定义 Skill、build 文档、AI Coding 日志、repo manifest |

参赛提交仓库：
<https://github.com/open-vela/contest2026_048_dijiugexiaxianyue>

本仓库的组织方式**参考**了参赛仓库，但两者职责不同：**提交材料以参赛仓库为准**，
本仓库不承担提交材料的维护。

## 参赛背景（供理解本仓库里的东西为什么长这样）

- **大赛**：2026 首届 openvela AI 硬件开发者大赛
- **队伍**：第九个下弦月（编号 `048`）
- **赛道**：AI 硬件产品创新 + 新硬件平台适配（两个方向都做）
- **硬性要求**：图形 / AI / 多媒体至少落地一项（本作品落【图形】）；
  以及沉淀一个**自定义 Skill**
- **协议**：Apache 2.0
- 本作品不含语音唤醒，不涉及唤醒词

这些背景能解释本仓库里的几处设计：

- **自定义 Skill 的源文件在固件树里**（`02_sw/vendor/vela_sifli/ai/skills/vela-helm-one.md`
  及其 `references/`），而不是在本仓库单独维护 —— 因为它跟着固件走，
  安装时拷到设备的 `/data/agent/skills/`。
- **固件仓库的 `docs/` 只保留参赛与启动烧录相关的内容**（pitch 材料 + 启动链 + 构建烧录），
  内部开发笔记不随提交走。取舍依据与遗留问题见 [07-仓库维护.md](07-仓库维护.md)。
- **`01_hw/` 放着 Altium 工程**，因为"新硬件平台适配"是作品的一半。

## 参赛材料在哪

想在提交仓库里找什么，对照这张表：

| 材料 | 在提交仓库的位置 |
|---|---|
| 作品说明 | `README.md` |
| 技术报告（`.pdf` 提交版 + `.docx` 可编辑版） | `docs/report/` |
| 自定义 Skill（运行时单文件 + PC 侧深读知识库） | `docs/ai/` |
| 构建 / 烧录 / 上板 / 分区 / 状态机口径 | `docs/build/` |
| AI Coding 日志 | `logs/` |
| repo manifest | `openvela.xml`、`contest2026_048_dijiugexiaxianyue.xml` |

本仓库里与之**同源**的东西（注意是各自维护的，不是链接）：

| 本仓库 | 提交仓库里的对应件 |
|---|---|
| `02_sw/vendor/vela_sifli/ai/skills/vela-helm-one*` | `docs/ai/vela-helm-one*` |
| `02_sw/vendor/vela_sifli/docs/{build_guide,boot_2sfbl,sd_partition,…}.md` | `docs/build/` |
| `02_sw/vendor/vela_sifli/boards/sf32lb52/my_vendor/`（固件主体） | 提交仓库以 vendor 树形式引入 |

> ⚠️ 这几对是**副本关系，不是引用**，改一边不会同步另一边。改动时想清楚谁是权威：
> 固件相关以**固件仓库**为准，提交材料以**提交仓库**为准。

## 相关链接

- 参赛提交仓库：<https://github.com/open-vela/contest2026_048_dijiugexiaxianyue>
- 固件源码：<https://gitee.com/jinsc123654/vela_sifli>
- openvela 上游：<https://github.com/open-vela>
