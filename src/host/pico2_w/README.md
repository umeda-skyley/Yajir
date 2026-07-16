# Yajir host for Raspberry Pi Pico 2 W

Pico SDK C/C++でYajirをRaspberry Pi Pico 2 Wへ載せるホスト実装です。

USB CDC serialからスクリプトを投入し、その場でコンパイルして実行できます。Pico 2 W固有のポート、イベント、定数については[組込みポート仕様](../../../docs/pico2_builtin_ports.md)を参照してください。

## ビルド

### VS Code

VS CodeのRaspberry Pi Pico拡張から、このディレクトリをPicoプロジェクトとして開きます。ボードには`pico2_w`を指定してください。

### コマンドライン

Pico SDKのパスを指定して、リポジトリのルートからビルドします。

```powershell
cmake -S src/host/pico2_w -B build/pico2_w `
    -DPICO_BOARD=pico2_w `
    -DPICO_SDK_PATH=C:/path/to/pico-sdk
cmake --build build/pico2_w
```

生成された`build/pico2_w/yajir_pico2_w.uf2`をPico 2 Wへ書き込みます。

USB serialにはPico SDKのUSB CDC stdioを使用します。実UARTによるstdio出力は無効です。

## スクリプトの投入

UF2の起動後、Pico 2 WのUSB serialポートを端末で開くと、次のプロンプトが表示されます。

```text
==== Yajir on Raspberry Pi Pico 2 W ====
Version: ...
VM arena: ... bytes
Paste a script, then enter @run on its own line.
>
```

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

ロードに成功すると、次のメッセージを表示して実行を開始します。

```text
[loader] script received: ... bytes
[loader] running. USB input is now sent to ON USB_SERIAL.
```

実行開始後のUSB入力は、1 byteずつ`ON USB_SERIAL`へpostされます。受信したbyteの整数値は`ARG[0]`に入ります。

```yajir
ON USB_SERIAL
    ARG[0] -> STDOUT
END
```

## Ctrl+Cによる中断

実行中に`Ctrl+C`（ASCII `0x03`）を送ると、スクリプトを強制中断して、新しいスクリプトの受信待ちへ戻ります。

```text
^C
[loader] script stopped.
Paste a script, then enter @run on its own line.
>
```

これにより、USBケーブルを抜いたりボードをリセットしたりせず、スクリプトの投入、実行、中断、再投入を繰り返せます。

中断時にはGPIO IRQとPWMを停止します。PWMに使用していたピンはGPIO出力へ戻し、Lowを設定します。

スクリプト受信途中やロードエラー後に`Ctrl+C`を送った場合は、受信済みの入力を破棄して先頭からやり直します。

`Ctrl+C`はローダ専用の制御文字であり、`ON USB_SERIAL`にはpostされません。ホストをブロッキングする`DELAY`の実行中は、処理が完了してUSB入力のポーリングへ戻った時点で中断されます。通常の`WAIT`中はすぐに反応します。

## 関連資料

- [Pico 2 W組込みポート仕様](../../../docs/pico2_builtin_ports.md)
- [Pico 2 Wサンプルスクリプト](../../../scripts/pico2_w)
- [Pico 2 W移植計画](../../../docs/pico2_w_plan.md)
