# Reason で使う

Reason Free（Windows）で試して分かったこと。

## VST3 はシングルパートの楽器として扱われる

置き場（`C:\Program Files\Common Files\VST3`）に入れれば、プラグインの管理に
`tarboh / S-MU2000` が出て、トラックに楽器として挿せる。

ただし Reason は **1 トラックに 1 台の楽器**を繋ぎ、MIDI チャンネルを選ばせない。
2 台挿しても、どちらも LCD が `01 A01`（パート 1）で鳴った。2 本目の入力バス
（`MIDI In B`）を選ぶ口も見当たらない。1 台の S-MU2000 を複数のトラックから
マルチティンバーで鳴らす道は、いまのところ VST3 には無い。

## マルチティンバーで鳴らすなら: MIDI Out Device → loopMIDI → gui.exe

Reason の `MIDI Out Device` は、トラックの MIDI をチャンネルを決めて外の口へ送る。

1. loopMIDI の口を用意し、`gui.exe` の MIDI IN A にする
2. Reason でトラックごとに `MIDI Out Device` を作り、送り先を `MME loopMIDI Port`、
   チャンネルをトラックごとに変える

音は `gui.exe` から直接サウンドカードへ出るので、Reason のミキサーは通らない。

## MIDI の輪に注意（PC ごと固まった）

**同じ loopMIDI の口を、Reason の入力にもしてはいけない。**

```
Reason の MIDI Out Device ──▶ loopMIDI Port ──▶ gui.exe
          ▲                        │
          └── Reason の入力ポート ◀┘   ← ここが有効だと輪になる
```

環境設定の MIDI の「簡単な MIDI 入力」で `loopMIDI Port` が有効になっていると、
送ったノートオン・オフが Reason の入力に戻り、選んでいるトラックを通ってまた
送り出される。メッセージが無限に増えて、

* `gui.exe` が溢れ、THRU で繋いでいた実機の MU2000 まで固まった
* Reason と `gui.exe` が落ちた
* そのあと **Windows の MIDI の仕組みそのものが固まり**（loopMIDI も RME の口も
  開けなくなった）、Reason が起動しなくなった。PC の再起動で直った

送る口と、Reason が受ける口は分けること。B の口（`loopMIDI Port 1` など）を
送り先に使うときも同じ。

`gui.exe` 側にはこのあと守りを入れた（[doc/gui.md](gui.md) の「口が応答しない
とき・MIDI が溢れたとき」）。輪ができても THRU には線の 2 倍までしか流さず、
開けない口は 2 秒で諦める。輪そのものは Reason 側の設定なので、こちらでは
消せない。
