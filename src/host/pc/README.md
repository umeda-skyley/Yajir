# Yajir PC host extensions

Windows PCホストに追加されるHTTP、AI、ファイルI/Oポートの仕様です。これらは
`src/host/pc`固有であり、Yajir coreや組み込みホストには含まれません。

## 設定

リポジトリ直下の`.env.template`を`.env`として用意し、利用するAPIのキーを設定します。
`.env`はGitの追跡対象外です。モデル、生成上限、温度、推論量なども同じファイルで指定します。

AIポートの第1引数には通常のプロンプト文字列、または`{`で始まるJSONオブジェクトを渡せます。
通常文字列では`.env`設定からリクエストJSONを生成し、JSON入力では指定済みフィールドを優先しつつ
不足する既定値を補います。第2引数の文字列を指定すると、その呼び出しだけモデルを上書きします。

## HTTP

| ポート | 形式 | 結果 |
|---|---|---|
| `HTTP_SYNC` | `url -> HTTP_SYNC` | `SRESULT`に本文、`RESULT`にHTTP status |
| `HTTP_ASYNC` | `url -> HTTP_ASYNC` | GETを開始し、完了時に`HTTP`イベントをpost |
| `ON HTTP` | 完了ハンドラ | `SARG[0]`に本文、`ARG[1]`にHTTP status |

HTTPはGET専用です。接続失敗などでstatusを取得できない場合は0になります。同期本文は
`CFG_SSTR_LEN - 1`、非同期本文は`CFG_SARG_LEN - 1`まで保持され、超過時は切り詰められます。

## OpenAI / Anthropic / Gemini

| プロバイダ | 同期ポート | 非同期ポート | 完了ハンドラ | 履歴ポート |
|---|---|---|---|---|
| OpenAI | `GPT_SYNC` | `GPT_ASYNC` | `ON GPT` | `GPT_HISTORY` |
| Anthropic | `CLAUDE_SYNC` | `CLAUDE_ASYNC` | `ON CLAUDE` | `CLAUDE_HISTORY` |
| Google | `GEMINI_SYNC` | `GEMINI_ASYNC` | `ON GEMINI` | `GEMINI_HISTORY` |

同期形式は`prompt -> GPT_SYNC`、または`prompt, model -> GPT_SYNC`です。返答テキストは
`SRESULT`、HTTP statusは`RESULT`に入ります。ClaudeとGeminiも同じ形式です。

非同期形式は`prompt -> GPT_ASYNC`、または`prompt, model -> GPT_ASYNC`です。完了ハンドラでは
`SARG[0]`に返答テキスト、`ARG[1]`にHTTP statusが入ります。ClaudeとGeminiも同じです。

通常のプロンプト入力は、PCホストを起動してから終了するまでプロバイダ別の会話履歴へ蓄積されます。
同期・非同期は同じ履歴を共有しますが、GPT、Claude、Gemini間の履歴は独立しています。JSONを直接
渡した呼び出しは履歴へ追加されません。履歴は最大約256KBで、古い会話から切り詰められます。

```yaj
GPT_HISTORY -> STDOUT       // 現在のおおよその履歴バイト数
0 -> GPT_HISTORY            // GPT履歴をクリア
```

## ファイルI/O

| ポート | 形式 | 結果 |
|---|---|---|
| `FILE_READER` | `path -> FILE_READER` | `SRESULT`に内容、`RESULT`に保持バイト数 |
| `FILE_WRITER` | `path, body -> FILE_WRITER` | 上書きし、`RESULT`に書込バイト数 |
| `FILE_WRITER` | `path, body, mode -> FILE_WRITER` | 指定モードで書き込み |

書込モードは`w`、`a`、`wb`、`ab`です。既定は`w`です。パスはUTF-8からWindowsのUnicode
パスへ変換されるため、日本語ファイル名も扱えます。戻り値が負の場合はエラーです。

| 戻り値 | 内容 |
|---:|---|
| `-1` | 引数またはモードが不正 |
| `-2` | ファイルを開けない |
| `-3` | 読み書きまたはメモリ確保に失敗 |

## サンプル

- `scripts/pc-extension/net_ai_demo.yaj`: HTTPと3社AIの同期・非同期呼び出し
- `scripts/pc-extension/file_demo.yaj`: 上書き、追記、読込、チェイン
- `scripts/pc-extension/utf8_stdout.yaj`: Windowsコンソールへの日本語出力
