两个 ZMR 文件的完整逆向分析结果
容器格式（已确认，两个文件一致）
字段	偏移	大小	说明
Magic	0x00	4B	zmr0（唯一版本，无 zmr1/zmr2）
Block count	0x04	4B	u32 LE，数据块数量
Header size	0x08	4B	u32 LE = 12 + count×4，同时是数据区起始偏移
Offset table	0x0C	count×4B	u32 LE 数组，单调递增的绝对偏移，每项标记一个块的结束位置
Data blocks	hdr_size	变长	每块由相邻偏移表项界定；首字节决定压缩方式
Footer	EOF−4	4B	u32 LE = 文件总大小（完整性校验）