#pragma once

#include <atomic>
#include <tuple>
#include <vector>
#include <mutex>

#include "../data_structures/circular_buffer.hpp"
#include "../mark_pointer.hpp"
#include "../output.hpp"
#include "../debug.hpp"


namespace utils_tm
{
namespace reclamation_tm
{

    // TODO THIS ONLY WORKS WITH THE DEFAULT DESTRUCTOR?

    template<class T, template<class> class Queue = circular_buffer> // create default parameters for D
    class counting_manager
    {
    private:
        using this_type           = counting_manager<T,Queue>;

        class internal_type : public T
        {
        public:
            template <class ... Args>
            internal_type(Args&& ... arg)
                : T(std::forward<Args>(arg)...), counter(0), epoch(0)
            { }

            void erase()
            { this->T::~T(); }

            template <class ... Args>
            void emplace(Args&& ... arg)
            { new (this) T(std::forward<Args>(arg)...); }

            std::atomic_uint counter;
            uint epoch;
            static constexpr uint mark = 1<<31;
        };
        using queue_type = Queue<internal_type*>;
    public:
        using pointer_type        = T*;
        using atomic_pointer_type = std::atomic<T*>;

        counting_manager() = default;
        counting_manager(const counting_manager&) = delete;
        counting_manager& operator=(const counting_manager&) = delete;
        counting_manager(counting_manager&& other) = default;
        counting_manager& operator=(counting_manager&& other) = default;
        ~counting_manager() = default;

        class handle_type
        {
        private:
            using parent_type   = counting_manager<T,Queue>;
            using this_type     = handle_type;
            using internal_type = typename parent_type::internal_type;

        public:
            using pointer_type        = typename parent_type::pointer_type;
            using atomic_pointer_type = typename parent_type::atomic_pointer_type;

            handle_type(parent_type& parent) : _parent(parent) { }
            handle_type(const handle_type&) = delete;
            handle_type& operator=(const handle_type&) = delete;
            handle_type(handle_type&& other) noexcept = default;
            handle_type& operator=(handle_type&& other) noexcept = default;
            ~handle_type() = default;

        private:
            parent_type& _parent;

        public:
            template <class ...Args>
            inline T*   create_pointer(Args&& ...arg) const;

            inline T*   protect(atomic_pointer_type& ptr);
            inline void safe_delete(pointer_type ptr);

            inline void protect_raw(pointer_type ptr) const;
            inline void delete_raw(pointer_type ptr);
            inline bool is_safe(pointer_type ptr);

            inline void unprotect(pointer_type ptr);
            inline void unprotect(std::vector<T*>& vec);


            void print() const;
        private:
            inline void increment_counter(pointer_type ptr) const;
            inline void decrement_counter(pointer_type ptr);
            inline void mark_counter(pointer_type ptr);
            inline void internal_delete(internal_type* iptr);

        };
        handle_type get_handle() { return handle_type(*this); }

    private:
        std::mutex _freelist_mutex;
        queue_type _freelist;
    };




    template <class T, template <class> class Q> template <class ...Args>
    T* counting_manager<T,Q>::handle_type::create_pointer(Args&& ... arg) const
    {
        internal_type* temp = nullptr;
        {   // area for the lock_guard
            std::lock_guard<std::mutex> guard(_parent._freelist_mutex);
            auto result = _parent._freelist.pop_front();
            if (result)
            {
                temp = result.value();
            }
        }
        if (temp)
        {
            temp->emplace(std::forward<Args>(arg)...);
            return temp;
        }
        return new internal_type(std::forward<Args>(arg)...);
    }

    template <class T, template <class> class Q>
    T* counting_manager<T,Q>::handle_type::protect(atomic_pointer_type& ptr)
    {
        auto temp  = ptr.load();
        increment_counter(temp);
        auto temp2 = ptr.load();
        while (temp != temp2)
        {
            decrement_counter(temp);
            temp = temp2;
            increment_counter(temp);
            temp2 = ptr.load();
        }
        return temp;
    }

    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::protect_raw(pointer_type ptr) const
    {
        increment_counter(ptr);
    }

    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::safe_delete(pointer_type ptr)
    {
        mark_counter(ptr);
    }



    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::delete_raw(pointer_type ptr)
    {
        mark_counter(ptr);
    }

    template <class T, template <class> class Q>
    bool counting_manager<T,Q>::handle_type::is_safe(pointer_type ptr)
    {
        internal_type* iptr = static_cast<internal_type*>(mark::clear(ptr));
        return !iptr->counter.load();
    }

    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::unprotect(pointer_type ptr)
    {
        decrement_counter(ptr);
    }

    template <class T, template <class> class Q>
    void
    counting_manager<T,Q>::handle_type::unprotect(std::vector<pointer_type>& vec)
    {
        for (auto ptr : vec)
            decrement_counter(ptr);
    }


    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::print() const
    {
        std::lock_guard<std::mutex> guard(_parent._freelist_mutex);
        out_tm::out() << "* print in counting reclamation strategy "
                      << _parent._freelist.size() << " elements in the freelist *"
                      << std::endl;
    }


    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::increment_counter(pointer_type ptr) const
    {
        static_cast<internal_type*>(mark::clear(ptr))->counter.fetch_add(1);
    }

    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::decrement_counter(pointer_type ptr)
    {
        internal_type* iptr = static_cast<internal_type*>(mark::clear(ptr));
        auto           temp = iptr->counter.fetch_sub(1);
        debug_tm::if_debug("Warning: in decrement_counter - "
                           "created a negative counter", temp == 0);
        debug_tm::if_debug("Warning: in decrement counter - "
                           "weird counter",
                           (temp>666) && (temp<internal_type::mark+1));

        if (temp == internal_type::mark+1)
        {
            internal_delete(iptr);
        }
    }

    template <class T, template <class> class Q>
    void counting_manager<T,Q>::handle_type::mark_counter(pointer_type ptr)
    {
        internal_type* iptr = static_cast<internal_type*>(mark::clear(ptr));
        auto           temp = iptr->counter.load();
        if (temp & internal_type::mark)
            return;

        temp = iptr->counter.fetch_or(internal_type::mark);

        if (temp == 0) // element was unused, and not marked before
            internal_delete();
    }

    template <class T, template <class> class Q>
    void
    counting_manager<T,Q>::handle_type::internal_delete(internal_type* iptr)
    {
        auto temp = internal_type::mark;
        if (! iptr->counter.compare_exchange_strong(temp, 0))
            return;

        iptr->internal_type::erase();
        iptr->epoch++;

        std::lock_guard<std::mutex> guard(_parent._freelist_mutex);
        _parent._freelist.push_back(iptr);
    }
};
};
