# Wake word models

Two models are embedded in the firmware (see ../CMakeLists.txt). They come
from different places and are under different terms.

## vad.tflite, vad.json

From the ESPHome micro_wake_word model collection
(https://github.com/esphome/micro-wake-word-models), Copyright Open Home
Foundation, Apache License 2.0. `LICENSE` and `NOTICE` in this folder are that
collection's files and apply to these two files only.

## hey_muse.tflite, hey_muse.json

The "Hey Muse" model was trained by museStopWatch (2026-11-10) with
microWakeWord (https://github.com/kahrendt/microWakeWord, Apache License 2.0),
following its basic training notebook. The scripts are in the museStopWatch
repository, `tools/wakeword/`. It is not part of the ESPHome collection, and
the Open Home Foundation `LICENSE` / `NOTICE` here do not cover it.

Training data:

- Positive samples: 5000 synthetic "hey muse" clips from piper-sample-generator
  (https://github.com/rhasspy/piper-sample-generator) with its
  `en_US-libritts_r-medium` voice model (trained on LibriTTS-R, CC BY 4.0).
- Augmentation (background and room sound):
  - MIT environmental impulse responses
    (https://huggingface.co/datasets/davidscripka/MIT_environmental_impulse_responses)
  - AudioSet, balanced train parts 09 and 19
    (https://huggingface.co/datasets/agkphysics/AudioSet)
  - Free Music Archive, xsmall set
    (https://huggingface.co/datasets/mchl914/fma_xsmall)
- Negative samples: the ready-made features of
  https://huggingface.co/datasets/kahrendt/microwakeword
  (speech, dinner_party, no_speech).

The microWakeWord training notebook notes that some of the augmentation data
has non-commercial licenses. Whether those terms reach the trained weights is
not settled. Check the licenses of the data above before you use
`hey_muse.tflite` commercially, or train your own model with data you have
cleared.
