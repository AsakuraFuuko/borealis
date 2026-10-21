/*
    Copyright 2022 xfangfang

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#pragma once

#include <iterator>
#include <stdexcept>

#include "borealis/core/singleton.hpp"

namespace brls
{

template <typename K, typename T>
struct Node
{
    K key;
    T value;

    /// Cache entries marked as dirty will be refreshed when fetching the cache next time
    bool dirty = false;

    /// Reference count, 1 for each cache hit
    size_t count = 1;

    /// Bytes this entry occupies; 0 when the owner does not account for it.
    size_t bytes = 0;

    Node(K k, T v, size_t b = 0)
        : key(k)
        , value(v)
        , bytes(b)
    {
    }
};

/**
 * LRU cache
 * If the reference count is not 0, the cache will never expire.
 * Cache items with a reference count of 0 are cached according to LRU rules
 */
template <typename K, typename T>
class LRUCache
{
  public:
    typedef typename std::list<Node<K, T>>::iterator CacheIter;
#define DIRTY "_$dirty$"
    inline static size_t DEFAULT_CAPACITY      = 400;
    inline static bool ALWAYS_CACHE_LOCAL_FILE = true;

    LRUCache(size_t c, T defaultValue)
        : capacity(c)
        , defaultValue(defaultValue)
    {
        if (c < 1)
            throw std::logic_error("Cache capacity cannot less than 1.");
    }

    T get(K key)
    {
        if (!isCacheHit(key))
        {
            // Cache not hit
            return defaultValue;
        }

        // Cache hit
        cacheList.splice(cacheList.begin(), cacheList, cacheMap[key]);
        cacheMap[key]                  = cacheList.begin();
        valueMap[cacheMap[key]->value] = cacheList.begin();
        cacheMap[key]->count++;
        return cacheMap[key]->value;
    }

    void set(K key, T value) { set(key, value, 0); }

    void set(K key, T value, size_t bytes)
    {
        if (isCacheHit(key))
        {
            throw std::logic_error("Can not cache the same key twice.");
        }

        // Add new cache, then evict from the oldest end. The new entry sits at
        // the front and is never the victim of its own insertion.
        cacheList.push_front(Node<K, T>(key, value, bytes));
        totalBytes += bytes;

        // Update the values of two maps
        cacheMap[key]                  = cacheList.begin();
        valueMap[cacheMap[key]->value] = cacheList.begin();

        reclaim();
    }

    /**
     * It is not really to remove a cache, but to reduce the counter of the corresponding cache by 1
     */
    void remove(T value)
    {
        if (!isExisted(value))
        {
            return;
        }
        Node<K, T>& node = *valueMap[value];
        if (node.count > 0)
            node.count--;
        // A cell that just scrolled out releases its reference here. When the
        // byte budget is over, drop it (and any older entries) right away
        // instead of waiting for the next insertion.
        if (node.count == 0 && overLimit())
            reclaim();
    }

    /**
     * Update a cache value
     */
    void update(T old_val, T new_val)
    {
        if (!isExisted(old_val))
        {
            return;
        }
        valueMap[old_val]->value = new_val;
        valueMap[new_val]        = valueMap[old_val];
        valueMap.erase(old_val);
    }

    void setCapacity(int c)
    {
        if (c < 1)
            throw std::logic_error("Cache capacity cannot less than 1.");
        // The setting standard of the DEFAULT_CAPACITY should be the upper limit of the number of all pictures.
        c += DEFAULT_CAPACITY;
        this->capacity = c;
        reclaim();
    }

    /** Set an absolute limit for platforms with a fixed GPU-memory budget. */
    void setCapacityExact(size_t c)
    {
        if (c < 1)
            throw std::logic_error("Cache capacity cannot less than 1.");
        this->capacity = c;
        reclaim();
    }

    /**
     * Upper bound on the bytes held by unreferenced entries.
     * Referenced entries are never evicted, so the real footprint can exceed
     * this while a page is on screen; 0 disables the byte limit.
     */
    void setCapacityBytes(size_t bytes)
    {
        this->capacityBytes = bytes;
        reclaim();
    }

    size_t getCapacityBytes() const { return capacityBytes; }

    size_t getTotalBytes() const { return totalBytes; }

    size_t getSize() const { return cacheList.size(); }

    /**
     * A dirty cache is not able to be hit
     * @param value
     */
    void markDirty(T value)
    {
        if (!isExisted(value))
        {
            return;
        }

        auto item = valueMap[value];
        if (item->dirty)
            return;

        // modify existing Key.
        cacheMap.erase(item->key);
        item->key += DIRTY;
        item->dirty = true;
    }

