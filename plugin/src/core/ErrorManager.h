// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <functional>
#include <atomic>

namespace MixCompare
{

/**
 * エラーの重要度レベル
 */
enum class ErrorSeverity
{
    Info,       // 情報（ユーザーに通知するが、動作に影響なし）
    Warning,    // 警告（一部機能が制限される可能性）
    Error,      // エラー（操作失敗、ただし復旧可能）
    Critical    // 致命的（プラグインの安定性に影響）
};

/**
 * エラーコード定義
 */
enum class ErrorCode
{
    // ファイル関連
    FileNotFound = 1000,
    FileReadError,
    FileFormatNotSupported,
    FileCorrupted,
    FileTooLarge,
    FileAccessDenied,
    FileDeleted,
    FileMoved,
    
    // オーディオ処理関連
    AudioFormatError = 2000,
    AudioDecodingError,
    AudioBufferOverflow,
    SampleRateMismatch,
    ChannelConfigError,
    
    // プレイリスト関連
    PlaylistItemNotFound = 3000,
    PlaylistEmpty,
    PlaylistLoadError,
    
    // メモリ関連
    OutOfMemory = 4000,
    BufferAllocationFailed,
    
    // Media Foundation関連
    MediaFoundationInitError = 5000,
    MediaFoundationDecodeError,
    MediaFoundationNotAvailable,
    
    // その他
    UnknownError = 9000
};

/**
 * エラー情報構造体
 */
struct ErrorInfo
{
    ErrorCode code;
    ErrorSeverity severity;
    juce::String message;
    juce::String details;
    juce::String filePath;  // ファイル関連エラーの場合
    juce::Time timestamp;
    
    ErrorInfo() = default;
    
    ErrorInfo(ErrorCode c, ErrorSeverity s, const juce::String& msg, 
              const juce::String& det = "", const juce::String& file = "")
        : code(c), severity(s), message(msg), details(det), filePath(file),
          timestamp(juce::Time::getCurrentTime()) {}
    
    juce::String toJSON() const;
};

/**
 * エラー管理クラス
 * シングルトンパターンで実装し、プラグイン全体のエラーを一元管理
 */
class ErrorManager
{
public:
    using ErrorCallback = std::function<void(const ErrorInfo&)>;
    
    static ErrorManager& getInstance();
    
    // エラー報告
    void reportError(ErrorCode code, const juce::String& message, 
                    const juce::String& details = "", const juce::String& filePath = "");
    void reportWarning(ErrorCode code, const juce::String& message,
                      const juce::String& details = "", const juce::String& filePath = "");
    void reportInfo(const juce::String& message, const juce::String& details = "");
    void reportCritical(ErrorCode code, const juce::String& message,
                       const juce::String& details = "", const juce::String& filePath = "");
    
    // エラー処理結果を返すヘルパー
    template<typename T>
    struct Result
    {
        bool success;
        T value;
        ErrorInfo error;
        
        Result(T val) : success(true), value(std::move(val)) {}
        Result(ErrorInfo err) : success(false), value{}, error(std::move(err)) {}
        
        operator bool() const { return success; }
        T& operator*() { return value; }
        const T& operator*() const { return value; }
    };
    
    // コールバック登録（WebUIへの通知用）
    void setErrorCallback(ErrorCallback callback);
    void clearErrorCallback();
    
    // エラー履歴
    juce::Array<ErrorInfo> getRecentErrors(int maxCount = 10) const;
    void clearErrorHistory();
    
    // エラー統計
    int getErrorCount(ErrorSeverity severity) const;
    bool hasRecentCriticalErrors() const;
    
private:
    ErrorManager() = default;
    ~ErrorManager() = default;
    
    ErrorManager(const ErrorManager&) = delete;
    ErrorManager& operator=(const ErrorManager&) = delete;
    
    void notifyError(const ErrorInfo& error);
    
    mutable juce::CriticalSection errorLock;
    juce::Array<ErrorInfo> errorHistory;
    ErrorCallback errorCallback;
    
    std::atomic<int> infoCount{0};
    std::atomic<int> warningCount{0};
    std::atomic<int> errorCount{0};
    std::atomic<int> criticalCount{0};
    
    static constexpr int MAX_ERROR_HISTORY = 100;
};

/**
 * RAIIパターンによるエラーガード
 * スコープ内でエラーが発生した場合、自動的にクリーンアップ
 */
class ErrorGuard
{
public:
    ErrorGuard(const juce::String& operation);
    ~ErrorGuard();
    
    void success();
    void failure(ErrorCode code, const juce::String& message);
    
private:
    juce::String operationName;
    bool succeeded = false;
};

} // namespace MixCompare