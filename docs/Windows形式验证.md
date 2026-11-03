# Windows形式验证

Windows版EQY可以运行。本工程增加Reprove-Windows.ps1入口，使用Windows版OSS CAD Suite、GNU Make和Git Bash完成归档网表复证，不调用WSL。

## 依赖

- Windows x64及Windows PowerShell 5.1或PowerShell 7。
- Windows版OSS CAD Suite，包含EQY、SBY、Yosys、ABC和自带Python。
- GNU Make，目录中有make.exe或mingw32-make.exe。不能用Visual Studio的nmake替代。
- Git Bash，目录中有bash.exe和sh.exe。已安装Git且git.exe在PATH中时，入口会尝试定位其usr/bin目录；也可显式指定-ShellBin。

不需要另装系统Python。依赖路径由参数传入，脚本不会修改系统PATH或已安装的工具文件。

## 运行

在项目根目录执行，路径换成实际安装位置：

```powershell
# 全部九例
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Reprove-Windows.ps1 -OssRoot 'D:/Tools/oss-cad-suite' -MakeBin 'D:/msys64/mingw64/bin' -Jobs 2

# 指定单例；需要时显式指定Git Bash目录
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Reprove-Windows.ps1 -OssRoot 'D:/Tools/oss-cad-suite' -MakeBin 'D:/msys64/mingw64/bin' -ShellBin 'C:/Program Files/Git/usr/bin' -Case test2 -Jobs 2
```

make.exe已在PATH中时可以省略-MakeBin。如果只有mingw32-make.exe，入口会在本次临时目录中建立同一程序的make.exe副本，不改安装目录。

默认在系统临时目录建立验证工作区。该工作区需要英文路径且不含空格；系统临时目录不符合时，使用-WorkRoot指定短路径，例如D:/eqy_work。项目输入按原相对路径复制到工作区，验证结束后完整记录复制至runs/eqy_windows_时间戳/work/。工作区保留，便于检查失败。

## 验证内容

入口先运行两个小型对照：等价电路应PASS，故意翻转一位输出的电路应FAIL。对照结果正确后，再执行所选用例的原EQY配置。每例包括原RTL至两版ABC9后网表、两版中间至最终网表及最终网表对照，共5项。

原配置及其读取的RTL、网表、器件形式模型均先核对MANIFEST_SHA256.json。PASS还须满足全部输出被覆盖、证明依赖闭合。未完成、超时及工具启动错误均不会记作通过。

旧版Windows EQY会把生成的shell脚本写成CRLF，可能出现run.ys文件名末尾混入回车的问题。入口在独立EQY进程中将工作目录内新写入的文本设为LF，不改EQY安装文件、证明配置或电路逻辑。验证进程及其子进程由独立Windows作业管理，超时仅结束本次验证。

该入口复证的是归档网表，不重新执行两版综合，也不更新面积或时间、内存评分。验证工具中的Yosys与冻结综合程序分别记录，TOOLS.json给出实际版本及哈希，RESULTS.json给出逐项结果和输入哈希。

## 常见错误

| 现象 | 处理 |
|---|---|
| make is not recognized | 用-MakeBin指定GNU Make目录。EQY能生成分区，但仍需make启动证明策略。 |
| 缺少DLL | 保留完整OSS CAD Suite目录，用入口配置bin和lib路径。 |
| 找不到bash或sh | 安装Git Bash，或用-ShellBin指定实际目录。 |
| run.ys后混入回车 | 使用此入口的LF兼容启动方式，不直接复用旧的失败工作区。 |
| 工作区路径含空格或中文 | 用-WorkRoot指定英文短路径；验证输出仍保存在项目runs目录。 |

测试记录见evidence/windows/VALIDATION.json。首次实测使用OSS CAD Suite 20260918、EQY v0.69-1-g7a92d84、SBY v0.69、GNU Make 4.4.1及Git Bash 5.2.37。
