#pragma once

#if defined(__SSE__) || defined(_M_X64)
 #include <xmmintrin.h>
#endif

namespace dustify
{

// Flush-to-zero / denormals-are-zero for the duration of a render call
struct ScopedFlushDenormals
{
#if defined(__SSE__) || defined(_M_X64)
    unsigned int previous = _mm_getcsr();
    ScopedFlushDenormals() { _mm_setcsr (previous | 0x8040); }
    ~ScopedFlushDenormals() { _mm_setcsr (previous); }
#endif
};

} // namespace dustify
