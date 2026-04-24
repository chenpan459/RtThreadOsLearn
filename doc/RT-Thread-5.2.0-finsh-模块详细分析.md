# RT-Thread 5.2.0 `components/finsh` 模块详细分析

本目录在 5.2.0 中实现的是 **MSH（Module Shell）**：类 Unix 的 **命令名 + 参数（argv）** 交互式 Shell，依赖 **符号表（FSymTab）** 注册命令，在独立线程里读串口/POSIX 控制台、解析行并分发执行。历史上「FinSH」曾包含 C 表达式解释器；当前树中 **`RT_USING_MSH` 打开时会 `select RT_USING_FINSH`**，编译仍落在 **`components/finsh`**，对外习惯称 **msh**。

---

## 1. 文件与编译关系

| 文件 | 作用 |
|------|------|
| **`shell.c`** | Shell 线程、`finsh_getchar`、行编辑、历史、回显、密码认证、`finsh_system_init`；提示符 **`msh `** 及可选 **`getcwd`** |
| **`msh.c`** | **`msh_exec`** 调度、**`msh_split`** 拆参、**`msh_get_cmd`** 查符号表、**`msh_auto_complete`**；**`help`**；可选 **`msh_exec_module`**、**`_msh_exec_lwp`**（Smart 可执行文件） |
| **`msh_parse.c`** | 辅助：**`msh_isint` / `msh_ishex` / `msh_strtohex`**（供命令或其它代码做数字解析） |
| **`cmd.c`** | 在 **`MSH_USING_BUILT_IN_COMMANDS`** 时编译：`version`、`clear`、`list_*`、`list_device` 等大量内建调试命令 |
| **`msh_file.c`** | 在 **`DFS_USING_POSIX`** 时编译：`cat`/`cp`/`ls`/`pwd`/`cd` 等文件命令及 **`msh_exec_script`**（`.sh`） |

**`SConscript`**：固定编译 `shell.c`、`msh.c`、`msh_parse.c`；按宏追加 `cmd.c`、`msh_file.c`。整组依赖 **`RT_USING_FINSH`**。GCC 下增加 **`-Wstack-usage=FINSH_THREAD_STACK_SIZE`**，便于发现栈溢出。

**`Kconfig`**（`components/finsh/Kconfig`）：顶层菜单为 **`RT_USING_MSH`**；其 `if` 块内 **`default y` 的 `RT_USING_FINSH` / `FINSH_USING_MSH`** 表示「开 MSH 即视为 FinSH 兼容开关打开」。**`RT_USING_NANO`** 时 MSH 默认关闭。

---

## 2. 配置项摘要

| 宏 | 含义 |
|----|------|
| **`FINSH_THREAD_NAME`** | Shell 线程名，默认 `tshell` |
| **`FINSH_THREAD_PRIORITY` / `FINSH_THREAD_STACK_SIZE`** | 线程优先级与栈（默认 20 / 4096） |
| **`FINSH_CMD_SIZE`** | 单行最大长度（默认 80） |
| **`FINSH_ARG_MAX`** | 每行最大参数个数（Kconfig 默认 10；`msh.c` 若未定义则用 8） |
| **`FINSH_USING_HISTORY` / `FINSH_HISTORY_LINES`** | 历史行数与缓冲 |
| **`FINSH_USING_SYMTAB`** | 使用链接段 **`FSymTab`** 收集 **`MSH_CMD_EXPORT*`** |
| **`FINSH_USING_DESCRIPTION`** | 符号表中保留 **`desc`**，`help` 可打印 |
| **`FINSH_USING_OPTION_COMPLETION`** | 子选项补全（**`msh_cmd_opt`**） |
| **`FINSH_USING_AUTH`** | 口令登录、`FINSH_DEFAULT_PASSWORD` 等 |
| **`FINSH_ECHO_DISABLE_DEFAULT`** | 默认关闭回显 |
| **`MSH_USING_BUILT_IN_COMMANDS`** | 编入 **`cmd.c`** |

---

## 3. 命令如何注册（符号表）

**`finsh.h`** 中 **`struct finsh_syscall`**：`name`、可选 `desc`、可选 `opt`、函数指针 **`func`**（类型为 **`syscall_func`**，即 `long (*)(void)`，实际命令为 **`int (*)(int argc, char **argv)`** 强转使用）。

GCC/Clang 下 **`MSH_FUNCTION_EXPORT_CMD`** 将 **`const struct finsh_syscall`** 放进 **`rt_section("FSymTab")`**，链接脚本提供 **`__fsymtab_start` / `__fsymtab_end`**（或等价符号，由 **`shell.c`** 里 **`_syscall_table_begin/end`** 引用，具体以 `rtdef`/链接脚本为准）。

常用宏：

- **`MSH_CMD_EXPORT(cmd, desc)`**：命令名与 C 函数名相同。  
- **`MSH_CMD_EXPORT_ALIAS(func, alias, desc)`**：对外命令名与实现函数不同（如 **`MSH_CMD_EXPORT_ALIAS(msh_help, help, ...)`**）。

关闭 **`FINSH_USING_SYMTAB`** 时上述宏不展开为表项，一般需配合其它注册方式（较少用）。

---

## 4. 执行路径：`msh_exec`

**`msh.c`** 中 **`msh_exec(char *cmd, rt_size_t length)`** 逻辑概要：

