// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "ValueTreeHelpers.h"

namespace MixCompare
{

// Define static property identifiers
const juce::Identifier ValueTreeHelpers::STATE_ROOT("state");
const juce::Identifier ValueTreeHelpers::PLAYLIST_NODE("playlist");
const juce::Identifier ValueTreeHelpers::TRANSPORT_NODE("transport");
const juce::Identifier ValueTreeHelpers::SOURCE_NODE("source");

// Playlist properties
const juce::Identifier ValueTreeHelpers::PLAYLIST_FILES("files");
const juce::Identifier ValueTreeHelpers::PLAYLIST_CURRENT_INDEX("currentIndex");
const juce::Identifier ValueTreeHelpers::PLAYLIST_CURRENT_ID("currentItemId");

// Transport properties
const juce::Identifier ValueTreeHelpers::TRANSPORT_PLAYING("playing");
const juce::Identifier ValueTreeHelpers::TRANSPORT_POSITION("position");
const juce::Identifier ValueTreeHelpers::TRANSPORT_DURATION("duration");
const juce::Identifier ValueTreeHelpers::TRANSPORT_LOOP_ENABLED("loopEnabled");
const juce::Identifier ValueTreeHelpers::TRANSPORT_LOOP_START("loopStart");
const juce::Identifier ValueTreeHelpers::TRANSPORT_LOOP_END("loopEnd");

// Source properties
const juce::Identifier ValueTreeHelpers::SOURCE_CURRENT("currentSource");
const juce::Identifier ValueTreeHelpers::SOURCE_HOST_GAIN("hostGain");
const juce::Identifier ValueTreeHelpers::SOURCE_PLAYLIST_GAIN("playlistGain");
const juce::Identifier ValueTreeHelpers::SOURCE_LPF_ENABLED("lpfEnabled");
const juce::Identifier ValueTreeHelpers::SOURCE_LPF_FREQ("lpfFreq");

} // namespace MixCompare