// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <memory>
#include <atomic>
#include "ValueTreeHelpers.h"


namespace MixCompare
{

class AudioEngine;
class MeteringService;

}

#include "TransportManager.h"
#include "PlaylistManager.h"

namespace MixCompare
{

/**
 * Central state coordinator for the plugin
 *
 * Delegates responsibilities to specialized managers:
 * - TransportManager: Playback control
 * - PlaylistManager: Playlist operations
 *
 * Maintains backward compatibility while focusing on coordination
 */
class StateManager : public juce::ValueTree::Listener,
                     public TransportManager::Listener,
                     public PlaylistManager::Listener
{
public:
    StateManager();
    ~StateManager() override;

    /**
     * State access - maintained for backward compatibility
     */
    juce::ValueTree& getState() { return state; }
    const juce::ValueTree& getState() const { return state; }

    /**
     * Create a thread-safe snapshot for real-time audio processing
     */
    // StateSnapshot removed

    /**
     * State validation for debug builds
     */
    bool validateState() const;

    /**
     * Initialize with managers - replaces direct APVTS handling
     */
    void initializeWithManagers(juce::AudioProcessorValueTreeState& apvts);

    /**
     * APVTS synchronization - delegates to ParameterManager
     * Maintained for backward compatibility
     */
    void syncWithAPVTS(juce::AudioProcessorValueTreeState& apvts);
    void detachFromAPVTS();

    /**
     * Playlist operations - maintained interface
     */
    juce::ValueTree getPlaylistState() const;
    void addPlaylistItem(const juce::File& file);
    void removePlaylistItem(const juce::Identifier& itemId);
    void reorderPlaylistItems(const juce::Array<juce::Identifier>& newOrder);
    void clearPlaylist();

    /**
     * Transport operations - maintained interface
     */
    juce::ValueTree getTransportState() const;
    void setTransportPlaying(bool shouldPlay);
    void setTransportPosition(double position);
    void setLoopEnabled(bool enabled);
    void setLoopRange(double start, double end);
    bool isPlaying() const;
    double getPosition() const;

    void setActiveFileId(const juce::Identifier& fileId);
    juce::Identifier getActiveFileId() const;



    // ValueTree::Listener
    void valueTreePropertyChanged(juce::ValueTree& treeWhosePropertyHasChanged,
                                 const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree& parentTree,
                            juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved(juce::ValueTree& parentTree,
                               juce::ValueTree& childWhichHasBeenRemoved,
                               int indexFromWhichChildWasRemoved) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parentTreeWhoseChildrenHaveMoved,
                                   int oldIndex, int newIndex) override;
    void valueTreeParentChanged(juce::ValueTree& treeWhoseParentHasChanged) override;

    // TransportManager::Listener
    void transportStateChanged(TransportManager::TransportState newState) override;
    void transportPositionChanged(double newPosition) override;
    void loopStateChanged(bool enabled, double start, double end) override;

    // PlaylistManager::Listener
    void playlistItemsChanged() override;
    void currentItemChanged(const juce::String& itemId) override;

    // APVTSはSnapshot作成時に直接読み取り

    /**
     * State change listener interface
     */
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void stateChanged(const juce::ValueTree& newState) { juce::ignoreUnused(newState); }
        virtual void playlistChanged() {}
        virtual void transportStateChanged() {}
    };

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    /**
     * Message handling for WebUI communication
     */
    struct Message
    {
        juce::String type;
        juce::var payload;
        juce::String nonce;
    };

    juce::var handleMessage(const Message& message);
    static juce::var createResponse(const juce::var& data, const juce::String& nonce = {});

    /**
     * Manager setup and access
     */
    void setupManagers(AudioEngine* engine,
                      TransportManager* transport,
                      PlaylistManager* playlist,
                      MeteringService* metering);

    AudioEngine* getAudioEngine() const { return audioEngine; }
    TransportManager* getTransportManager() const { return transportManager; }
    PlaylistManager* getPlaylistManager() const { return playlistManager; }
    MeteringService* getMeteringService() const { return meteringService; }

    /**
     * Reset momentary hold values in metering
     */
    void resetMomentaryHold();

private:
    // Referenced managers (not owned)
    AudioEngine* audioEngine{nullptr};
    TransportManager* transportManager{nullptr};
    PlaylistManager* playlistManager{nullptr};
    MeteringService* meteringService{nullptr};

    // State management
    mutable juce::CriticalSection stateLock;
    juce::ValueTree state;
    juce::ListenerList<Listener> listeners;

    // Synchronization flags
    std::atomic<bool> updatingFromManager{false};
    std::atomic<bool> updatingToManager{false};

    // APVTS synchronization
    juce::AudioProcessorValueTreeState* attachedAPVTS{nullptr};
    std::atomic<bool> updatingFromAPVTS{false};
    std::atomic<bool> updatingToAPVTS{false};

    // State tree identifiers
    static const juce::Identifier treeType;
    static const juce::Identifier playlistType;
    static const juce::Identifier transportType;
    static const juce::Identifier playlistItemType;

    // Property identifiers
    static const juce::Identifier propIsPlaying;
    static const juce::Identifier propPosition;
    static const juce::Identifier propLoopEnabled;
    static const juce::Identifier propLoopStart;
    static const juce::Identifier propLoopEnd;
    static const juce::Identifier propActiveFileId;
    static const juce::Identifier propFilePath;
    static const juce::Identifier propFileName;
    static const juce::Identifier propFileId;

    void initializeState();
    void notifyListeners(const std::function<void(Listener&)>& callback);
    static juce::Identifier generateUniqueId();

    juce::var handlePlaylistMessage(const juce::String& action, const juce::var& data);

    void syncStateFromManagers();
    void syncParametersToState(const juce::ParameterID& id, float value);
    void updateSnapshotFromState();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StateManager)
};

} // namespace MixCompare