1. 去掉行首空白。  
2. **内建符号命令**：**`_msh_exec_cmd`** — 用首词长度在 **`[_syscall_table_begin, _syscall_table_end)`** 中线性匹配 **`index->name`**，命中则 **`msh_split`** 得到 **`argc/argv`**，调用 **`cmd_func(argc, argv)`**。  
3. 若失败且编译了 **`DFS_USING_POSIX`**：  
   - 若定义 **`DFS_USING_WORKDIR`**：尝试 **`msh_exec_script`**（**`msh_file.c`**，执行 **`.sh`** 脚本）。  
   - 若 **`RT_USING_MODULE`**：尝试 **`msh_exec_module`**（打开 **`.mo`** 动态模块，`dlmodule_exec`）。  
   - 若 **`RT_USING_SMART`**：尝试 **`_msh_exec_lwp`**（按当前目录、`/bin`、`PATH` 环境变量查找 **`.elf`** 等并拉起进程）。  
4. 仍失败：截断到首词，打印 **`command not found`**（Smart 下可对 **`-EACCES`** 打 **Permission denied**）。

**注意**：**`msh_split` 会原地写 `'\0'`** 分隔参数，传入的 **`cmd`** 缓冲区需可写。

**`msh_split` 行为**：跳过空格/Tab；支持 **双引号字符串** 与 **`\"`** 转义；参数个数超过 **`FINSH_ARG_MAX`** 会打印 **Too many args** 并截断。

---

## 5. `shell.c`：线程与 I/O

- **`struct finsh_shell`**：`rx_sem`、输入状态机 **`input_stat`**、`echo_mode`、`prompt_mode`、**`line[]`** 与光标位置；可选 **`cmd_history[][]`**；非 POSIX stdio 时 **`rt_device_t device`** 为控制台 UART。  
- **`finsh_getchar`**：  
  - **`RT_USING_POSIX_STDIO`**：`read(rt_posix_stdio_get_console(), ...)`。  
  - 否则 **`RT_USING_DEVICE`**：在 **`rt_device_read`** 与 **`rx_sem`** 间阻塞；**`finsh_rx_ind`** 在 RX 指示回调里 **`rt_sem_release`**。  
  - 无设备：退化为 **`rt_hw_console_getchar`**。  
- **`finsh_set_device`**：打开新串口设备、关旧设备、清行缓冲、设置 **`RT_DEVICE_FLAG_INT_RX | STREAM`**。  
- **`finsh_get_prompt`**：默认 **`msh `**；自定义 **`finsh_set_prompt`**（需 heap）；若 **`DFS_USING_POSIX` && `DFS_USING_WORKDIR`**，在提示符后 **`getcwd`** 并加 **`>`**。  
- 初始化：**`finsh_system_init`** 创建线程、初始化 **`shell`**、注册默认控制台等（细节见文件后部 **`INIT_APP_EXPORT`** 等）。

---

## 6. `msh_file.c`：与 DFS 的耦合

仅在 **`DFS_USING_POSIX`** 下编译，包含 **`dfs_file.h`**、**`unistd.h`**。典型能力：

- **`msh_exec_script`**：识别 **`.sh`**，按行 **`msh_exec`** 或读入解释（实现见该文件）。  
- **`cd`/`pwd`/`ls`/`cp`/`cat`** 等：直接 **`open`/`read`/`write`/`close`** 或 **`dfs_*`**，与 **DFS v1/v2** 通过 POSIX 层统一。  
- DFS v2 下可能 **`#include <dfs_mnt.h>`** 做挂载相关辅助。

因此：**要文件类 MSH 命令，需 DFS + POSIX，而不仅是 `RT_USING_DFS`。**

---

## 7. `cmd.c`：内建调试命令

在 **`MSH_USING_BUILT_IN_COMMANDS`** 下提供 **`version`、`clear`、`list_thread`（ps 在 msh.c 别名）、`list_sem`、`list_timer`、`list_device`、`memtrace`** 等大量 **`list_*`**，便于现场查看内核对象与资源。各命令以 **`MSH_CMD_EXPORT`** 注册。

---

## 8. `msh_parse.c` 的定位

本文件**不负责**拆词或执行，只提供 **整型/十六进制字符串** 判断与转换，供 **`cmd.c`** 或其它导出命令解析数值参数时使用。

---

## 9. 与其它组件的关系

| 组件 | 关系 |
|------|------|
| **DFS** | **`DFS_USING_POSIX`** → **`msh_file.c`**；**`msh_exec`** 中脚本/模块/Smart 可执行路径 |
| **动态模块** | **`RT_USING_MODULE`** + **`msh_exec_module`** |
| **Smart / LWP** | **`RT_USING_SMART`** + **`_msh_exec_lwp`**、`PATH`、`/bin` |
| **AT 组件** | **`net/at`** 中部分能力 **`depends on RT_USING_FINSH`** |
| **WLAN** | 可选 **`RT_WLAN_MSH_CMD_ENABLE`** 增加 WiFi 相关 MSH 命令（在 `drivers/wlan`） |

---

## 10. 扩展命令的写法示例

```c
static int my_cmd(int argc, char **argv)
{
    if (argc > 1)
        rt_kprintf("arg1=%s\n", argv[1]);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(my_cmd, demo, My demo command);
```

重新编译后，在 msh 中输入 **`demo hello`** 即可。需保证 **栈深度** 足够（递归或大局部变量会触发 GCC **`-Wstack-usage`** 警告）。

---

## 11. 相关文档

- 组件总览：`doc/RT-Thread-5.2.0-components-模块详解.md`  
- DFS 与 POSIX：`doc/RT-Thread-5.2.0-dfs-模块详细分析.md`

---

*文档对应源码树：`rt-thread-5.2.0/components/finsh`（5.2.0）。*
