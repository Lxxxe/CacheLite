#include "cachelite/cache/CacheService.h"

#include <chrono>
#include <exception>
#include <random>
#include <stdexcept>
#include <utility>

namespace cachelite::cache {

namespace {

std::int64_t nowMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t randomJitterMilliseconds(
    std::chrono::seconds maximum
) {
    const auto maximumMilliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds
    >(maximum).count();
    if (maximumMilliseconds <= 0) {
        return 0;
    }

    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<std::int64_t> distribution(
        0,
        maximumMilliseconds
    );
    return distribution(generator);
}

}  // namespace

CacheService::CacheService(
    storage::MemoryStore& memory,
    database::KeyValueRepository& repository,
    CacheOptions options
)
    : memory_(memory),
      repository_(repository),
      options_(std::move(options)) {
    //保护逻辑，如果调用方传入 0 个工作线程，强制改成 1，防止线程池数量为 0 导致无法处理任务
    if (options_.workerCount == 0) {
        options_.workerCount = 1;
    }
    if (options_.maxPendingLookups == 0) {
        options_.maxPendingLookups = 1;
    }
    if (options_.maxWaitersPerKey == 0) {
        options_.maxWaitersPerKey = 1;
    }
    if (options_.backendFailureThreshold == 0) {
        options_.backendFailureThreshold = 1;
    }
    if (options_.negativeCacheTtl < std::chrono::seconds::zero()) {
        options_.negativeCacheTtl = std::chrono::seconds::zero();
    }
    if (options_.backendCacheTtl < std::chrono::seconds::zero()) {
        options_.backendCacheTtl = std::chrono::seconds::zero();
    }
    if (options_.backendCacheJitter < std::chrono::seconds::zero()) {
        options_.backendCacheJitter = std::chrono::seconds::zero();
    }
    if (options_.circuitCooldown < std::chrono::seconds::zero()) {
        options_.circuitCooldown = std::chrono::seconds::zero();
    }

    workers_.reserve(options_.workerCount);//`reserve`预分配 vector 内存
    try {
        for (std::size_t index = 0;
             index < options_.workerCount;
             ++index) {
            //`std::vector` 的成员函数,在 vector 的尾部原地直接构造对象，不产生临时对象、不拷贝 / 移动临时对象
            workers_.emplace_back(&CacheService::workerLoop, this);//在 vector 里就地构造一个线程，传入workerLoop和当前`CacheService`对象
            //线程构建完成自动跑workLoop，所有 worker 线程共享同一个 CacheService 实例，访问必须加锁
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

CacheService::~CacheService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::optional<std::string> CacheService::getLocal(
    std::string_view key
) {
    return memory_.get(key);
}

bool CacheService::exists(std::string_view key) {
    return memory_.exists(key);
}

CacheService::SetResult CacheService::set(
    std::string key,
    std::string value
) {
    const std::string keyForInvalidation = key;
    const auto result = memory_.set(std::move(key), std::move(value));
    if (result != SetResult::RejectedByMaxMemory) {
        std::lock_guard lock(mutex_);
        negativeCache_.erase(keyForInvalidation);
    }
    return result;
}

CacheService::SetResult CacheService::setFromBackend(
    std::string key,
    std::string value
) {
    const std::string keyForExpiration = key;//提前保存一份 key 的拷贝
    const auto result = memory_.set(std::move(key), std::move(value));//把键值写入内存 LRU 缓存,
    //如果内存不足且无法淘汰旧数据，那么就返回 RejectedByMaxMemory，表示写入失败。
    if (result == SetResult::RejectedByMaxMemory) {
        return result;//**即使无法缓存，也会把结果返回给客户端**，只是不做缓存加速而已。
    }

    {
        std::lock_guard lock(mutex_);
        negativeCache_.erase(keyForExpiration);//如果写入成功，那么就把这个 key 从负缓存里删除，避免后续的查询被负缓存拦截。
    }
    //如果配置了 backendCacheTtl，那么就给这个 key 设置一个带抖动的短 TTL，避免缓存雪崩。
    if (options_.backendCacheTtl > std::chrono::seconds::zero()) {
        const auto baseMilliseconds = std::chrono::duration_cast<
            std::chrono::milliseconds
        >(options_.backendCacheTtl).count();//把 backendCacheTtl 转换为毫秒数
        const auto jitterMilliseconds = randomJitterMilliseconds(
            options_.backendCacheJitter
        );//随机生成一个 0 到 backendCacheJitter 毫秒的抖动值
        //把这个 key 的过期时间设置为当前时间 + baseMilliseconds + jitterMilliseconds，单位是毫秒
        static_cast<void>(memory_.expireAtMilliseconds(
            keyForExpiration,
            nowMilliseconds() + baseMilliseconds + jitterMilliseconds
        ));
    }

    return result;
}

bool CacheService::del(std::string_view key) {
    return memory_.del(key);
}

bool CacheService::expire(
    std::string_view key,
    std::chrono::seconds lifetime
) {
    return memory_.expire(key, lifetime);
}

bool CacheService::expireAtMilliseconds(
    std::string_view key,
    std::int64_t timestampMilliseconds
) {
    return memory_.expireAtMilliseconds(key, timestampMilliseconds);
}

std::optional<std::int64_t> CacheService::expirationMilliseconds(
    std::string_view key
) {
    return memory_.expirationMilliseconds(key);
}

std::int64_t CacheService::ttlSeconds(std::string_view key) {
    return memory_.ttlSeconds(key);
}

void CacheService::lookupAsync(
    std::string key,//要查询的 key
    LookupCallback callback//查询完成后通知原客户端的回调函数
) {
    //每个异步请求都必须提供一个回调，用于查询完成后通知原客户端。
    if (!callback) {
        throw std::invalid_argument("cache lookup callback is empty");
    }

    LookupCallback immediateCallback;//如果查询无法提交到后台线程，那么就直接调用这个回调函数，通知调用方失败。
    database::LookupResult immediateResult;//如果查询无法提交到后台线程，那么就直接返回这个结果，通知调用方失败。
    bool notifyWorker = false;//标记是否需要唤醒后台 worker 线程。只有新增了数据库回源任务时才设为 true。

    {
        //加锁，保护共享变量：`stopping_`（线程池停止标记）、`tasks_`（任务队列）、`inflight_`（正在处理的 key 映射到等待回调列表）、`negativeCache_`（负缓存）
        std::lock_guard lock(mutex_);

        //如果服务正在析构，不再创建新任务，而是直接通知调用方失败。
        if (stopping_) {
            immediateCallback = std::move(callback);
            immediateResult = database::LookupResult::failure(
                "cache service is stopping"
            );
        } else {
            //负缓存检查
            const auto now = std::chrono::steady_clock::now();
            const auto negativeIt = negativeCache_.find(key);
            if (negativeIt != negativeCache_.end()) {
                //那么在空值缓存有效期内，直接返回 NotFound，而不再访问数据库。
                if (negativeIt->second > now) {
                    immediateCallback = std::move(callback);
                    immediateResult = database::LookupResult::notFound();
                } else {
                    //如果空值缓存已经过期,那么删除它，允许后续的数据库查询。
                    negativeCache_.erase(negativeIt);
                }
            }

            //熔断检查,如果 MySQL 连续失败达到阈值circuitOpen_=true，那么打开熔断器，直接返回失败，而不再访问数据库。
            if (!immediateCallback && circuitOpen_) {
                //如果熔断时间已经结束，关闭熔断器，允许后续的数据库查询。
                if (now - circuitOpenedAt_ >= options_.circuitCooldown) {
                    circuitOpen_ = false;
                    backendFailureCount_ = 0;
                } else {
                    immediateCallback = std::move(callback);
                    immediateResult = database::LookupResult::failure(
                        "cache backend circuit is open"
                    );
                }
                }
            //如果没有负缓存，也没有熔断，那么就把查询任务提交到后台线程，等待数据库查询完成后通知调用方。
            if (!immediateCallback) {
                //检查同 key 是否正在查询，如果是，那么把回调加入等待列表，而不再创建新任务。inserted=true表示新插入，false表示已存在
                auto [it, inserted] = inflight_.try_emplace(key);

                //如果等待列表已经满了，那么拒绝新请求，直接返回失败，而不再访问数据库。
                if (it->second.waiters.size() >=
                    options_.maxWaitersPerKey) {
                    if (inserted) {
                        //如果是新插入的 key，说明 inflight_ 里没有这个 key 的等待列表，那么就把它从 inflight_ 里删除，避免占用内存。
                        inflight_.erase(it);
                    }
                    immediateCallback = std::move(callback);
                    immediateResult = database::LookupResult::failure(
                        "too many requests waiting for the same key"
                    );
                //限制回源任务队列,默认最多保存 1024 个回源任务，超过则拒绝新任务
                } else if (inserted &&    //只有第一个请求（`inserted = true`）才会检查全局队列。
                           tasks_.size() >= options_.maxPendingLookups) {
                    inflight_.erase(it);
                    immediateCallback = std::move(callback);
                    immediateResult = database::LookupResult::failure(
                        "cache lookup queue is full"
                    );
                //如果等待列表没有满，那么把回调加入等待列表，等待数据库查询完成后通知调用方。
                } else {
                    it->second.waiters.push_back(std::move(callback));
                    //如果是新插入的 key，那么就创建一个新的回源任务，放入任务队列，等待 worker 线程处理。
                    if (inserted) {
                        try {
                            tasks_.push(Task{key});
                            notifyWorker = true;
                        } catch (...) {
                            immediateCallback = std::move(
                                it->second.waiters.back()
                            );
                            it->second.waiters.pop_back();
                            inflight_.erase(it);
                            immediateResult = database::LookupResult::failure(
                                "cache lookup queue allocation failed"
                            );
                        }
                    }
                }
            }
        }
    }

    if (notifyWorker) {
        condition_.notify_one();//唤醒一个等待中的 worker 线程,去处理刚加入的回源任务 tasks_.push(Task{key});
    }
    //执行立即回调,所有不需要走数据库的场景（停止、负缓存、熔断、限流），都在这里执行回调返回结果。
    if (immediateCallback) {
        immediateCallback(std::move(immediateResult));//
    }
}

void CacheService::completeLookup(
    const std::string& key,
    database::LookupResult result
) {
    std::vector<LookupCallback> waiters;//暂存本次要执行的所有回调

    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();

        //如果数据库查询结果是 NotFound，那么就把这个 key 加入负缓存negativeCache_，避免后续重复查询。
        if (result.status == database::LookupResult::Status::NotFound) {
            backendFailureCount_ = 0;//清零失败计数
            circuitOpen_ = false;//关闭熔断
            if (options_.negativeCacheTtl > std::chrono::seconds::zero()) {
                negativeCache_[key] = now + options_.negativeCacheTtl;
            }
        //如果数据库查询结果是 Found，那么就把这个 key 从负缓存里删除，避免后续误判。
        } else if (result.status == database::LookupResult::Status::Found) {
            backendFailureCount_ = 0;
            circuitOpen_ = false;
            negativeCache_.erase(key);
        //如果数据库查询结果是 Error，那么就增加失败计数，如果达到阈值，就打开熔断器，避免后续重复查询。
        } else {
            ++backendFailureCount_;//累加连续失败次数
            //如果连续失败次数达到阈值，就打开熔断器，避免后续重复查询。直到冷却时间结束自动恢复。
            if (backendFailureCount_ >= options_.backendFailureThreshold) {
                circuitOpen_ = true;
                circuitOpenedAt_ = now;
            }
        }
        //从 inflight_ 里取出等待这个 key 查询结果的所有回调，并删除这个 key 的等待列表。
        const auto it = inflight_.find(key);
        if (it == inflight_.end()) {
            return;
        }
        //把整个回调列表转移到局部变量 `waiters`,这一步是请求合并的核心：一次数据库查询的结果，对应着 N 个等待的客户端请求，全部一次性取出。
        waiters = std::move(it->second.waiters);
        inflight_.erase(it);//立刻删除 inflight 中的这条记录
    }// 回调可能写 eventfd 或再次进入 CacheService，不能持有 mutex 调用。

    //遍历所有等待的回调，逐个传入查询结果执行；
    for (LookupCallback& waiter : waiters) {
        try {
            waiter(result);//执行回调，通知原客户端查询结果
        } catch (...) {
            // 单个客户端回调失败不能影响同 key 的其他等待者。
        }
    }
}

void CacheService::workerLoop() {
    while (true) {
        Task task;
        {
            //拿到互斥锁，保护共享变量：`stopping_`（线程池停止标记）、`tasks_`（任务队列）
            std::unique_lock lock(mutex_);
            //处于睡眠状态,直到stopping_ == true或者tasks_ 不为空
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
                //如果 lambda 返回 false：`wait()` 释放 mutex，线程进入休眠，不占用 CPU，等待通知
                //如果 lambda 返回 true：不会休眠，直接继续往下执行
            });
            //worker 的退出条件
            //设置停止后，不再接收新任务；队列中已有任务会继续处理；队列清空后，worker 线程退出；主线程 join() 等待它们结束。
            if (stopping_ && tasks_.empty()) {
                return;//停止且任务队列为空，退出
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        database::LookupResult result;
        try {
            result = repository_.find(task.key);//真正执行数据库查询的是这里
        } catch (const std::exception& error) {
            result = database::LookupResult::failure(error.what());
        } catch (...) {
            result = database::LookupResult::failure(
                "unknown cache backend exception"
            );
        }

        completeLookup(task.key, std::move(result));
    }
}

}  // namespace cachelite::cache
