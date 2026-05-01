// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "StateManager.h"
#include "AudioEngine.h"
#include "TransportManager.h"
#include "PlaylistManager.h"
#include "MeteringService.h"
#include "../ParameterIDs.h"
#include <cmath>

namespace {
    // metering mode helper removed (APVTS直結のため)
}

namespace MixCompare
{

const juce::Identifier StateManager::treeType("MixCompareState");
const juce::Identifier StateManager::playlistType("Playlist");
const juce::Identifier StateManager::transportType("Transport");
const juce::Identifier StateManager::playlistItemType("PlaylistItem");

const juce::Identifier StateManager::propIsPlaying("isPlaying");
const juce::Identifier StateManager::propPosition("position");
const juce::Identifier StateManager::propLoopEnabled("loopEnabled");
const juce::Identifier StateManager::propLoopStart("loopStart");
const juce::Identifier StateManager::propLoopEnd("loopEnd");
const juce::Identifier StateManager::propActiveFileId("activeFileId");
const juce::Identifier StateManager::propFilePath("filePath");
const juce::Identifier StateManager::propFileName("fileName");
const juce::Identifier StateManager::propFileId("fileId");

StateManager::StateManager()
    : state(treeType)
{
    initializeState();
    state.addListener(this);

    // ParameterManager removed: parameters are read from APVTS directly
}

StateManager::~StateManager()
{
    state.removeListener(this);

    if (transportManager)
        transportManager->removeListener(this);
    if (playlistManager)
        playlistManager->removeListener(this);

    detachFromAPVTS();
}
void StateManager::initializeState()
{
    juce::ValueTree playlist(playlistType);
    state.appendChild(playlist, nullptr);
    
    juce::ValueTree transport(transportType);
    transport.setProperty(propIsPlaying, false, nullptr);
    transport.setProperty(propPosition, 0.0, nullptr);
    transport.setProperty(propLoopEnabled, false, nullptr);
    transport.setProperty(propLoopStart, 0.0, nullptr);
    transport.setProperty(propLoopEnd, 1.0, nullptr);
    state.appendChild(transport, nullptr);
}

juce::ValueTree StateManager::getPlaylistState() const
{
    return state.getChildWithName(playlistType);
}

void StateManager::addPlaylistItem(const juce::File& file)
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return;
    
    juce::ValueTree item(playlistItemType);
    auto id = generateUniqueId();
    item.setProperty(propFileId, id.toString(), nullptr);
    item.setProperty(propFilePath, file.getFullPathName(), nullptr);
    item.setProperty(propFileName, file.getFileName(), nullptr);
    
    playlist.appendChild(item, nullptr);
}

void StateManager::removePlaylistItem(const juce::Identifier& itemId)
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return;
    
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto item = playlist.getChild(i);
        if (item.getProperty(propFileId).toString() == itemId.toString())
        {
            playlist.removeChild(item, nullptr);
            break;
        }
    }
}

void StateManager::reorderPlaylistItems(const juce::Array<juce::Identifier>& newOrder)
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return;
    
    juce::Array<juce::ValueTree> items;
    for (const auto& id : newOrder)
    {
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto item = playlist.getChild(i);
            if (item.getProperty(propFileId).toString() == id.toString())
            {
                items.add(item);
                break;
            }
        }
    }
    
    playlist.removeAllChildren(nullptr);
    for (const auto& item : items)
    {
        playlist.appendChild(item, nullptr);
    }
}

void StateManager::clearPlaylist()
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return;
    
    playlist.removeAllChildren(nullptr);
}

juce::ValueTree StateManager::getTransportState() const
{
    return state.getChildWithName(transportType);
}

void StateManager::setTransportPlaying(bool shouldPlay)
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return;
    
    transport.setProperty(propIsPlaying, shouldPlay, nullptr);
}

void StateManager::setTransportPosition(double position)
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return;
    
    transport.setProperty(propPosition, position, nullptr);
}

void StateManager::setLoopEnabled(bool enabled)
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return;
    
    transport.setProperty(propLoopEnabled, enabled, nullptr);
}

void StateManager::setLoopRange(double start, double end)
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return;
    
    transport.setProperty(propLoopStart, start, nullptr);
    transport.setProperty(propLoopEnd, end, nullptr);
}

bool StateManager::isPlaying() const
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return false;
    
    return transport.getProperty(propIsPlaying);
}

double StateManager::getPosition() const
{
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid()) return 0.0;
    
    return transport.getProperty(propPosition);
}

void StateManager::setActiveFileId(const juce::Identifier& fileId)
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return;

    // プレイリスト配下に選択中ファイルIDを保持（ソース種別に依存しない）
    playlist.setProperty(propActiveFileId, fileId.toString(), nullptr);
}

juce::Identifier StateManager::getActiveFileId() const
{
    auto playlist = state.getChildWithName(playlistType);
    if (!playlist.isValid()) return {};

    // プレイリスト配下から選択中ファイルIDを取得
    juce::String idString = playlist.getProperty(propActiveFileId).toString();
    if (idString.isEmpty()) return {};
    
    return juce::Identifier(idString);
}


