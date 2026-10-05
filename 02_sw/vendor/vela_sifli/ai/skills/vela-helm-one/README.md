# Helm One 知识库（PC 侧深读版）

> **运行时 Skill 不在这里。** 给设备/助手运行时用的 Skill 是单文件
> `vendor/my_vendor/ai/skills/vela-helm-one.md`，可直接拷成
> `/data/agent/skills/vela-helm-one.md`（openvela `packages/ai_agent` 只识别该目录下的
> 扁平 `.md` 文件，子目录会被忽略）。
>
> 本目录是**同源的 PC 侧深读知识库**：内容更细、带唯一源出处，供人或在 PC 上工作的
> AI 助手查阅。两者冲突时以仓库文档为准，并回头修正。

```
ai/skills/
├── vela-helm-one.md          # ← 运行时 Skill（单文件，openvela ai_agent 规范）
└── vela-helm-one/            # ← 本知识库（PC 侧）
    ├── README.md
    └── references/
        ├── board-facts.md        # 权威值：分区/引脚/时钟/阈值/地图参数/协议口径
        ├── constraints.md        # 强制约束与禁止事项
        ├── troubleshooting.md    # 故障恢复手册（现象 → 根因 → 处置 → 源）
        └── commands.md           # 构建/烧录/监视/机上探针/地图工具链
```

## 何时加载

| 触发场景 | 典型提问 | 先读 |
|---|---|---|
| 一、板级 / 驱动 | 「给 SF32LB52 加一路 SD/LCD/GNSS 驱动」「SDIO 和 MPI2 抢脚怎么办」 | `references/board-facts.md` |
| 二、地图工程 | 「重新生成全国分片」「改吸附半径 / 折线上限」「为什么这条路没规划出来」 | `references/board-facts.md` §5、`docs/map/` |
| 三、协议与 App | 「加一个 Companion 特征」「App 收不到里程」 | `references/constraints.md` §1 |
| 四、稳定性排查 | 「开机 15 s 必复位」「SD 整卷僵死」「GNSS 静默」「蓝牙连上就断」 | `references/troubleshooting.md` |
| 五、构建 / 烧录 / 上板 | 「怎么只烧固件」「AI 怎么读串口」 | `references/commands.md` |

## 工作方式

1. **先查表，再动手。** 阈值、上限、偏移一律从 `references/` 与仓库文档取；不要凭记忆或凭 AI 常识写数字。
2. **改一处，同步一处。** 阈值同步 `docs/system_states.md`；分区同步 `ptab.sdmmc.json` 与两份 `ptab_sdmmc.h` 并重新生成表；协议同步固件与 App 两端头文件。
3. **结论必须过真机。** nsh 探针 → 串口日志 → diag 巡检，三者取证据，不以「看起来对」结案。
4. **不许绕过既有抽象。** 不改上游 `nuttx/`，板级差异走 `vela_override/`。
5. **收尾留痕。** 每解决一个非平凡问题，写进对应 `docs/*.md`，并在 `troubleshooting.md` §E 增一行索引。

沉淀过程与维护约定见 `docs/ai_native_skill.md`。
