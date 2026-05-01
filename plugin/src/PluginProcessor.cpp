// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath> // std::abs for TruePeak accumulation
#include "ParameterIDs.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "core/StateManager.h"
#include "core/AudioEngine.h"
#include "core/FormatUtils.h" // フロントエンド準拠の周波数表示用ユーティリティ
#include "core/TransportManager.h"
#include "core/PlaylistManager.h"
#include "core/MeteringService.h"
#include "core/ErrorManager.h"
#include "util/CrashHandler.h"
#include "audio/StreamingPlaybackSource.h"
#include "audio/InMemoryPlaybackSource.h"
#include "audio/MonkeyAudioFormat.h"
#if JUCE_WINDOWS
#include "audio/MediaFoundationAACFormat.h"
#endif


MixCompare3AudioProcessor::MixCompare3AudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    mc3::CrashHandler::install();
    // パラメータ変更を監視
    parameters.addParameterListener(mc3::id::HOST_GAIN.getParamID(), this);
    parameters.addParameterListener(mc3::id::PLAYLIST_GAIN.getParamID(), this);
    parameters.addParameterListener(mc3::id::LPF_FREQ.getParamID(), this);
    parameters.addParameterListener(mc3::id::LPF_ENABLED.getParamID(), this);
    parameters.addParameterListener(mc3::id::HOST_SYNC_ENABLED.getParamID(), this);
    parameters.addParameterListener(mc3::id::SOURCE_SELECT.getParamID(), this);
    parameters.addParameterListener(mc3::id::METERING_MODE.getParamID(), this);
    // Transport/Loop normalized params (non-automatable) もリッスン
    parameters.addParameterListener(mc3::id::TRANSPORT_PLAYING.getParamID(), this);
    parameters.addParameterListener(mc3::id::TRANSPORT_LOOP_ENABLED.getParamID(), this);
    parameters.addParameterListener(mc3::id::TRANSPORT_SEEK_NORM.getParamID(), this);
    parameters.addParameterListener(mc3::id::LOOP_START_NORM.getParamID(), this);
    parameters.addParameterListener(mc3::id::LOOP_END_NORM.getParamID(), this);
    parameters.addParameterListener(mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID(), this);
    
    // Initialize managers
    stateManager = std::make_unique<MixCompare::StateManager>();
    audioEngine = std::make_unique<MixCompare::AudioEngine>();
    transportManager = std::make_unique<MixCompare::TransportManager>(stateManager.get());
    playlistManager = std::make_unique<MixCompare::PlaylistManager>(stateManager.get());
    meteringService = std::make_unique<MixCompare::MeteringService>();
    
    // Set up AudioEngine callbacks
    audioEngine->onPlaybackStopped = [this]()
    {
        // This is called from audio thread, so we need to handle it safely
        juce::MessageManager::callAsync([this]()
        {
            handlePlaybackStopped();
        });
    };
    
    // Position update callback
    audioEngine->onPositionChanged = [this](double positionInSeconds)
    {
        // オーディオスレッドからは直接ValueTreeを操作せず、atomic変数に値を設定
        pendingPositionUpdate.store(positionInSeconds);
        hasPendingPositionUpdate.store(true);
        pendingPositionEpoch.store(positionEpoch.load(std::memory_order_acquire), std::memory_order_release);
        
        // ProcessorのローカルtransportState更新は廃止（TransportManager経由に統一）
        
        // Notify WebUI about position change (throttled to avoid flooding)
        static double lastUiUpdateTime = 0.0;
        static double lastUiPosition = -1.0;
        const double currentTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const double updateInterval = 0.05; // Update UI at most 20 times per second
        
        if (currentTime - lastUiUpdateTime >= updateInterval || 
            std::abs(positionInSeconds - lastUiPosition) > 1.0) // Also update on large jumps
        {
            lastUiUpdateTime = currentTime;
            lastUiPosition = positionInSeconds;
            
            // Editorへの直接送信は廃止（Editorのリスナー: transportPositionChanged 経由で送信）
        }
    };

    // Wire up managers
    stateManager->setupManagers(audioEngine.get(), 
                               transportManager.get(),
                               playlistManager.get(),
                               meteringService.get());
    
    // リスナー登録
    stateManager->addListener(this);
    transportManager->addListener(this);
    
    // デバッグ：パラメータIDと初期値を確認    
    // 位置更新用のタイマーを開始（50Hz）
    startTimer(50); // 20Hz - 位置更新用（高頻度は不要）

    // コンストラクタ時点で APVTS を StateManager に同期
    // - prepareToPlay 前でも DAW からのオートメーション再生に対応できるようにする
    // - StateSnapshot 作成時に APVTS の実値が常に反映される
    if (stateManager)
    {
        stateManager->syncWithAPVTS(parameters);
    }

    // 初期化直後は未決定。prepareToPlay/setStateInformationで判定・設定する。
    pendingHostSyncCapable.store(-1);
    
}

MixCompare3AudioProcessor::~MixCompare3AudioProcessor()
{
    // タイマーを停止
    stopTimer();
    cancelBackgroundLoadThread();
    isLoadingSource.store(false, std::memory_order_release);
    
    // パラメータリスナーを先に削除（parametersはメンバー変数なので最後まで有効）
    parameters.removeParameterListener(mc3::id::HOST_GAIN.getParamID(), this);
    parameters.removeParameterListener(mc3::id::PLAYLIST_GAIN.getParamID(), this);
    parameters.removeParameterListener(mc3::id::LPF_FREQ.getParamID(), this);
    parameters.removeParameterListener(mc3::id::LPF_ENABLED.getParamID(), this);
    parameters.removeParameterListener(mc3::id::HOST_SYNC_ENABLED.getParamID(), this);
    parameters.removeParameterListener(mc3::id::SOURCE_SELECT.getParamID(), this);
    parameters.removeParameterListener(mc3::id::METERING_MODE.getParamID(), this);
    parameters.removeParameterListener(mc3::id::TRANSPORT_PLAYING.getParamID(), this);
    parameters.removeParameterListener(mc3::id::TRANSPORT_LOOP_ENABLED.getParamID(), this);
    parameters.removeParameterListener(mc3::id::TRANSPORT_SEEK_NORM.getParamID(), this);
    parameters.removeParameterListener(mc3::id::LOOP_START_NORM.getParamID(), this);
    parameters.removeParameterListener(mc3::id::LOOP_END_NORM.getParamID(), this);
    parameters.removeParameterListener(mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID(), this);
    
    // managerリスナーを削除（unique_ptrの自動破棄前に実行）
    if (stateManager)
        stateManager->removeListener(this);
    if (transportManager)
        transportManager->removeListener(this);

    {
        const std::lock_guard<std::mutex> fmGuard(backgroundFormatManagerMutex);
        // Media Foundation フォーマットはここで明示的に破棄し、DLL 解放時の MFShutdown 呼び出しを避ける。
        backgroundFormatManager.clearFormats();
    }
    
    // managersを明示的に破棄して順序を制御
    meteringService.reset();
    playlistManager.reset();
    transportManager.reset();
    audioEngine.reset();
    stateManager.reset();
}

const juce::String MixCompare3AudioProcessor::getName() const { return "MixCompare"; }

double MixCompare3AudioProcessor::getTailLengthSeconds() const {
    // AAX/Pro Tools 対策:
    // 入力が完全サイレンスの区間でも Render を継続させるため、
    // 十分に大きな tail 長を常時報告する。
    // これにより Playlist ソースのみでの発音でも即時に処理が継続される。
    return 3600.0; // 1時間相当（実際には「継続処理あり」を示すための大値）
}

bool MixCompare3AudioProcessor::acceptsMidi() const { return false; }
bool MixCompare3AudioProcessor::producesMidi() const { return false; }
bool MixCompare3AudioProcessor::isMidiEffect() const { return false; }

int MixCompare3AudioProcessor::getNumPrograms() { return 1; }
int MixCompare3AudioProcessor::getCurrentProgram() { return 0; }
void MixCompare3AudioProcessor::setCurrentProgram(int) {}
const juce::String MixCompare3AudioProcessor::getProgramName(int) { return {}; }
void MixCompare3AudioProcessor::changeProgramName(int, const juce::String&) {}

