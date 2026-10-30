# 来源与许可

本仓库收录 OpenHarmony Linux 5.10.208 的修改快照，目标基线提交为 `8d8762e824a04bd99b37e5cd635919ea926c8357`，上游仓库 https://gitcode.com/openharmony/kernel_linux_5.10 。移植时参考工作区 OpenHarmony Linux 6.6 的 EEVDF 实现。

EEVDF 及 Linux 调度器基础代码属于上游作者，项目不主张原创算法所有权。文件中的 SPDX、作者和版权声明均保留。Linux 源码的总体许可和解释见 `LICENSES/COPYING`、`LICENSES/GPL-2.0` 和 `LICENSES/Linux-syscall-note`，具体许可以各文件为准。

`repo/vendor/ohemu/` 中脚本与配置，以及 `repo/kernel/linux/common_modules/qos_auth/` 中代码按原文件声明适用 Apache-2.0 或文件自身标明的许可证，见 `LICENSES/Apache-2.0`。未用单一新许可证覆盖上游代码。新增复现脚本采用 Apache-2.0；竞赛文档记录项目实现和实验，不改变引用代码的许可。

内核补丁仅覆盖 linux-5.10 子项目。QoS 公共模块、配置和 vendor 脚本仍须按目录覆盖；详见复现指南。原始日志可能包含本机构建用户名、仿真设备日志和构建时间，不包含登录凭据。
