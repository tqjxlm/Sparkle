#pragma once

#include "core/Exception.h"

#include <memory>
#include <string>

namespace sparkle
{
class RHIContext;

struct DrawArgs
{
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t instance_count = 1;
    uint32_t first_index = 0;
    uint32_t first_vertex = 0;
    uint32_t first_instance = 0;
};

class RHIResource
{
public:
    explicit RHIResource(std::string name) : name_(std::move(name))
    {
    }

    virtual ~RHIResource() = default;

    [[nodiscard]] const std::string &GetName() const
    {
        return name_;
    }

    [[nodiscard]] virtual bool IsDynamic() const
    {
        return false;
    }

    [[nodiscard]] virtual bool IsBindless() const
    {
        return false;
    }

    // the resource id uniquely identifies the underlying GPU resource.
    // whenever the GPU resource is regenerated, the id should be updated by marking id_dirty_.
    // in most cases this id will not change throughout RHIResource lifetime.
    [[nodiscard]] size_t GetId() const
    {
        if (id_dirty_)
        {
            UpdateId();
        }

        ASSERT(!id_dirty_);

        return id_;
    }

protected:
    mutable bool id_dirty_ = true;

private:
    void UpdateId() const;

    mutable size_t id_ = 0;
    std::string name_;

#ifndef NDEBUG
public:
    void SetDebugStack(std::string &&debug_stack)
    {
        debug_stack_ = std::move(debug_stack);
    }

    [[nodiscard]] const std::string &GetDebugStack() const
    {
        return debug_stack_;
    }

    static void PrintTrace(RHIResource *resource, int64_t use_count);

private:
    std::string debug_stack_;
#endif
};

template <class T> using RHIResourceWeakRef = std::weak_ptr<T>;

template <class T> bool IsRefValid(RHIResourceWeakRef<T> const &weak)
{
    using wt = RHIResourceWeakRef<T>;
    return !weak.owner_before(wt{}) && !wt{}.owner_before(weak);
}

// a smart pointer implementation compatible with std::shared_ptr
// it tracks ref count changes when debugging_==true
template <class T> class RHIResourceRef
{
public:
#pragma region Constructors

    RHIResourceRef() = default;

    RHIResourceRef(std::nullptr_t) // NOLINT
    {
    }

    RHIResourceRef(const RHIResourceRef &other) : ptr_(other.ptr_)
    {
#ifndef NDEBUG
        MarkDebugging(other.debugging_);
#endif
    }

    RHIResourceRef(RHIResourceRef &&other) noexcept : ptr_(std::move(other.ptr_))
    {
#ifndef NDEBUG
        other.PrintTrace();
        MarkDebugging(other.debugging_);
#endif
    }

    RHIResourceRef &operator=(const RHIResourceRef &other)
    {
        if (this == &other)
        {
            return *this;
        }

        ptr_ = other.ptr_;

#ifndef NDEBUG
        MarkDebugging(other.debugging_);
#endif

        return *this;
    }

    RHIResourceRef &operator=(RHIResourceRef &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        ptr_ = std::move(other.ptr_);

#ifndef NDEBUG
        MarkDebugging(other.debugging_);
#endif

        return *this;
    }

    RHIResourceRef(const std::shared_ptr<T> &ptr) : ptr_(ptr) // NOLINT
    {
    }

#pragma endregion

#ifndef NDEBUG
    ~RHIResourceRef()
    {
        PrintTrace(-1);
    }
#endif

#pragma region STL Interfaces

    T *get() // NOLINT
    {
        return ptr_.get();
    }

    [[nodiscard]] T *get() const // NOLINT
    {
        return ptr_.get();
    }

    T *operator->() const
    {
        return ptr_.get();
    }

    [[nodiscard]] int use_count() const // NOLINT
    {
        return ptr_.use_count();
    }

    bool operator==(const RHIResourceRef &other) const
    {
        return other.ptr_ == ptr_;
    }

    bool operator==(const T *ptr) const
    {
        return ptr_.get() == ptr;
    }

#pragma endregion

#pragma region Implicit Casts

    operator bool() const // NOLINT
    {
        return ptr_ != nullptr;
    }

    operator T *() const // NOLINT
    {
        return ptr_.get();
    }

    operator RHIResourceWeakRef<T>() const // NOLINT
    {
        return ptr_;
    }

    template <typename U>
        requires std::derived_from<T, U>
    operator RHIResourceRef<U>() && noexcept // NOLINT
    {
        auto ref = RHIResourceRef<U>(std::move(ptr_));
#ifndef NDEBUG
        ref.MarkDebugging(debugging_);
#endif
        return ref;
    }

    template <typename U>
        requires std::derived_from<T, U>
    operator RHIResourceRef<U>() const & // NOLINT
    {
        auto ref = RHIResourceRef<U>(ptr_);
#ifndef NDEBUG
        ref.MarkDebugging(debugging_);
#endif
        return ref;
    }

#pragma endregion

private:
    std::shared_ptr<T> ptr_;

#ifndef NDEBUG

#pragma region Debugging

public:
    void MarkDebugging(bool on)
    {
        debugging_ = on;
        PrintTrace();
    }

private:
    void PrintTrace(int64_t offset = 0) const
    {
        if (!debugging_ || !ptr_)
        {
            return;
        }

        RHIResource::PrintTrace(reinterpret_cast<RHIResource *>(ptr_.get()), ptr_.use_count() + offset);
    }

    bool debugging_ = false;

#pragma endregion

#endif
};

template <class T> RHIResourceRef<T> LockRHIResource(const RHIResourceWeakRef<T> &weak)
{
    return weak.lock();
}

// WARNING: this cast does not check type safety
template <class T> T *RHICast(RHIResourceRef<RHIResource> &ptr)
{
    return static_cast<T *>(ptr.get());
}

// WARNING: this cast does not check type safety
template <class T> T *RHICast(RHIResource *ptr)
{
    return static_cast<T *>(ptr);
}

// WARNING: this cast does not check type safety
template <class T> const T *RHICast(const RHIResource *ptr)
{
    return static_cast<const T *>(ptr);
}

} // namespace sparkle