void MixCompare3AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    tempBuffer.setSize(2, samplesPerBlock);
    
    // Prepare managers
    if (audioEngine)
        audioEngine->prepareToPlay(sampleRate, samplesPerBlock);
    if (meteringService)
        meteringService->prepareToPlay(sampleRate, samplesPerBlock);
    
    // Sync StateManager with APVTS
    if (stateManager)
    {
        stateManager->syncWithAPVTS(parameters);
        
        // Create initial snapshot for AudioEngine
        // snapshot removed
    }
    
    // 再生位置を初期化（TransportManager 経由で管理）
    playbackPosition = 0.0;

    // 起動時に METERING_MODE の現在値をホストへ通知して、
    // DAW 側のオートメーション再生/追従を確実に開始させる（初回ポーク）。
    if (auto* ranged = parameters.getParameter(mc3::id::METERING_MODE.getParamID()))
    {
        // 既存値を取得して同じ値で通知（値は変えない）
        const float normalized = ranged->getValue();
        ranged->setValueNotifyingHost(normalized);
    }

    // prepare時に動作モード判定: 実行時のwrapperTypeで判定（VST3/AU等=true、Standalone=false）
    {
        const bool capable = (wrapperType != juce::AudioProcessor::wrapperType_Standalone);
        
        pendingHostSyncCapable.store(capable ? 1 : 0, std::memory_order_release);
    }
}

void MixCompare3AudioProcessor::releaseResources() 
{
}

bool MixCompare3AudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet().isDisabled())
        return true;
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

void MixCompare3AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    juce::ScopedNoDenormals noDenormals;
    
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
    
    // =========================================================================
    // APVTS から全パラメータをオーディオブロック毎に直接読み取り（デモ準拠・最短経路）
    // =========================================================================
    if (audioEngine)
    {
        MixCompare::AudioEngine::RealtimeParams rtParams;
        
        // Gain parameters (robust fallback using normalized -> plain conversion)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                parameters.getParameter(mc3::id::HOST_GAIN.getParamID())))
        {
            rtParams.hostGainDb = p->get();
        }
        else if (auto* rp = parameters.getParameter(mc3::id::HOST_GAIN.getParamID()))
        {
            rtParams.hostGainDb = rp->convertFrom0to1(rp->getValue());
        }
        
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                parameters.getParameter(mc3::id::PLAYLIST_GAIN.getParamID())))
        {
            rtParams.playlistGainDb = p->get();
        }
        else if (auto* rp = parameters.getParameter(mc3::id::PLAYLIST_GAIN.getParamID()))
        {
            rtParams.playlistGainDb = rp->convertFrom0to1(rp->getValue());
        }
        
        // LPF parameters
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(
                parameters.getParameter(mc3::id::LPF_ENABLED.getParamID())))
            rtParams.lpfEnabled = p->get();
        
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(
                parameters.getParameter(mc3::id::LPF_FREQ.getParamID())))
            rtParams.lpfFrequencyHz = p->get();
        
        // Metering mode
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(
                parameters.getParameter(mc3::id::METERING_MODE.getParamID())))
        {
            rtParams.meteringMode = p->getIndex();
        }
        else if (auto* rp = parameters.getParameter(mc3::id::METERING_MODE.getParamID()))
        {
            const int idx = (int) std::lround(rp->convertFrom0to1(rp->getValue()));
            rtParams.meteringMode = juce::jlimit(0, 2, idx);
        }

        // Source select (0=Host, 1=Playlist)
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(
                parameters.getParameter(mc3::id::SOURCE_SELECT.getParamID())))
        {
            rtParams.currentSourceIndex = p->getIndex();
        }
        else if (auto* rp = parameters.getParameter(mc3::id::SOURCE_SELECT.getParamID()))
        {
            // Choiceの正規化値をIndexへ丸め（フォールバック）
            const int idx = (int) std::lround(rp->convertFrom0to1(rp->getValue()));
            rtParams.currentSourceIndex = juce::jlimit(0, 1, idx);
        }

        // Transport playing state (同期OFF時のみ有効)
        bool hostSyncEnabled = false;
        if (auto* pSync = dynamic_cast<juce::AudioParameterBool*>(
                parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
        {
            hostSyncEnabled = pSync->get();
        }
        // Standalone は同期機能無しとして通常再生
        const bool treatAsSynced = hostSyncEnabled && (wrapperType != juce::AudioProcessor::wrapperType_Standalone);
        if (!treatAsSynced)
        {
            bool playing = false;
            if (auto* p = parameters.getRawParameterValue(mc3::id::TRANSPORT_PLAYING.getParamID()))
                playing = (p->load() >= 0.5f);
            if (transportManager && transportManager->isPlaying())
                playing = true;
            rtParams.transportPlaying = playing;
        }
        else
        {
            // 同期ON時は AudioEngine 側でホスト位置に従うため、内部トランスポートは無効扱い
            rtParams.transportPlaying = false;
        }

        // Loop params → AudioEngine（ファイルSR基準サンプルへ変換）
        if (transportManager && audioEngine)
        {
            const double fileSR = audioEngine->getFileSampleRate();
            const bool loopOn = transportManager->isLoopEnabled();
            const double loopStartSec = transportManager->getLoopStart();
            const double loopEndSec = transportManager->getLoopEnd();
            rtParams.loopEnabled = loopOn;
            rtParams.loopStartSamples = loopStartSec * fileSR;
            rtParams.loopEndSamples = loopEndSec * fileSR;
        }
        
        // AudioEngine にリアルタイムパラメータを即座に反映
        audioEngine->setRealtimeParams(rtParams);
        
        // ホスト同期: getPlayHead から位置/再生状態を取得し AudioEngine へ注入
        if (treatAsSynced)
        {
            if (auto* ph = getPlayHead())
            {
                if (const auto pos = ph->getPosition())
                {
                    const bool isPlaying = pos->getIsPlaying();
                    double posSec = 0.0;
                    if (auto t = pos->getTimeInSeconds())
                        posSec = *t;
                    // AudioEngine にホスト同期状態を注入
                    audioEngine->setHostSyncState(true, isPlaying, posSec);
                    // TransportManager 反映はメッセージスレッドに委譲（ListenerList再入防止）
                    pendingTransportHostPosSec.store(posSec, std::memory_order_release);
                    pendingTransportHostIsPlaying.store(isPlaying, std::memory_order_release);
                    pendingTransportHostUpdate.store(true, std::memory_order_release);
                }
                else
                {
                    // 位置が取れないホストは同期不能 → 内部再生にフォールバック
                    audioEngine->setHostSyncState(false, false, 0.0);
                    // UI側トグル操作はメッセージスレッドで（ListenerList再入を避ける）
                    pendingDisableHostSync.store(true, std::memory_order_release);
                }

            }
            else
            {
                // PlayHeadが無い場合もフォールバック
                audioEngine->setHostSyncState(false, false, 0.0);
                pendingDisableHostSync.store(true, std::memory_order_release);
            }
        }
        else
        {
            // 同期OFF または Standalone の場合は同期無効を通知
            audioEngine->setHostSyncState(false, false, 0.0);
        }

        // オーディオ処理実行（AudioEngine が全信号チェーンを担当）
        juce::MidiBuffer midiBuffer;
        audioEngine->processBlock(buffer, midiBuffer);
        
        // メータリングは MeteringService に委譲（AudioEngine の値は使用しない）
        // 出力レベルは MeteringService(Output) から取得するため、ここでの計算は行わない
        
        // Process metering for each source
        if (meteringService)
        {
            // Get the actual processed buffers from AudioEngine
            juce::AudioBuffer<float> processedHostBuffer(2, buffer.getNumSamples());
            juce::AudioBuffer<float> processedPlaylistBuffer(2, buffer.getNumSamples());
            audioEngine->getLastProcessedBuffers(processedHostBuffer, processedPlaylistBuffer);
            
            // Process host buffer (input from DAW)
            meteringService->processBuffer(processedHostBuffer, MixCompare::MeteringService::MeterSource::Host);
            // Process playlist buffer
            meteringService->processBuffer(processedPlaylistBuffer, MixCompare::MeteringService::MeterSource::Playlist);

            // Fallback 用に Processor 側の原子 dB 値も更新（Editor がサービス未使用の場合）
            const auto hostValues = meteringService->getMeterValues(MixCompare::MeteringService::MeterSource::Host);
            const auto playlistValues = meteringService->getMeterValues(MixCompare::MeteringService::MeterSource::Playlist);
            hostLevelLeft.store(juce::Decibels::gainToDecibels(hostValues.rmsLeft, -60.0f), std::memory_order_release);
            hostLevelRight.store(juce::Decibels::gainToDecibels(hostValues.rmsRight, -60.0f), std::memory_order_release);
            playlistLevelLeft.store(juce::Decibels::gainToDecibels(playlistValues.rmsLeft, -60.0f), std::memory_order_release);
            playlistLevelRight.store(juce::Decibels::gainToDecibels(playlistValues.rmsRight, -60.0f), std::memory_order_release);
        }
        
        // AudioEngine has already applied all processing
        // No further processing needed
    }
    
    // 出力メータリング（LPF適用後）
    if (meteringService)
    {
        meteringService->processBuffer(buffer, MixCompare::MeteringService::MeterSource::Output);
        // Output dB を更新
        auto outputValues = meteringService->getMeterValues(MixCompare::MeteringService::MeterSource::Output);
        outputLevelLeft = juce::Decibels::gainToDecibels(outputValues.rmsLeft, -60.0f);
        outputLevelRight = juce::Decibels::gainToDecibels(outputValues.rmsRight, -60.0f);
    }
    
    // Loop settings are now synced via StateSnapshot, no need for manual sync
    
    // Transport position is now managed via StateSnapshot
    // No need to manually sync here
}

