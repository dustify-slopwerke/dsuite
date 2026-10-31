#pragma once

// Counts allocations made while `counting` is set on this thread, to show the audio thread
// doesn't allocate (operator new is replaced in LooperTests.cpp, for the whole test binary)
namespace allocation
{
extern thread_local bool counting;
extern thread_local long allocations;
} // namespace allocation
