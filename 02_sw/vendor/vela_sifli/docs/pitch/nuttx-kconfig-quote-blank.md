# nuttx/cmake/nuttx_kconfig.cmake：`.config` 的空行不能再当参数传（CMake ≥ 3.23 直接报错）

**症状**：全新构建目录（或删掉 `.config` 后的首次 configure）在 cmake 阶段直接失败，
报的是 `string` 参数不足一类的话，例如
`string sub-command REGEX, mode REPLACE needs at least 6 arguments total to command.`；
换成别的 CMake 小版本可能表现为莫名其妙的宏展开错误。**只要 `.config` 里存在空行就会触发**，
而本板生成的 `.config` 有 **351 行空行**（kconfig 写法如此），所以这台机器上是"必现"。

**机制**：这两个函数都用

```cmake
file(STRINGS ${kconfigfile} ConfigContents)
foreach(NameAndValue ${ConfigContents})
  ...
  string(REGEX REPLACE "^[ ]+" "" NameAndValue ${NameAndValue})   # ← 未加引号
```

`file(STRINGS)` **会把空行也读进来**；空行走一遍上面那行时 `${NameAndValue}` 展开成**零个**参数，
这条 `string()` 就少一个实参。CMake 3.23 起这种"展开后参数个数塌掉"的写法不再被容忍
（本工作区是 **CMake 3.31.6**，见 `prebuilts/tools/cmake`），旧版本只是静默地少传参数。

**修法**：给展开**加引号**（空串也是一个参数），并让空行 `continue()` 跳过。两个函数都要改。

## 组件 git id（改前先核这三行）

| 项 | 值 |
| --- | --- |
| 仓库 | `nuttx`（OpenVela 根下的独立仓） |
| 写入时 `HEAD` | `2ce740a0ac1052c5f51083a334ef3093f59ff780` |
| 该文件在 HEAD 的 blob | `dedec049e3f4bafadb9672d908cc2d89db05e3c3` |
| 打完补丁后的 blob | `68b089be94f9ed43e075976f9fda90c08faedab2` |

```bash
# 在 OpenVela 根目录执行
git -C nuttx rev-parse HEAD:cmake/nuttx_kconfig.cmake       # 应为 dedec049e3f…（未打过）
git -C nuttx apply --check -R vendor/my_vendor/docs/pitch/patches/nuttx-kconfig-quote-blank.patch \
  && echo "本机已是补丁后状态"
git -C nuttx apply --check    vendor/my_vendor/docs/pitch/patches/nuttx-kconfig-quote-blank.patch \
  && echo "本机是上游状态，可以打"
```

blob 变了说明上游动过这个文件 ⇒ 先看上游怎么改的再重做本补丁。

## 改动前（上游 HEAD，两个函数）

```cmake
function(nuttx_export_kconfig_by_value kconfigfile config)
  file(STRINGS ${kconfigfile} ConfigContents)
  encode_brackets(ConfigContents)
  foreach(NameAndValue ${ConfigContents})
    decode_brackets(NameAndValue)
    encode_semicolon(NameAndValue)
    string(REGEX REPLACE "^[ ]+" "" NameAndValue ${NameAndValue})
    string(REGEX MATCH "^CONFIG[^=]+" Name ${NameAndValue})
    if(Name STREQUAL ${config})
      string(REPLACE "${Name}=" "" Value ${NameAndValue})
      string(REPLACE "\"" "" Value ${Value})
      decode_semicolon(Value)
      set(${Name}
          ${Value}
          PARENT_SCOPE)
      break()
    endif()
  endforeach()
endfunction()

function(nuttx_export_kconfig kconfigfile)
  # First delete the expired configuration items
  get_property(expired_keys GLOBAL PROPERTY NUTTX_CONFIG_KEYS)
  foreach(key ${expired_keys})
    set(${key} PARENT_SCOPE)
  endforeach()
  file(STRINGS ${kconfigfile} ConfigContents)
  encode_brackets(ConfigContents)
  foreach(NameAndValue ${ConfigContents})
    decode_brackets(NameAndValue)
    encode_semicolon(NameAndValue)
    # Strip leading spaces
    string(REGEX REPLACE "^[ ]+" "" NameAndValue ${NameAndValue})

    # Find variable name
    string(REGEX MATCH "^CONFIG[^=]+" Name ${NameAndValue})

    if(Name)
      # Find the value
      string(REPLACE "${Name}=" "" Value ${NameAndValue})

      # remove extra quotes
      if(Value MATCHES "^\"(.*)\"$")
```

## 改动后（本工作区）

```cmake
function(nuttx_export_kconfig_by_value kconfigfile config)
  file(STRINGS ${kconfigfile} ConfigContents)
  encode_brackets(ConfigContents)
  foreach(NameAndValue ${ConfigContents})
    decode_brackets(NameAndValue)
    encode_semicolon(NameAndValue)
    # Quote: blank .config lines expand to zero args and CMake 3.23+ errors.
    string(REGEX REPLACE "^[ ]+" "" NameAndValue "${NameAndValue}")
    if(NameAndValue STREQUAL "")
      continue()
    endif()
    string(REGEX MATCH "^CONFIG[^=]+" Name "${NameAndValue}")
    if(Name STREQUAL ${config})
      string(REPLACE "${Name}=" "" Value "${NameAndValue}")
      string(REPLACE "\"" "" Value "${Value}")
      decode_semicolon(Value)
      set(${Name}
          ${Value}
          PARENT_SCOPE)
      break()
    endif()
  endforeach()
endfunction()

function(nuttx_export_kconfig kconfigfile)
  # First delete the expired configuration items
  get_property(expired_keys GLOBAL PROPERTY NUTTX_CONFIG_KEYS)
  foreach(key ${expired_keys})
    set(${key} PARENT_SCOPE)
  endforeach()
  file(STRINGS ${kconfigfile} ConfigContents)
  encode_brackets(ConfigContents)
  foreach(NameAndValue ${ConfigContents})
    decode_brackets(NameAndValue)
    encode_semicolon(NameAndValue)
    # Strip leading spaces (quote: blank lines must not drop args)
    string(REGEX REPLACE "^[ ]+" "" NameAndValue "${NameAndValue}")
    if(NameAndValue STREQUAL "")
      continue()
    endif()

    # Find variable name
    string(REGEX MATCH "^CONFIG[^=]+" Name "${NameAndValue}")

    if(Name)
      # Find the value
      string(REPLACE "${Name}=" "" Value "${NameAndValue}")

      # remove extra quotes
      if(Value MATCHES "^\"(.*)\"$")
```

## 打完怎么确认

```bash
grep -c 'blank' nuttx/cmake/nuttx_kconfig.cmake        # 应为 2（两个函数各一处引子）
git -C nuttx status --porcelain cmake/nuttx_kconfig.cmake   # 应为 " M"
git -C nuttx apply --check -R vendor/my_vendor/docs/pitch/patches/nuttx-kconfig-quote-blank.patch  # 通过
```

**为什么不能只在 `vendor` 里解决**：这份文件是 nuttx 仓库的构建脚本，`nuttx` 目录下没有
能被 `vela_override` 顶掉的 `.c` 产物（抽换层只能换源文件、换不了构建脚本本身），
所以只能作为上游补丁存在。`repo sync` 之后 nuttx 会回到上游版本 ⇒ **必须重打这一份**，
否则下一次"删 `.config` / 换干净构建目录"的 configure 会当场失败。

日期：2026-09-25（首收）。上游 `nuttx` HEAD 见上表。