bool MixCompare3AudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* MixCompare3AudioProcessor::createEditor()
{
    return new MixCompare3AudioProcessorEditor(*this);
}

void MixCompare3AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // ルートのValueTreeを作成
    juce::ValueTree state("MixCompare3State");
    
    // APVTSの状態を子として追加。ただしランタイム用（非永続）パラメータは除外
    {
        auto paramState = parameters.copyState();
        // 子ノード（各パラメータ）を走査し、除外対象を削除
        for (int i = paramState.getNumChildren() - 1; i >= 0; --i)
        {
            auto child = paramState.getChild(i);
            const auto pid = child.getProperty("id").toString();
            if (pid == mc3::id::TRANSPORT_PLAYING.getParamID()
                || pid == mc3::id::TRANSPORT_SEEK_NORM.getParamID()
                || pid == mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID()
                || pid == mc3::id::HOST_SYNC_CAPABLE.getParamID()) // ランタイム専用は保存しない
            {
                paramState.removeChild(i, nullptr);
            }
        }
        state.addChild(paramState, -1, nullptr);
    }
    
    // デバッグ: LPF周波数の値を確認
    // 取得とログは不要なため削除（未使用変数の警告を回避）
    
    // プレイリスト情報を追加
    juce::ValueTree playlistTree("Playlist");
    {
        // PlaylistManagerから現在のプレイリストを取得
        auto currentPlaylist = getPlaylist();
        int currentIndex = getCurrentPlaylistIndex();
        playlistTree.setProperty("currentIndex", currentIndex, nullptr);
        
        for (const auto& item : currentPlaylist)
        {
            juce::ValueTree itemTree("Item");
            itemTree.setProperty("id", item.id, nullptr);
            itemTree.setProperty("path", item.file.getFullPathName(), nullptr);
            itemTree.setProperty("name", item.name, nullptr);
            playlistTree.addChild(itemTree, -1, nullptr);
        }
    }
    state.addChild(playlistTree, -1, nullptr);
    
    // トランスポート状態を追加（TransportManager から取得）
    juce::ValueTree transportTree("Transport");
    if (transportManager)
    {
        transportTree.setProperty("loopStart", transportManager->getLoopStart(), nullptr);
        transportTree.setProperty("loopEnd", transportManager->getLoopEnd(), nullptr);
        transportTree.setProperty("loopEnabled", transportManager->isLoopEnabled(), nullptr);
    }
    state.addChild(transportTree, -1, nullptr);
    
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    
    copyXmlToBinary(*xml, destData);
}

