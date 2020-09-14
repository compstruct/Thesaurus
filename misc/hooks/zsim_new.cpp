#include <cstdlib>
#include <iostream>

inline void* operator new[] (std::size_t size)
{
    std::cout << "Test passed\n";
    void* Tmp = std::malloc(size);
    return Tmp;
}

inline void operator delete[] (void* ptr) noexcept
{
    std::free(ptr);
}