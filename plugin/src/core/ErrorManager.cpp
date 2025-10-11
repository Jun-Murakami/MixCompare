#include "ErrorManager.h"

namespace MixCompare
{

juce::String ErrorInfo::toJSON() const
{
    auto obj = juce::DynamicObject::Ptr(new juce::DynamicObject());
    
    obj->setProperty("code", static_cast<int>(code));
    obj->setProperty("severity", 
        severity == ErrorSeverity::Info ? "info" :
        severity == ErrorSeverity::Warning ? "warning" :
        severity == ErrorSeverity::Error ? "error" : "critical");
    obj->setProperty("message", message);
    obj->setProperty("details", details);
    obj->setProperty("filePath", filePath);
    obj->setProperty("timestamp", timestamp.toISO8601(true));
    
    return juce::JSON::toString(juce::var(obj.get()));
}

ErrorManager& ErrorManager::getInstance()
{
    static ErrorManager instance;
    return instance;
}

void ErrorManager::reportError(ErrorCode code, const juce::String& message,
                              const juce::String& details, const juce::String& filePath)
{
    ErrorInfo error(code, ErrorSeverity::Error, message, details, filePath);
    
    {
        const juce::ScopedLock sl(errorLock);
        errorHistory.insert(0, error);
        if (errorHistory.size() > MAX_ERROR_HISTORY)
            errorHistory.removeLast();
    }
    
    errorCount++;
    notifyError(error);
}

void ErrorManager::reportWarning(ErrorCode code, const juce::String& message,
                                const juce::String& details, const juce::String& filePath)
{
    ErrorInfo error(code, ErrorSeverity::Warning, message, details, filePath);
    
    {
        const juce::ScopedLock sl(errorLock);
        errorHistory.insert(0, error);
        if (errorHistory.size() > MAX_ERROR_HISTORY)
            errorHistory.removeLast();
    }
    
    warningCount++;
    notifyError(error);
}

void ErrorManager::reportInfo(const juce::String& message, const juce::String& details)
{
    ErrorInfo error(ErrorCode::UnknownError, ErrorSeverity::Info, message, details);
    
    {
        const juce::ScopedLock sl(errorLock);
        errorHistory.insert(0, error);
        if (errorHistory.size() > MAX_ERROR_HISTORY)
            errorHistory.removeLast();
    }
    
    infoCount++;
    notifyError(error);
}

void ErrorManager::reportCritical(ErrorCode code, const juce::String& message,
                                 const juce::String& details, const juce::String& filePath)
{
    ErrorInfo error(code, ErrorSeverity::Critical, message, details, filePath);
    
    {
        const juce::ScopedLock sl(errorLock);
        errorHistory.insert(0, error);
        if (errorHistory.size() > MAX_ERROR_HISTORY)
            errorHistory.removeLast();
    }
    
    criticalCount++;
    notifyError(error);
    
    // 致命的エラーの場合、デバッグビルドではアサート
    #if JUCE_DEBUG
    jassertfalse;
    #endif
}

void ErrorManager::setErrorCallback(ErrorCallback callback)
{
    const juce::ScopedLock sl(errorLock);
    errorCallback = callback;
}

void ErrorManager::clearErrorCallback()
{
    const juce::ScopedLock sl(errorLock);
    errorCallback = nullptr;
}

juce::Array<ErrorInfo> ErrorManager::getRecentErrors(int maxCount) const
{
    const juce::ScopedLock sl(errorLock);
    juce::Array<ErrorInfo> recent;
    
    for (int i = 0; i < juce::jmin(maxCount, errorHistory.size()); ++i)
        recent.add(errorHistory[i]);
    
    return recent;
}

void ErrorManager::clearErrorHistory()
{
    const juce::ScopedLock sl(errorLock);
    errorHistory.clear();
    infoCount = 0;
    warningCount = 0;
    errorCount = 0;
    criticalCount = 0;
}

int ErrorManager::getErrorCount(ErrorSeverity severity) const
{
    switch (severity)
    {
        case ErrorSeverity::Info: return infoCount.load();
        case ErrorSeverity::Warning: return warningCount.load();
        case ErrorSeverity::Error: return errorCount.load();
        case ErrorSeverity::Critical: return criticalCount.load();
        default: return 0;
    }
}

bool ErrorManager::hasRecentCriticalErrors() const
{
    return criticalCount.load() > 0;
}

void ErrorManager::notifyError(const ErrorInfo& error)
{
    // デバッグ出力（開発時のみ）
    
    
    // コールバック呼び出し（WebUIへの通知）
    ErrorCallback callback;
    {
        const juce::ScopedLock sl(errorLock);
        callback = errorCallback;
    }
    
    if (callback)
        callback(error);
}

// ErrorGuard実装
ErrorGuard::ErrorGuard(const juce::String& operation)
    : operationName(operation)
{
    
}

ErrorGuard::~ErrorGuard()
{
    if (!succeeded)
    {
        
    }
}

void ErrorGuard::success()
{
    succeeded = true;
    
}

void ErrorGuard::failure(ErrorCode code, const juce::String& message)
{
    ErrorManager::getInstance().reportError(code, 
        operationName + " failed: " + message);
}

} // namespace MixCompare