void MixCompare3AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    
    if (xmlState.get() != nullptr)
    {
        
        if (xmlState->hasTagName("MixCompare3State"))
        {
            auto newState = juce::ValueTree::fromXml(*xmlState);
            
            // デバッグ取得はReleaseで未使用警告になるため削除
            
            // APVTSの状態のみを復元
            auto parametersChild = newState.getChildWithName(parameters.state.getType());
            if (parametersChild.isValid())
            {
                // 不要になった METERING_MODE の事前確認ループを削除
                
                parameters.replaceState(parametersChild);

                // ランタイム専用: HOST_SYNC_CAPABLE は実行時wrapperTypeに基づき更新
                if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_CAPABLE.getParamID())))
                {
                    const bool capable = (wrapperType != juce::AudioProcessor::wrapperType_Standalone);
                    
                    p->setValueNotifyingHost(capable ? 1.0f : 0.0f);
                }
                
                // ランタイム用（非永続）パラメータは復元直後にデフォルトへリセット（Loop/Loop Range は維持）
                if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID())))
                    p->setValueNotifyingHost(0.0f); // 常に停止
                if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(parameters.getParameter(mc3::id::TRANSPORT_SEEK_NORM.getParamID())))
                    p->setValueNotifyingHost(0.0f);
            }
            // プレイリスト復元
            auto playlistTree = newState.getChildWithName("Playlist");
            if (playlistTree.isValid() && playlistManager)
            {
                
                // PlaylistManagerをクリア
                playlistManager->removeAllItems();
                
                // プレイリストアイテムを復元
                juce::Array<juce::File> filesToAdd;
                juce::String firstItemId;
                for (int i = 0; i < playlistTree.getNumChildren(); ++i)
                {
                    auto itemTree = playlistTree.getChild(i);
                    juce::File file(itemTree.getProperty("path").toString());
                    if (file.existsAsFile())
                    {
                        filesToAdd.add(file);
                        if (i == 0)
                        {
                            firstItemId = itemTree.getProperty("id").toString();
                        }
                    }
                }
                
                // ファイルを追加
                if (!filesToAdd.isEmpty())
                {
                    auto addedIds = playlistManager->addFiles(filesToAdd);
                    
                    
                    // 現在選択中のインデックスを復元
                    int savedIndex = playlistTree.getProperty("currentIndex", -1);
                    if (savedIndex >= 0 && savedIndex < addedIds.size())
                    {
                        // インデックスに対応するアイテムを選択
                        playlistManager->selectItem(addedIds[savedIndex]);
                        
                        
                        
                        // オーディオファイルをロード（ソースがPlaylistの場合）
                        // 注：この時点でcurrentSourceはまだ復元されていない可能性があるため、後で処理する
                    }
                }
            }

            // オーディオファイルのロードは prepareToPlay 後（SR/BS確定後）に非同期で行う
            // ここではペンディングフラグのみ立てる
            {
                pendingSourceRefresh.store(true, std::memory_order_release);
            }

            // APVTS の PLAYLIST_CURRENT_INDEX_NORM を現在のインデックスに同期
            // これにより「既に0.0のため最初の項目を選べない」問題を防ぐ
                if (auto* p = parameters.getParameter(mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID()))
                {
                    const int num = playlistManager ? playlistManager->getNumItems() : 0;
                    const int cur = playlistManager ? playlistManager->getCurrentIndex() : -1;
                    float norm = 0.0f;
                    if (num > 1 && cur >= 0)
                        norm = static_cast<float>(cur) / static_cast<float>(num - 1);
                    // setValueNotifyingHost で parameterChanged を確実に発火させる
                    p->setValueNotifyingHost(norm);
                }
            
            // Editorへの直接送信は廃止（Editorのリスナー経由で送信）
        }
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout MixCompare3AudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    
    // HOST側ゲイン (-∞ ~ 0dB)
    // -120dB以下は-∞として扱う
    // setSkewForCentreは使わず、フロントエンドのカーブをそのまま使用
    auto gainRange = juce::NormalisableRange<float>(-120.0f, 0.0f);
    
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::HOST_GAIN, "Host Gain", 
        gainRange, 
        0.0f,  // デフォルト0dB
        juce::AudioParameterFloatAttributes().withLabel("dB")));
    
    // PLAYLIST側ゲイン (-∞ ~ 0dB)  
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::PLAYLIST_GAIN, "Playlist Gain", 
        gainRange, 
        0.0f,  // デフォルト0dB
        juce::AudioParameterFloatAttributes().withLabel("dB")));
    
    // LPFカットオフ周波数 (20Hz ~ 20kHz、デフォルト120Hz - LFE標準)
    // より細かいステップサイズ（0.01Hz）で精度を向上
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::LPF_FREQ, "LPF Frequency (24dB/oct)", 
        juce::NormalisableRange<float>(20.0f, 20000.0f, 0.01f),  // 0.01Hzステップ
        120.0f,  // デフォルト120Hz（5.1ch LFE標準）
        juce::AudioParameterFloatAttributes().withLabel("Hz")));
    
    layout.add(std::make_unique<juce::AudioParameterBool>(
        mc3::id::LPF_ENABLED, "LPF Enabled", false));

    // ホスト同期が利用可能か（Standaloneではfalse）- 非オートメーション
    layout.add(std::make_unique<juce::AudioParameterBool>(
        mc3::id::HOST_SYNC_CAPABLE,
        "Host Sync Capable",
        true, // デフォルトは true（プラグインで即有効）。Standalone は後段で prepareToPlay/timer で false に上書き
        juce::AudioParameterBoolAttributes().withAutomatable(false).withMeta(true)));

    // ホスト同期（自動化・永続化対応）
    layout.add(std::make_unique<juce::AudioParameterBool>(
        mc3::id::HOST_SYNC_ENABLED,
        "Sync To Host",
        false,
        juce::AudioParameterBoolAttributes().withAutomatable(true).withMeta(true)));

    // ソース選択 (0=Host, 1=Playlist) - Choice に統一
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        mc3::id::SOURCE_SELECT,
        "Source Select",
        juce::StringArray{"Host", "Playlist"},
        0));
    
    // メータリングモード (0=Peak, 1=RMS, 2=Momentary)
    juce::StringArray meteringChoices{ "Peak", "RMS", "Momentary" };
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        mc3::id::METERING_MODE,
        "Metering Mode",
        meteringChoices,
        0));
    
    // ===== Transport (non-automatable) =====
    layout.add(std::make_unique<juce::AudioParameterBool>(
        mc3::id::TRANSPORT_PLAYING,
        "Transport Playing",
        false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));
    
    layout.add(std::make_unique<juce::AudioParameterBool>(
        mc3::id::TRANSPORT_LOOP_ENABLED,
        "Loop Enabled (Transport)",
        false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    // シーク（0..1 正規化、入力専用）
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::TRANSPORT_SEEK_NORM,
        "Transport Seek (Normalized)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f),
        0.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false)));

    // ループ範囲（0..1 正規化）
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::LOOP_START_NORM,
        "Loop Start (Normalized)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f),
        0.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false)));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::LOOP_END_NORM,
        "Loop End (Normalized)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f),
        1.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false)));

    // Playlist current index (normalized, non-automatable)
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        mc3::id::PLAYLIST_CURRENT_INDEX_NORM,
        "Playlist Current Index (Normalized)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f),
        0.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false)));
    
    return layout;
}

void MixCompare3AudioProcessor::addFilesToPlaylist(const juce::Array<juce::File>& files)
{
    
    
    // Use PlaylistManager
    if (playlistManager)
    {
        // 追加前の空状態を保持
        const bool wasEmpty = (playlistManager->getNumItems() == 0);
        auto addedIds = playlistManager->addFiles(files);
        
        if (wasEmpty && !addedIds.isEmpty())
        {
            // プレイリストが空だった場合のみ、最初の項目を自動選択
            if (playlistManager->selectItemByIndex(0))
            {
                asyncLoadAndApplyCurrentItem();
            }
        }
        return;
    }
    else
    {
    }
}

void MixCompare3AudioProcessor::removeFromPlaylist(const juce::String& id)
{
    if (!playlistManager)
        return;

    // 現在の選択インデックスを把握
    const int beforeIndex = playlistManager->getCurrentIndex();
    const bool isCurrent = (playlistManager->getCurrentItemId() == id);

    // アイテムを削除
    const bool removed = playlistManager->removeItem(id);
    if (!removed)
        return;

    const int afterCount = playlistManager->getNumItems();

    // 再生中の曲を削除した場合は即停止（自動再開を防ぐ）
    if (isCurrent)
    {
        if (transportManager) transportManager->pause();
        if (auto* p = parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()))
            p->setValueNotifyingHost(0.0f);
    }

    // 選択の移動規則
    // - クリアされたのが現在曲なら、次（同じindex）へ。末尾だった場合は前へ。
    // - それ以外は選択を維持。
    if (isCurrent)
    {
        if (afterCount <= 0)
        {
            // 何も残っていない → 選択なし
            // 再生位置/ループ/長さを 0 にリセットして UI を即時更新
            if (transportManager)
            {
                transportManager->setLoopEnabled(false);
                transportManager->setLoopRange(0.0, 0.0);
                transportManager->setDuration(0.0);
                transportManager->setPosition(0.0);
            }
            if (audioEngine)
            {
                audioEngine->seekSeconds(0.0);
                audioEngine->requestForceZeroPositionNotify();
                // ソースを完全に解除して以降の位置通知を停止
                audioEngine->setPlaybackSource(nullptr);
            }
            if (auto* editor = dynamic_cast<MixCompare3AudioProcessorEditor*>(getActiveEditor()))
            {
                auto safeEditor = juce::Component::SafePointer(editor);
                juce::MessageManager::callAsync([safeEditor]()
                {
                    if (!safeEditor) return;
                    // セッションIDを進めて position=0/duration=0 を確実に反映
                    safeEditor->sendTrackChangeUpdate();
                });
            }
        }
        else
        {
            const int newIndex = (beforeIndex < afterCount) ? beforeIndex : (afterCount - 1);
            if (playlistManager->selectItemByIndex(newIndex))
            {
                // 新しい選曲としてエポックを必ず進め、進行中のロードを打ち切ってから再始動する
                // これにより、削除直後のレースで古いソースが適用されるのを防ぐ
                loadEpoch.fetch_add(1, std::memory_order_acq_rel);

                // 位置/ループを即時初期化して UI と内部状態を同期
                playbackPosition = 0.0;
                if (auto* item = playlistManager->getCurrentItem())
                {
                    if (transportManager)
                    {
                        transportManager->setLoopRange(0.0, item->duration);
                        transportManager->setPosition(0.0);
                    }
                }
                else if (transportManager)
                {
                    transportManager->setPosition(0.0);
                }

                // 新曲が実際に処理開始するまで位置通知を一時抑止し、0 秒を即時通知
                if (audioEngine)
                {
                    audioEngine->requestForceZeroPositionNotify();
                    audioEngine->setSuppressPositionNotify(true);
                }

                // 進行中のバックグラウンドロードを安全に中断してから最新で再ロード
                cancelBackgroundLoadThread();
                isLoadingSource.store(false, std::memory_order_release);

                // 自動再生はしない。ロードのみ行う
                asyncLoadAndApplyCurrentItem();
            }
        }
    }
}

