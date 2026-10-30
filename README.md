# OpenHarmony Linux 5.10 的 EEVDF 移植与 QoS 调度验证

2026 开源鸿蒙大学生创新大赛 · 系统与技术创新赛道参赛项目。

参赛仓库：[GitCode](https://gitcode.com/Ye_Mingyun/ohos-eevdf-qos)；源码镜像：[GitHub](https://github.com/Shadow-ye/ohos-eevdf-qos)。已于平台显示时间 2026-10-30 13:50:18 完成作品提交，[提交回执](docs/competition/evidence/submission-receipt.png)已保存，尚无测评记录。

本项目将 Linux 6.6 的 EEVDF 公平调度机制移植至 OpenHarmony 5.0.1 的 Linux 5.10.208，并将已有任务 QoS 等级映射为调度请求 slice。交付内容包括内核源码快照、可应用补丁、开关对照测试、不变量检查和 CFS 对比实验。

**当前成果是可运行、可复核的系统研究原型。** QEMU 上已验证 QoS 改变调度粒度且保持近似公平份额；在周期唤醒与忙等竞争场景中，当前 EEVDF 方案的尾延迟高于 CFS。项目如实保留这一限制，不宣称已改善实机交互性能。

## 评审入口

- [开发设计文档](docs/competition/开发设计文档.md)，[可编辑 Word 版](docs/competition/materials/开发设计文档.docx)。
- [答辩 PPT](docs/competition/materials/答辩演示.pptx) 与 [演示讲稿](docs/competition/演示讲稿.md)。
- [复现指南](docs/competition/复现指南.md) 和 [实验结果](docs/competition/实验结果.md)。
- [本次原始串口日志与元数据](docs/competition/evidence/)。
- [提交状态及官方模板链接](docs/competition/提交说明.md)。

## 实现范围

`repo/` 按 OpenHarmony 源码根目录保留 14 个文件。该目录是覆盖快照，不是完整操作系统。主要改动位于 `kernel/linux/linux-5.10/kernel/sched/`，包含 EEVDF 的 eligibility、虚拟 deadline、增强红黑树、lag 放置与 reweight/fork 适配。`common_modules/qos_auth` 含 QoS-only 配置的编译修复。

请求长度映射：QoS 0 至 6 分别为 14、10、4、2、1.5、1、4 ms。无有效 QoS 的任务使用默认粒度。`/proc/sys/kernel/sched_qos_slice` 允许关闭映射；关闭后仍为 EEVDF，恢复 CFS 需要启动独立 CFS 内核。

测试平台为 QEMU ARM64 TCG，4 vCPU，Cortex-A57，1 GiB，HZ=250。测试没有覆盖实机、应用帧率、SMP 迁移、长期稳定性或生产部署。

## 最小复现

需要另行准备 OpenHarmony 5.0.1 源码、官方预编译工具链和 QEMU 最小系统镜像。仓库不包含完整源码和磁盘镜像。

```bash
# 在已有镜像目录中同时提供 Image-eevdf 和 Image-cfs
python3 scripts/run_bench.py --images /path/to/images --mode qos --seconds 100 --output /tmp/qos-check
python3 scripts/run_bench.py --images /path/to/images --kernel cfs --mode compare --seconds 65 --output /tmp/cfs-check
python3 scripts/run_bench.py --images /path/to/images --kernel eevdf --mode compare --seconds 65 --output /tmp/eevdf-check
```

脚本使用 QEMU `-snapshot`，记录命令、镜像及日志 SHA-256、完成标志和故障行，并核对输入镜像未发生改变。退出前由宿主超时终止 QEMU 是预期行为。慢速宿主机应增加 `--seconds`，未得到完整结果时脚本失败。

## 来源与许可

本项目基于 OpenHarmony 与 Linux 社区代码，EEVDF 算法来自上游 Linux，项目贡献在于跨版本移植、OpenHarmony QoS 适配和验证工程。上游源码保留原版权和许可证声明，Linux 部分遵循 GPL-2.0 及各文件标注的例外；其他文件遵循其原有许可证。见 [来源与许可](NOTICE.md)。

`内核分析/` 保留开发历史；早期计划与最终实验存在差异，参赛结论以 `docs/competition/` 和源码为准。
