#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace sparkle
{
class EventListener;

class EventSubscription
{
public:
    static constexpr uint32_t InvalidId = std::numeric_limits<uint32_t>::max();

    EventSubscription(std::weak_ptr<EventListener> listener, uint32_t id) : listener_(std::move(listener)), id_(id)
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
    std::weak_ptr<EventListener> listener_;
    uint32_t id_;
};

class EventListener : public std::enable_shared_from_this<EventListener>
{
public:
    // subscriber is responsible for managing the lifetime of the subscription. do not just get and destroy it.
    [[nodiscard]] std::unique_ptr<EventSubscription> Subscribe(std::function<void()> &&callback);

    void Unsubscribe(EventSubscription &subscription);

private:
    void Broadcast()
    {
        for (auto &[_, callback] : callbacks_)
        {
            callback();
        }
    }

    std::unordered_map<uint32_t, std::function<void()>> callbacks_;
    std::unordered_set<uint32_t> free_ids_;
    uint32_t next_id_ = 0;

    friend class Event;
};

class Event
{
public:
    Event() : listener_(std::make_shared<EventListener>())
    {
    }

    void Trigger()
    {
        listener_->Broadcast();
    }

    [[nodiscard]] auto &OnTrigger()
    {
        return *listener_;
    }

private:
    std::shared_ptr<EventListener> listener_;
};
} // namespace sparkle