void MixCompare3AudioProcessor::reorderPlaylist(int fromIndex, int toIndex)
{
    // 新実装: PlaylistManager/StateManager を真実源として並べ替える
    if (!playlistManager)
        return;

    // 現在のID順を取得
    const auto items = playlistManager->getAllItems();
    if (items.isEmpty()) return;
    const int n = static_cast<int>(items.size());
    if (fromIndex < 0 || fromIndex >= n || toIndex < 0 || toIndex >= n || fromIndex == toIndex)
        return;

    juce::Array<juce::String> orderedIds;
    orderedIds.ensureStorageAllocated(n);
    for (const auto& it : items)
        orderedIds.add(it.id);

    // ID配列を移動
    const juce::String movedId = orderedIds[(int)fromIndex];
    orderedIds.remove((int)fromIndex);
    orderedIds.insert((int)toIndex, movedId);

    // マネージャへ反映（StateManager に永続化され、Editorリスナー経由でUIへ送信される）
    playlistManager->reorderItems(orderedIds);
}

void MixCompare3AudioProcessor::clearPlaylist()
{
    // Delegate to PlaylistManager
    if (playlistManager)
    {
        // 再生を即停止（自動再開を防止）
        if (transportManager) transportManager->pause();
        if (auto* p = parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()))
            p->setValueNotifyingHost(0.0f);

        playlistManager->removeAllItems();

        // 位置/ループ/長さを 0 にリセットし、UI を即時更新
        if (transportManager)
        {
            transportManager->setLoopEnabled(false);
            transportManager->setLoopRange(0.0, 0.0);
            transportManager->setDuration(0.0);
            transportManager->setPosition(0.0);
        }
        if (audioEngine)
        {
            audioEngine->seekSeconds(0.0);
            audioEngine->requestForceZeroPositionNotify();
            // ソースを完全に解除して以降の位置通知を停止
            audioEngine->setPlaybackSource(nullptr);
        }

    if (auto* editor = dynamic_cast<MixCompare3AudioProcessorEditor*>(getActiveEditor()))
    {
        auto safeEditor = juce::Component::SafePointer(editor);
        juce::MessageManager::callAsync([safeEditor]()
        {
            if (!safeEditor) return;
            // セッションIDを進め、プレイリスト空＋position/duration=0 をまとめて送信
            safeEditor->sendTrackChangeUpdate();
        });
    }
    }
    
    // Also clear old playlist for backward compatibility
    juce::ScopedLock sl(playlistLock);
    playlist.clear();
    // Don't reset currentPlaylistIndex to maintain selection state
    // currentPlaylistIndex = -1;  // Removed to keep selection state
}

void MixCompare3AudioProcessor::exportPlaylistToM3U8(const juce::File& file)
{
    // M3U8形式でプレイリストをエクスポート
    juce::String content = "#EXTM3U\n";
    
    auto currentPlaylist = getPlaylist();
    for (const auto& item : currentPlaylist)
    {
        // #EXTINF:duration,title
        int durationSeconds = static_cast<int>(item.duration);
        juce::String title = item.name;
        // 拡張子を除去してタイトルとする
        int lastDot = title.lastIndexOf(".");
        if (lastDot > 0)
            title = title.substring(0, lastDot);
            
        content += "#EXTINF:" + juce::String(durationSeconds) + "," + title + "\n";
        // ファイルパスは絶対パスで出力
        content += item.file.getFullPathName() + "\n";
    }
    
    // UTF-8でファイルに書き込み（LF改行）
    file.replaceWithText(content, false, false, "\n");
}

void MixCompare3AudioProcessor::importPlaylistFromM3U8(const juce::File& file)
{
    if (!file.existsAsFile())
        return;
        
    // ファイルを読み込み
    juce::StringArray lines = juce::StringArray::fromLines(file.loadFileAsString());
    
    // 既存のプレイリストをクリア
    clearPlaylist();
    
    // M3U8をパース
    juce::Array<juce::File> filesToAdd;
    for (int i = 0; i < lines.size(); ++i)
    {
        juce::String line = lines[i].trim();
        
        // コメント行やEXTINF行はスキップ
        if (line.startsWith("#"))
            continue;
            
        // 空行はスキップ
        if (line.isEmpty())
            continue;
            
        // ファイルパスとして処理
        juce::File audioFile(line);
        if (audioFile.existsAsFile())
        {
            filesToAdd.add(audioFile);
        }
    }
    
    // ファイルを追加
    if (!filesToAdd.isEmpty())
    {
        addFilesToPlaylist(filesToAdd);
    }
}

void MixCompare3AudioProcessor::play()
{
    
    // 非同期ロード中でも先にTransportManagerを再生状態へ（ロード完了後にAudioEngineは追従）
    if (transportManager)
        transportManager->play();
}

void MixCompare3AudioProcessor::pause()
{
    if (transportManager)
        transportManager->pause();
    // スナップショット運用は廃止
}

void MixCompare3AudioProcessor::stop()
{
    // 再生状態は APVTS パラメータを真実源として変更する
    if (auto* p = parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()))
    {
        // setValueNotifyingHost 経由で parameterChanged → pause() が呼ばれ、
        // StateManager/Editor 連携も一貫して更新される
        p->setValueNotifyingHost(0.0f);
    }
}

void MixCompare3AudioProcessor::seek(double position)
{
    
    
    if (transportManager)
        transportManager->seek(position);
    
    // New: 秒指定シークを AudioEngine 経由で IPlaybackSource に伝達
    if (audioEngine)
    {
        audioEngine->seekSeconds(position);
    }
    
    // snapshot removed
    
    // Keep backward compatibility with playbackPosition member (file SR based)
    if (audioEngine)
    {
        const double fileSR = audioEngine->getFileSampleRate();
        playbackPosition = position * fileSR;
    }
    
    // Processor のローカル transportState 更新は廃止
}

void MixCompare3AudioProcessor::setLoopRange(double start, double end)
{
    
    
    if (transportManager)
        transportManager->setLoopRange(start, end);
    
    // ループ範囲設定時、現在位置が In 点より前にある場合のみ In 点へスナップ
    // Out 点より先にいる場合は、そのまま再生を続行（ループ折り返しは自然に発生）
    if (transportManager)
    {
        const double currentPosition = transportManager->getPosition();
        const bool loopOn = transportManager->isLoopEnabled();
        
        if (loopOn && currentPosition < start)
        {
            // In 点より前にいる場合のみジャンプ
            seek(start);
        }
    }
        
    // Processor のローカル transportState 更新は廃止
}

void MixCompare3AudioProcessor::setLoopEnabled(bool enabled)
{
    
    
    if (transportManager)
        transportManager->setLoopEnabled(enabled);
    
    if (enabled)
    {
        double duration = transportManager ? transportManager->getDuration() : 0.0;
        double loopStart = transportManager ? transportManager->getLoopStart() : 0.0;
        double loopEnd = transportManager ? transportManager->getLoopEnd() : duration;
        
        if (loopEnd <= loopStart || loopEnd <= 0.0)
        {
            loopStart = 0.0;
            loopEnd = duration > 0.0 ? duration : 1.0;
            if (transportManager)
                transportManager->setLoopRange(loopStart, loopEnd);
        }
        const double pos = transportManager ? transportManager->getPosition() : 0.0;
        if (!(pos >= loopStart && pos < loopEnd))
        {
            
            seek(loopStart);
        }
    }
        
    // Processor のローカル transportState 更新は廃止
}

float MixCompare3AudioProcessor::getHostTruePeak(int channel) const
{
    // MeteringService 経由に一本化（減衰付き）
    if (!meteringService) return 0.0f;
    return const_cast<MixCompare::MeteringService*>(meteringService.get())
        ->getTruePeakLevelAndDecay(MixCompare::MeteringService::MeterSource::Host, channel);
}

float MixCompare3AudioProcessor::getPlaylistTruePeak(int channel) const
{
    // MeteringService 経由に一本化（減衰付き）
    if (!meteringService) return 0.0f;
    return const_cast<MixCompare::MeteringService*>(meteringService.get())
        ->getTruePeakLevelAndDecay(MixCompare::MeteringService::MeterSource::Playlist, channel);
}

