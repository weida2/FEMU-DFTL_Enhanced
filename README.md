# FEMU-Enhanced 加强版本

由于[原版FEMU](https://github.com/vtess/FEMU)中存在一些不足和Bug，因此实验室将自行维护一个版本，同时定期和上游保持同步。

对于FEMU的环境配置和使用，请参考[FEMU环境配置/镜像制作文档](./README-Setup.md)，后续也计划更新更为完善的中文文档。

项目包含两个分支：
- enhanced: 加强版本的FEMU，添加FEMU不支持的功能，并修复了其中的Bug
- base: 基础版本的FEMU，用于和上游保持同步和作为参考

# 修改日志
1. 为启动脚本添加NUMA支持，环境配置脚本增加`numactl`安装命令。由于实验室服务器大多采用NUMA架构，直接运行可能会导致CPU/内存的跨节点访问，造成严重的性能损失，因此修改了FEMU启动脚本，通过`numactl`默认绑定到0号节点运行（非NUMA架构下，即NUMA#0），实际可以更改其他的节点号。[Issues #1](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/1)
2. 令NVMe SQ/CQ默认开启多线程处理。默认配置`multipoller_enabled=1`，开启多个`to_ftl`和`to_poller`线程加快IO的处理速度。[Issues #2](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/2)
3. 为FEMU添加NVMe SGL支持。FEMU在实际处理IO时通过`memcpy()`来将所需要的数据复制到NVMe所提供的内存地址中，FEMU原版只支持PRP（需要和内核物理页4K对齐），[实验结果证明](https://github.com/vtess/FEMU/pull/129#issuecomment-1815153431)执行一次1024K的`memcpy()`或者多次更大的IO比执行256次4K `memcpy()`更高效，因此将QEMU中NVMe SGL的逻辑移植到了FEMU中。[Issues #3](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/3)
4. FEMU ZNS FTL基础版本。FEMU最新版添加的ZNS FTL功能存在诸多Bug，延迟/性能模拟不准确，相关Bug可见[[FEMU Issues #131]](https://github.com/vtess/FEMU/issues/131)，修复后添加了NAND锁保证多线程FTL的正确性，支持了通过启动脚本配置擦出延迟以及Zone Size，更正了FEMU中的错误代码。[Issues #4](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/4)
5. ZNS模式下除去`zns_read`和`zns_write`外还有其他Nvme Command，需要添加对这些Nvme Command的SGL支持，否则在某些情况下会出错（例如挂载F2FS），通过在`dma.c`添加`dma_read/write_sgl`进行修复。[Issues #5](https://github.com/NNSS-HASCODE/FEMU-Enhanced/issues/5)