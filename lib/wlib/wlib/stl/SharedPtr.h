/**
 * @file SharedPtr.h
 * @brief Shared pointer implementation.
 *
 * Implementation of a shared pointer class, many of which may have
 * ownership of the same underlying pointer, and which intelligently
 * disposes of the pointer.
 *
 * The implementation is not thread-safe, and is assuming single-threaded
 * use on Arduino.
 *
 * @author Jeff Niu
 * @date November 24, 2017
 * @bug No known bugs
 */

#ifndef EMBEDDEDCPLUSPLUS_SHAREDPTR_H
#define EMBEDDEDCPLUSPLUS_SHAREDPTR_H

#include <wlib/stl/UniquePtr.h>

namespace wlp {

    template<typename IntType>
    static inline IntType exchange_and_add(IntType *mem, IntType val) {
        IntType res = *mem;
        *mem = static_cast<IntType>(*mem + val);
        return res;
    }

    template<typename IntType>
    static inline IntType exchange_and_sub(IntType *mem, IntType val) {
        IntType res = *mem;
        *mem = static_cast<IntType>(*mem - val);
        return res;
    }

    /**
     * This class tracks the number of weak references and strong references
     * to an underlying pointer. That is, the number of weak pointers and
     * shared pointers which refer to the managed pointer, respectively.
     *
     * This class is dynamically allocated by using classes once per managed
     * pointer and is shared between all classes which refer to the managed pointer.
     * The class frees the managed pointer when there are no more strong references
     * to the pointer and destroys itself when there are no references at all.
     *
     * @tparam Ptr
     */
    template<typename Ptr>
    class ReferenceCount {
    public:
        typedef Ptr pointer;
        typedef uint16_t ptr_use_count;

    private:
        /**
         * The managed underlying pointer. This class cannot
         * be used to access the pointe directly; it only
         * has a copy to manage its deletion.
         */
        pointer m_ptr;
        /**
         * The number of shared pointers which claim ownership
         * of the underlying pointer.
         */
        ptr_use_count m_use_count;
        /**
         * The number of weak pointers which have a view
         * of the underlying pointer.
         */
        ptr_use_count m_weak_count;

    public:
        /**
         * A reference count is created with a copy of the underlying pointer
         * and with the use count and weak count set to 1. There is always
         * exactly one shared pointer referring to the pointer when this class
         * is constructed for the first time. The choice of 1 for weak count
         * is to avoid underflow.
         *
         * @param ptr the underlying pointer
         */
        ReferenceCount(pointer ptr)
                : m_ptr(ptr),
                  m_use_count(1),
                  m_weak_count(1) {}

        ~ReferenceCount() {}

        /**
         * Free the underlying pointer.
         */
        void dispose() {
            wlp::destroy(m_ptr);
        }

        /**
         * Destroy the reference count instance.
         * The instance must be dynamically allocated.
         */
        void destroy() {
            wlp::destroy(this);
        }

        /**
         * A shared pointer has been copied through construction
         * or assignment, thus increment the use count.
         */
        void add_copied_reference() {
            ++m_use_count;
        }

        /**
         * A weak pointer is locking its reference to the
         * underlying pointer, but if the actual strong reference
         * count is zero, then the underlying pointer has expired.
         */
        void add_locked_reference() {
            if (exchange_and_add<ptr_use_count>(&m_use_count, 1) == 0) {
                m_use_count = 0;
            }
        }

        /**
         * Method called when a strong reference is releasing ownership
         * of the underlying pointer. If all strong references have been
         * released, then the underlying pointer is freed. If there are
         * no more weak references, then this object is also destroyed.
         */
        void release() {
            if (exchange_and_sub<ptr_use_count>(&m_use_count, 1) == 1) {
                dispose();
                if (exchange_and_sub<ptr_use_count>(&m_weak_count, 1) == 1) {
                    destroy();
                }
            }
        }

        /**
         * A weak reference has been created to the underlying pointer,
         * that reference must be locked before accessing the pointer, thus
         * increment the weak reference count.
         */
        void add_weak_reference() {
            ++m_weak_count;
        }

