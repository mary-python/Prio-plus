/**
Oblivious Transfer code.
For share conversion
*/

#ifndef PROTO_H
#define PROTO_H

#include <emp-ot/emp-ot.h>
#include <emp-tool/emp-tool.h>
#include <iostream>
#include <queue>

#include "share.h"

using namespace emp;

uint64_t bitsum_ot_sender(NetIO* const io, const bool* const shares, const bool* const valid, const size_t n);
uint64_t bitsum_ot_receiver(NetIO* const io, const bool* const shares, const size_t n);

// Non-batched versions. Nwo unused
// uint64_t intsum_ot_sender(NetIO* const io, const uint64_t* const shares,
//                           const bool* const valid,
//                           const size_t n, const size_t num_bits);
// uint64_t intsum_ot_receiver(NetIO* const io, const uint64_t* const shares,
//                             const size_t n, const size_t num_bits);

// Batched version, for multiple values
// shares: #shares * #values, as (s0v0, s0v1, s0v2, s1v0, ...)
// valid: validity of share i
// bits: length of value j
uint64_t* intsum_ot_sender(NetIO* const io, const uint64_t* const shares,
                           const bool* const valid, const size_t* const num_bits,
                           const size_t num_shares, const size_t num_values);
uint64_t* intsum_ot_receiver(NetIO* const io, const uint64_t* const shares,
                             const size_t* const num_bits,
                             const size_t num_shares, const size_t num_values);

std::queue<BooleanBeaverTriple*> gen_boolean_beaver_triples(const int server_num, const unsigned int m, NetIO* const io0, NetIO* const io1);

BeaverTriple* generate_beaver_triple(const int serverfd, const int server_num, NetIO* const io0, NetIO* const io1);

BeaverTriple* generate_beaver_triple_lazy(const int serverfd, const int server_num);

#endif
