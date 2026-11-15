# power014：113短PC验证与实际释放

2026-11-09人类明确选择113，并随后要求无人值守、各现有会话协调手机、人工环节后移。原厂换线、固定9V/1500mA和恢复段因此保留待测。

固定source `087f37f477eb0a684d80c461b30e9ee27247a307`、USB `829f7a3bfb41e6010212d04a691d4246de4f87e4`；Native42514245970/attempt1/head6e3d0edd及全部14项电脑资格已通过，未重复构建。boot `a68345620b29850b3ac808ac8b53d96c68f7596bc06771f6eaa5fd6df676d058`，payload `099314fd84cec6ca4fcc0ea56c55be6f15f8bd15377ad04b04ec9430684165e3`。

接住音频023实际后验及GPU024最后磁盘payload；新鲜113 Android/root、原payload/Data映射-pin和userdata/GPT几何通过后，一次原27payload范围rawread/write身份核对和临时boot。唯一r00813260，session `52634e15d620bc68`、COM3、generation1。

- provider9真实mode4/backend2；SC8585 ID85、泵停、输入约5V、原USB OVP低位15。42个P1中首个泵未就绪样本错误5单独保留，后续41个泵error0。
- 实际PC源最终type1/SDP，ICLffffffff与FCC0均invalid，PDO全空。没有FCC/PD/OVP控制命令；40个稳定样本owned0、ctl_errors0、confirmed_vbus0。0表示未执行，不能当已恢复5V/OVP。
- 稳定段92.317–179.937秒，电池−1440..17mA、平均−437.8mA、remaining约−11mAh，VBUS4.965–4.984V、电池24.65–27.15°C；这是电脑低预算源下的净放电。没有合格连续180秒PD段，不计算三分钟充电量或90W。
- 一次正常XNU请求 `724633786e9d45ceba20084811f5b938`，绑定session/seq67。queued不是重启证据；实际Android/root已返回，workererror0、S1 owned0/pending_effect0。必要短后验与r008退出通过。
- 自有日志18683B，SHA `d84481fe317626dbd550bea0b9fde67d1807b7a811c14f907b30259dd53539ee`；CRC/58条计数通过：42P1/1U1/1CPREG/13A1/1S1，capped0/drop0。仅只读APFS3,280,896B，前后Data头相同，完整性覆盖保留文件。
- 实际释放2026-11-08 21:11:07 UTC（本机2026-11-09 05:11:07）。Data当时generation1/869段/861757页/prefixc57ab723d356，17头保持，全Data SHA未知；后续音频正常持久写入产生新合法头，不能继续把这个旧头当当前值。

证据入口 `I:/Code/UniXNU-nezha/runs/2026-11-09-power27-gpu015-014-113001/phone-window-released.json`，实际目录 `C:/Users/Hua/NezhaTrials/n27-power014-gpu015/run-113-001`，分析 `out/power014-runtime-analysis.json`。

保留014为固定PD诊断候选及PC拒绝控制/退出证据；普通控制基线仍013，读取基线009。高压功能和主包均未晋升。按真实释放交给蓝牙短只读窗口，再由音频024接续；电源继续电脑工作，不重跑相同PC轮。