void MixCompare3AudioProcessor::handlePlaybackStopped()
{
    // メッセージスレッドからのみ呼ばれる前提
    // 過剰進行を避けるため、まず現在の再生状態を明示停止に揃える
    if (transportManager)
        transportManager->pause();

    // Check if we're at the last item in the playlist
    if (playlistManager)
    {
        int currentIndex = playlistManager->getCurrentIndex();
        int itemCount = playlistManager->getNumItems();
        
        
        
        if (currentIndex >= 0 && currentIndex < itemCount - 1)
        {
            // Not the last item - move to next and continue playing
            
            selectNextPlaylistItem();
            
            // Resume playback for the next item
            if (transportManager)
                transportManager->play();
            
            // UI更新はselectNextPlaylistItem内で既に行われている
        }
        else
        {
            // Last item or invalid index - check if loop is enabled
            if (transportManager ? transportManager->isLoopEnabled() : false)
            {
                
                // Loop back to the first item
                selectPlaylistItem(0);
                
                // Resume playback
                if (transportManager)
                    transportManager->play();
            }
            else
            {
                // 仕様: 最後の曲の再生が終わったら、一時停止で位置を 0 に戻す
                if (transportManager)
                    transportManager->setPosition(0.0);
                if (audioEngine)
                    audioEngine->seekSeconds(0.0);
                // APVTS 側の TRANSPORT_PLAYING も停止へ反映（rtTransportPlaying との不一致を解消）
                if (auto* p = parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()))
                    p->setValueNotifyingHost(0.0f);
            }
        }
    }
    else
    {
        // No playlist manager - just stop
        stop();
    }
}

mc3::TransportState MixCompare3AudioProcessor::getTransportState() const
{
    // 後方互換API: TransportManager の状態をスナップショットにして返す
    mc3::TransportState ts;
    if (transportManager)
    {
        ts.isPlaying = transportManager->isPlaying();
        ts.position = transportManager->getPosition();
        ts.loopEnabled = transportManager->isLoopEnabled();
        ts.loopStart = transportManager->getLoopStart();
        ts.loopEnd = transportManager->getLoopEnd();
    }
    return ts;
}


void MixCompare3AudioProcessor::selectPlaylistItem(int index)
{
    if (playlistManager)
    {
        // Use PlaylistManager to select item
        if (playlistManager->selectItemByIndex(index))
        {
            // 新しい選曲エポック（ロード要求番号）を進める
            loadEpoch.fetch_add(1, std::memory_order_acq_rel);
            // Update current index
            currentPlaylistIndex = index;
            
            // Load the selected item
            if (auto* item = playlistManager->getCurrentItem())
            {
                // FIRST: Immediately reset position AND loop range (TransportManager 経由)
                playbackPosition = 0.0;
                if (transportManager)
                {
                    transportManager->setLoopRange(0.0, item->duration);
                    transportManager->setPosition(0.0);
                }
                // AudioEngine 側にも強制 0 秒通知を要求（UI の一瞬の旧位置表示を抑止）
                if (audioEngine)
                {
                    audioEngine->requestForceZeroPositionNotify();
                    // 新曲が実際に処理開始するまで position 通知を一時抑止
                    // （ロード完了後の applyLoadedSource で解除される）
                    audioEngine->setSuppressPositionNotify(true);
                }
                // Load the item (non-blocking)
                asyncLoadAndApplyCurrentItem();
                
                
            }
        }
    }
}

void MixCompare3AudioProcessor::selectPreviousPlaylistItem()
{
    if (playlistManager)
    {
        int currentIndex = playlistManager->getCurrentIndex();
        if (currentIndex > 0)
        {
            selectPlaylistItem(currentIndex - 1);
        }
    }
}

void MixCompare3AudioProcessor::selectNextPlaylistItem()
{
    if (playlistManager)
    {
        int currentIndex = playlistManager->getCurrentIndex();
        int itemCount = playlistManager->getNumItems();
        if (currentIndex < itemCount - 1)
        {
            selectPlaylistItem(currentIndex + 1);
        }
    }
}

std::vector<mc3::PlaylistItem> MixCompare3AudioProcessor::getPlaylist() const
{
    std::vector<mc3::PlaylistItem> items;
    if (playlistManager)
    {
        auto playlistItems = playlistManager->getAllItems();
        for (const auto& item : playlistItems)
        {
            mc3::PlaylistItem mcItem;
            mcItem.id = item.id;
            mcItem.name = item.displayName;
            mcItem.file = item.file;
            mcItem.duration = item.duration;
            mcItem.isLoaded = item.isLoaded;
            items.push_back(mcItem);
        }
    }
    return items;
}

int MixCompare3AudioProcessor::getCurrentPlaylistIndex() const
{
    if (playlistManager)
        return playlistManager->getCurrentIndex();
    return -1;
}