void StateManager::valueTreePropertyChanged(juce::ValueTree& treeWhosePropertyHasChanged,
                                           const juce::Identifier& property)
{
    // 未使用パラメータの警告を抑止
    juce::ignoreUnused(property);
    
    try
    {
        if (treeWhosePropertyHasChanged.getType() == transportType)
        {
            listeners.call(&StateManager::Listener::transportStateChanged);
        }
        else if (treeWhosePropertyHasChanged.getType() == playlistType)
        {
            // プレイリスト配下（例: activeFileId）のプロパティ変更も通知
            listeners.call(&StateManager::Listener::playlistChanged);
        }
        listeners.call(&StateManager::Listener::stateChanged, state);
    }
    catch (const std::exception& e)
    {
        // 例外を記録（未使用変数の警告を抑止）
        juce::ignoreUnused(e);
        
        // Attempt recovery if state is corrupted
        if (!validateState())
        {
            
            initializeState();
        }
    }
}

void StateManager::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&)
{
    listeners.call(&StateManager::Listener::playlistChanged);
    listeners.call(&StateManager::Listener::stateChanged, state);
}

void StateManager::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int)
{
    listeners.call(&StateManager::Listener::playlistChanged);
    listeners.call(&StateManager::Listener::stateChanged, state);
}

void StateManager::valueTreeChildOrderChanged(juce::ValueTree&, int, int)
{
    listeners.call(&StateManager::Listener::playlistChanged);
    listeners.call(&StateManager::Listener::stateChanged, state);
}

void StateManager::valueTreeParentChanged(juce::ValueTree&)
{
    listeners.call(&StateManager::Listener::stateChanged, state);
}

void StateManager::addListener(Listener* listener)
{
    listeners.add(listener);
}

void StateManager::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

juce::var StateManager::handleMessage(const Message& message)
{
    if (message.type.startsWith("playlist/"))
    {
        return handlePlaylistMessage(message.type.substring(9), message.payload);
    }
    // transport/* は APVTS 直結に移行済み
    return {};
}

juce::var StateManager::handlePlaylistMessage(const juce::String& action, const juce::var& payload)
{
    if (action == "addFiles")
    {
        if (payload.isArray())
        {
            for (int i = 0; i < payload.size(); ++i)
            {
                auto path = payload[i].toString();
                if (path.isNotEmpty())
                {
                    addPlaylistItem(juce::File(path));
                }
            }
        }
    }
    else if (action == "remove")
    {
        auto id = payload["id"].toString();
        if (id.isNotEmpty())
        {
            removePlaylistItem(id);
        }
    }
    else if (action == "reorder")
    {
        if (payload.isArray())
        {
            juce::Array<juce::Identifier> newOrder;
            for (int i = 0; i < payload.size(); ++i)
            {
                newOrder.add(payload[i].toString());
            }
            reorderPlaylistItems(newOrder);
        }
    }
    else if (action == "clear")
    {
        clearPlaylist();
    }
    return {};
}

// transport/* メッセージ処理は不要（APVTS へ移行）

juce::var StateManager::createResponse(const juce::var& data, const juce::String& nonce)
{
    auto response = juce::DynamicObject::Ptr(new juce::DynamicObject());
    response->setProperty("data", data);
    if (nonce.isNotEmpty())
    {
        response->setProperty("nonce", nonce);
    }
    return juce::var(response.get());
}

juce::Identifier StateManager::generateUniqueId()
{
    static std::atomic<int> counter{0};
    auto timestamp = juce::Time::getCurrentTime().toMilliseconds();
    return juce::Identifier("item_" + juce::String(timestamp) + "_" + juce::String(++counter));
}


void StateManager::resetMomentaryHold()
{
    if (meteringService)
    {
        meteringService->resetMomentaryHold();
    }
}

void StateManager::setupManagers(AudioEngine* engine, 
                                TransportManager* transport,
                                PlaylistManager* playlist,
                                MeteringService* metering)
{
    // Remove old listeners if any
    if (transportManager)
        transportManager->removeListener(this);
    if (playlistManager)
        playlistManager->removeListener(this);
    
    // Set new managers
    audioEngine = engine;
    transportManager = transport;
    playlistManager = playlist;
    meteringService = metering;
    
    // Add listeners to new managers
    if (transportManager)
        transportManager->addListener(this);
    if (playlistManager)
        playlistManager->addListener(this);
    
    // Managers now communicate via StateSnapshot
    // Initial sync handled by syncStateFromManagers
    syncStateFromManagers();
}

