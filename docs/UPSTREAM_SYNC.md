# 上游同步 / Upstream synchronization

`readll` 以 readll 产品固件为基线。`product` 指向
`wegooo-cell/read-pico-reader`，`official` 指向
`MindReset/read_pico_firmware`。

`readll` uses the readll product firmware as its baseline. The
`product` remote points to `wegooo-cell/read-pico-reader`; `official` points to
`MindReset/read_pico_firmware`.

## 规则 / Rules

- 按提交审查官方驱动修复，再按当前产品语义移植。不要整树合并官方 demo。
  / Review official driver fixes commit by commit and port them using current
  product semantics. Do not merge the official demo tree wholesale.
- 不覆盖产品的 SD、传书、阅读、OTA、品牌、硬件时序和发布逻辑。
  / Do not overwrite product SD, transfer, reader, OTA, branding, timing or
  release behavior.
- 不修改 `components/epdiy/**`、生成字体或波形表。
  / Do not modify `components/epdiy/**`, generated fonts or waveform tables.
- 每次同步记录官方提交、受影响文件、采用/拒绝理由和验证结果。
  / Record the official commit, affected files, acceptance/rejection rationale
  and validation for every synchronization.

## 已同步 / Synchronized

| Official commit | Result |
| --- | --- |
| `6a247cc` | `sleep.c` 的睡眠前停止逻辑已存在；补入 `app_sd` 离页停止曲谱 ISR。 / Sleep handling already existed; added score ISR shutdown when leaving `app_sd`. |
