# Incursion Red River — PQOverlay

## 这是什么

PQOverlay 是为 **Incursion Red River** 编写的 Windows x64 游戏修改菜单和画面覆盖层，使用 C++20、Win32 和 DirectX 11。这个仓库不是游戏本体源码，只包含覆盖层、加载器和构建所需的第三方依赖。游戏更新后，需要重新确认运行时接口是否兼容。

**版本兼容性：** 仅在游戏 `ALPHA#1.4.0.4` 版本测试通过，不保证其他版本可用。

## 有什么用

| 菜单页 | 功能 |
| --- | --- |
| 透视 | 在画面上绘制目标方框、骨骼、血量和距离，并标记尸体。 |
| 自瞄 | 设置瞄准热键、目标部位、距离和范围。 |
| 玩家 | 调整血量、耐力、受伤状态、移速和跳跃高度；可传送到地图标记。 |
| 武器 | 提供弹药、手雷、耐久、后坐力、扩散、近战攻速和伤害相关选项。 |
| 库存 | 提供即时搜索、添加金钱、添加标记硬币、添加经验和据点升级操作。 |
| 世界 | 调整全局游戏速度与游戏内时间；打开菜单时读取当前游戏时间。 |

`INS` 显示或隐藏菜单，`END` 卸载覆盖层。配置文件为 `PQOverlay.ini`，始终从 DLL 所在目录读取和保存，不依赖游戏的工作目录。

## 构建

安装 Visual Studio 2022 Build Tools 的「使用 C++ 的桌面开发」工作负载，并包含 MSVC x64 编译器、Windows SDK、CMake 和 Ninja。项目要求 CMake 3.20 或更新版本。在源码目录用 PowerShell 运行：

```powershell
.\build.ps1
```

脚本会初始化 x64 编译环境并执行 Release 构建。如果 Visual Studio 安装在其他位置，可打开对应的 x64 开发者命令行，在源码目录手动运行：

```text
cmake -S . -B "E:\BuildOutput\Incursion Red River" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "E:\BuildOutput\Incursion Red River"
```

## 各部分负责什么

| 文件或目录 | 职责 |
| --- | --- |
| `src/dllmain.cpp` | 初始化覆盖层、绘制菜单和画面标记、处理热键、读写配置。 |
| `src/game.h`、`src/game.cpp` | 定义菜单设置与游戏数据结构，读取玩家、摄像机和目标状态，处理画面投影、玩家选项及传送。 |
| `src/weapon_hooks.h`、`src/weapon_hooks.cpp` | 处理游戏线程上的武器、库存和世界功能，把菜单请求交给相应的游戏接口。 |
| `src/injury_protection.h`、`src/injury_protection.cpp` | 处理防流血、防骨折选项及退出时的清理。 |
| `src/loader.cpp` | 独立加载器的入口，不负责菜单或功能实现。 |
| `CMakeLists.txt` | 定义两个构建目标、参与编译的文件和需要链接的库。 |
| `build.ps1` | 一键构建脚本；初始化编译环境并调用 CMake，手动构建时可不用。 |
| `third_party/imgui/` | Dear ImGui 菜单组件及 Win32、DirectX 11 渲染后端。 |
| `third_party/minhook/` | 当前 x64 构建所用的函数钩子库。 |

第三方许可证：[ImGui](third_party/imgui/LICENSE.txt)、[MinHook](third_party/minhook/LICENSE.txt)。