void StateManager::syncStateFromManagers()
{
    if (!transportManager || !playlistManager)
        return;
    
    // Sync transport state
    auto transportTree = state.getChildWithName(transportType);
    if (transportTree.isValid())
    {
        transportTree.setProperty(propIsPlaying, 
                                 transportManager->getState() == TransportManager::TransportState::Playing,
                                 nullptr);
        transportTree.setProperty(propPosition, transportManager->getPosition(), nullptr);
        transportTree.setProperty(propLoopEnabled, transportManager->isLoopEnabled(), nullptr);
        transportTree.setProperty(propLoopStart, transportManager->getLoopStart(), nullptr);
        transportTree.setProperty(propLoopEnd, transportManager->getLoopEnd(), nullptr);
    }
    
    // Sync playlist state
    auto playlistTree = state.getChildWithName(playlistType);
    if (playlistTree.isValid())
    {
        playlistTree.removeAllChildren(nullptr);
        
        auto items = playlistManager->getAllItems();
        for (const auto& item : items)
        {
            juce::ValueTree itemTree(playlistItemType);
            itemTree.setProperty(propFileId, item.id, nullptr);
            itemTree.setProperty(propFilePath, item.file.getFullPathName(), nullptr);
            itemTree.setProperty(propFileName, item.displayName, nullptr);
            playlistTree.appendChild(itemTree, nullptr);
        }
        
        // Don't set current item here - it will be set when an item is selected
        // This avoids circular dependency during initialization
    }
}

// TransportManager::Listener implementation
void StateManager::transportStateChanged(MixCompare::TransportManager::TransportState newState)
{
    auto transportTree = state.getChildWithName(transportType);
    if (transportTree.isValid())
    {
        bool isPlaying = (newState == TransportManager::TransportState::Playing);
        transportTree.setProperty(propIsPlaying, isPlaying, nullptr);
    }
    
    // AudioEngine now gets transport state from StateSnapshot
    
    listeners.call(&StateManager::Listener::transportStateChanged);
}

void StateManager::transportPositionChanged(double newPosition)
{
    auto transportTree = state.getChildWithName(transportType);
    if (transportTree.isValid())
    {
        transportTree.setProperty(propPosition, newPosition, nullptr);
    }
}

void StateManager::loopStateChanged(bool enabled, double start, double end)
{
    auto transportTree = state.getChildWithName(transportType);
    if (transportTree.isValid())
    {
        transportTree.setProperty(propLoopEnabled, enabled, nullptr);
        transportTree.setProperty(propLoopStart, start, nullptr);
        transportTree.setProperty(propLoopEnd, end, nullptr);
    }
}

// PlaylistManager::Listener implementation  
void StateManager::playlistItemsChanged()
{
    syncStateFromManagers();
    listeners.call(&StateManager::Listener::playlistChanged);
}

void StateManager::currentItemChanged(const juce::String& itemId)
{
    juce::ignoreUnused(itemId);
    // ストリーミング再生に統一したため、バッファ管理不要
}

void StateManager::syncWithAPVTS(juce::AudioProcessorValueTreeState& apvts)
{
    

    // Direct APVTS attachment only
    attachedAPVTS = &apvts;
    
}

void StateManager::detachFromAPVTS()
{
    attachedAPVTS = nullptr;
}

bool StateManager::validateState() const
{
#if DEBUG
    const juce::ScopedLock sl(stateLock);
    
    // Validate main state tree
    if (!state.isValid())
    {
        return false;
    }
    
    // Validate transport state
    auto transport = state.getChildWithName(transportType);
    if (!transport.isValid())
    {
        return false;
    }
    
    // Check transport properties
    if (!transport.hasProperty(propIsPlaying) ||
        !transport.hasProperty(propPosition))
    {
        return false;
    }
    
    // Validate loop range
    if (transport.hasProperty(propLoopEnabled) &&
        static_cast<bool>(transport.getProperty(propLoopEnabled)))
    {
        double loopStart = static_cast<double>(transport.getProperty(propLoopStart, 0.0));
        double loopEnd = static_cast<double>(transport.getProperty(propLoopEnd, 0.0));
        
        if (loopEnd <= loopStart)
        {
            return false;
        }
    }
    
    // Validate playlist
    auto playlist = state.getChildWithName(playlistType);
    if (playlist.isValid())
    {
        juce::StringArray ids;
        for (auto child : playlist)
        {
            juce::String id = child.getProperty(propFileId, "").toString();
            if (id.isEmpty())
            {
                return false;
            }
            
            // Check for duplicate IDs
            if (ids.contains(id))
            {
                return false;
            }
            ids.add(id);
        }
        
        // Active file id tracking removed
    }
    
    return true;
#else
    // In release builds, always return true
    return true;
#endif
}

void StateManager::initializeWithManagers(juce::AudioProcessorValueTreeState& apvts)
{
    syncWithAPVTS(apvts);
}



void StateManager::updateSnapshotFromState()
{
    // Update the cached snapshot from current state
    // This is handled by createSnapshot() method
}

void StateManager::notifyListeners(const std::function<void(Listener&)>& callback)
{
    listeners.call(callback);
}

} // namespace MixCompare