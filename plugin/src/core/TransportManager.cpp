#include "TransportManager.h"
#include "StateManager.h"

namespace MixCompare
{

TransportManager::TransportManager(StateManager* sm)
    : stateManager(sm)
{
    jassert(stateManager != nullptr);
}

TransportManager::~TransportManager()
{
}

void TransportManager::play()
{
    if (stateManager && !isPlaying())
    {
        stateManager->setTransportPlaying(true);
        notifyStateChange(TransportState::Playing);
    }
}

void TransportManager::pause()
{
    if (stateManager && isPlaying())
    {
        stateManager->setTransportPlaying(false);
        notifyStateChange(TransportState::Paused);
    }
}

void TransportManager::stop()
{
    if (stateManager)
    {
        stateManager->setTransportPlaying(false);
        stateManager->setTransportPosition(0.0);
        notifyStateChange(TransportState::Stopped);
        notifyPositionChange(0.0);
    }
}

void TransportManager::seek(double positionInSeconds)
{
    if (!stateManager)
        return;
        
    double duration = getDuration();
    // duration 未確定時はクランプせず、そのまま反映（0への誤クランプを防ぐ）
    double clampedPosition = (duration > 0.0) ? juce::jlimit(0.0, duration, positionInSeconds)
                                             : positionInSeconds;
    
    stateManager->setTransportPosition(clampedPosition);
    notifyPositionChange(clampedPosition);
    
    if (positionCallback)
    {
        positionCallback(clampedPosition);
    }
}

void TransportManager::setLoopEnabled(bool enabled)
{
    if (stateManager)
    {
        stateManager->setLoopEnabled(enabled);
        notifyLoopChange();
    }
}

void TransportManager::setLoopRange(double startInSeconds, double endInSeconds)
{
    if (stateManager)
    {
        stateManager->setLoopRange(startInSeconds, endInSeconds);
        notifyLoopChange();
    }
}

bool TransportManager::isPlaying() const
{
    return stateManager ? stateManager->isPlaying() : false;
}

bool TransportManager::isPaused() const
{
    // No explicit paused state in StateManager, so it's "not playing and position > 0"
    if (!stateManager)
        return false;
        
    return !stateManager->isPlaying() && stateManager->getPosition() > 0.0;
}

bool TransportManager::isStopped() const
{
    // Stopped means not playing and position at 0
    if (!stateManager)
        return true;
        
    return !stateManager->isPlaying() && stateManager->getPosition() == 0.0;
}

TransportManager::TransportState TransportManager::getState() const
{
    if (isPlaying())
        return TransportState::Playing;
    else if (isPaused())
        return TransportState::Paused;
    else
        return TransportState::Stopped;
}

double TransportManager::getPosition() const
{
    return stateManager ? stateManager->getPosition() : 0.0;
}

void TransportManager::setPosition(double positionInSeconds)
{
    if (stateManager)
    {
        stateManager->setTransportPosition(positionInSeconds);
        notifyPositionChange(positionInSeconds);
    }
}

bool TransportManager::isLoopEnabled() const
{
    if (!stateManager)
        return false;
        
    auto transportTree = stateManager->getTransportState();
    if (transportTree.isValid())
    {
        return transportTree.getProperty("loopEnabled", false);
    }
    return false;
}

double TransportManager::getLoopStart() const
{
    if (!stateManager)
        return 0.0;
        
    auto transportTree = stateManager->getTransportState();
    if (transportTree.isValid())
    {
        return transportTree.getProperty("loopStart", 0.0);
    }
    return 0.0;
}

double TransportManager::getLoopEnd() const
{
    if (!stateManager)
        return 0.0;
        
    auto transportTree = stateManager->getTransportState();
    if (transportTree.isValid())
    {
        return transportTree.getProperty("loopEnd", 1.0);
    }
    return 1.0;
}

void TransportManager::setDuration(double durationInSeconds)
{
    if (!stateManager)
        return;
        
    // Duration is stored in transport state
    auto transportTree = stateManager->getState().getChildWithName("Transport");
    if (!transportTree.isValid())
    {
        transportTree = juce::ValueTree("Transport");
        stateManager->getState().appendChild(transportTree, nullptr);
    }
    
    transportTree.setProperty("duration", durationInSeconds, nullptr);
}

double TransportManager::getDuration() const
{
    if (!stateManager)
        return 0.0;
        
    auto transportTree = stateManager->getTransportState();
    if (transportTree.isValid())
    {
        return transportTree.getProperty("duration", 0.0);
    }
    return 0.0;
}

void TransportManager::updatePosition(double deltaSeconds)
{
    if (!stateManager || !isPlaying())
        return;
    
    double currentPosition = getPosition();
    double newPosition = currentPosition + deltaSeconds;
    
    // Handle looping
    if (isLoopEnabled())
    {
        double loopEnd = getLoopEnd();
        double loopStart = getLoopStart();
        
        if (newPosition >= loopEnd && loopEnd > loopStart)
        {
            // Wrap back to loop start
            double overshoot = newPosition - loopEnd;
            double loopLength = loopEnd - loopStart;
            newPosition = loopStart + std::fmod(overshoot, loopLength);
        }
    }
    else
    {
        // Clamp to duration
        double duration = getDuration();
        if (newPosition >= duration)
        {
            newPosition = duration;
            // 終端に到達：同一フレームで isPlaying=false と position=duration を確定通知
            pause();
            setPosition(newPosition);
        }
    }
    
    setPosition(newPosition);
}

bool TransportManager::shouldLoop() const
{
    if (!isLoopEnabled())
        return false;
        
    double position = getPosition();
    double loopEnd = getLoopEnd();
    
    return position >= loopEnd;
}

void TransportManager::reset()
{
    stop();
}

void TransportManager::setPositionUpdateCallback(PositionCallback callback)
{
    positionCallback = callback;
}

void TransportManager::notifyStateChange(TransportState newState)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = this, newState]()
        {
            if (safe)
                safe->listeners.call(&Listener::transportStateChanged, newState);
        });
        return;
    }
    listeners.call(&Listener::transportStateChanged, newState);
}

void TransportManager::notifyPositionChange(double newPosition)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = this, newPosition]()
        {
            if (safe)
                safe->listeners.call(&Listener::transportPositionChanged, newPosition);
        });
        return;
    }
    listeners.call(&Listener::transportPositionChanged, newPosition);
}

void TransportManager::notifyLoopChange()
{
    bool enabled = isLoopEnabled();
    double start = getLoopStart();
    double end = getLoopEnd();
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = this, enabled, start, end]()
        {
            if (safe)
                safe->listeners.call(&Listener::loopStateChanged, enabled, start, end);
        });
        return;
    }
    listeners.call(&Listener::loopStateChanged, enabled, start, end);
}

void TransportManager::addListener(Listener* listener)
{
    listeners.add(listener);
}

void TransportManager::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

} // namespace MixCompare