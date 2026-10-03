#include "cachelite/storage/MemoryStore.h"

#include <chrono>
#include <iterator>
#include <limits>
#include <utility>

namespace cachelite::storage {

MemoryStore::MemoryStore(std::size_t maxBytes) noexcept
    : maxBytes_(maxBytes) {
}

MemoryStore::SetResult MemoryStore::set(
    std::string key,
    std::string value
) {
    // 1. 计算这条key+value占用多少字节
    const std::size_t bytes = entryBytes(key, value);
    // 如果单个key本身就超过全局最大内存，直接拒绝
    if (bytes > maxBytes_) {
        return SetResult::RejectedByMaxMemory;
    }
    // 在哈希表查找key
    auto it = values_.find(key);
    // 如果找到，但是这个key已经过期：先删掉过期条目
    if (it != values_.end() &&
        isExpired(it->second, nowMilliseconds())) {
        eraseEntry(it);
        it = values_.end();
    }
    //key存在，更新旧key
    if (it != values_.end()) {
        const std::size_t oldBytes = it->second.bytes;
        if (!ensureCapacity(oldBytes, bytes, &key)) {
            return SetResult::RejectedByMaxMemory;
        }
        // 先扣除旧内存占用
        usedBytes_ -= oldBytes;
        // 更新value、过期时间、字节统计
        it->second.value = std::move(value);
        it->second.expiresAtMilliseconds = 0;
        it->second.bytes = bytes;
        usedBytes_ += bytes;
        touch(it);// LRU：把这个key挪到链表头部，刷新热度
        return SetResult::Updated;
    }
    //key不存在，新增key
    if (!ensureCapacity(0, bytes, nullptr)) {
        return SetResult::RejectedByMaxMemory;
    }
    // 放到LRU链表头部（新key，最近被访问）
    lru_.push_front(std::move(key));
    const auto lruIt = lru_.begin();

    try {
        //往哈希表values_插入Entry
        const auto insertion = values_.emplace(
            *lruIt,
            Entry{
                std::move(value),
                0,
                bytes,
                lruIt
            }
        );
        // 插入失败：key又突然被别的线程插入（并发冲突）
        if (!insertion.second) {
            lru_.pop_front();//回滚，删掉刚加入LRU的节点
            return SetResult::Updated;
        }
        usedBytes_ += bytes;
    } catch (...) {
        lru_.pop_front();
        throw;
    }

    return SetResult::Inserted;
}

std::optional<std::string> MemoryStore::get(std::string_view key) {
    const auto it = values_.find(std::string(key));//unordered_map的查找
    if (it == values_.end()) {
        return std::nullopt;
    }
    //已过期
    if (isExpired(it->second, nowMilliseconds())) {
        eraseEntry(it);
        return std::nullopt;
    }

    touch(it);
    return it->second.value;
}

bool MemoryStore::del(std::string_view key) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return false;
    }
    ////已过期
    if (isExpired(it->second, nowMilliseconds())) {
        eraseEntry(it);
        return false;
    }

    eraseEntry(it);
    return true;
}

bool MemoryStore::expire(
    std::string_view key,
    std::chrono::seconds lifetime
) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return false;
    }
    //已过期
    if (isExpired(it->second, nowMilliseconds())) {
        eraseEntry(it);
        return false;
    }
    //传入的存活时间 `lifetime ≤ 0`：直接**删除这个 key**
    if (lifetime <= std::chrono::seconds::zero()) {
        eraseEntry(it);
        return true;
    }

    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds//把传入的时间转成毫秒
    >(lifetime).count();
    return expireAtMilliseconds(
        key,
        nowMilliseconds() + milliseconds//用当前时间戳 + 时长，算出过期时刻，
    );
}
//设置过期时间
bool MemoryStore::expireAtMilliseconds(
    std::string_view key,
    std::int64_t timestampMilliseconds
) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return false;
    }

    const std::int64_t now = nowMilliseconds();
    if (isExpired(it->second, now)) {
        eraseEntry(it);
        return false;
    }

    if (timestampMilliseconds <= now) {
        eraseEntry(it);
        return true;
    }

    it->second.expiresAtMilliseconds = timestampMilliseconds;
    touch(it);
    return true;
}
//返回过期时间
std::optional<std::int64_t> MemoryStore::expirationMilliseconds(
    std::string_view key
) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return std::nullopt;
    }

    if (isExpired(it->second, nowMilliseconds())) {
        eraseEntry(it);
        return std::nullopt;
    }

    touch(it);
    if (it->second.expiresAtMilliseconds == 0) {
        return std::nullopt;
    }

    return it->second.expiresAtMilliseconds;
}

std::int64_t MemoryStore::ttlSeconds(std::string_view key) {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return -2;
    }

    const std::int64_t now = nowMilliseconds();
    if (isExpired(it->second, now)) {
        eraseEntry(it);
        return -2;
    }

    if (it->second.expiresAtMilliseconds == 0) {
        touch(it);
        return -1;
    }

    const std::int64_t remainingMilliseconds =
        it->second.expiresAtMilliseconds - now;
    touch(it);
    return remainingMilliseconds > 0
        ? remainingMilliseconds / 1000
        : 0;
}

std::size_t MemoryStore::maxBytes() const noexcept {
    return maxBytes_;
}

std::size_t MemoryStore::usedBytes() const noexcept {
    return usedBytes_;
}

