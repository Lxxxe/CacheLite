#include "cachelite/cache/CacheService.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace cachelite::cache {

CacheService::CacheService(
    storage::MemoryStore& memory,
    database::KeyValueRepository& repository,
    std::size_t workerCount
)
    : memory_(memory),
      repository_(repository) {
    //保护逻辑，如果调用方传入 0 个工作线程，强制改成 1，防止线程池数量为 0 导致无法处理任务
    if (workerCount == 0) {
        workerCount = 1;
    }

    workers_.reserve(workerCount);//`reserve`预分配 vector 内存
    try {
        for (std::size_t index = 0; index < workerCount; ++index) {
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

CacheService::SetResult CacheService::set(
    std::string key,
    std::string value
) {
    return memory_.set(std::move(key), std::move(value));
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
    std::string key,
    LookupCallback callback
) {
    if (!callback) {
        throw std::invalid_argument("cache lookup callback is empty");
    }

    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("cache service is stopping");
        }
        //将任务放入队列
        tasks_.push(Task{
            std::move(key),//要查询的 key
            std::move(callback)//查询完成后的回调函数
        });
    }
    condition_.notify_one();//唤醒一个等待中的 worker 线程
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

        try {
            task.callback(std::move(result));
        } catch (...) {
            // 回调由事件循环提供，后台线程不能因回调异常退出。
        }
    }
}

}  // namespace cachelite::cache
