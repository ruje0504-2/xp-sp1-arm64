# XP SP1 ARM64

此分支保存原生 ARM64 移植中经审查的源文件，包括启动设备路径、页表、低地址用户空间、上下文与 QEMU 检查代码。源快照来自本地主线 `a478fe4eae68a996bc75001e6ed4b9f7d78379bc`。独立构建仍需另行取得适当授权的 NT 源码、头文件和构建环境。

ARM64 展开器基于 .NET Runtime v8.0.0。固定上游版本、文件哈希和 MIT 许可见 [来源记录](third_party/dotnet-unwind/source.json) 和 [许可证](third_party/dotnet-unwind/LICENSE.TXT)。
