#pragma once

#include <JuceHeader.h>

#if JUCE_MAC
// macOS の物理メモリ情報取得に必要なヘッダーを明示的にインクルード
// - vm_statistics64_data_t, mach_port_t, host_page_size など: <mach/mach.h>
// - sysctlbyname("hw.memsize")                         : <sys/sysctl.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace MixCompare
{

/**
 * システム情報ユーティリティ
 * メモリやCPU情報を取得して、動的な制限を適用
 */
class SystemInfo
{
public:
    /**
     * 利用可能な物理メモリを取得（バイト単位）
     */
    static juce::int64 getAvailablePhysicalMemory()
    {
        #if JUCE_WINDOWS
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            return static_cast<juce::int64>(memStatus.ullAvailPhys);
        }
        #elif JUCE_MAC
        vm_size_t page_size;
        mach_port_t mach_port = mach_host_self();
        vm_statistics64_data_t vm_stat;
        mach_msg_type_number_t host_size = sizeof(vm_stat) / sizeof(natural_t);
        
        if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
            host_statistics64(mach_port, HOST_VM_INFO, (host_info64_t)&vm_stat, &host_size) == KERN_SUCCESS)
        {
            return static_cast<juce::int64>(vm_stat.free_count * page_size);
        }
        #endif
        
        // フォールバック：4GB
        return 4LL * 1024LL * 1024LL * 1024LL;
    }
    
    /**
     * 総物理メモリを取得（バイト単位）
     */
    static juce::int64 getTotalPhysicalMemory()
    {
        #if JUCE_WINDOWS
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            return static_cast<juce::int64>(memStatus.ullTotalPhys);
        }
        #elif JUCE_MAC
        int64_t memsize;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0)
        {
            return memsize;
        }
        #endif
        
        // フォールバック：8GB
        return 8LL * 1024LL * 1024LL * 1024LL;
    }
    
    /**
     * オーディオファイル用の推奨最大メモリサイズを取得
     * （利用可能メモリの50%、ただし最大4GB）
     */
    static juce::int64 getRecommendedMaxAudioBufferSize()
    {
        const juce::int64 availableMemory = getAvailablePhysicalMemory();
        const juce::int64 halfAvailable = availableMemory / 2;
        const juce::int64 maxAllowed = 4LL * 1024LL * 1024LL * 1024LL; // 4GB上限
        
        return juce::jmin(halfAvailable, maxAllowed);
    }
    
    /**
     * プレイリスト全体の推奨最大メモリサイズを取得
     * （総メモリの25%、ただし最大8GB）
     */
    static juce::int64 getRecommendedMaxPlaylistMemory()
    {
        const juce::int64 totalMemory = getTotalPhysicalMemory();
        const juce::int64 quarterTotal = totalMemory / 4;
        const juce::int64 maxAllowed = 8LL * 1024LL * 1024LL * 1024LL; // 8GB上限
        
        return juce::jmin(quarterTotal, maxAllowed);
    }
    
    /**
     * メモリ使用量の文字列表現
     */
    static juce::String formatMemorySize(juce::int64 bytes)
    {
        if (bytes < 1024)
            return juce::String(bytes) + " B";
        else if (bytes < 1024 * 1024)
            return juce::String(bytes / 1024.0, 1) + " KB";
        else if (bytes < 1024LL * 1024LL * 1024LL)
            return juce::String(bytes / (1024.0 * 1024.0), 2) + " MB";
        else
            return juce::String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }
};

} // namespace MixCompare