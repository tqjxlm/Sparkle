#include "core/Event.h"

#include "core/Exception.h"

namespace sparkle
{
void EventListener::Unsubscribe(EventSubscription &subscription)
{
    auto id = subscription.GetId();

    ASSERT(callbacks_.contains(id));

    free_ids_.insert(id);
    callbacks_.erase(id);
}

std::unique_ptr<EventSubscription> EventListener::Subscribe(std::function<void()> &&callback)
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
    callbacks_.emplace(id, callback);

    return std::make_unique<EventSubscription>(weak_from_this(), id);
}

void EventSubscription::Unsubscribe()
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

EventSubscription &EventSubscription::operator=(EventSubscription &&other) noexcept
{
    if (this != &other)
    {
        Unsubscribe(); // Clean up current subscription
        listener_ = std::move(other.listener_);
        id_ = other.id_;
        other.id_ = InvalidId;
    }
    return *this;
}
} // namespace sparkle