std::size_t MemoryStore::entryBytes(
    std::string_view key,
    std::string_view value
) noexcept {
    if (value.size() >
        std::numeric_limits<std::size_t>::max() - key.size()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return key.size() + value.size();
}
//当 key 被访问（读 / 写）时，更新 LRU 链表，把这个 key 移动到链表最前面（代表最近被使用）
void MemoryStore::touch(StoreIterator it) noexcept {
    lru_.splice(
        lru_.begin(),// 目标位置：移动到 lru_ 的开头（begin）
        lru_,// 源链表：还是当前 lru_ 自己
        it->second.lruIterator// 要剪切的节点迭代器：这个key在LRU链表里面对应的节点
    );
}

void MemoryStore::eraseEntry(StoreIterator it) noexcept {
    usedBytes_ -= it->second.bytes;
    lru_.erase(it->second.lruIterator);
    values_.erase(it);
}

void MemoryStore::purgeExpired() {
    const std::int64_t now = nowMilliseconds();
    for (auto it = values_.begin(); it != values_.end();) {
        //遍历判断是否过期
        if (isExpired(it->second, now)) {
            const auto expired = it++;
            eraseEntry(expired);
        } else {
            ++it;
        }
    }
}

bool MemoryStore::ensureCapacity(
    std::size_t replacedBytes,//被覆盖旧 key 占用的字节数
    std::size_t incomingBytes,//本次要写入的新 key + 新 value 需要占用的字节
    const std::string* protectedKey//**受保护不允许被淘汰的 key 指针**。一般就是本次正要写入的 key，防止刚要写入的 key 自己被淘汰掉
) {
    //如果**单个新数据本身就比整个最大内存上限还要大**，直接失败
    if (incomingBytes > maxBytes_) {
        return false;
    }
    //安全校验：要释放的旧内存，不可能大于当前已使用总内存，出现这种异常直接返回失败
    if (replacedBytes > usedBytes_) {
        return false;
    }

    std::size_t baseBytes = usedBytes_ - replacedBytes;//假设先把旧 key 删掉之后，剩下的内存占用量
    const std::size_t availableBytes =
        baseBytes <= maxBytes_ ? maxBytes_ - baseBytes : 0;//baseBytes <= maxBytes_返回maxBytes_ - baseBytes剩余内存
    if (incomingBytes <= availableBytes) {
        return true;
    }
    //内存不够，清理所有已经过期的 key
    purgeExpired();
    //重新计算剩余内存
    baseBytes = usedBytes_ - replacedBytes;
    const std::size_t availableAfterPurge =
        baseBytes <= maxBytes_ ? maxBytes_ - baseBytes : 0;
    if (incomingBytes <= availableAfterPurge) {
        return true;
    }
    //清理过期 key 之后，内存**仍然不够**。
    //计算还需要额外释放多少字节`bytesToFree`，调用`evictBytes`执行内存淘汰（淘汰未过期的 key，一般 LRU 策略）
    const std::size_t bytesToFree =
        incomingBytes - availableAfterPurge;
    return evictBytes(bytesToFree, protectedKey);
}
//LRU 内存淘汰函数
bool MemoryStore::evictBytes(
    std::size_t bytesToFree,
    const std::string* protectedKey
) noexcept {
    while (bytesToFree > 0) {
        //LRU 链表空，没有任何可以淘汰的 key，直接返回失败
        if (lru_.empty()) {
            return false;
        }
        //`std::prev`拿到**链表最后一个元素**
        auto victim = std::prev(lru_.end());
        //碰到受保护的key时跳过
        while (protectedKey != nullptr && *victim == *protectedKey) {
            //一路找到链表头部全都是这个保护 key，没有别的 key 可以淘汰
            if (victim == lru_.begin()) {
                return false;
            }
            --victim;
        }
        //去哈希表`values_`查找这个候选 key
        const auto it = values_.find(*victim);
        //如果哈希表里找不到这个 key：说明 LRU 链表里面这个 key 是脏的、已经被删掉了，只从 LRU 链表擦掉，继续下一轮循环。
        if (it == values_.end()) {
            lru_.erase(victim);
            continue;
        }

        const std::size_t victimBytes = it->second.bytes;
        eraseEntry(it);//删除
        bytesToFree = victimBytes >= bytesToFree
            ? 0//被删掉的 key 占用空间 ≥ 还需要释放的大小,结束循环
            : bytesToFree - victimBytes;//用目标值减去本次释放的字节
    }

    return true;
}

//返回当前时间 - 1970-01-01 00:00:00，单位毫秒
std::int64_t MemoryStore::nowMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(//时间单位强制转换,把下面拿到的时间间隔，统一转换成**毫秒**
        std::chrono::system_clock::now().time_since_epoch()//`.now()` 获取当前时间点,.time_since_epoch()拿到从 Unix 纪元到这个时间点的时间间隔
    ).count();//取出里面保存的数值，返回整数,duration是时间对象
}

bool MemoryStore::isExpired(
    const Entry& entry,
    std::int64_t nowMilliseconds
) noexcept {
    // expiresAtMilliseconds 为 0 表示永不过期。
    return entry.expiresAtMilliseconds != 0 &&
        entry.expiresAtMilliseconds <= nowMilliseconds;//存储的时间戳小于现在的时间戳（已过期），1
}

std::size_t MemoryStore::size() const noexcept {
    return values_.size();
}

}  // namespace cachelite::storage
