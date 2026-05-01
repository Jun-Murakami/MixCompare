// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <memory>
#include <atomic>
#if JUCE_WINDOWS
#include "../audio/MediaFoundationAACFormat.h"
#endif

namespace MixCompare
{

class StateManager; // Forward declaration

class PlaylistManager
{
public:
    struct PlaylistItem
    {
        juce::String id;
        juce::File file;
        juce::String displayName;
        double duration{0.0};
        double sampleRate{0.0};
        int bitDepth{0};
        int numChannels{0};
        bool isLoaded{false};
        
        PlaylistItem() = default;
        PlaylistItem(const juce::String& itemId, const juce::File& itemFile)
            : id(itemId), file(itemFile), displayName(itemFile.getFileName()) {}
    };

    PlaylistManager(StateManager* stateManager);
    ~PlaylistManager();

    juce::Array<juce::String> addFiles(const juce::Array<juce::File>& files);
    juce::Array<juce::String> addFiles(const juce::StringArray& filePaths);
    
    bool removeItem(const juce::String& itemId);
    void removeAllItems();
    
    void reorderItems(const juce::Array<juce::String>& orderedIds);
    
    bool selectItem(const juce::String& itemId);
    bool selectItemByIndex(int index);
    bool selectNext();
    bool selectPrevious();
    
    juce::String getCurrentItemId() const;
    int getCurrentIndex() const;
    PlaylistItem* getCurrentItem();
    const PlaylistItem* getCurrentItem() const;
    
    PlaylistItem* getItem(const juce::String& itemId);
    const PlaylistItem* getItem(const juce::String& itemId) const;
    
    PlaylistItem* getItemByIndex(int index);
    const PlaylistItem* getItemByIndex(int index) const;
    
    int getNumItems() const { return itemsCache.size(); }
    juce::Array<PlaylistItem> getAllItems() const;
    juce::Array<juce::String> getAllItemIds() const;
    
    /** 現在アイテムのファイルサイズ（バイト）。不明時は 0。 */
    uint64_t getCurrentFileSizeBytes() const { return currentFileSizeBytes; }
    
    juce::ValueTree getState() const;
    void setState(const juce::ValueTree& state);
    
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playlistItemsChanged() {}
        virtual void currentItemChanged(const juce::String& itemId) { juce::ignoreUnused(itemId); }
    };
    
    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    juce::String toJSON() const;
    void fromJSON(const juce::String& json);
    
    bool isValidIndex(int index) const;
    
    // Sync cache with StateManager
    void syncCacheFromState();

private:
    StateManager* stateManager{nullptr};
    
    // Cache for loaded audio buffers (not the source of truth)
    juce::Array<PlaylistItem> itemsCache;
    
    mutable juce::CriticalSection itemsLock;
    juce::AudioFormatManager formatManager;
    
    juce::ListenerList<Listener> listeners;
    
    uint64_t currentFileSizeBytes{0};
    
    juce::String generateUniqueId() const;
    void updateItemDuration(PlaylistItem& item);
    void notifyItemsChanged();
    void notifyCurrentChanged();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaylistManager)
};

} // namespace MixCompare