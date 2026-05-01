// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>

namespace MixCompare
{

/**
 * Type-safe helpers for ValueTree operations
 * 
 * Provides template-based getters and setters that handle type conversion
 * and validation for ValueTree properties, eliminating runtime type errors.
 */
class ValueTreeHelpers
{
public:
    // Property identifiers for state tree
    static const juce::Identifier STATE_ROOT;
    static const juce::Identifier PLAYLIST_NODE;
    static const juce::Identifier TRANSPORT_NODE;
    static const juce::Identifier SOURCE_NODE;
    
    // Playlist properties
    static const juce::Identifier PLAYLIST_FILES;
    static const juce::Identifier PLAYLIST_CURRENT_INDEX;
    static const juce::Identifier PLAYLIST_CURRENT_ID;
    
    // Transport properties  
    static const juce::Identifier TRANSPORT_PLAYING;
    static const juce::Identifier TRANSPORT_POSITION;
    static const juce::Identifier TRANSPORT_DURATION;
    static const juce::Identifier TRANSPORT_LOOP_ENABLED;
    static const juce::Identifier TRANSPORT_LOOP_START;
    static const juce::Identifier TRANSPORT_LOOP_END;
    
    // Source properties
    static const juce::Identifier SOURCE_CURRENT;
    static const juce::Identifier SOURCE_HOST_GAIN;
    static const juce::Identifier SOURCE_PLAYLIST_GAIN;
    static const juce::Identifier SOURCE_LPF_ENABLED;
    static const juce::Identifier SOURCE_LPF_FREQ;
    
    // Generic property getter with type conversion
    template<typename T>
    static T getProperty(const juce::ValueTree& tree, 
                        const juce::Identifier& propertyName, 
                        T defaultValue)
    {
        if (!tree.isValid())
            return defaultValue;
            
        const juce::var& value = tree.getProperty(propertyName);
        
        if (value.isVoid())
            return defaultValue;
            
        return convertVar<T>(value, defaultValue);
    }
    
    // Generic property setter with validation
    template<typename T>
    static void setProperty(juce::ValueTree& tree,
                           const juce::Identifier& propertyName,
                           T value,
                           juce::UndoManager* undoManager = nullptr)
    {
        if (!tree.isValid())
            return;
            
        tree.setProperty(propertyName, juce::var(value), undoManager);
    }
    
    // Get child node, creating if necessary
    static juce::ValueTree getOrCreateChild(juce::ValueTree& parent,
                                           const juce::Identifier& childType)
    {
        auto child = parent.getChildWithName(childType);
        
        if (!child.isValid())
        {
            child = juce::ValueTree(childType);
            parent.appendChild(child, nullptr);
        }
        
        return child;
    }
    
    // Specialized getters for common types
    
    static bool getBool(const juce::ValueTree& tree,
                       const juce::Identifier& propertyName,
                       bool defaultValue = false)
    {
        return getProperty<bool>(tree, propertyName, defaultValue);
    }
    
    static int getInt(const juce::ValueTree& tree,
                     const juce::Identifier& propertyName,
                     int defaultValue = 0)
    {
        return getProperty<int>(tree, propertyName, defaultValue);
    }
    
    static float getFloat(const juce::ValueTree& tree,
                         const juce::Identifier& propertyName,
                         float defaultValue = 0.0f)
    {
        return getProperty<float>(tree, propertyName, defaultValue);
    }
    
    static double getDouble(const juce::ValueTree& tree,
                           const juce::Identifier& propertyName,
                           double defaultValue = 0.0)
    {
        return getProperty<double>(tree, propertyName, defaultValue);
    }
    
    static juce::String getString(const juce::ValueTree& tree,
                                 const juce::Identifier& propertyName,
                                 const juce::String& defaultValue = {})
    {
        return getProperty<juce::String>(tree, propertyName, defaultValue);
    }
    
    // Range validation helpers
    
    template<typename T>
    static T clampValue(T value, T minValue, T maxValue)
    {
        return juce::jlimit(minValue, maxValue, value);
    }
    
    static float clampGain(float gainDb)
    {
        return clampValue(gainDb, -60.0f, 12.0f);
    }
    
