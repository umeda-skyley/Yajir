# Yajir host for Raspberry Pi Pico 2 W

Pico SDK C/C++でYajirをRaspberry Pi Pico 2 Wへ移植するホスト実装です。
通常動作中は、USB CDCシリアルと256 KiBのUSBマスストレージ
`YAJIR`を同時に提供します。

Pico 2 W固有のポート、イベント、定数については
[Pico 2 W組み込みポート仕様](../../../docs/pico2_builtin_ports.md)を参照してください。

## ビルド

### VS Code

VS CodeのRaspberry Pi Pico拡張から、このディレクトリをPicoプロジェクトとして
開きます。ボードには`pico2_w`を指定してください。

### コマンドライン

Pico SDKのパスを指定し、リポジトリのルートからビルドします。

```powershell
cmake -S src/host/pico2_w -B build/pico2_w `
    -DPICO_BOARD=pico2_w `
    -DPICO_SDK_PATH=C:/path/to/pico-sdk
cmake --build build/pico2_w
```

生成された`build/pico2_w/yajir_pico2_w.uf2`を、BOOTSELモードの
Pico 2 Wへ書き込みます。

ファームウェアは4 MiBフラッシュの末尾256 KiBを`YAJIR`ドライブ用に予約します。
この領域を守るため、CMakeのファームウェア領域は3840 KiBに制限されています。

## YAJIRスクリプトドライブ

ファームウェアの起動後、WindowsにはUSBシリアルポートと`YAJIR`ドライブが
表示されます。これはBOOTSEL時に見える`RPI-RP2`ドライブとは別の、通常動作中の
スクリプト用ドライブです。

1. Windowsで`YAJIR`ドライブを開きます。
2. 実行したいスクリプトをルートへ`autorun.yaj`という名前で保存します。
3. Windowsの「取り出し」を実行して書き込みを完了させます。
4. Pico 2 Wを再接続または再起動します。

起動時に`autorun.yaj`が見つかると、自動的にロード、コンパイル、実行します。

```text
[autorun] script received: ... bytes
[autorun] running. USB input is now sent to ON USB_SERIAL.
```

ファイルがない場合、14 KiBを超える場合、またはロードに失敗した場合は、
USBシリアルからのスクリプト投入待ちへ戻ります。Yajirは起動時だけ
`autorun.yaj`を読むため、ドライブ上のファイルを変更した後は再起動が必要です。

## USBシリアルからの投入

`autorun.yaj`がない場合は、USBシリアル端末に次のプロンプトを表示します。

```text
==== Yajir on Raspberry Pi Pico 2 W ====
Version: ...
VM arena: ... bytes
[autorun] autorun.yaj not found.
Paste a script, then enter @run on its own line.
>
```

受信待ち中は、Pico 2 Wが受け取った文字をUSBシリアルへエコーバックします。
実行開始後も、入力文字を表示しながら同じbyteを`ON USB_SERIAL`へpostします。
端末ソフト側のローカルエコーは無効にしてください。

スクリプト本文を貼り付け、その直後に単独行で`@run`を送ります。

```text
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
@run
```

実行開始後のUSB入力は、1 byteずつ`ON USB_SERIAL`へpostされます。
受信したbyteの整数値は`ARG[0]`に入ります。

```yajir
ON USB_SERIAL
    ARG[0] -> STDOUT
END
```

## Ctrl+Cによる中断

実行中に`Ctrl+C`（ASCII `0x03`）を送ると、スクリプトを強制中断して
USBシリアルからの投入待ちへ戻ります。

```text
^C
[loader] script stopped.
Paste a script, then enter @run on its own line.
>
```

中断時はGPIO IRQとPWMを停止し、PWMに使用していたピンをLowへ戻します。
`Ctrl+C`はローダー専用の制御文字であり、`ON USB_SERIAL`にはpostされません。
ブロッキングする`DELAY`の実行中は、処理がUSBポーリングへ戻った時点で
中断されます。通常の`WAIT`中はすぐに反応します。

## 関連資料

- [Pico 2 W組み込みポート仕様](../../../docs/pico2_builtin_ports.md)
- [Pico 2 Wサンプルスクリプト](../../../scripts/pico2_w)
