# GPU015独立电源候选 power001

这是001首包的集成档案。下文schema=0及尚未原生/实测的状态仅属于当时；当前OS4资格、009读取基线、013普通控制和014诊断进展见[README](README.md)及各轮报告。GPU015资源边界继续适用。

人类2026-11-07明确指定GPU015为测试基底，源码/构建停点已解除。原包与原源树只读保护。
本候选基于device154f8a08、Native033/run42363354270、boot0ec953c6468d、payload6c36daff7420。

新增电源归档及late caller。沿GPU015现有PE_builtin_boot_args机制在内核配置中启用nezha-power=1，
loader参数仍994B，原input-test=0及全部有效GPU/CPU/USB/Data选项保留。

实际绑定：UFS先于IOBSD完成，touch完成apps-SMMU路由后电源线程才规划0xb6独立精确SMR。
每次DMA授权核实际分配范围、地址宽度、UFS源路由、CBAR/CBA2R/SCTLR；不更改context bank、UFS、USB或touch路由。
GPII从原厂允许0..4中取空闲一项并驻留保留；GCC只增减本方必要位，保留进入前的投票及其它实时位。

**边界：GPU015没有统一ICC控制器。首个遥测候选沿同机17读取器使用原厂继承互联状态，不新增RPMh/ICC投票。**
日志明确标记ICC inherited；这不是已完成主动互联电源管理，也不能据此资格化深睡/快充。
SMEM软件锁是本包唯一AP↔SOCCP客户端的锁；GPU015未编入音频/Wi-Fi远端客户端。
未来联合包有其它SMEM分配者时必须改为同一共享锁/dispatcher，不直接复用这个局部独占绑定。

SOCCP仅attachment。当前OS4原厂模块尚未读到，schema_verified保持0：gauge发布真实电量，
未资格化的充电状态不发布。需要在获授权只读手机窗口核实际模块后再绑定MCA字段。
SC8585/HL7606/认证/充电控制不随读取模式启动。首包不声称正常充电或90W已实现。

构建前两组核心/协议C测试已以GCC、ASan/UBSan通过。初始Clang因本地sanitizer库缺失未运行；
GCC初次发现头文件/定义的array-parameter不一致，已经修正后重编通过。
原生arm64e和真实类/符号资格留给本次构建；未用宿主通过替代原生或实机验证。
