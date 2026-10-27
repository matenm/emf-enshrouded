#pragma once
// Scoped SRW locks: released on every way out of the scope, including a C++ exception (bad_alloc). A hardware fault
// inside a fault guard (__try/__except) does not run destructors, so code under such a guard must not call into
// drivers or the game while holding one of these.
#include <windows.h>

namespace emf {

class SrwExclusive {
public:
    explicit SrwExclusive(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~SrwExclusive() { ReleaseSRWLockExclusive(&lock_); }
    SrwExclusive(const SrwExclusive&) = delete;
    SrwExclusive& operator=(const SrwExclusive&) = delete;

private:
    SRWLOCK& lock_;
};

class SrwShared {
public:
    explicit SrwShared(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockShared(&lock_); }
    ~SrwShared() { ReleaseSRWLockShared(&lock_); }
    SrwShared(const SrwShared&) = delete;
    SrwShared& operator=(const SrwShared&) = delete;

private:
    SRWLOCK& lock_;
};

}  // namespace emf
