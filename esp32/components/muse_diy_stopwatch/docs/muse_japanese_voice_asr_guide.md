# 音声認識フロー再現ガイド（要点版）

別のスレッド／環境の Muse が、現在の音声認識の流れを再現するための説明。

## 前提

- ボイスノートは WAV（16 kHz・モノラル）で `~/workspace/user/files/voice_note-<番号>.wav` に届く。
- プラットフォームの自動文字起こしは日本語でよく化けるうえ、差し替え・無効化はできない。→ **届いた WAV を助手側で読み直す**。処理はすべてローカル、GPU 不要。

## 流れ

1. まず **ReazonSpeech K2 v2 INT8**（日本語専用、sherpa-onnx）で読む。これが既定。
2. **faster-whisper base** で照合するのは、英語まじり／Reazon の読みが怪しい／行動を変える指示のときだけ。
3. どちらの読みも怪しければ補完せず、返ってきた文字列をそのまま引用し「暫定」と明示して言い直しを頼む。
4. Reazon の空出力＝言葉ではない音（鼻歌など）の信号。エラーではない。

## セットアップ

### Reazon（第一パス）

```bash
python3 -m venv ~/workspace/reazon-venv
~/workspace/reazon-venv/bin/pip install sherpa-onnx==2.0.1
```

HF の `reazon-research/reazonspeech-k2-v2` から4ファイルを `~/workspace/reazon/model/` へ:
`encoder.int8.onnx`（約148 MB）、`decoder.onnx`（約11 MB）、`joiner.int8.onnx`（約2.6 MB）、`tokens.txt`。

`~/workspace/reazon/transcribe_reazon.py`:

```python
#!/usr/bin/env python3
import sys, time, wave
import numpy as np
import sherpa_onnx

MODEL_DIR = "/home/hatch/workspace/reazon/model"  # 環境に合わせて変更

def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1
        frames = w.readframes(w.getnframes())
    return np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0

recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
    tokens=f"{MODEL_DIR}/tokens.txt",
    encoder=f"{MODEL_DIR}/encoder.int8.onnx",
    decoder=f"{MODEL_DIR}/decoder.onnx",
    joiner=f"{MODEL_DIR}/joiner.int8.onnx",
    num_threads=4, sample_rate=16000, feature_dim=80,
    decoding_method="greedy_search", provider="cpu",
)
for path in sys.argv[1:]:
    stream = recognizer.create_stream()
    stream.accept_waveform(16000, read_wav(path))
    t0 = time.time()
    recognizer.decode_stream(stream)
    print(f"{path.split('/')[-1]} [{time.time()-t0:.2f}s] {stream.result.text}")
```

実行: `~/workspace/reazon-venv/bin/python ~/workspace/reazon/transcribe_reazon.py <wav> [<wav> ...]`
速度目安: 読込数秒＋1件 0.5〜0.9 秒。

### Whisper（照合用）

```bash
python3 -m venv ~/workspace/whisper-venv
~/workspace/whisper-venv/bin/pip install faster-whisper==1.2.1
```

**注意: 内蔵の PyAV デコードが壊れている。** ffmpeg で f32 に変換してから渡す。`~/workspace/whisper-venv/transcribe_one.py`:

```python
import sys, subprocess
import numpy as np
from faster_whisper import WhisperModel

out = subprocess.run(["ffmpeg","-v","error","-i",sys.argv[1],"-f","f32le","-ac","1","-ar","16000","-"], capture_output=True, check=True).stdout
model = WhisperModel("base", device="cpu", compute_type="int8", cpu_threads=4)
segments, _ = model.transcribe(np.frombuffer(out, dtype=np.float32), language="ja", beam_size=5)
print("".join(s.text for s in segments).strip())
```

実行: `~/workspace/whisper-venv/bin/python ~/workspace/whisper-venv/transcribe_one.py <wav>`
速度目安: 読込約3秒＋1件約2秒。Whisper は medium 以上だと CPU では遅すぎるので base 固定。

## エンジンの癖

- **Reazon**: 句読点なし、短い音声はひらがな寄り、日本語専用。聞き取れない末尾は作らず落とす。
- **Whisper**: 英語に強いが日本語の短い音声は Reazon より化けやすく、聞き取れない部分をそれらしく作ることがある。単独で信用しない。

## 注意点

- デバイスの WAV は RIFF ヘッダが `0xFFFFFFFF` 形式のことがある。`wave` モジュールでは読める。scipy の「Reached EOF」警告は無害。
- iOS のディクテーションはテキストのみで WAV が残らないため、読み直し不可。
- 16 kHz・モノラル以外の音声は ffmpeg で変換してから渡す。

（実績環境: Linux VM 4コア / Python 3.12.3 / ffmpeg 8.1.2 / sherpa-onnx 2.0.1 / faster-whisper 1.2.1）