        /**
         * Method called when a weak reference is releasing its temporary
         * ownership of the underlying pointer. If the weak reference is the
         * last remaining reference which claims ownership of the (now-expired)
         * underlying pointer, then this object is subsequently destroyed.
         */
        void weak_release() {
            if (exchange_and_sub<ptr_use_count>(&m_weak_count, 1) == 1) {
                destroy();
            }
        }

        /**
         * @return the number of active strong references
         */
        ptr_use_count use_count() const {
            return m_use_count;
        }

        /**
         * @return the number of active weak references
         */
        ptr_use_count weak_count() const {
            return m_weak_count;
        }

    private:
        ReferenceCount(const ReferenceCount &) = delete;

        ReferenceCount &operator=(const ReferenceCount &) = delete;
    };

    /**
     * Specialization for null pointers which avoid freeing
     * null pointers upon destruction.
     */
    template<>
    inline void ReferenceCount<nullptr_t>::dispose() {}

    template<typename T>
    class shared_ptr;

    template<typename T>
    class weak_ptr;

    template<typename Ptr>
    class SharedCount;

    template<typename Ptr>
    class WeakCount;

    /**
     * This class is responsible for managing a reference count
     * object shared among all classes which refer to the same
     * underlying pointer, for shared pointers.
     *
     * @tparam Ptr the type of the managed pointer
     */
    template<typename Ptr>
    class SharedCount {
    public:
        typedef Ptr pointer;
        typedef typename ReferenceCount<Ptr>::ptr_use_count ptr_use_count;

    private:
        friend class WeakCount<Ptr>;

        ReferenceCount<Ptr> *m_pi;

    public:
        constexpr SharedCount()
                : m_pi(nullptr) {}

        /**
         * Constructor called upon first creation of a strong reference
         * to a pointer. The reference count object is created here.
         *
         * The caller must relinquish control of the pointer to the class,
         * or else undefined behaviour may result.
         *
         * @tparam PtrType the supplied pointer type
         * @param ptr the new underlying pointer
         */
        template<typename PtrType>
        explicit SharedCount(PtrType ptr)
                : m_pi(nullptr) {
            m_pi = create<ReferenceCount<PtrType>>(ptr);
        }

        /**
         * Constructor from a unique pointer. This class assumes ownership
         * of the pointer managed from the unique pointer such that the latter
         * no longer controls any pointers.
         *
         * @tparam U unique pointer type
         * @param up the unique pointer to take
         */
        template<typename U>
        SharedCount(unique_ptr <U> &&up)
                : m_pi(nullptr) {
            m_pi = create<ReferenceCount<U *>>(up.get());
            up.release();
        }

        explicit SharedCount(const WeakCount<Ptr> &);

        /**
         * Upon destruction, the shared pointer managing
         * this object has been destroyed, thus decrement
         * the number of strong references. The @code release
         * @endcode function handles freeing resources.
         */
        ~SharedCount() {
            if (m_pi) { m_pi->release(); }
        }

        /**
         * When copy-constructing this object, another reference
         * to the underlying pointer is created, thus the reference
         * count object has its use count incremented.
         *
         * @param sc shared count to copy
         */
        SharedCount(const SharedCount<Ptr> &sc)
                : m_pi(sc.m_pi) {
            if (m_pi) { m_pi->add_copied_reference(); }
        }

        /**
         * When copy-assigning this object, another reference
         * to the underlying pointer is created, thus the reference
         * count object has its use count incremented. If this
         * shared count refers to a different existing pointer,
         * that pointer has one strong reference released.
         *
         * @param sc shared count to copy
         * @return reference to this shared count
         */
        SharedCount<Ptr> &operator=(const SharedCount<Ptr> &sc) {
            ReferenceCount<Ptr> *tmp = sc.m_pi;
            if (tmp != m_pi) {
                if (tmp) { tmp->add_copied_reference(); }
                if (m_pi) { m_pi->release(); }
                m_pi = tmp;
            }
            return *this;
        }

        void swap(SharedCount<Ptr> &sc) {
            wlp::swap(m_pi, sc.m_pi);
        }