void MixCompare3AudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // パラメータ変更の通知
    if (parameterID == mc3::id::HOST_GAIN.getParamID())
    {
    }
    else if (parameterID == mc3::id::PLAYLIST_GAIN.getParamID())
    {
    }
    else if (parameterID == mc3::id::LPF_FREQ.getParamID())
    {
    }
    else if (parameterID == mc3::id::LPF_ENABLED.getParamID())
    {
    }
    else if (parameterID == mc3::id::SOURCE_SELECT.getParamID())
    {
    }
    else if (parameterID == mc3::id::METERING_MODE.getParamID())
    {
        // メータリングモードの変更 (true=Peak, false=VU)
        // TruePeakホールド値をリセット
        // AudioEngine 側での TruePeak リセットは廃止し、MeteringService に統一
        if (meteringService)
        {
            meteringService->resetTruePeakMeters();
        }
        
        // PluginProcessor側のTruePeak値もリセット
        hostTruePeakMaxLeft = 0.0f;
        hostTruePeakMaxRight = 0.0f;
        playlistTruePeakMaxLeft = 0.0f;
        playlistTruePeakMaxRight = 0.0f;
        
        // フロントエンドにTruePeakリセット通知を送信 & キャッシュ更新（安全化）
        if (auto* editor = dynamic_cast<MixCompare3AudioProcessorEditor*>(getActiveEditor()))
        {
            auto safeEditor = juce::Component::SafePointer(editor);
            juce::MessageManager::callAsync([safeEditor]()
            {
                if (!safeEditor) return;
                safeEditor->updateMeteringModeCache();  // キャッシュ更新
                safeEditor->sendMeteringReset();
            });
        }
    }
    else if (parameterID == mc3::id::TRANSPORT_PLAYING.getParamID())
    {
        // 同期ON時は無視（DAWに追従）
        const bool hostSync = [this]() {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
                return p->get();
            return false;
        }();
        if (hostSync && wrapperType != juce::AudioProcessor::wrapperType_Standalone)
            return; // プラグイン時の同期ONはDAWに追従
        if (hostSync && wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        {
            // Standaloneで同期ONを受け取ったら即オフに戻す（UIは押せるが効かない仕様）
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
                p->setValueNotifyingHost(0.0f);
        }
        const bool playRequested = (newValue >= 0.5f);
        if (playRequested) {
            if (transportManager) transportManager->play();
        } else {
            if (transportManager) transportManager->pause();
        }
    }
    else if (parameterID == mc3::id::HOST_SYNC_ENABLED.getParamID())
    {
        const bool requested = (newValue >= 0.5f);
        // Standalone では強制無効化
        if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        {
            if (requested)
            {
                if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
                    p->setValueNotifyingHost(0.0f);
            }
            return;
        }

        // 同期ONにしたら、ローカルのループは常に無効化する
        // - 仕様: ホスト同期モード中は内部ループ機能を使用しない
        if (requested)
        {
            if (auto* loopParam = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::TRANSPORT_LOOP_ENABLED.getParamID())))
            {
                if (loopParam->get())
                    loopParam->setValueNotifyingHost(0.0f);
            }
            if (transportManager)
                transportManager->setLoopEnabled(false);
        }

        // 同期を OFF に切り替えた直後は、APVTS 側の TRANSPORT_PLAYING を
        // 実際の再生状態（TransportManager）に合わせて即時更新する。
        // これにより、同期ON中にホスト側で再生していた場合でも、
        // 最初の一時停止クリックが確実に pause へ作用する。
        if (!requested)
        {
            // 同期OFFに切替時は、直前に積まれているホスト由来の保留更新を破棄
            pendingTransportHostUpdate.store(false, std::memory_order_release);

            // 実再生状態に合わせて TRANSPORT_PLAYING を反映
            if (auto* playParam = parameters.getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()))
            {
                const bool isPlayingNow = (transportManager &&
                    transportManager->getState() == MixCompare::TransportManager::TransportState::Playing);
                playParam->setValueNotifyingHost(isPlayingNow ? 1.0f : 0.0f);
            }
        }
    }
    else if (parameterID == mc3::id::TRANSPORT_LOOP_ENABLED.getParamID())
    {
        // ホスト同期ON中はループを常に無効化し、ループ有効化操作を拒否
        const bool hostSync = [this]() {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
                return p->get();
            return false;
        }();
        if (hostSync && wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        {
            // UIからON要求が来ても即座にOFFへ戻す（不整合防止）
            if (newValue >= 0.5f)
            {
                if (auto* loopParam = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::TRANSPORT_LOOP_ENABLED.getParamID())))
                    loopParam->setValueNotifyingHost(0.0f);
            }
            if (transportManager)
                transportManager->setLoopEnabled(false);
            return;
        }

        setLoopEnabled(newValue >= 0.5f);
    }
    else if (parameterID == mc3::id::TRANSPORT_SEEK_NORM.getParamID())
    {
        // 同期ON時は無視（DAWに追従）
        const bool hostSync = [this]() {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
                return p->get();
            return false;
        }();
        if (hostSync && wrapperType != juce::AudioProcessor::wrapperType_Standalone)
            return;
        // 正規化位置を秒へ変換して1回シーク（duration が未知の場合は堅牢にフォールバック）
        auto getEffectiveDurationSec = [this]() -> double
        {
            double d = transportManager ? transportManager->getDuration() : 0.0;
            if (d <= 0.0 && audioEngine)
                d = audioEngine->getDuration();
            if (d <= 0.0 && playlistManager)
            {
                if (auto* item = playlistManager->getCurrentItem())
                    d = item->duration;
            }
            return d;
        };

        const double durationSec = getEffectiveDurationSec();
        if (durationSec > 0.0)
        {
            const double posSec = juce::jlimit(0.0, durationSec, durationSec * static_cast<double>(newValue));
            // 新しいシーク要求としてエポックを進める
            positionEpoch.fetch_add(1, std::memory_order_acq_rel);
            seek(posSec);
        }
        else
        {
                
        }
        // UIへ即時反映（タイマー更新待ちによる巻き戻り防止）
        if (auto* editor = dynamic_cast<MixCompare3AudioProcessorEditor*>(getActiveEditor()))
        {
            auto safeEditor = juce::Component::SafePointer(editor);
            juce::MessageManager::callAsync([safeEditor]()
            {
                if (!safeEditor) return;
                // Editorへの直接送信は廃止
            });
        }
    }
    else if (parameterID == mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID())
    {
        // トラック切替開始: 旧曲の遅延 position 更新を即時に無効化
        positionEpoch.fetch_add(1, std::memory_order_acq_rel);
        hasPendingPositionUpdate.store(false, std::memory_order_release);
        pendingPositionUpdate.store(-1.0, std::memory_order_release);
        // 次ブロックで 0.0 を一度だけ即時通知して、ちらつきを抑制
        if (audioEngine)
        {
            audioEngine->requestForceZeroPositionNotify();
            // 新曲が実際に処理開始するまで position 通知を一時抑止
            audioEngine->setSuppressPositionNotify(true);
        }
        // 正規化インデックス → 実インデックスへ変換して選択（バッファ適用時にも追加で epoch を進める）
        if (playlistManager)
        {
            const int num = playlistManager->getNumItems();
            if (num > 0)
            {
                // 端を含む丸め：0..num-1
                const int index = juce::jlimit(0, num - 1, (int)std::round(newValue * (num - 1)));
                if (playlistManager->selectItemByIndex(index))
                {
                    // 非同期ロード・適用
                    asyncLoadAndApplyCurrentItem();
                }
            }
        }
    }
    else if (parameterID == mc3::id::LOOP_START_NORM.getParamID() || parameterID == mc3::id::LOOP_END_NORM.getParamID())
    {
        auto getEffectiveDurationSec = [this]() -> double
        {
            double d = transportManager ? transportManager->getDuration() : 0.0;
            if (d <= 0.0 && audioEngine)
                d = audioEngine->getDuration();
            if (d <= 0.0 && playlistManager)
            {
                if (auto* item = playlistManager->getCurrentItem())
                    d = item->duration;
            }
            return d;
        };

        const double durationSec = getEffectiveDurationSec();
        if (durationSec > 0.0)
        {
            // 変更された側だけを計算し、もう一方は TransportManager の現在値を使用
            const double startNormParam = parameters.getRawParameterValue(mc3::id::LOOP_START_NORM.getParamID())->load();
            const double endNormParam   = parameters.getRawParameterValue(mc3::id::LOOP_END_NORM.getParamID())->load();
            const double startNormClamped = juce::jlimit(0.0, 1.0, static_cast<double>(startNormParam));
            const double endNormClamped   = juce::jlimit(0.0, 1.0, static_cast<double>(endNormParam));

            double startSec = transportManager ? transportManager->getLoopStart() : 0.0;
            double endSec   = transportManager ? transportManager->getLoopEnd()   : durationSec;

            if (parameterID == mc3::id::LOOP_START_NORM.getParamID())
                startSec = startNormClamped * durationSec;
            else
                endSec = endNormClamped * durationSec;

            const double newStart = (std::min)(startSec, endSec);
            const double newEnd   = (std::max)(startSec, endSec);
            setLoopRange(newStart, newEnd);

            // Out点確定時は TransportManager の現在 In 点へ即ジャンプ
            if (parameterID == mc3::id::LOOP_END_NORM.getParamID())
            {
                const bool loopOn = transportManager ? transportManager->isLoopEnabled() : false;
                if (loopOn && transportManager)
                {
                    seek(transportManager->getLoopStart());
                }
            }
        }
        else
        {
            
        }
        if (auto* editor = dynamic_cast<MixCompare3AudioProcessorEditor*>(getActiveEditor()))
        {
            auto safeEditor = juce::Component::SafePointer(editor);
            juce::MessageManager::callAsync([safeEditor]()
            {
                if (!safeEditor) return;
                // Editorへの直接送信は廃止
            });
        }
    }
    // スナップショット更新は廃止（APVTS直読の最短経路に統一）
}



void MixCompare3AudioProcessor::transportStateChanged(MixCompare::TransportManager::TransportState newState)
{
    // 現在は再生/停止のトリガーパラメーターなので、状態変更の通知は不要
    // 将来的に再生状態を表示するパラメーターを追加する場合はここで処理
    juce::ignoreUnused(newState);
}

void MixCompare3AudioProcessor::transportPositionChanged(double newPosition)
{
    // 将来的にポジションパラメーターを追加する場合はここで処理
    juce::ignoreUnused(newPosition);
}

void MixCompare3AudioProcessor::loopStateChanged(bool enabled, double start, double end)
{
    // 将来的にループパラメーターを追加する場合はここで処理
    juce::ignoreUnused(enabled, start, end);
}

