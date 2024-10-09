# FEMU-DFTL_Enhanced
## 解决FEMU关于DFTL中的经典双读问题的延迟模拟，使其能够在理论上正确且最优地模拟读延迟
Resolve the classic double-read dependency issue (multiple-read problem) in FEMU regarding DFTL (A type of FTL that stores the mapping table in flash memory), enabling it to theoretically and optimally simulate the latency corresponding to the multiple-read problem.


由于[原版FEMU](https://github.com/vtess/FEMU)中存在一些不足和Bug，因此实验室将自行维护一个版本，同时定期和上游保持同步。

对于FEMU的环境配置和使用，请参考[FEMU环境配置/镜像制作文档](./README-Setup.md)，后续也计划更新更为完善的中文文档。

项目包含两个分支：
- enhanced: 加强版本的FEMU-DFTL版本，能够在理论上正确且最优地模拟读延迟
- base: 基础版本的FEMU，用于和上游保持同步和作为参考

# 修改日志
1. 解决FEMU-FTL线程中延迟模拟的读依赖问题。[Issues #9](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/9)
