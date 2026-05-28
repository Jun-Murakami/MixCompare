// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "FileDialog.h"

#import <Cocoa/Cocoa.h>

namespace mc
{

std::vector<std::string> PromptForAudioFiles(void* /*nativeParent*/)
{
  std::vector<std::string> result;
  @autoreleasepool
  {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:NO];
    [panel setCanChooseFiles:YES];
    [panel setAllowedFileTypes:@[ @"wav", @"flac", @"mp3", @"ogg", @"m4a", @"aac", @"mp4", @"ape" ]];

    if ([panel runModal] == NSModalResponseOK)
    {
      for (NSURL* url in [panel URLs])
      {
        const char* p = [[url path] UTF8String];
        if (p) result.emplace_back(p);
      }
    }
  }
  return result;
}

bool PromptForSavePlaylist(void* /*nativeParent*/, std::string& outPathUtf8)
{
  @autoreleasepool
  {
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setAllowedFileTypes:@[ @"m3u8" ]];
    [panel setNameFieldStringValue:@"playlist.m3u8"];
    if ([panel runModal] == NSModalResponseOK)
    {
      const char* p = [[[panel URL] path] UTF8String];
      if (p) { outPathUtf8 = p; return true; }
    }
  }
  return false;
}

bool PromptForOpenPlaylist(void* /*nativeParent*/, std::string& outPathUtf8)
{
  @autoreleasepool
  {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:NO];
    [panel setCanChooseDirectories:NO];
    [panel setCanChooseFiles:YES];
    [panel setAllowedFileTypes:@[ @"m3u8" ]];
    if ([panel runModal] == NSModalResponseOK)
    {
      NSArray<NSURL*>* urls = [panel URLs];
      if ([urls count] > 0)
      {
        const char* p = [[[urls objectAtIndex:0] path] UTF8String];
        if (p) { outPathUtf8 = p; return true; }
      }
    }
  }
  return false;
}

} // namespace mc