        /**
         * @return the current number of strong references to the
         * underlying pointer, or zero if the reference is null.
         */
        ptr_use_count use_count() const {
            return m_pi ? m_pi->use_count() : static_cast<ptr_use_count>(0);
        }

        /**
         * @return whether there is exactly one shared pointer
         * which claims ownership to the underlying pointer.
         */
        bool unique() const {
            return use_count() == 1;
        }

        bool less(const SharedCount<Ptr> &) const;

        bool less(const WeakCount<Ptr> &) const;

        template<typename PtrType>
        friend inline bool operator==(const SharedCount<PtrType> &sc1, const SharedCount<PtrType> &sc2) {
            return sc1.m_pi == sc2.m_pi;
        }

    };

    /**
     * This class is responsible for managing a reference count
     * object that all weak references to the underlying pointer
     * contain, for weak pointers.
     *
     * @tparam Ptr pointer type
     */
    template<typename Ptr>
    class WeakCount {
    public:
        typedef Ptr pointer;
        typedef typename ReferenceCount<Ptr>::ptr_use_count ptr_use_count;

    private:
        friend class SharedCount<Ptr>;

        ReferenceCount<Ptr> *m_pi;

    public:
        constexpr WeakCount()
                : m_pi(nullptr) {}

        /**
         * When creating a weak reference from a shared reference,
         * a weak pointer is being created, thus increment the
         * number of weak references to the underlying pointer.
         *
         * @param sc shared count to copy
         */
        WeakCount(const SharedCount<Ptr> &sc)
                : m_pi(sc.m_pi) {
            if (m_pi) { m_pi->add_weak_reference(); }
        }

        /**
         * When creating a weak reference from another weak reference,
         * a weak pointer is being created, this increment the
         * number of weak references to the underlying pointer.
         *
         * @param wc weak count to copy
         */
        WeakCount(const WeakCount<Ptr> &wc)
                : m_pi(wc.m_pi) {
            if (m_pi) { m_pi->add_weak_reference(); }
        }

        /**
         * When this object is destructed, the using weak pointer
         * is also being destructed, thus reduce the number of
         * weak references. @code weak_release @endcode handles
         * cleaning up the resource count resource.
         */
        ~WeakCount() {
            if (m_pi) { m_pi->weak_release(); }
        }

        /**
         * When copying a shared count, a new weak reference is being
         * created. If there is a previous and different pointer,
         * that pointer has one weak reference released, and a
         * new weak reference is created to the current pointer.
         *
         * @param sc shared count to copy
         * @return reference to this weak count
         */
        WeakCount<Ptr> &operator=(const SharedCount<Ptr> &sc) {
            ReferenceCount<Ptr> *tmp = sc.m_pi;
            if (tmp) { tmp->add_weak_reference(); }
            if (m_pi) { m_pi->weak_release(); }
            m_pi = tmp;
            return *this;
        }

        /**
         * When copying a weak count, a new weak reference is being
         * created. If there is a previous and different pointer,
         * that pointer has one weak reference released, and a
         * new weak reference is created to the current pointer.
         *
         * @param sc shared count to copy
         * @return reference to this weak count
         */
        WeakCount<Ptr> &operator=(const WeakCount<Ptr> &wc) {
            ReferenceCount<Ptr> *tmp = wc.m_pi;
            if (tmp) { tmp->add_weak_reference(); }
            if (m_pi) { m_pi->weak_release(); }
            m_pi = tmp;
            return *this;
        }

        void swap(WeakCount<Ptr> &wc) {
            wlp::swap(m_pi, wc.m_pi);
        }

        ptr_use_count use_count() const {
            return m_pi->use_count();
        }

        bool less(const SharedCount<Ptr> &) const;

        bool less(const WeakCount<Ptr> &) const;
    };

    template<typename Ptr>
    inline bool SharedCount<Ptr>::less(const WeakCount<Ptr> &wc) const {
        return m_pi < wc.m_pi;
    }

    template<typename Ptr>
    inline bool SharedCount<Ptr>::less(const SharedCount<Ptr> &sc) const {
        return m_pi < sc.m_pi;
    }

