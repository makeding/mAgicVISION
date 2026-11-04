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

## 対応リクエストについて
対応機種をさらに増やしていきたいと考えておりますが、新規機器の検証にはどうしても実機が必要となり、現時点ではリモートでの対応がかないません。  
もし対応をご希望いただけるようでしたら、お手数ですが対象機器をこちらまでお送りいただけないでしょうか。


## 現在未実装の機能
- MULTI2復号
- H.264出力
- Windows WinUSB対応
- BonDriver対応

## インストール（Debian / Ubuntu）
[GitHub Releases](https://github.com/makeding/mAgicVISION/releases) から `firmware` と `dkms` の2つの `.deb` をダウンロードします。
```sh
sudo apt install linux-headers-$(uname -r)
sudo apt install ./magicvision-firmware_*_all.deb ./magicvision-usb-dkms_*_all.deb
```

## インストール（NixOS）

NixOS では [jodtv-packages](https://github.com/makeding/jodtv-packages) の flake モジュールからインストールできます。

```nix
inputs.jodtv-packages = {
  url = "github:makeding/jodtv-packages";
  inputs.nixpkgs.follows = "nixpkgs";
};

imports = [
  inputs.jodtv-packages.nixosModules.packages
  inputs.jodtv-packages.nixosModules.dtv
];
```

## Linux DVB で使う

```sh
sudo modprobe dummy_hcd num=2
sudo modprobe mb86H_ccid
sudo modprobe mb86H_usb
```

機器を接続すると `/dev/dvb/xz0/frontend0`、 `/dev/dvb/torne0/frontend0` または `/dev/dvb/dt2950/frontend0` が作成されます。
同じ機種が複数ある場合は番号が増えます。CCID 用の仮想 USB コントローラーはリーダー数に合わせて用意してください。

録画には標準 Linux DVB に対応した recisdb などを別途使用します。ただし `/dev/dvb/xz0` のようなデバイス名を使うには [makeding/recisdb-rs](https://github.com/makeding/recisdb-rs) のフォークが必要です。  
本家では `/dev/dvb/adapter0/frontend0`、`/dev/dvb/adapter1/frontend0` 形式のみ利用できます。  

例では地上波 24ch を 10 秒録画します。

```sh
recisdb tune --device /dev/dvb/xz0/frontend0 --channel T24 --time 10  sample.ts
```

## Mirakurun

`tuners.yml` の例です。recisdb を Mirakurun の実行ユーザーから使えるようにしてください。

```yaml
- name: mAgicVISION torne
  types: [GR]
  command: recisdb tune --device /dev/dvb/torne0/frontend0 --channel <channel> -
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
## 実機ベンチマーク

### 3機種の短時間録画

2026-11-04 15:23 JST、Linux DVB 経由で各機種を個別に8秒録画しました（`recisdb tune --no-decode --no-strip --time 8`）。torne は午後の測定値です。

| 機種 | チャンネル | TS データ量 | sync / TEI / CC エラー |
| --- | --- | ---: | ---: |
| GV-MVP/XZ | GR24 | 17.26 MB | 0 / 0 / 0 |
| torne | GR24 | 17.25 MB | 0 / 0 / 0 |
| PIX-DT295 | BS15_0 | 23.90 MB | 0 / 0 / 0 |

3件とも録画コマンドは終了コード 0、PAT CRC は有効、末尾の不完全 TS データはありません。  
各8秒の個別測定であり、同時録画や長時間安定性の測定ではありません。

### DT295 のチャンネル切り替え

同一 USB セッションで各チャンネルを5秒ずつ1回測定しました（標準: 2026-11-04 05:44 JST、高速: 06:03 JST）。時刻は `TUNE` 呼び出しからホスト受信までです。

| チャンネル | 手順 | 最初の完全な TS | CRC 有効な対象 PAT | TUNE 戻り |
| --- | --- | ---: | ---: | ---: |
| GR24 | 標準 | 501 ms | 1,084 ms | 1,370 ms |
| GR24 | 高速 | 271 ms | 699 ms | 1,110 ms |
| BS15_0 | 標準 | 789 ms | 848 ms | 822 ms |
| BS15_0 | 高速 | 298 ms | 449 ms | 432 ms |
| GR22 | 標準 | 642 ms | 1,916 ms | 3,155 ms |
| GR22 | 高速 | 279 ms | 1,685 ms | 2,642 ms |


## ユーザーランド / PC/SC

libusb の確認用ツールは `build/usbctl`、起動・ファームウェア検証ツールは `build/usbboot` です。  
Linux のカーネルドライバーとユーザーランドから同じ物理 USB 機器を同時に開かないでください。

```sh
./build/usbctl devices
./build/usbboot check-image 04bb 0530 firmware/firmware.bin
```

PC/SC IFD のビルドと外部ファームウェアの配置方法は [PC/SC の説明](userland/pcsc/README.ja.md)を参照してください。

## ライセンス

- Linux カーネル用コード（`linux/`、`usb/linux/`）は [GPL-2.0-only](LICENSE) です。
- userland とその再利用可能な共有コード（`core/`、`device/`、`include/mb86H/`、`usb/boot.c`、`usb/window.c`、`usb/host_contract.h`、`usb/userland/`、`userland/`）は [MIT](LICENSE-MIT) です。対象ソースには `SPDX-License-Identifier: MIT` を記載しています。
- Linux カーネルモジュールに共有コードを組み込んで配布する場合、モジュール全体は GPL-2.0-only の条件で配布します。
- `firmware.bin` などのファームウェア成果物は再配布できます。


Made with ❤️  
ひろめ隊

