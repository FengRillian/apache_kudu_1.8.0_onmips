// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#ifndef KUDU_UTIL_RANDOM_H_
#define KUDU_UTIL_RANDOM_H_

#include <cmath>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

#include "kudu/gutil/casts.h"
#include "kudu/gutil/map-util.h"
#include "kudu/util/locks.h"

namespace kudu {

namespace random_internal {

static const uint32_t M = 2147483647L;   // 2^31-1

} // namespace random_internal

template<class R>
class StdUniformRNG;

// A very simple random number generator.  Not especially good at
// generating truly random bits, but good enough for our needs in this
// package. This implementation is not thread-safe.
class Random {
 private:
  uint32_t seed_;
 public:
  explicit Random(uint32_t s) {
    Reset(s);
  }

  // Reset the RNG to the given seed value.
  void Reset(uint32_t s) {
    seed_ = s & 0x7fffffffu;
    // Avoid bad seeds.
    if (seed_ == 0 || seed_ == random_internal::M) {
      seed_ = 1;
    }
  }

  // Next pseudo-random 32-bit unsigned integer.
  // FIXME: This currently only generates 31 bits of randomness.
  // The MSB will always be zero.
  uint32_t Next() {
    static const uint64_t A = 16807;  // bits 14, 8, 7, 5, 2, 1, 0
    // We are computing
    //       seed_ = (seed_ * A) % M,    where M = 2^31-1
    //
    // seed_ must not be zero or M, or else all subsequent computed values
    // will be zero or M respectively.  For all other values, seed_ will end
    // up cycling through every number in [1,M-1]
    uint64_t product = seed_ * A;

    // Compute (product % M) using the fact that ((x << 31) % M) == x.
    seed_ = static_cast<uint32_t>((product >> 31) + (product & random_internal::M));
    // The first reduction may overflow by 1 bit, so we may need to
    // repeat.  mod == M is not possible; using > allows the faster
    // sign-bit-based test.
    if (seed_ > random_internal::M) {
      seed_ -= random_internal::M;
    }
    return seed_;
  }

  // Alias for consistency with Next64
  uint32_t Next32() { return Next(); }

  // Next pseudo-random 64-bit unsigned integer.
  uint64_t Next64() {
    uint64_t large = Next();
    large <<= 31;
    large |= Next();
    // Fill in the highest two MSBs.
    large |= implicit_cast<uint64_t>(Next32()) << 62;
    return large;
  }

  // Returns a uniformly distributed value in the range [0..n-1]
  // REQUIRES: n > 0
  uint32_t Uniform(uint32_t n) { return Next() % n; }

  // Alias for consistency with Uniform64
  uint32_t Uniform32(uint32_t n) { return Uniform(n); }

  // Returns a uniformly distributed 64-bit value in the range [0..n-1]
  // REQUIRES: n > 0
  uint64_t Uniform64(uint64_t n) { return Next64() % n; }

  // Randomly returns true ~"1/n" of the time, and false otherwise.
  // REQUIRES: n > 0
  bool OneIn(int n) { return (Next() % n) == 0; }

  // Skewed: pick "base" uniformly from range [0,max_log] and then
  // return "base" random bits.  The effect is to pick a number in the
  // range [0,2^max_log-1] with exponential bias towards smaller numbers.
  uint32_t Skewed(int max_log) {
    return Uniform(1 << Uniform(max_log + 1));
  }

  // Samples a random number from the given normal distribution.
  double Normal(double mean, double std_dev);

  // Return a random number between 0.0 and 1.0 inclusive.
  double NextDoubleFraction() {
    return Next() / static_cast<double>(random_internal::M + 1.0);
  }

  // Sample 'k' random elements from the collection 'c' into 'result', taking care not to sample any
  // elements that are already present in 'avoid'.
  //
  // In the case that 'c' has fewer than 'k' elements then all elements in 'c' will be selected.
  //
  // 'c' should be an iterable STL collection such as a vector, set, or list.
  // 'avoid' should be an STL-compatible set.
  //
  // The results are not stored in a randomized order: the order of results will
  // match their order in the input collection.
  template<class Collection, class Set, class T>
  void ReservoirSample(const Collection& c, int k, const Set& avoid,
                       std::vector<T>* result) {
    result->clear();
    result->reserve(k);
    int i = 0;
    for (const T& elem : c) {
      if (ContainsKey(avoid, elem)) {
        continue;
      }
      i++;
      // Fill the reservoir if there is available space.
      if (result->size() < k) {
        result->push_back(elem);
        continue;
      }
      // Otherwise replace existing elements with decreasing probability.
      int j = Uniform(i);
      if (j < k) {
        (*result)[j] = elem;
      }
    }
  }
};

// Thread-safe wrapper around Random.
class ThreadSafeRandom {
 public:
  explicit ThreadSafeRandom(uint32_t s)
      : random_(s) {
  }

  void Reset(uint32_t s) {
    std::lock_guard<simple_spinlock> l(lock_);
    random_.Reset(s);
  }

  uint32_t Next() {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Next();
  }

  uint32_t Next32() {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Next32();
  }

  uint64_t Next64() {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Next64();
  }

  uint32_t Uniform(uint32_t n) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Uniform(n);
  }

  uint32_t Uniform32(uint32_t n) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Uniform32(n);
  }

  uint64_t Uniform64(uint64_t n) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Uniform64(n);
  }

  bool OneIn(int n) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.OneIn(n);
  }

  uint32_t Skewed(int max_log) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Skewed(max_log);
  }

  double Normal(double mean, double std_dev) {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.Normal(mean, std_dev);
  }

  double NextDoubleFraction() {
    std::lock_guard<simple_spinlock> l(lock_);
    return random_.NextDoubleFraction();
  }

  template<class Collection, class Set, class T>
  void ReservoirSample(const Collection& c, int k, const Set& avoid,
                       std::vector<T>* result) {
    std::lock_guard<simple_spinlock> l(lock_);
    random_.ReservoirSample(c, k, avoid, result);
  }

 private:
  simple_spinlock lock_;
  Random random_;
};

// Wraps either Random or ThreadSafeRandom as a C++ standard library
// compliant UniformRandomNumberGenerator:
//   http://en.cppreference.com/w/cpp/concept/UniformRandomNumberGenerator
template<class R>
class StdUniformRNG {
 public:
  typedef uint32_t result_type;

  explicit StdUniformRNG(R* r) : r_(r) {}
  uint32_t operator()() {
    return r_->Next32();
  }
  constexpr static uint32_t min() { return 0; }
  constexpr static uint32_t max() { return (1L << 31) - 1; }

 private:
  R* r_;
};

// Defined outside the class to make use of StdUniformRNG above.
inline double Random::Normal(double mean, double std_dev) {
  std::normal_distribution<> nd(mean, std_dev);
  StdUniformRNG<Random> gen(this);
  return nd(gen);
}

}  // namespace kudu

#endif  // KUDU_UTIL_RANDOM_H_
