# Yajir Language Support for VS Code

Yajirスクリプト（`.yaj`）用のVisual Studio Code拡張です。

## 機能

- Yajirの予約語、宣言、ハンドラ、ポート、スロット、文字列、数値、コメントの色分け
- ブロックの自動インデントと折りたたみ
- `INIT`、`MAIN`、`ON`、`PORT`などのスニペット
- PC拡張版Yajirローダーによるコンパイルチェック
- ロードエラーをVS Codeの「問題」とエディタ上の波線へ表示
- 統合ターミナルでPC拡張版Yajirを実行

## 操作

`.yaj`ファイルを開くと、エディタ右上に次のアイコンが表示されます。

| アイコン | コマンド | 内容 |
|---|---|---|
| チェック | `Yajir: Check Current Script` | PCローダーでコンパイルだけを実行 |
| 再生 | `Yajir: Run Current Script` | 統合ターミナルでスクリプトを実行 |

Yajirファイルの編集中は`Ctrl+Shift+B`でもコンパイルチェックできます。

## インストール

VSIXファイルを使う場合は、VS Codeの拡張機能画面右上にある`...`メニューから
「VSIXからのインストール」を選び、`yajir-language-0.1.2.vsix`を指定します。

コマンドラインからもインストールできます。

```text
code --install-extension yajir-language-0.1.2.vsix
```

## PCローダー

設定が空の場合、ワークスペースの`build/`から最新の`script_extension*.exe`を探します。
標準PCホストは`check`モードを持たないため、自動検出の対象にはしません。

別の場所にあるローダーを使う場合は、設定`yajir.loaderPath`へ指定します。
VS Codeの設定画面へ入力するとき、パスの外側に引用符は付けません。

```json
{
  "yajir.loaderPath": "${workspaceFolder}/build/script_extension.exe"
}
```

絶対パス、`${workspaceFolder}`、`${fileDirname}`、ワークスペースからの相対パスを使用できます。
誤って`"C:\path\loader.exe"`のような引用符付きの値を保存した場合も、v0.1.1以降は外側の
引用符を自動的に取り除きます。

コンパイルチェックには、`check <file>`に対応したYajir PC拡張ローダーが必要です。
チェックはスクリプトをロード・コンパイルしますが、`INIT`や`MAIN`は実行しません。

`def_import`の探索基準などを変えたい場合は、`yajir.workingDirectory`を設定します。

```json
{
  "yajir.workingDirectory": "${workspaceFolder}"
}
```

## 開発

このフォルダーをVS Codeで開いて`F5`を押すと、Extension Development Hostで試せます。

VSIXを作る場合は、`@vscode/vsce`を用意して次を実行します。

```text
vsce package --no-dependencies
```
