// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>

namespace MixCompare
{

class StateManager; // Forward declaration

class TransportManager
{
public:
    TransportManager(StateManager* stateManager);
    ~TransportManager();

    enum class TransportState
    {
        Stopped,
        Playing,
        Paused
    };

    void play();
    void pause();
    void stop();
    void seek(double positionInSeconds);
    
    void setLoopEnabled(bool enabled);
    void setLoopRange(double startInSeconds, double endInSeconds);
    
    bool isPlaying() const;
    bool isPaused() const;
    bool isStopped() const;
    TransportState getState() const;
    
    double getPosition() const;
    void setPosition(double positionInSeconds);
    
    bool isLoopEnabled() const;
    double getLoopStart() const;
    double getLoopEnd() const;
    
    void setDuration(double durationInSeconds);
    double getDuration() const;
    
    void updatePosition(double deltaSeconds);
    
    bool shouldLoop() const;
    double getLoopRestartPosition() const;
    
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void transportStateChanged(TransportState newState) { juce::ignoreUnused(newState); }
        // 引数名がメンバ position と衝突してシャドー警告になるため、引数名を変更
        virtual void transportPositionChanged(double newPosition) { juce::ignoreUnused(newPosition); }
        virtual void loopStateChanged(bool enabled, double start, double end) { juce::ignoreUnused(enabled, start, end); }
    };
    
    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    struct TransportInfo
    {
        TransportState state{TransportState::Stopped};
        double position{0.0};
        double duration{0.0};
        bool loopEnabled{false};
        double loopStart{0.0};
        double loopEnd{0.0};
        
        juce::String toJSONString() const;
        static TransportInfo fromJSONString(const juce::String& json);
    };
    
    TransportInfo getTransportInfo() const;
    void setTransportInfo(const TransportInfo& info);

    void reset();
    
    using PositionCallback = std::function<void(double)>;
    void setPositionUpdateCallback(PositionCallback callback);

private:
    StateManager* stateManager{nullptr};
    
    juce::ListenerList<Listener> listeners;
    PositionCallback positionCallback;
    
    void notifyStateChange(TransportState newState);
    void notifyPositionChange(double newPosition);
    void notifyLoopChange();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportManager)
};

} // namespace MixCompare