void MixCompare3AudioProcessor::timerCallback()
{
    // オーディオスレッドからの位置更新をメッセージスレッドで処理
    if (hasPendingPositionUpdate.load())
    {
        const double position = pendingPositionUpdate.load();
        const uint64_t epoch = pendingPositionEpoch.load(std::memory_order_acquire);
        hasPendingPositionUpdate.store(false);
        
        // TransportManagerのValueTreeを安全に更新（古いエポックの更新は破棄）
        if (transportManager && epoch == positionEpoch.load(std::memory_order_acquire))
        {
            transportManager->setPosition(position);
        }
    }

    // 選曲後/復元後のロード/ソース適用のフォールバック（メッセージスレッド）
    // AudioEngine::prepareToPlay が完了し SR/BS が確定した後に実行されるようタイマーで処理
    if (pendingSourceRefresh.exchange(false))
    {
        asyncLoadAndApplyCurrentItem();
        // ロード/適用が開始されたので position 通知抑止を解除（新曲の実際の位置が流れ始める）
        if (audioEngine)
            audioEngine->setSuppressPositionNotify(false);
    }

    // ホスト同期のUIトグルはメッセージスレッドでのみ変更
    if (pendingDisableHostSync.exchange(false))
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID())))
        {
            if (p->get()) p->setValueNotifyingHost(0.0f);
        }
    }

    // TransportManager のUI反映もメッセージスレッドでのみ
    if (pendingTransportHostUpdate.exchange(false))
    {
        if (transportManager)
        {
            const double posSec = pendingTransportHostPosSec.load(std::memory_order_acquire);
            const bool isPlaying = pendingTransportHostIsPlaying.load(std::memory_order_acquire);
            transportManager->setPosition(posSec);
            if (isPlaying) transportManager->play(); else transportManager->pause();
        }
    }

    // HOST_SYNC_CAPABLE 反映（メッセージスレッドでUIへ確実に伝播）
    const int ph = pendingHostSyncCapable.load(std::memory_order_acquire);
    if (ph == 0 || ph == 1)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(mc3::id::HOST_SYNC_CAPABLE.getParamID())))
        {
            
            p->setValueNotifyingHost(ph == 1 ? 1.0f : 0.0f);
        }
        pendingHostSyncCapable.store(-1, std::memory_order_release);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixCompare3AudioProcessor();
}

// 非同期ロードとソース適用を UI メッセージスレッドで安全に行う
void MixCompare3AudioProcessor::cancelBackgroundLoadThread()
{
    cancelBackgroundLoad.store(true, std::memory_order_release);

    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(backgroundLoadMutex);
        if (backgroundLoadThread.joinable())
            toJoin = std::move(backgroundLoadThread);
    }

    if (toJoin.joinable())
        toJoin.join();

    cancelBackgroundLoad.store(false, std::memory_order_release);
}

void MixCompare3AudioProcessor::initialiseBackgroundFormatManager()
{
    std::call_once(backgroundFormatManagerInitFlag, [this]()
    {
        // フォーマット登録をプロセッサ寿命内に限定して、グローバル static の破棄を回避する。
        backgroundFormatManager.registerBasicFormats();
        backgroundFormatManager.registerFormat(new mc3::MonkeyAudioFormat(), false);
#if JUCE_WINDOWS
        if (mc3::MediaFoundationAACFormat::isMediaFoundationAvailable())
            backgroundFormatManager.registerFormat(new mc3::MediaFoundationAACFormat(), false);
#endif
    });
}

void MixCompare3AudioProcessor::runBackgroundLoad(uint64_t epoch)
{
    if (cancelBackgroundLoad.load(std::memory_order_acquire))
        return;

    bool loaded = false;
    std::unique_ptr<juce::AudioFormatReader> prebuiltReader;
    std::shared_ptr<MixCompare::IPlaybackSource> preparedSource;
    juce::File fileToOpen;

    if (playlistManager && playlistManager->getCurrentItem())
    {
        auto* item = playlistManager->getCurrentItem();
        fileToOpen = item->file;
        
        initialiseBackgroundFormatManager();
        {
            const std::lock_guard<std::mutex> guard(backgroundFormatManagerMutex);
            prebuiltReader.reset(backgroundFormatManager.createReaderFor(fileToOpen));
        }
        if (prebuiltReader)
        {
            const juce::String ext = fileToOpen.getFileExtension().toLowerCase();
            const bool preferInMemory = (ext == ".m4a" || ext == ".aac");
            if (preferInMemory)
            {
                juce::AudioBuffer<float> mem;
                if (MixCompare::InMemoryPlaybackSource::readAllToBuffer(*prebuiltReader, mem))
                {
                    auto src = std::make_shared<MixCompare::InMemoryPlaybackSource>(std::move(mem), prebuiltReader->sampleRate);
                    const double sr = getSampleRate();
                    const int bs = getBlockSize();
                    src->prepare(sr, bs);
                    preparedSource = src;
                    loaded = true;
                }
            }
            // フォールバック（失敗時/他拡張子）: ストリーミング
            if (!loaded)
            {
                auto src = std::make_shared<MixCompare::StreamingPlaybackSource>(std::move(prebuiltReader));
                const double sr = getSampleRate();
                const int bs = getBlockSize();
                src->prepare(sr, bs);
                preparedSource = src;
                loaded = true;
            }
        }
        else
        {
            // 失敗時に詳細を報告（拡張子/存在/サイズ）
            const bool exists = fileToOpen.existsAsFile();
            const auto size = exists ? (juce::String) juce::String(fileToOpen.getSize()) + " bytes" : juce::String("n/a");
            juce::String details;
            details << "extension=" << fileToOpen.getFileExtension()
                    << ", exists=" << (exists ? "true" : "false")
                    << ", size=" << size;
            MixCompare::ErrorManager::getInstance().reportError(MixCompare::ErrorCode::PlaylistLoadError,
                "Failed to create audio reader", details, fileToOpen.getFullPathName());
        }
    }

    if (cancelBackgroundLoad.load(std::memory_order_acquire))
        return;

    if (!loaded)
    {
        const auto path = fileToOpen.getFullPathName();
        MixCompare::ErrorManager::getInstance().reportError(MixCompare::ErrorCode::PlaylistLoadError,
            "Failed to load playlist item", "", path);
    }

    auto weakThis = juce::WeakReference<MixCompare3AudioProcessor>(this);

    // キャプチャリストで同名変数のシャドウイングを避けるため、明示的に source として取り込む
    juce::MessageManager::callAsync([weakThis, loaded, epoch, source = std::move(preparedSource)]() mutable
    {
        if (auto* self = weakThis.get())
            self->applyLoadedSource(loaded, epoch, std::move(source));
    });
}

void MixCompare3AudioProcessor::applyLoadedSource(bool loaded,
                                                  uint64_t epoch,
                                                  std::shared_ptr<MixCompare::IPlaybackSource> preparedSource)
{
    if (epoch != loadEpoch.load(std::memory_order_acquire))
    {
        
        isLoadingSource.store(false, std::memory_order_release);
        return;
    }

    if (cancelBackgroundLoad.load(std::memory_order_acquire))
    {
        
        isLoadingSource.store(false, std::memory_order_release);
        return;
    }

    if (loaded && playlistManager && audioEngine)
    {
        if (auto* item = playlistManager->getCurrentItem())
        {
            
            if (preparedSource)
                audioEngine->setPlaybackSourcePrepared(std::move(preparedSource));

            if (transportManager)
            {
                transportManager->setDuration(item->duration);
                transportManager->setLoopRange(0.0, item->duration);
                transportManager->setPosition(0.0);
            }
            
            // ソース適用完了：位置通知抑止を解除して通常の位置更新を再開
            if (audioEngine)
                audioEngine->setSuppressPositionNotify(false);
        }
    }

    
    isLoadingSource.store(false, std::memory_order_release);
}

void MixCompare3AudioProcessor::asyncLoadAndApplyCurrentItem()
{
    if (!playlistManager || !audioEngine)
        return;

    if (isLoadingSource.exchange(true))
        return;

    const uint64_t epoch = loadEpoch.load(std::memory_order_acquire);
    

    cancelBackgroundLoadThread();
    cancelBackgroundLoad.store(false, std::memory_order_release);

    auto weakThis = juce::WeakReference<MixCompare3AudioProcessor>(this);

    {
        std::lock_guard<std::mutex> lock(backgroundLoadMutex);
        backgroundLoadThread = std::thread([weakThis, epoch]() mutable
        {
            if (auto* self = weakThis.get())
                self->runBackgroundLoad(epoch);
        });
    }
}