    template<typename Ptr>
    inline bool WeakCount<Ptr>::less(const WeakCount<Ptr> &wc) const {
        return m_pi < wc.m_pi;
    }

    template<typename Ptr>
    inline bool WeakCount<Ptr>::less(const SharedCount<Ptr> &sc) const {
        return m_pi < sc.m_pi;
    }

    /**
     * Creation of a shared count from a weak count, means that
     * the weak pointer is locking its reference to a shared pointer.
     *
     * @tparam Ptr pointer type
     * @param wc weak count to copy
     */
    template<typename Ptr>
    inline SharedCount<Ptr>::SharedCount(const WeakCount<Ptr> &wc)
            : m_pi(wc.m_pi)
    { m_pi->add_locked_reference(); }

    template<typename T>
    class shared_ptr {
    public:
        typedef T val_type;
        typedef typename ReferenceCount<T *>::ptr_use_count ptr_use_count;

    private:
        template<typename U> friend
        class shared_ptr;

        template<typename U> friend
        class weak_ptr;

        SharedCount<T *> m_refcount;
        val_type *m_ptr;

    public:
        constexpr shared_ptr()
                : m_refcount(),
                  m_ptr(nullptr) {
        }

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        explicit shared_ptr(U *ptr)
                : m_refcount(ptr),
                  m_ptr(ptr)
        { static_assert(sizeof(U) > 0, "Pointer to incomplete type"); }

        template<typename U>
        shared_ptr(const shared_ptr<U> &sp, T *ptr)
                : m_refcount(sp.m_refcount),
                  m_ptr(ptr) {}

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        shared_ptr(const shared_ptr<U> &sp)
                : m_refcount(sp.m_refcount),
                  m_ptr(sp.m_ptr) {}

        shared_ptr(const shared_ptr<T> &sp)
                : m_refcount(sp.m_refcount),
                  m_ptr(sp.m_ptr) {}

        shared_ptr(shared_ptr<T> &&sp)
                : m_refcount(),
                  m_ptr(sp.m_ptr) {
            m_refcount.swap(sp.m_refcount);
            sp.m_ptr = nullptr;
        }

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        shared_ptr(shared_ptr<U> &&sp)
                : m_refcount(),
                  m_ptr(sp.m_ptr) {
            m_refcount.swap(sp.m_refcount);
            sp.m_ptr = nullptr;
        }

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        explicit shared_ptr(const weak_ptr<U> &wp)
                : m_refcount(wp.m_refcount),
                  m_ptr(wp.m_ptr) {}

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        shared_ptr(unique_ptr <U> &&up)
                : m_refcount(),
                  m_ptr(up.get()) {
            U *tmp = up.get();
            m_refcount = SharedCount<T *>(move(up));
        }

        constexpr shared_ptr(nullptr_t)
                : m_refcount(),
                  m_ptr(nullptr) {}

        shared_ptr<T> &operator=(const shared_ptr<T> &sp) {
            m_ptr = sp.m_ptr;
            m_refcount = sp.m_refcount;
            return *this;
        }

        template<typename U>
        shared_ptr<T> &operator=(const shared_ptr<U> &sp) {
            m_ptr = sp.m_ptr;
            m_refcount = sp.m_refcount;
            return *this;
        }

        shared_ptr<T> &operator=(shared_ptr<T> &&sp) {
            shared_ptr(move(sp)).swap(*this);
            return *this;
        }

        template<typename U>
        shared_ptr<T> &operator=(shared_ptr<U> &&sp) {
            shared_ptr(move(sp)).swap(*this);
            return *this;
        }

        template<typename U>
        shared_ptr<T> &operator=(unique_ptr <U> &&up) {
            shared_ptr(move(up)).swap(*this);
            return *this;
        }

        void reset() {
            shared_ptr().swap(*this);
        }

        template<typename U>
        void reset(U *ptr) {
            shared_ptr(ptr).swap(*this);
        }

        typename add_lvalue_reference<T>::type operator*() const {
            return *m_ptr;
        }

        val_type *operator->() const {
            return m_ptr;
        }

        val_type *get() const {
            return m_ptr;
        }

        explicit operator bool() const {
            return m_ptr != 0;
        }

