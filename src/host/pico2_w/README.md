# Yajir host for Raspberry Pi Pico 2 W

Pico SDK C/C++ でYajir v0.4.5をPico 2 Wへ載せるホストです。第一段階として、USB CDC serialから
スクリプトを投入し、オンボードLEDを制御できます。Wi-FiとHTTPはまだ含みません。

## ビルド

VS CodeのRaspberry Pi Pico拡張からこのディレクトリをPicoプロジェクトとして開くか、Pico SDKの
パスを指定してコマンドラインからビルドします。

```powershell
cmake -S src/host/pico2_w -B build/pico2_w -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=C:/path/to/pico-sdk
cmake --build build/pico2_w
```

生成された `yajir_pico2_w.uf2` をPico 2 Wへ書き込みます。USB serialはSDKのCDC stdioを使い、
実UART stdioは無効です。

## スクリプト投入

USB serial端末を開き、スクリプト本文を貼り付けた後、単独行で `@run` を送ります。

```text
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
@run
```

実行開始後のUSB入力は、1バイトずつ `ON USB_SERIAL` へpostされ、文字コードが `ARG[0]` に
入ります。再投入は現段階ではボードをリセットして行います。

## 第一段階のポート

| ポート | 内容 |
|---|---|
| `LED1` | Pico 2 WオンボードLED。読み書き可能 |
| `STDOUT` | USB serialへUTF-8文字列または整数を出力 |
| `NOW` | 起動後のミリ秒 |
| `DELAY` | ブロッキング遅延 |
| `VMSIZE` | Pico設定でのVM arenaサイズ |
| `USB_SERIAL` | USB入力のイベント源。`ARG[0]`に1バイト |
