# SGL in 2SFBL

Snapshot of [sgl-org/sgl](https://gitee.com/sgl-org/sgl)
(same project as [GitHub](https://github.com/sgl-org/sgl); the old
`gitee.com/li-shan-asked/sgl` repo moved here).

- Commit: `b28c7cdcf9af0104ae33d144b899cf42a3f09a20` (`fix: object visible issue`)
  on Gitee `main`
- Nested `.git` was removed so this tree is a vendor snapshot, not a submodule.
- Build objects go to `ram_v2/build_*/sgl/` via `third_party/sgl/SConscript`.

Boot does **not** use `source/sgl_config.h` (heap 100 KiB, all fonts, animation).
The 2SFBL override is:

`project/butterflmicro/board/sgl_cfg/sgl_config.h`

That directory is first on the include path. Only core, bump allocator, rect/line/text
draw, label, progress, snprintf, and Consolas 14 are compiled.

`sgl_misc.h` is patched so `sgl_scroll_t` compiles with `CONFIG_SGL_ANIMATION=0`
(`anim` is `void *` instead of `sgl_anim_t *`). Animation, logo, and unused widgets
are not linked.