        bool unique() const {
            return m_refcount.unique();
        }

        ptr_use_count use_count() const {
            return m_refcount.use_count();
        }

        void swap(shared_ptr<T> &sp) {
            wlp::swap(m_ptr, sp.m_ptr);
            m_refcount.swap(sp.m_refcount);
        }

        template<typename U>
        bool owner_before(const shared_ptr<U> &sp) const {
            return m_refcount.less(sp.m_refcount);
        }

        template<typename U>
        bool owner_before(const weak_ptr<U> &wp) const {
            return m_refcount.less(wp.m_refcount);
        }

        weak_ptr<T> weak() const {
            return weak_ptr<T>(*this);
        }

    };

    template<typename T>
    inline void swap(shared_ptr<T> &sp1, shared_ptr<T> &sp2) {
        sp1.swap(sp2);
    }

    template<typename T, typename U>
    inline shared_ptr<T> static_pointer_cast(const shared_ptr<U> &sp) {
        return shared_ptr<T>(sp, static_cast<T *>(sp.get()));
    }

    template<typename T, typename U>
    inline shared_ptr<T> const_pointer_cast(const shared_ptr<U> &sp) {
        return shared_ptr<T>(sp, const_cast<T *>(sp.get()));
    };

    template<typename T, typename U>
    inline shared_ptr<T> dynamic_pointer_cast(const shared_ptr<U> &sp) {
        if (T *ptr = dynamic_cast<T *>(sp.get())) {
            return shared_ptr<T>(sp, ptr);
        }
        return shared_ptr<T>();
    };

    template<typename T, typename U>
    inline shared_ptr<T> reinterpret_pointer_cast(const shared_ptr<U> &sp) {
        return shared_ptr<T>(sp, reinterpret_cast<T *>(sp.get()));
    };

    template<typename T>
    class weak_ptr {
    public:
        typedef T val_type;
        typedef typename ReferenceCount<T *>::ptr_use_count ptr_use_count;

    private:

        template<typename U> friend
        class shared_ptr;

        template<typename U> friend
        class weak_ptr;

        WeakCount<T *> m_refcount;
        val_type *m_ptr;

    public:
        constexpr weak_ptr()
                : m_refcount(),
                  m_ptr(nullptr) {
        }

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        weak_ptr(const weak_ptr<U> &wp)
                : m_refcount(wp.m_refcount) {
            m_ptr = wp.lock().get();
        };

        template<typename U, typename = typename enable_if<
                is_convertible<U *, T *>::value
        >::type>
        weak_ptr(const shared_ptr<U> &sp)
                : m_refcount(sp.m_refcount),
                  m_ptr(sp.m_ptr) {}

        template<typename U>
        weak_ptr &operator=(const weak_ptr<U> &wp) {
            m_ptr = wp.lock().get();
            m_refcount = wp.m_refcount;
            return *this;
        }

        template<typename U>
        weak_ptr &operator=(const shared_ptr<U> &sp) {
            m_ptr = sp.m_ptr;
            m_refcount = sp.m_refcount;
            return *this;
        }

        shared_ptr<T> lock() const {
            return expired() ? shared_ptr<T>() : shared_ptr<T>(*this);
        }

        ptr_use_count use_count() const {
            return m_refcount.use_count();
        }

        bool expired() const {
            return m_refcount.use_count() == 0;
        }

        template<typename U>
        bool owner_before(const shared_ptr<U> &sp) {
            return m_refcount.less(sp.m_refcount);
        }

        template<typename U>
        bool owner_before(const weak_ptr<U> &wp) {
            return m_refcount.less(wp.m_refcount);
        }

        void reset() {
            weak_ptr().swap(*this);
        }

        void swap(weak_ptr<T> &wp) {
            wlp::swap(m_ptr, wp.m_ptr);
            m_refcount.swap(wp.m_refcount);
        }
    };

    template<typename T>
    inline void swap(weak_ptr<T> &wp1, weak_ptr<T> &wp2) {
        wp1.swap(wp2);
    }

}

#endif //EMBEDDEDCPLUSPLUS_SHAREDPTR_H
