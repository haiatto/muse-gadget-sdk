# museStopWatch: wake words (msw_wake.h). microWakeWord models in models/:
# hey_muse is ours (tools/wakeword); vad is from esphome/micro-wake-word-models
# (Apache-2.0: models/LICENSE, models/NOTICE). They
# run on TensorFlow Lite Micro (espressif/esp-tflite-micro) with its audio
# frontend, set up as ESPHome's micro_wake_word does.
idf_component_register(
    SRCS "msw_wake.cc"
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
    EMBED_FILES "models/hey_muse.tflite" "models/vad.tflite"
    PRIV_REQUIRES espressif__esp-tflite-micro esphome__esp-micro-speech-features muse esp_timer nvs_flash
)

# The models table lists only each manifest's values; the rest starts zeroed.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
