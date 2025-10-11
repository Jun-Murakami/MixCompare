#include "PlaylistManager.h"
#include "StateManager.h"
#include "ErrorManager.h"

namespace MixCompare
{

PlaylistManager::PlaylistManager(StateManager* sm)
    : stateManager(sm)
{
    jassert(stateManager != nullptr);
    
    formatManager.registerBasicFormats();

#if JUCE_WINDOWS
    // Windows用のMedia Foundation AACフォーマットを追加
    if (mc3::MediaFoundationAACFormat::isMediaFoundationAvailable())
    {
        formatManager.registerFormat(new mc3::MediaFoundationAACFormat(), false);
    }
#endif

    // Initialize cache from StateManager
    syncCacheFromState();
}

PlaylistManager::~PlaylistManager()
{
}

void PlaylistManager::syncCacheFromState()
{
    if (!stateManager)
        return;
        
    const juce::ScopedLock sl(itemsLock);
    
    auto playlistTree = stateManager->getPlaylistState();
    if (!playlistTree.isValid())
        return;
        
    itemsCache.clear();
    
    for (int i = 0; i < playlistTree.getNumChildren(); ++i)
    {
        auto itemTree = playlistTree.getChild(i);
        if (itemTree.getType().toString() == "PlaylistItem")
        {
            PlaylistItem item;
            item.id = itemTree.getProperty("fileId", "").toString();
            item.file = juce::File(itemTree.getProperty("filePath", "").toString());
            item.displayName = itemTree.getProperty("fileName", item.file.getFileName()).toString();
            item.duration = itemTree.getProperty("duration", 0.0);
            item.sampleRate = itemTree.getProperty("sampleRate", 0.0);
            item.bitDepth = itemTree.getProperty("bitDepth", 0);
            item.numChannels = itemTree.getProperty("numChannels", 0);
            
            itemsCache.add(item);
        }
    }
}

juce::Array<juce::String> PlaylistManager::addFiles(const juce::Array<juce::File>& files)
{
    
    juce::Array<juce::String> addedIds;
    
    if (!stateManager)
        return addedIds;
    
    for (const auto& file : files)
    {
        
        if (!file.existsAsFile())
        {
            ErrorManager::getInstance().reportError(
                ErrorCode::FileNotFound,
                "File not found",
                "",
                file.getFullPathName());
            continue;
        }
        
        // Add to StateManager (source of truth)
        stateManager->addPlaylistItem(file);
        
        // Create cache entry
        PlaylistItem item(generateUniqueId(), file);
        updateItemDuration(item);
        
        {
            const juce::ScopedLock sl(itemsLock);
            itemsCache.add(item);
        }
        
        addedIds.add(item.id);
        
    }
    
    if (addedIds.size() > 0)
    {
        notifyItemsChanged();
    }
    
    return addedIds;
}

juce::Array<juce::String> PlaylistManager::addFiles(const juce::StringArray& filePaths)
{
    juce::Array<juce::File> files;
    for (const auto& path : filePaths)
    {
        files.add(juce::File(path));
    }
    return addFiles(files);
}

bool PlaylistManager::removeItem(const juce::String& itemId)
{
    if (!stateManager || itemId.isEmpty())
        return false;
    
    bool removed = false;
    
    {
        const juce::ScopedLock sl(itemsLock);
        
        for (int i = 0; i < itemsCache.size(); ++i)
        {
            if (itemsCache[i].id == itemId)
            {
                itemsCache.remove(i);
                removed = true;
                break;
            }
        }
    }
    
    // Remove from StateManager
    if (removed)
    {
        stateManager->removePlaylistItem(juce::Identifier(itemId));
        notifyItemsChanged();
    }
    
    return removed;
}


void PlaylistManager::removeAllItems()
{
    {
        const juce::ScopedLock sl(itemsLock);
        itemsCache.clear();
    }
    
    if (stateManager)
    {
        stateManager->clearPlaylist();
    }
    
    notifyItemsChanged();
}

bool PlaylistManager::selectItem(const juce::String& itemId)
{
    if (!stateManager || itemId.isEmpty())
        return false;
    
    // Verify item exists in cache
    bool exists = false;
    {
        const juce::ScopedLock sl(itemsLock);
        for (const auto& item : itemsCache)
        {
            if (item.id == itemId)
            {
                exists = true;
                break;
            }
        }
    }
    
    if (exists)
    {
        stateManager->setActiveFileId(juce::Identifier(itemId));
        notifyCurrentChanged();
    }
    
    return exists;
}

bool PlaylistManager::selectItemByIndex(int index)
{
    if (!stateManager || !isValidIndex(index))
        return false;
    
    juce::String newItemId;
    
    {
        const juce::ScopedLock sl(itemsLock);
        if (index >= 0 && index < itemsCache.size())
        {
            newItemId = itemsCache[index].id;
        }
    }
    
    if (newItemId.isNotEmpty())
    {
        stateManager->setActiveFileId(juce::Identifier(newItemId));
        notifyCurrentChanged();
        return true;
    }
    
    return false;
}

bool PlaylistManager::selectNext()
{
    int currentIndex = getCurrentIndex();
    if (currentIndex < 0)
    {
        // No current selection, select first item
        return selectItemByIndex(0);
    }
    
    // Wrap around to first item if at end
    int nextIndex = (currentIndex + 1) % getNumItems();
    return selectItemByIndex(nextIndex);
}

bool PlaylistManager::selectPrevious()
{
    int currentIndex = getCurrentIndex();
    if (currentIndex < 0)
    {
        // No current selection, select last item
        int lastIndex = getNumItems() - 1;
        return selectItemByIndex(lastIndex);
    }
    
    // Wrap around to last item if at beginning
    int prevIndex = currentIndex > 0 ? currentIndex - 1 : getNumItems() - 1;
    return selectItemByIndex(prevIndex);
}

int PlaylistManager::getCurrentIndex() const
{
    if (!stateManager)
        return -1;
        
    auto currentId = getCurrentItemId();
    
    const juce::ScopedLock sl(itemsLock);
    for (int i = 0; i < itemsCache.size(); ++i)
    {
        if (itemsCache[i].id == currentId)
            return i;
    }
    
    return -1;
}

juce::String PlaylistManager::getCurrentItemId() const
{
    if (!stateManager)
        return {};
        
    auto id = stateManager->getActiveFileId();
    return id.isValid() ? id.toString() : juce::String();
}

PlaylistManager::PlaylistItem* PlaylistManager::getCurrentItem()
{
    auto currentId = getCurrentItemId();
    
    const juce::ScopedLock sl(itemsLock);
    for (auto& item : itemsCache)
    {
        if (item.id == currentId)
            return &item;
    }
    
    return nullptr;
}

const PlaylistManager::PlaylistItem* PlaylistManager::getCurrentItem() const
{
    auto currentId = getCurrentItemId();
    
    const juce::ScopedLock sl(itemsLock);
    for (const auto& item : itemsCache)
    {
        if (item.id == currentId)
            return &item;
    }
    
    return nullptr;
}

PlaylistManager::PlaylistItem* PlaylistManager::getItem(const juce::String& itemId)
{
    const juce::ScopedLock sl(itemsLock);
    for (auto& item : itemsCache)
    {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

const PlaylistManager::PlaylistItem* PlaylistManager::getItem(const juce::String& itemId) const
{
    const juce::ScopedLock sl(itemsLock);
    for (const auto& item : itemsCache)
    {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

PlaylistManager::PlaylistItem* PlaylistManager::getItemByIndex(int index)
{
    const juce::ScopedLock sl(itemsLock);
    if (index >= 0 && index < itemsCache.size())
        return &itemsCache.getReference(index);
    return nullptr;
}

const PlaylistManager::PlaylistItem* PlaylistManager::getItemByIndex(int index) const
{
    const juce::ScopedLock sl(itemsLock);
    if (index >= 0 && index < itemsCache.size())
        return &itemsCache.getReference(index);
    return nullptr;
}

juce::Array<PlaylistManager::PlaylistItem> PlaylistManager::getAllItems() const
{
    const juce::ScopedLock sl(itemsLock);
    return itemsCache;
}

juce::Array<juce::String> PlaylistManager::getAllItemIds() const
{
    juce::Array<juce::String> ids;
    const juce::ScopedLock sl(itemsLock);
    for (const auto& item : itemsCache)
    {
        ids.add(item.id);
    }
    return ids;
}

void PlaylistManager::reorderItems(const juce::Array<juce::String>& orderedIds)
{
    if (!stateManager)
        return;
    
    // Convert String array to Identifier array
    juce::Array<juce::Identifier> identifiers;
    for (const auto& id : orderedIds)
    {
        identifiers.add(juce::Identifier(id));
    }
    
    // First, reorder in StateManager
    stateManager->reorderPlaylistItems(identifiers);
    
    // Then sync cache
    syncCacheFromState();
    
    notifyItemsChanged();
}

juce::ValueTree PlaylistManager::getState() const
{
    if (!stateManager)
        return {};
        
    return stateManager->getPlaylistState();
}

void PlaylistManager::setState(const juce::ValueTree& state)
{
    juce::ignoreUnused(state);
    
    if (!stateManager)
        return;
        
    // Update StateManager with new state
    // This would require a method in StateManager to replace playlist state
    // For now, we'll sync from existing state
    syncCacheFromState();
}

bool PlaylistManager::isValidIndex(int index) const
{
    const juce::ScopedLock sl(itemsLock);
    return index >= 0 && index < itemsCache.size();
}

juce::String PlaylistManager::toJSON() const
{
    auto obj = juce::DynamicObject::Ptr(new juce::DynamicObject());
    auto itemsArray = juce::Array<juce::var>();
    
    {
        const juce::ScopedLock sl(itemsLock);
        
        for (const auto& item : itemsCache)
        {
            auto itemObj = juce::DynamicObject::Ptr(new juce::DynamicObject());
            itemObj->setProperty("id", item.id);
            itemObj->setProperty("path", item.file.getFullPathName());
            itemObj->setProperty("name", item.displayName);
            itemObj->setProperty("duration", item.duration);
            itemObj->setProperty("sampleRate", item.sampleRate);
            itemObj->setProperty("bitDepth", item.bitDepth);
            itemObj->setProperty("numChannels", item.numChannels);
            itemObj->setProperty("loaded", item.isLoaded);
            
            itemsArray.add(itemObj.get());
        }
    }
    
    obj->setProperty("items", itemsArray);
    obj->setProperty("currentItemId", getCurrentItemId());
    
    return juce::JSON::toString(juce::var(obj.get()));
}

void PlaylistManager::fromJSON(const juce::String& json)
{
    auto parsed = juce::JSON::parse(json);
    if (!parsed.isObject())
        return;
    
    removeAllItems();
    
    if (auto itemsArray = parsed.getProperty("items", juce::var()).getArray())
    {
        for (const auto& itemVar : *itemsArray)
        {
            if (auto itemObj = itemVar.getDynamicObject())
            {
                auto path = itemObj->getProperty("path").toString();
                if (path.isNotEmpty())
                {
                    addFiles({juce::File(path)});
                }
            }
        }
    }
    
    auto currentId = parsed.getProperty("currentItemId", "").toString();
    if (currentId.isNotEmpty() && stateManager)
    {
        stateManager->setActiveFileId(juce::Identifier(currentId));
    }
}

juce::String PlaylistManager::generateUniqueId() const
{
    return juce::Uuid().toString();
}

void PlaylistManager::updateItemDuration(PlaylistItem& item)
{
    // Try to read duration without fully loading the file
    if (auto reader = formatManager.createReaderFor(item.file))
    {
        item.sampleRate = reader->sampleRate;
        item.numChannels = static_cast<int>(reader->numChannels);
        item.bitDepth = static_cast<int>(reader->bitsPerSample);
        item.duration = reader->lengthInSamples / reader->sampleRate;
        delete reader;
    }
}

void PlaylistManager::notifyItemsChanged()
{
    
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = this]()
        {
            if (safe)
                safe->listeners.call(&Listener::playlistItemsChanged);
        });
        return;
    }
    listeners.call(&Listener::playlistItemsChanged);
}

void PlaylistManager::notifyCurrentChanged()
{
    auto currentId = getCurrentItemId();
    
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = this, currentId]()
        {
            if (safe)
                safe->listeners.call(&Listener::currentItemChanged, currentId);
        });
        return;
    }
    listeners.call(&Listener::currentItemChanged, currentId);
}

void PlaylistManager::addListener(Listener* listener)
{
    listeners.add(listener);
}

void PlaylistManager::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

} // namespace MixCompare
