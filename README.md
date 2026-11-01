# PMUX多输出共享与查找表优化分解

本项目面向FPGA逻辑综合中的多路选择网络，通过多输出共享、互斥运算合并和查找表优化分解，减少重复的选择与计算逻辑。优化以内建命令接入Yosys，在ABC9映射前完成结构变换。

## 运行九例综合

Windows x64环境下，在项目根目录双击START.cmd。命令行可运行START.cmd --no-pause。普通综合支持Windows PowerShell 5.1或PowerShell 7，无需Python、编译器或WSL。保留runtime内的ABC程序、DLL及share器件库。

两版使用相同RTL、顶层、参数及外部脚本：读取RTL → hierarchy → proc → setundef -init -params -zero → synth_intel -family cycloneiv → check → 输出网表。每次运行在runs下新建目录，并核对资源与已证明的参考网表一致。

```powershell
# 指定单例
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Run9.ps1 -Case test4
# 核对项目文件哈希
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Verify.ps1 -Full
```

## 工程内容与评测结果

- docs/current/九例原始截图/：九例综合结果及形式验证、效率记录的11张真实控制台截图，附原始日志和来源清单。
- RESULTS.html、REPORT.md、PER_CASE.csv、ALL_PAIRS.csv：结果页面、评测说明和逐例逐次数据。
- source/yosys/、source/overlay_files/：完整源码及修改文件副本。
- runtime/：原版与本版Yosys、同哈希ABC、运行依赖及器件库。
- evidence/current/、runs/：参考网表、45项形式证明以及十组完整九例运行记录。
- BUILD.json、BUILD_CONDITIONS.json、CHANGE_SCOPE.json：源码、程序身份、同条件构建配置和修改范围。

本地九例最终查找表与独立取反单元由2402减少至1712，逐例算术平均优化率为31.764225%；总量加权缩减率28.726062%为补充指标。总单元数2725→2020，触发器323→308。45项形式等价验证全部通过。

十组首次时间88/90通过，短用例追加复测后时间90/90、内存90/90通过；100个原始测量配对的内存均通过。最坏判定完整耗时增幅3.301738%，主进程峰值物理内存增幅3.158181%。测试时杀毒软件已关闭。

## 重新测量

测量入口要求PowerShell 7。先关闭杀毒软件并记录环境，然后执行：

```powershell
pwsh -NoProfile -File scripts/Run9.ps1 -Mode Measure -Groups 10
```

入口在会话开始时对每例原版预热10次，再按原版→本版完成各组测量。首次耗时超过5%且两版均不超过10秒时，追加5对，分别取两版追加耗时的均值再计算增幅。保留首次记录；内存按原始配对逐对检查。完整时间覆盖程序启动、读取RTL、逻辑综合、ABC、网表输出至退出；EQY另行运行。

## 形式等价验证

验证入口使用Linux/WSL，预装OSS CAD Suite并指定安装路径。

```bash
python3 scripts/Reprove.py --oss-root /path/to/oss-cad-suite --jobs 2
python3 scripts/Reprove.py --oss-root /path/to/oss-cad-suite --case test2 --jobs 2
```

九例共45项：两版RTL至ABC9后中间网表18项、两版中间至最终网表18项、两版最终网表对照9项。新输出另存runs目录。

## 重新构建

在MSVC x64、CMake、Ninja环境中运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Build-Windows.ps1 -Jobs 8
```

双方使用MSVC19.51、相同Release参数和相同ABC。核心源码提交为9f75ca1f9834a39a863915b5dae0c7b1e33533bc，构建参数见BUILD_CONDITIONS.json。构建输出位于rebuild目录。

## 第三方组件

Yosys、ABC及其他第三方组件的原许可证保留在源码和licenses目录。
