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
    - 具体问题场景为:
    
      DFTL 先加载映射表产生一次闪存读，再读数据产生二段读，理论上这样应该是串行操作。但在FEMU中这两次读请求如果在不同的LUN上，比如数据页在LUN1上，映射表页在LUN2上，这样的话就变成了读数据页和读映射表页就变成了并行操作。
    - 可能的解决方案:
  
      为了体现并行的影响，阻塞住数据页所在的LUN1，等读映射表页LUN2处理完后，再释放LUN1来读取数据页。但这样缺点的话数据页LUN1在LUN2处理完之前不能服务与其他请求，造成性能下降
    - 实现的解决方案:
     
      一段读之后将直接释放LUN1，将请求暂存进tmp_sq，等LUN2处理完后再把让请求丢进FTL线程中使其被二次被调度
