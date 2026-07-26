#pragma once

#include "core/Exception.h"

#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace sparkle
{
class EventListenerBase;

class EventSubscription
{
public:
    static constexpr uint32_t InvalidId = std::numeric_limits<uint32_t>::max();

    EventSubscription(std::weak_ptr<EventListenerBase> listener, uint32_t id) : listener_(std::move(listener)), id_(id)
    {
    }

    ~EventSubscription()
    {
        Unsubscribe();
    }

    EventSubscription(const EventSubscription &) = delete;
    EventSubscription &operator=(const EventSubscription &) = delete;

    EventSubscription(EventSubscription &&other) noexcept : listener_(std::move(other.listener_)), id_(other.id_)
    {
        other.id_ = InvalidId;
    }

    EventSubscription &operator=(EventSubscription &&other) noexcept;

    void Unsubscribe();

    [[nodiscard]] uint32_t GetId() const
    {
        return id_;
    }

    [[nodiscard]] bool IsValid() const
    {
        return id_ != InvalidId && !listener_.expired();
    }

private:
    std::weak_ptr<EventListenerBase> listener_;
    uint32_t id_;
};

class EventListenerBase : public std::enable_shared_from_this<EventListenerBase>
{
public:
    virtual ~EventListenerBase() = default;

    void Unsubscribe(EventSubscription &subscription)
    {
        auto id = subscription.GetId();

        const bool removed = RemoveCallback(id);
        ASSERT(removed);

        free_ids_.insert(id);
    }

protected:
    [[nodiscard]] uint32_t AllocateId()
    {
        uint32_t id;
        if (!free_ids_.empty())
        {
            id = *free_ids_.begin();
            free_ids_.erase(id);
        }
        else
        {
            id = next_id_;
            next_id_++;
        }
        return id;
    }

    virtual bool RemoveCallback(uint32_t id) = 0;

private:
    std::unordered_set<uint32_t> free_ids_;
    uint32_t next_id_ = 0;
};

template <typename... Args> class Event;

template <typename... Args> class EventListener : public EventListenerBase
{
public:
    // subscriber is responsible for managing the lifetime of the subscription. do not just get and destroy it.
    [[nodiscard]] std::unique_ptr<EventSubscription> Subscribe(std::function<void(Args...)> &&callback)
    {
        auto id = AllocateId();
        callbacks_.emplace(id, std::move(callback));

        return std::make_unique<EventSubscription>(weak_from_this(), id);
    }

protected:
    bool RemoveCallback(uint32_t id) override
    {
        return callbacks_.erase(id) > 0;
    }

private:
    void Broadcast(Args... args)
    {
        for (auto &[_, callback] : callbacks_)
        {
            callback(args...);
        }
    }

    std::unordered_map<uint32_t, std::function<void(Args...)>> callbacks_;

    friend class Event<Args...>;
};

template <typename... Args> class Event
{
public:
    Event() : listener_(std::make_shared<EventListener<Args...>>())
    {
    }

    void Trigger(Args... args)
    {
        listener_->Broadcast(args...);
    }

    [[nodiscard]] auto &OnTrigger()
    {
        return *listener_;
    }

private:
    std::shared_ptr<EventListener<Args...>> listener_;
};

inline EventSubscription &EventSubscription::operator=(EventSubscription &&other) noexcept
{
    if (this != &other)
    {
        Unsubscribe();
        listener_ = std::move(other.listener_);
        id_ = other.id_;
        other.id_ = InvalidId;
    }
    return *this;
}

inline void EventSubscription::Unsubscribe()
{
    if (!IsValid())
    {
        return;
    }

    if (auto listener = listener_.lock())
    {
        listener->Unsubscribe(*this);
    }
    id_ = EventSubscription::InvalidId;
}
} // namespace sparkle
