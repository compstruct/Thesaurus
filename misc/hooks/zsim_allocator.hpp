#ifndef __ZSIM_ALLOCATOR_HPP__
#define __ZSIM_ALLOCATOR_HPP__

#include <memory>
#include <cfloat>
#include "zsim_hooks.h"

template <typename T>
class zsim_allocator: public std::allocator<T>
{
    private:
        // DataValue zsim_minValue;
        // DataValue zsim_maxValue;

    public:

        template<typename U>
		struct rebind
        {
			typedef zsim_allocator<U> other;
		};

        zsim_allocator()
            :std::allocator<T>()
        {}

        zsim_allocator(const zsim_allocator<T>& o)
            :std::allocator<T>(o)
        {}

        T* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0)
        {
            T* p = std::allocator<T>::allocate(n, hint);
            // if (p)
            //     zsim_allocate_approximate(p, n*sizeof(T));
            return p;
        }

        void deallocate(T* p, std::size_t n)
        {
            std::allocator<T>::deallocate(p, n);
            zsim_deallocate_approximate(p);
        }
};

template <>
class zsim_allocator<float>: public std::allocator<float>
{
    private:
        // DataValue zsim_minValue;
        // DataValue zsim_maxValue;

    public:

        template<typename U>
		struct rebind
        {
			typedef zsim_allocator<U> other;
		};

        zsim_allocator()
            :std::allocator<float>()
        {}

        zsim_allocator(const zsim_allocator<float>& o)
            :std::allocator<float>(o)
        {}

        float* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0)
        {
            float* p = std::allocator<float>::allocate(n, hint);
            if (p)
            {
                // zsim_minValue.HOOKS_FLOAT = -FLT_MAX;
                // zsim_maxValue.HOOKS_FLOAT = FLT_MAX;
                zsim_allocate_approximate(p, n*sizeof(float), HOOKS_FLOAT);
            }
            return p;
        }

        void deallocate(float* p, std::size_t n)
        {
            std::allocator<float>::deallocate(p, n);
            zsim_deallocate_approximate(p);
        }
};

template <>
class zsim_allocator<double>: public std::allocator<double>
{
    private:
        // DataValue zsim_minValue;
        // DataValue zsim_maxValue;

    public:

        template<typename U>
		struct rebind
        {
			typedef zsim_allocator<U> other;
		};

        zsim_allocator()
            :std::allocator<double>()
        {}

        zsim_allocator(const zsim_allocator<double>& o)
            :std::allocator<double>(o)
        {}

        double* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0)
        {
            double* p = std::allocator<double>::allocate(n, hint);
            if (p)
            {
                // zsim_minValue.HOOKS_DOUBLE = -DBL_MAX;
                // zsim_maxValue.HOOKS_DOUBLE = DBL_MAX;
                zsim_allocate_approximate(p, n*sizeof(double), HOOKS_DOUBLE);
            }
            return p;
        }

        void deallocate(double* p, std::size_t n)
        {
            std::allocator<double>::deallocate(p, n);
            zsim_deallocate_approximate(p);
        }
};

// Use this for pure float structs
template <typename T>
class explicit_zsim_float_allocator: public std::allocator<T>
{
    private:
        // DataValue zsim_minValue;
        // DataValue zsim_maxValue;

    public:

        template<typename U1>
		struct rebind
        {
			typedef explicit_zsim_float_allocator<U1> other;
		};

        explicit_zsim_float_allocator()
            :std::allocator<T>()
        {}

        explicit_zsim_float_allocator(const explicit_zsim_float_allocator<T>& o)
            :std::allocator<T>(o)
        {}

        T* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0)
        {
            T* p = std::allocator<T>::allocate(n, hint);
            if (p)
            {
                // zsim_minValue.HOOKS_FLOAT = -FLT_MAX;
	            // zsim_maxValue.HOOKS_FLOAT = FLT_MAX;
                zsim_allocate_approximate(p, n*sizeof(T), HOOKS_FLOAT);
            }
            return p;
        }

        void deallocate(T* p, std::size_t n)
        {
            std::allocator<T>::deallocate(p, n);
            zsim_deallocate_approximate(p);
        }
};

// Use this for pure double structs
template <typename T>
class explicit_zsim_double_allocator: public std::allocator<T>
{
    private:
        // DataValue zsim_minValue;
        // DataValue zsim_maxValue;

    public:

        template<typename U1>
		struct rebind
        {
			typedef explicit_zsim_double_allocator<U1> other;
		};

        explicit_zsim_double_allocator()
            :std::allocator<T>()
        {}

        explicit_zsim_double_allocator(const explicit_zsim_double_allocator<T>& o)
            :std::allocator<T>(o)
        {}

        T* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0)
        {
            T* p = std::allocator<T>::allocate(n, hint);
            if (p)
            {
                // zsim_minValue.HOOKS_DOUBLE = -DBL_MAX;
	            // zsim_maxValue.HOOKS_DOUBLE = DBL_MAX;
                zsim_allocate_approximate(p, n*sizeof(T), HOOKS_DOUBLE);
            }
            return p;
        }

        void deallocate(T* p, std::size_t n)
        {
            std::allocator<T>::deallocate(p, n);
            zsim_deallocate_approximate(p);
        }
};

#endif // __ZSIM_ALLOCATOR_HPP__
