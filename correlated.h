#ifndef CORRELATED_H
#define CORRELATED_H

/*
Correlated precomputes

CorrelatedStore object, which has synced correlated precomputes between servers
Also includes io objects for OT, which have their own precomputes

CorrelatedStore maintains a cache of precomputes, made in batch_size chunks at a time to reduce rounds
New batches are build either as it runs out, or by calling maybeUpdate

For now, only uses DaBits for b2a Share conversion.
boolean beaver triples are supported as they are straightforward, but not currently made.

Due to send buffers potentially filling up, it forks out a child to do sending, while parent receives
It also waits for the child to finish before exiting or moving to a substep that will send, to stay synced
*/

#include <emp-ot/emp-ot.h>
#include <emp-tool/emp-tool.h>
#include <queue>

#include "constants.h"
#include "ot.h"
#include "share.h"

// A Cache of correlated bits of different types
// Makes batch_size at once, when running low
class CorrelatedStore {
  const size_t batch_size;  // How many to make at once
  const int server_num;
  const int serverfd;

  // If lazy, does fast but insecure offline.
  const bool lazy;

  // Arithmetic triple generator
  // ArithTripleGenerator* triple_gen = nullptr;

  std::queue<const DaBit* const > dabit_store;
  std::queue<const BooleanBeaverTriple* const > btriple_store;
  std::queue<const BeaverTriple* const> atriple_store;

  // return N new daBits
  const DaBit* const * const generateDaBit(const size_t N);

  // add to the store.
  // Adds at least batch_size (or bool_batch_size), or n if bigger
  void addBoolTriples(const size_t n = 0);
  void addTriples(const size_t n = 0);
  void addDaBits(const size_t n = 0);

  OT_Wrapper* const ot0;
  OT_Wrapper* const ot1;

public:

  // True to work correctly on large batches, and faster
  // False to do debugging
  const bool do_fork;

  CorrelatedStore(const int serverfd, const int idx,
                  OT_Wrapper* const ot0, OT_Wrapper* const ot1,
                  const size_t batch_size,
                  const bool lazy = false, const bool do_fork = true)
  : batch_size(batch_size)
  , server_num(idx)
  , serverfd(serverfd)
  , lazy(lazy)
  , ot0(ot0)
  , ot1(ot1)
  , do_fork(do_fork)
  {
    if (lazy) {
      std::cout << "Doing fast but insecure dabit precomputes." << std::endl;
    }
    /*
    else if (fmpz_cmp_ui(Int_Modulus, 1ULL << 49) < 0) {
      std::cout << "Using PALISADE SHE arith triples" << std::endl;
      triple_gen = new ArithTripleGenerator(serverfd, server_num);
    } else {
      std::cout << "Mod big, using slower arith triples" << std::endl;
    }
    */
  }

  ~CorrelatedStore();

  // get from store, and maybe add if necessary
  const BooleanBeaverTriple* const getBoolTriple();
  const BeaverTriple* const getTriple();
  const DaBit* const getDaBit();

  void printSizes();
  // Precompute if not enough.
  void maybeUpdate();

  // check if enough to make n. if not, call add
  void checkBoolTriples(const size_t n = 0);
  // Always forces it to make it even if lazy, for testing purposes.
  void checkTriples(const size_t n = 0, const bool always = false);
  void checkDaBits(const size_t n = 0);

  // compute with store elements. Does batches of size N.

  // x, y, ret are [N]
  // does ret[i] = x[i] * y[i], as shares
  const bool* const multiplyBoolShares(
    const size_t N, const bool* const x, const bool* const y);
  fmpz_t* const multiplyArithmeticShares(
    const size_t N, const fmpz_t* const x, const fmpz_t* const y);

  // x, y, z are [N][num_bits], ret is [N]
  // Treats x[i], y[i], z[i] as array of bits
  // sets z[i] and ret[i] as x[i] + y[i] and carry[i], as shares
  // Currently unused
  const bool* const addBinaryShares(
      const size_t N, const size_t* const num_bits,
      const bool* const * const x, const bool* const * const y,
      bool* const * const z);

  // x, ret is [N]
  // Turns binary share x[i] into arith share ret[i]
  // Single bit. One round.
  fmpz_t* const b2a_daBit_single(const size_t N, const bool* const x);
  // Multiple bits. Use a dabit per bit in parallel, so one round
  fmpz_t* const b2a_daBit_multi(const size_t N, const size_t* const num_bits,
                                const fmpz_t* const x);

  // N inputs of b bits
  // 00, 01 -> 0, 10 -> 1, 11 -> -1, accumulate into buckets
  // shares are 2*N*b sized. first N*b is bucket 0, second is bucket 1
  void heavy_ot(const size_t N, const size_t b,
                const bool* const shares_x0, const bool* const shares_x1,
                const bool* const valid,
                fmpz_t* const bucket0, fmpz_t* const bucket1);

  // Using intsum_ot, multiple bits
  // TODO: shift mod to fmpz
  fmpz_t* b2a_ot(const size_t num_shares, const size_t num_values,
                 const size_t* const num_bits, const fmpz_t* const shares,
                 const size_t mod = 0);

  // Comparisons. 
  // Assumes modulus is large enough, so <N/2 is positive, >N/2 is negative (x-N)
  // Return true if first < second. (so is index of larger).
  // Equality is merged in, since rare/should not happen case so far.

  // True if [x] > c
  bool* cmp_c(const size_t N, const fmpz_t* const x, const fmpz_t* const c);
  // True if [x] > [y]
  bool* cmp(const size_t N, const fmpz_t* const x, const fmpz_t* const y);
  // Given [x], sets out = [|x|] via negating if [x] < 0
  void abs(const size_t N, const fmpz_t* const x, fmpz_t* const out);
  // True if [|x|] > [|y|]
  bool* abs_cmp(const size_t N, const fmpz_t* const x, const fmpz_t* const y);

  // x, y are Nxb shares of N total b-bit numbers. 
  // I.e. x[i,j] is additive share of bit j of number i. 
  // Returns additive shares of [x < y]
  fmpz_t* cmp_bit(const size_t N, const size_t b,
                  const fmpz_t* const x, const fmpz_t* const y);
};

#endif
