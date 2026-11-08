# museStopWatch: sanoTTS-jp (https://github.com/ayutaz/sanoTTS-jp), local Japanese TTS.
#
# upstream/ is a verbatim copy made by tools/sync_sanotts.py (see upstream/SOURCE.txt).
# The build settings follow upstream esp32/components/saanotts_core/CMakeLists.txt
# for the kanji build on ESP32-S3:
#   - SAAN_KANJI with CHARSET_UTF_8, LABEL_IDS_EXTERNAL_SCRATCH, LABEL_IDS_MAX_TOKENS 256
#   - Open JTalk's temporary heap goes to PSRAM (-include oj_heap_psram.h on its sources)
#   - W8A8 + PIE (SAAN_INT8_ACT, SAAN_PIE), the upstream default on ESP32-S3
#   - SAAN_PORT_HEADER saan_port_esp32.h (erf tables in DRAM, hot code in IRAM)
# The model and the dictionary live in their own partitions (partitions_musestopwatch.csv)
# and `idf.py flash` writes them there.
set(U "${CMAKE_CURRENT_LIST_DIR}/upstream")
file(GLOB OJT "${U}/openjtalk/*.c")

idf_component_register(
    SRCS "${U}/saanotts.c" "${U}/saanotts_stream.c" "${U}/fft.c" "${U}/saanotts_int8.c" "${U}/g2p.c"
         "${U}/jdict.c" "${U}/accent.c" "${U}/njd_rules.c" "${U}/label_ids.c" "${U}/oj_heap_psram.c"
         "${U}/saan_kanji.c" "${U}/saan_pcm.c" ${OJT}
         "msw_tts.c"
    INCLUDE_DIRS "${U}" "${U}/openjtalk" "."
    PRIV_REQUIRES esp_partition esp_timer spi_flash muse nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC CHARSET_UTF_8 SAAN_KANJI=1
                           LABEL_IDS_EXTERNAL_SCRATCH=1 LABEL_IDS_MAX_TOKENS=256)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1
                           "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")
set_source_files_properties(${OJT} PROPERTIES COMPILE_OPTIONS "-include;${U}/oj_heap_psram.h;-w")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare)

# msw_tts_console() replaces a weak stub in the muse component; make the linker take it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u msw_tts_console")

esptool_py_flash_to_partition(flash "sano_model" "${CMAKE_CURRENT_LIST_DIR}/model/voice-v4-int8.bin")
esptool_py_flash_to_partition(flash "sano_dict" "${CMAKE_CURRENT_LIST_DIR}/model/k1-dict-44000-2mb.bin")
