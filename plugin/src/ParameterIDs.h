// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace mc3::id {
    const juce::ParameterID HOST_GAIN{"HOST_GAIN", 1};  // HOST側ゲイン
    const juce::ParameterID PLAYLIST_GAIN{"PLAYLIST_GAIN", 1};  // PLAYLIST側ゲイン
    const juce::ParameterID LPF_FREQ{"LPF_FREQ", 1};
    const juce::ParameterID LPF_ENABLED{"LPF_ENABLED", 1};
    
    // ホスト同期（true=ホストと再生位置/再生状態を同期）
    const juce::ParameterID HOST_SYNC_ENABLED{"HOST_SYNC_ENABLED", 1};
    // ホスト同期が利用可能か（Standaloneではfalse）- 非オートメーション
    const juce::ParameterID HOST_SYNC_CAPABLE{"HOST_SYNC_CAPABLE", 1};
    
    // ソース選択パラメーター（true=Playlist, false=Host）
    const juce::ParameterID SOURCE_SELECT{"SOURCE_SELECT", 1};
    
    // メータリングモード（0=Peak, 1=RMS, 2=Momentary）
    const juce::ParameterID METERING_MODE{"METERING_MODE", 1};

    // トランスポート（非オートメーション）
    const juce::ParameterID TRANSPORT_PLAYING{"TRANSPORT_PLAYING", 1};
    const juce::ParameterID TRANSPORT_LOOP_ENABLED{"TRANSPORT_LOOP_ENABLED", 1};
    const juce::ParameterID TRANSPORT_SEEK_NORM{"TRANSPORT_SEEK_NORM", 1};
    const juce::ParameterID LOOP_START_NORM{"LOOP_START_NORM", 1};
    const juce::ParameterID LOOP_END_NORM{"LOOP_END_NORM", 1};
    // プレイリスト選択インデックス（0..1 正規化、非オートメーション）
    const juce::ParameterID PLAYLIST_CURRENT_INDEX_NORM{"PLAYLIST_CURRENT_INDEX_NORM", 1};
}  // namespace mc3::id
