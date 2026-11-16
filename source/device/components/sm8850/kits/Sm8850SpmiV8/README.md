# SM8850 SPMI v8

源码阶段的公共事务层，尚未编译或实机验证。资源来自 nezha N0：core
0x0c400000、chnl_map 0x0c403000、observer 0x0c4c0000、writer
0x0c900000；bus0/bus1 owner 分别为 0x0c42a000/0x0c43b000。

global APID 用于 observer/writer；owner 使用 global APID 减去本总线起点。
该规则对应 Qualcomm `spmi-pmic-arb.c` 的 `pmic_arb_apid_owner_v8` 与
`pmic_arb_offset_v8`。MMIO 由调用者映射，事务复用 keys/ADC 的
`nezha_spmi_bus_acquire/release`。本层不授予任何寄存器写入范围，外设驱动
在其自己的入口限制；写入前再次核对实际 PPID 与 EE owner。超时不重发。

来源：OnePlusOSS/android_kernel_oneplus_sm8850
`fc30e54174d254ff7f33622a9278e4435f6718d2`，
`drivers/spmi/spmi-pmic-arb.c`；本地已验事务的参考为 nezha power
`04e0a9b18935d52b21b02f34563f5d98eb3a5511` 的 keys/ADC 总线锁。
本次是依据协议与既有 BSD 接口实现的独立 C 代码，没有复制 Linux 函数体。

测试包指定后，将这两个源文件放入目标源码树，并由 HAP530 组件的正式
archive recipe 编译。当前未改变 soc.toml、设备 feed 或活动构建。
