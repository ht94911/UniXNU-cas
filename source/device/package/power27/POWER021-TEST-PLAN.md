# power021：按原厂顺序启动认证与独立电源角色判断

020/source c5112c3/Native42612560631的15组宿主与14项电脑资格已通过，固定
boot94bdead61d99/payload10ebe54860ac。保留这个已完成电脑结果，不重复构建；
本轮新021承接020的负载过渡反馈修正后再验证原厂认证，避免连续换线测试两个
已知仍缺少原厂握手步骤的包。

同机MDY-18-EW100W USB-A/原厂线的安卓对照300样本、362289B日志自然结束，
同boot_id、Android/root、文件哈希及自有进程退出/临时文件清理通过，12:40UTC
已释放。原厂PD段201.10s，SOC90→92、温度29.4–30.9°C、VBUS最高8.929V。
连接后约1.28s SVID2717/Adapter ID dd62/data-role DFP出现；随后verified1、
verify_process0，电源仍sink。连续180s实计电池净78.391962mAh，主BUCK输入
电流不是整口功率，不能作90W峰值验收。电芯接口返回atl、设备名BM6N；CN
区域文字已读，原厂country枚举仍待明确，不盲目选全局温控表。

当前OS4实际batterysecret先写verify_process1（主循环7560..7568/723c），
检查SNK_Ready（6b24/6bcc），通过data_role接口请求并读回dfp（6750..6b20），
才执行SVID/VDM（6ccc/5e1c）和最终verified/结束过程。实际PD模块证明
READ2100e四字节中2=SNK_Ready，READ/WRITE2100d四字节是独立数据角色，
1=UFP、2=DFP；DR请求200只映射此字段，不是OTG或电源角色切换。

021在同一已有worker完成新鲜PDO及sink-ready状态后，先检查现有验证过程为空、
取得本方过程所有权，再按原厂请求DFP并真实读回，随后清verified、读SVID/ID
并执行原HMAC。角色交换只请求一次，ACK不当角色已完成，等待上限3000ms；
SVID原3000ms期限从协议初始化完成开始，外厂商非零仍拒绝，没有SVID旁路或
认证失败后的高压放行。保留主电池独立认证、原字节序和nonce不重放，以及原
过程/verified清理。DFP是当前PD连接的数据角色，物理换回PC须独立验证UFP
恢复和新USB会话，不能靠ACK或模型宣称USB恢复。

固定PD以实际2100e确认sink，再独立接收UFP或DFP数据角色；原来把DFP直接
当错误电源角色会拒绝已认证的原厂连接。新增元数据沿现有provider/log记录，
不增加USB/trace框架。保留泵关闭、OVP、温度/电量/电压/能力和真实反馈条件，
仍只请求9V/FCC1500mA及按序恢复5V。源变为source/状态失效立即撤销增压资格。

宿主重放初始化前SVID不可用、角色交换无响应/短包/源切换/nonce超时、DFP sink
与真正source区别、旧32实际Runtime和缓存/日志回归；Native保留全部原14项。
候选与本机入口均固定通过后才文件排队，接受最新实际Data/payload写者；本机
一次临时启动，原厂换线后取完整认证/角色/固定PD及连续180s日志。同次正常XNU
一次返回、必要Android/root后验、自有采集退出和CRC后释放，原安卓/root/17/
只读27根/generation1 Data/分区禁刷/固定复位0保护保持。未过实机不晋升主包。
