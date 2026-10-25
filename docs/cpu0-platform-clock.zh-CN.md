# CPU0 平台与时基观测005

004 已以 raw55 证明 VM 初始化与早期 PE 返回。本候选记录实际 SecureDT/PE 与
CNTFRQ/CNTVCT/MPIDR 字段，观察 cpu_timebase_init 和 PE_init_cpu 前后，最后在
machine_startup 之前以66停止。IRQ/FIQ仍须mask，早期终止异常向量保留。
不声称 IOKit、真实timer中断、SMP、存储或用户态通过。新版专用早期reader读取512B，
仅在双读稳定、boot/tag/XOR/反码正确后接受384偏移处的平台packet；expected tag是
构建参数，避免与源提交身份循环依赖。独立Validate不再要求，每次Boot仍保留实时
事实与完整held内存读回门禁。新镜像独立验收，不挪用004运行结果。
