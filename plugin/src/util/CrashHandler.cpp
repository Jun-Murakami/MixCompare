// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "CrashHandler.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <DbgHelp.h>
#endif

namespace
{
#if JUCE_WINDOWS
bool writeMiniDump(EXCEPTION_POINTERS* exceptionPointers)
{
    auto dumpDir = mc3::CrashHandler::getDumpDirectory();
    dumpDir.createDirectory();

    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    const auto hostName = juce::SystemStats::getComputerName();
    const juce::String fileName = "MixCompareCrash_" + timestamp + "_" + hostName + ".dmp";

    auto dumpFile = dumpDir.getChildFile(fileName);
    HANDLE dumpHandle = ::CreateFileW(dumpFile.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (dumpHandle == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = ::GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const BOOL success = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                             ::GetCurrentProcessId(),
                                             dumpHandle,
                                             MiniDumpWithDataSegs,
                                             exceptionPointers != nullptr ? &exceptionInfo : nullptr,
                                             nullptr,
                                             nullptr);

    ::CloseHandle(dumpHandle);

    if (!success)
    {
        dumpFile.deleteFile();
        return false;
    }

    juce::Logger::writeToLog("Crash dump written to: " + dumpFile.getFullPathName());
    return true;
}
#endif

void writeTextDump(const juce::String& message)
{
    auto dumpDir = mc3::CrashHandler::getDumpDirectory();
    dumpDir.createDirectory();

    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    auto file = dumpDir.getChildFile("CrashLog_" + timestamp + ".txt");

    if (auto stream = file.createOutputStream())
    {
        stream->writeString(message + "\n");
        stream->flush();
        juce::Logger::writeToLog("Crash log written to: " + file.getFullPathName());
    }
}

void crashHandlerCallback(void* data)
{
#if JUCE_WINDOWS
    if (auto* exceptionPointers = static_cast<EXCEPTION_POINTERS*>(data))
    {
        if (writeMiniDump(exceptionPointers))
            return;
    }
#endif

    juce::String message = "MixCompare crash captured at "
        + juce::Time::getCurrentTime().toString(true, true);

   #if JUCE_MAC || JUCE_LINUX
    const auto signalNumber = static_cast<int>(reinterpret_cast<intptr_t>(data));
    if (signalNumber != 0)
        message += "\nSignal: " + juce::String(signalNumber);
   #endif

    writeTextDump(message);
}

} // namespace

namespace mc3
{

juce::File CrashHandler::getDumpDirectory()
{
   #if JUCE_MAC
    // JUCE v8 では userLibraryDirectory は提供されないため、
    // userApplicationDataDirectory（= ~/Library/Application Support）から親を辿って
    // ~/Library/Logs/MixCompare を構築する。
    auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getParentDirectory()      // ~/Library
                    .getChildFile("Logs")
                    .getChildFile("MixCompare");
   #else
    auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("MixCompare");
   #endif
    return base.getChildFile("CrashDumps");
}

void CrashHandler::install()
{
    static std::atomic<bool> installed{ false };
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true))
        return;

    juce::SystemStats::setApplicationCrashHandler(crashHandlerCallback);
    juce::Logger::writeToLog("CrashHandler installed. Dumps will be stored in "
        + getDumpDirectory().getFullPathName());
}

} // namespace mc3


