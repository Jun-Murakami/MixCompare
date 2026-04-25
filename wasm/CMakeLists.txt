cmake_minimum_required(VERSION 3.15)
project(MixCompareDSP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mixcompare_dsp
    src/wasm_exports.cpp
)

target_include_directories(mixcompare_dsp PRIVATE src)

if(EMSCRIPTEN)
    set_target_properties(mixcompare_dsp PROPERTIES
        SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    )

    set(EXPORTED_FUNCTIONS
        # 初期化
        "_dsp_init" "_dsp_destroy"
        # メモリ
        "_dsp_alloc_buffer" "_dsp_free_buffer" "_malloc" "_free"
        # Host
        "_dsp_load_host_track" "_dsp_set_source_select"
        # Host トランスポート (playlist と独立)
        "_dsp_host_set_playing" "_dsp_host_is_playing"
        "_dsp_host_seek" "_dsp_host_set_loop"
        "_dsp_host_get_position" "_dsp_host_get_duration"
        "_dsp_host_consume_stopped_at_end"
        # トラック管理
        "_dsp_load_track" "_dsp_remove_track" "_dsp_clear_tracks"
        "_dsp_select_track" "_dsp_get_track_count" "_dsp_get_track_duration"
        # トランスポート (playlist)
        "_dsp_set_playing" "_dsp_is_playing" "_dsp_seek" "_dsp_set_loop"
        "_dsp_get_position" "_dsp_get_duration"
        "_dsp_get_current_track_index" "_dsp_consume_stopped_at_end"
        # DSP パラメータ
        "_dsp_set_host_gain" "_dsp_set_playlist_gain"
        "_dsp_set_lpf_enabled" "_dsp_set_lpf_frequency"
        # メイン処理
        "_dsp_process_block"
        # メータリング
        "_dsp_set_metering_mode" "_dsp_get_meter_data"
        "_dsp_reset_true_peak" "_dsp_reset_momentary_hold"
    )
    list(JOIN EXPORTED_FUNCTIONS "," EXPORTED_FUNCTIONS_STR)

    target_link_options(mixcompare_dsp PRIVATE
        "-sSTANDALONE_WASM=1"
        "-sEXPORTED_FUNCTIONS=[${EXPORTED_FUNCTIONS_STR}]"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=16777216"
        "-sSTACK_SIZE=65536"
        "-sWASM=1"
        "--no-entry"
        "-O3"
    )
endif()