    void markAllDirty()
    {
        for (auto& i : cacheList)
        {
            if (i.dirty)
                continue;
            cacheMap.erase(i.key);
            i.key += DIRTY;
            i.dirty = true;
        }
    }

    std::list<Node<K, T>>& getCacheList() { return cacheList; }

    void debug()
    {
        printf("===== cache size: %zu =====\n", cacheList.size());
        for (auto& i : cacheList)
        {
            printf("count: %zu, dirty: %d, value: %zu, key: %s\n", i.count,
                i.dirty, i.value, i.key.c_str());
        }
    }

  private:
    size_t capacity      = 1;
    size_t capacityBytes = 0;
    size_t totalBytes    = 0;
    T defaultValue;
    std::list<Node<K, T>> cacheList;
    std::unordered_map<K, CacheIter> cacheMap;
    std::unordered_map<T, CacheIter> valueMap;

    bool overLimit() const
    {
        if (cacheList.size() > capacity)
            return true;
        return capacityBytes != 0 && totalBytes > capacityBytes;
    }

    /**
     * Drop the least recently used entry with a zero reference count.
     * The front of the list (the entry inserted or hit last) is protected so a
     * freshly added texture is never evicted by its own insertion.
     * Entries still referenced by a visible view are skipped: they are exactly
     * the ones the screen needs.
     */
    bool evictOne()
    {
        if (cacheList.size() <= 1)
            return false;
        auto vg = brls::Application::getNVGContext();
        for (auto i = cacheList.rbegin(); i != cacheList.rend(); ++i)
        {
            auto item = std::prev(i.base());
            if (item == cacheList.begin())
                break;
            if (item->count > 0)
                continue;
            totalBytes -= item->bytes;
            nvgDeleteImage(vg, item->value);
            cacheMap.erase(item->key);
            valueMap.erase(item->value);
            cacheList.erase(item);
            return true;
        }
        return false;
    }

    void reclaim()
    {
        while (overLimit() && evictOne())
        {
        }
    }

    bool isExisted(K key) { return cacheMap.find(key) != cacheMap.end(); }

    bool isExisted(T val) { return valueMap.find(val) != valueMap.end(); }

    bool isCacheHit(K key)
    {
        if (isExisted(key))
            return !cacheMap[key]->dirty;
        return false;
    }
};

class TextureCache : public Singleton<TextureCache>
{
  public:
    TextureCache()
    {
        brls::Application::getWindowSizeChangedEvent()->subscribe(
            [this]()
            { this->cache.markAllDirty(); });

        brls::Application::getExitEvent()->subscribe([this]()
            { this->clean(); });
    }

    int getCache(const std::string& key) { return cache.get(key); }

    /**
     * Add cache. The texture's byte size is read back from NanoVG so the byte
     * budget reflects what the GPU backend actually allocated.
     */
    void addCache(const std::string& key, size_t texture)
    {
        if (texture <= 0)
            return;
        cache.set(key, texture, textureBytes(texture));
    }

    /**
     * Cap the memory held by cached textures. Referenced textures (those a
     * visible view still draws) are never evicted; everything else is dropped
     * in LRU order once the cap is exceeded.
     */
    void setBudgetBytes(size_t bytes) { cache.setCapacityBytes(bytes); }

    size_t getTotalBytes() const { return cache.getTotalBytes(); }

    size_t getSize() const { return cache.getSize(); }

    static size_t textureBytes(size_t texture)
    {
        if (texture <= 0)
            return 0;
        int width = 0, height = 0;
        nvgImageSize(brls::Application::getNVGContext(), (int)texture, &width, &height);
        if (width <= 0 || height <= 0)
            return 0;
        // All cached textures are RGBA (covers, bitmaps, SVG rasters).
        return (size_t)width * (size_t)height * 4;
    }

    /**
     * Subtract the matching texture cache counter by 1,
     * and delete the cache if the counter returns to zero.
     */
    void removeCache(size_t texture)
    {
        if (texture <= 0)
            return;
        cache.remove(texture);
    }

    void markDirty(size_t texture)
    {
        if (texture <= 0)
            return;
        cache.markDirty(texture);
    }

    /**
     * update texture id
     */
    void updateCache(size_t old_tex, size_t new_tex)
    {
        if (old_tex <= 0 || new_tex <= 0)
            return;
        cache.update(old_tex, new_tex);
    }

    void clean()
    {
        auto vg = brls::Application::getNVGContext();
        for (auto& i : cache.getCacheList())
        {
            nvgDeleteImage(vg, i.value);
        }
    }

    void debug() { cache.debug(); }

    LRUCache<std::string, size_t> cache = LRUCache<std::string, size_t>(200, 0);
};

}