<p align="center"><img src="assets/banner.png" width="960" alt="マジックビジョン — mAgic VISION — 魔法映像"></p>

mAgicVISION は、mb86 系テレビチューナー向けのドライバーです。  
**Windows の WinUSB ドライバー／BonDriver はまだ未完成品です、PR大歓迎**  

[![ひろめ隊](https://img.youtube.com/vi/FGhd4D_h9xc/hqdefault.jpg)](https://www.youtube.com/watch?v=FGhd4D_h9xc)

## 対象機器

| 機器 | USB ID | 対象方式 |
| --- | --- | --- |
| I社 GV-MVP/XZ〜XZ3 | `04bb:0530` | ISDB-T/ISDB-S |
| S社 torne CECH-ZD1 J | `054c:04b2` | ISDB-T |
| P社 PIX-DT295| `04bb:0530` | ISDB-T/ISDB-S |


* それぞれ１チューナーになります


## 現在未実装の機能
- MULTI2復号
- H.264出力
- Windows WinUSB対応
- BonDriver対応

## インストール（Debian / Ubuntu）
リリースで
```sh
sudo apt install linux-headers-$(uname -r)
sudo apt install ./magicvision-firmware_*_all.deb ./magicvision-usb-dkms_*_all.deb
```

## Linux DVB で使う

```sh
sudo modprobe dummy_hcd num=2
sudo modprobe mb86H_ccid
sudo modprobe mb86H_usb
```

機器を接続すると `/dev/dvb/xz0/frontend0`、 `/dev/dvb/torne0/frontend0` または `/dev/dvb/dt2950/frontend0` が作成されます。
同じ機種が複数ある場合は番号が増えます。CCID 用の仮想 USB コントローラーはリーダー数に合わせて用意してください。

録画には標準 Linux DVB に対応した recisdb などを別途使用します。例では地上波 24ch を 10 秒録画します。

```sh
recisdb tune --device /dev/dvb/xz0/frontend0 --channel 24 --time 10 --no-decode --no-strip sample.ts
```

受信地域に合わせてチャンネルを変更してください。`--no-decode` の出力は復号していない TS です。

## Mirakurun

torne は電源投入後の初回校正に **5 秒以上**かかることがあります。
Mirakurun の初回 TS 待ちが 5 秒に固定されている版では、[付属パッチ](integrations/mirakurun/README.md)で
**初回 TS 待ちを 15 秒、HTTP idle timeout を 30 秒**に変更してください。
既存の `tuners.yml` に timeout を追加するだけでは変更できません。

`tuners.yml` の例です。recisdb を Mirakurun の実行ユーザーから使えるようにしてください。

```yaml
- name: mAgicVISION torne
  types: [GR]
  command: recisdb tune --device /dev/dvb/torne0/frontend0 --channel <channel> --no-decode --no-strip -
  checkDevicePath: /dev/dvb/torne0/frontend0

- name: mAgicVISION XZ
  types: [GR, BS, CS]
  command: recisdb tune --device /dev/dvb/xz0/frontend0 --channel <channel> -
  checkDevicePath: /dev/dvb/xz0/frontend0

- name: mAgicVISION DT295
  types: [GR, BS, CS]
  command: recisdb tune --device /dev/dvb/dt2950/frontend0 --channel <channel> -
  checkDevicePath: /dev/dvb/dt2950/frontend0

```
## ユーザーランド / PC/SC

libusb の確認用ツールは `build/usbctl`、起動・ファームウェア検証ツールは `build/usbboot` です。  
Linux のカーネルドライバーとユーザーランドから同じ物理 USB 機器を同時に開かないでください。

```sh
./build/usbctl devices
./build/usbboot check-image 04bb 0530 firmware/firmware.bin
```

PC/SC IFD のビルドと外部ファームウェアの配置方法は [PC/SC の説明](userland/pcsc/README.ja.md)を参照してください。

## ライセンス
[**GPL-2.0**](LICENSE)


Made with ❤️  
ひろめ隊