    static float clampFrequency(float freq)
    {
        return clampValue(freq, 20.0f, 20000.0f);
    }
    
    static double clampPosition(double position, double duration)
    {
        return clampValue(position, 0.0, juce::jmax(0.0, duration));
    }
    
    // Create default state tree structure
    static juce::ValueTree createDefaultStateTree()
    {
        juce::ValueTree root(STATE_ROOT);
        
        // Create main nodes
        juce::ValueTree playlist(PLAYLIST_NODE);
        playlist.setProperty(PLAYLIST_CURRENT_INDEX, -1, nullptr);
        playlist.setProperty(PLAYLIST_CURRENT_ID, "", nullptr);
        
        juce::ValueTree transport(TRANSPORT_NODE);
        transport.setProperty(TRANSPORT_PLAYING, false, nullptr);
        transport.setProperty(TRANSPORT_POSITION, 0.0, nullptr);
        transport.setProperty(TRANSPORT_DURATION, 0.0, nullptr);
        transport.setProperty(TRANSPORT_LOOP_ENABLED, false, nullptr);
        transport.setProperty(TRANSPORT_LOOP_START, 0.0, nullptr);
        transport.setProperty(TRANSPORT_LOOP_END, 1.0, nullptr);
        
        juce::ValueTree source(SOURCE_NODE);
        source.setProperty(SOURCE_CURRENT, 0, nullptr);
        source.setProperty(SOURCE_HOST_GAIN, 0.0f, nullptr);
        source.setProperty(SOURCE_PLAYLIST_GAIN, 0.0f, nullptr);
        source.setProperty(SOURCE_LPF_ENABLED, false, nullptr);
        source.setProperty(SOURCE_LPF_FREQ, 20000.0f, nullptr);
        
        root.appendChild(playlist, nullptr);
        root.appendChild(transport, nullptr);
        root.appendChild(source, nullptr);
        
        return root;
    }
    
    // Validate tree structure
    static bool isValidStateTree(const juce::ValueTree& tree)
    {
        if (!tree.isValid() || tree.getType() != STATE_ROOT)
            return false;
            
        return tree.getChildWithName(PLAYLIST_NODE).isValid() &&
               tree.getChildWithName(TRANSPORT_NODE).isValid() &&
               tree.getChildWithName(SOURCE_NODE).isValid();
    }
    
private:
    // Type conversion specializations
    // テンプレート特殊化前のジェネリック実装（通常は使用されない）
    template<typename T>
    static T convertVar(const juce::var& value, T /*defaultValue*/)
    {
        // Generic conversion, will be specialized below
        // defaultValue はテンプレート特殊化で使用される
        return static_cast<T>(value);
    }
};

// Template specializations for type conversion
template<>
inline bool ValueTreeHelpers::convertVar<bool>(const juce::var& value, bool defaultValue)
{
    if (value.isBool())
        return static_cast<bool>(value);
    if (value.isInt() || value.isInt64())
        return static_cast<int>(value) != 0;
    if (value.isDouble())
        return static_cast<double>(value) != 0.0;
    return defaultValue;
}

template<>
inline int ValueTreeHelpers::convertVar<int>(const juce::var& value, int defaultValue)
{
    if (value.isInt() || value.isInt64())
        return static_cast<int>(value);
    if (value.isDouble())
        return static_cast<int>(static_cast<double>(value));
    if (value.isBool())
        return static_cast<bool>(value) ? 1 : 0;
    return defaultValue;
}

template<>
inline float ValueTreeHelpers::convertVar<float>(const juce::var& value, float defaultValue)
{
    if (value.isDouble() || value.isInt() || value.isInt64())
        return static_cast<float>(static_cast<double>(value));
    return defaultValue;
}

template<>
inline double ValueTreeHelpers::convertVar<double>(const juce::var& value, double defaultValue)
{
    if (value.isDouble() || value.isInt() || value.isInt64())
        return static_cast<double>(value);
    return defaultValue;
}

template<>
inline juce::String ValueTreeHelpers::convertVar<juce::String>(const juce::var& value, juce::String defaultValue)
{
    if (value.isString())
        return value.toString();
    if (!value.isVoid())
        return value.toString();
    return defaultValue;
}

} // namespace MixCompare