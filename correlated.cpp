#include "correlated.h"

#include <sys/wait.h>

#include <iostream>

#include "constants.h"
#include "net_share.h"
#include "ot.h"
#include "utils.h"

void CorrelatedStore::addBoolTriples(const size_t n) {
  auto start = clock_start();
  const size_t num_to_make = (n > batch_size ? n : batch_size);
  std::cout << "adding booltriples: " << num_to_make << std::endl;
  std::queue<BooleanBeaverTriple*> new_triples = gen_boolean_beaver_triples(server_num, num_to_make, ot0, ot1);
  for (unsigned int i = 0; i < num_to_make; i++) {
    btriple_store.push(new_triples.front());
    new_triples.pop();
  }
  std::cout << "addBoolTriples timing : " << sec_from(start) << std::endl;
}

void CorrelatedStore::addTriples(const size_t n) {
  auto start = clock_start();
  const size_t num_to_make = (n > batch_size ? n : batch_size);
  std::cout << "adding triples: " << num_to_make << std::endl;
  bool triple_gen = false;  // TODO: re-implement
  if (triple_gen) {  // not null pointer
    // std::vector<BeaverTriple*> new_triples = triple_gen->generateTriples(num_to_make);
    // for (unsigned int i = 0; i < num_to_make; i++)
    //   atriple_store.push(new_triples[i]);
  } else {
    std::cout << "Using lazy beaver triples" << std::endl;
    for (unsigned int i = 0; i < num_to_make; i++) {
      BeaverTriple* triple = generate_beaver_triple_lazy(serverfd, server_num);
      atriple_store.push(triple);
    }
  }
  std::cout << "addTriples timing : " << sec_from(start) << std::endl;
}

void CorrelatedStore::addDaBits(const size_t n) {
  auto start = clock_start();
  // const size_t num_to_make = (n > batch_size ? n : batch_size);
  const size_t num_to_make = n;  // Currently to make "end to end" easier to benchmark
  std::cout << "adding dabits: " << num_to_make << std::endl;
  if (!lazy) {
    DaBit** dabit = generateDaBit(num_to_make);
    for (unsigned int i = 0; i < num_to_make; i++)
      dabit_store.push(dabit[i]);
    delete[] dabit;
  } else {  // Lazy generation: make local and send over
    DaBit** const dabit = new DaBit*[num_to_make];
    for (unsigned int i = 0; i < num_to_make; i++)
      dabit[i] = new DaBit;
    if (server_num == 0) {
      DaBit** const other_dabit = new DaBit*[num_to_make];
      for (unsigned int i = 0; i < num_to_make; i++) {
        other_dabit[i] = new DaBit;
        makeLocalDaBit(dabit[i], other_dabit[i]);
      }
      send_DaBit_batch(serverfd, other_dabit, num_to_make);
      for (unsigned int i = 0; i < num_to_make; i++) {
        dabit_store.push(dabit[i]);
        delete other_dabit[i];
      }
      delete[] other_dabit;
    } else {
      recv_DaBit_batch(serverfd, dabit, num_to_make);
      for (unsigned int i = 0; i < num_to_make; i++)
        dabit_store.push(dabit[i]);
    }
    delete[] dabit;
  }
  std::cout << "addDaBits timing : " << sec_from(start) << std::endl;
}

void CorrelatedStore::checkBoolTriples(const size_t n) {
  if (btriple_store.size() < n) addBoolTriples(n - btriple_store.size());
}

void CorrelatedStore::checkTriples(const size_t n, const bool always) { 
  if ((!lazy or always) and atriple_store.size() < n) addTriples(n - atriple_store.size());
}

void CorrelatedStore::checkDaBits(const size_t n) {
  if (dabit_store.size() < n) addDaBits(n - dabit_store.size());
}

BooleanBeaverTriple* CorrelatedStore::getBoolTriple() {
  checkBoolTriples(1);
  BooleanBeaverTriple* ans = btriple_store.front();
  btriple_store.pop();
  return ans;
}

BeaverTriple* CorrelatedStore::getTriple() {
  checkTriples(1);
  BeaverTriple* ans = atriple_store.front();
  atriple_store.pop();
  return ans;
}

DaBit* CorrelatedStore::getDaBit() {
  checkDaBits(1);
  DaBit* ans = dabit_store.front();
  dabit_store.pop();
  return ans;
}

void CorrelatedStore::printSizes() {
  std::cout << "Current store sizes:" << std::endl;
  std::cout << " Dabits: " << dabit_store.size() << std::endl;
  // std::cout << " Bool  Triples: " << btriple_store.size() << std::endl;
  std::cout << " Arith Triples: " << atriple_store.size() << std::endl;
}

void CorrelatedStore::maybeUpdate() {
  auto start = clock_start();

  // Make top level if stores not enough
  const bool make_da = dabit_store.size() < (batch_size / 2);
  // Determine how much of each to make
  const size_t da_target = 2 * make_da * batch_size;
  const size_t btrip_target = 0;  // NOTE: Currently disabled

  // For heavy
  const bool make_arith = atriple_store.size() < (batch_size / 2);
  const size_t atrip_target = 2 * make_arith;

  if (btriple_store.size() < btrip_target)
    addBoolTriples(btrip_target);

  if (dabit_store.size() < da_target)
    addDaBits(da_target);

  if (atriple_store.size() < atrip_target)
    addTriples(atrip_target);

  printSizes();

  std::cout << "precompute timing : " << sec_from(start) << std::endl;
}

CorrelatedStore::~CorrelatedStore() {
  while (!dabit_store.empty()) {
    DaBit* bit = dabit_store.front();
    dabit_store.pop();
    delete bit;
  }
  while (!btriple_store.empty()) {
    BooleanBeaverTriple* triple = btriple_store.front();
    btriple_store.pop();
    delete triple;
  }
  while (!atriple_store.empty()) {
    BeaverTriple* triple = atriple_store.front();
    atriple_store.pop();
    delete triple;
  }
  // if (triple_gen)
  //   delete triple_gen;
}

bool* CorrelatedStore::multiplyBoolShares(const size_t N,
                                          const bool* const x,
                                          const bool* const y) {
  bool* const z = new bool[N];

  bool* d_this = new bool[N];
  bool* e_this = new bool[N];

  checkBoolTriples(N);
  for (unsigned int i = 0; i < N; i++) {
    BooleanBeaverTriple* triple = getBoolTriple();
    d_this[i] = x[i] ^ triple->a;
    e_this[i] = y[i] ^ triple->b;
    z[i] = triple->c;
    delete triple;
  }
  pid_t pid = 0;
  int status = 0;
  if (do_fork) pid = fork();
  if (pid == 0) {
    send_bool_batch(serverfd, d_this, N);
    send_bool_batch(serverfd, e_this, N);

    if (do_fork) exit(EXIT_SUCCESS);
  }

  bool* d_other = new bool[N];
  bool* e_other = new bool[N];
  recv_bool_batch(serverfd, d_other, N);
  recv_bool_batch(serverfd, e_other, N);

  for (unsigned int i = 0; i < N; i++) {
    bool d = d_this[i] ^ d_other[i];
    bool e = e_this[i] ^ e_other[i];
    z[i] ^= (x[i] and e) ^ (y[i] and d);
    if (server_num == 0)
      z[i] ^= (d and e);
  }

  delete[] d_this;
  delete[] e_this;
  delete[] d_other;
  delete[] e_other;

  if (do_fork) waitpid(pid, &status, 0);
  return z;
}

fmpz_t* CorrelatedStore::multiplyArithmeticShares(
    const size_t N, const fmpz_t* const x, const fmpz_t* const y) {
  fmpz_t* z; new_fmpz_array(&z, N);

  fmpz_t* d; new_fmpz_array(&d, N);
  fmpz_t* e; new_fmpz_array(&e, N);
  fmpz_t* d_other; new_fmpz_array(&d_other, N);
  fmpz_t* e_other; new_fmpz_array(&e_other, N);

  checkTriples(N, true);

  for (unsigned int i = 0; i < N; i++) {
    BeaverTriple* triple = getTriple();

    fmpz_sub(d[i], x[i], triple->A);  // [d] = [x] - [a]
    fmpz_mod(d[i], d[i], Int_Modulus);
    fmpz_sub(e[i], y[i], triple->B);  // [e] = [y] - [b]
    fmpz_mod(e[i], e[i], Int_Modulus);

    fmpz_set(z[i], triple->C);

    // consume the triple
    delete triple;
  }

  // Spawn a child to do the sending, so that can recieve at the same time
  pid_t pid = 0;
  int status = 0;
  if (do_fork) pid = fork();
  if (pid == 0) {
    send_fmpz_batch(serverfd, d, N);
    send_fmpz_batch(serverfd, e, N);
    if (do_fork) exit(EXIT_SUCCESS);
  }
  recv_fmpz_batch(serverfd, d_other, N);
  recv_fmpz_batch(serverfd, e_other, N);

  for (unsigned int i = 0; i < N; i++) {
    fmpz_add(d[i], d[i], d_other[i]);  // x - a
    fmpz_mod(d[i], d[i], Int_Modulus);
    fmpz_add(e[i], e[i], e_other[i]);  // y - b
    fmpz_mod(e[i], e[i], Int_Modulus);

    // [xy] = [c] + [x] e + [y] d - de
    // Is it more efficient using a tmp for product, and modding more?
    fmpz_addmul(z[i], x[i], e[i]);
    fmpz_addmul(z[i], y[i], d[i]);
    if (server_num == 0)
      fmpz_submul(z[i], d[i], e[i]);
    fmpz_mod(z[i], z[i], Int_Modulus);
  }

  clear_fmpz_array(d_other, N);
  clear_fmpz_array(e_other, N);
  clear_fmpz_array(d, N);
  clear_fmpz_array(e, N);

  // Wait for send child to finish
  if (do_fork) waitpid(pid, &status, 0);

  return z;
}

// c_{i+1} = c_i xor ((x_i xor c_i) and (y_i xor c_i))
// output z_i = x_i xor y_i xor c_i
// Unused
bool* CorrelatedStore::addBinaryShares(const size_t N,
                                       const size_t* const num_bits,
                                       const bool* const * const x,
                                       const bool* const * const y,
                                       bool* const * const z) {
  bool* carry = new bool[N];
  memset(carry, false, N);
  bool* xi = new bool[N];
  bool* yi = new bool[N];

  size_t max_bits = 0;
  size_t total_bits = 0;
  for (unsigned int i = 0; i < N; i++) {
    max_bits = (num_bits[i] > max_bits ? num_bits[i] : max_bits);
    total_bits += num_bits[i];
  }

  checkBoolTriples(total_bits);

  for (unsigned int j = 0; j < max_bits; j++) {
    size_t idx = 0;
    for (unsigned int i = 0; i < N; i++) {
      if (j >= num_bits[i])
        continue;
      z[i][j] = carry[i] ^ x[i][j] ^ y[i][j];
      xi[idx] = carry[i] ^ x[i][j];
      yi[idx] = carry[i] ^ y[i][j];
      idx++;
    }

    bool* new_carry = multiplyBoolShares(idx, xi, yi);

    idx = 0;
    for (unsigned int i = 0; i < N; i++) {
      if (j >= num_bits[i])
        continue;
      carry[i] ^= new_carry[idx];
      idx++;
    }

    delete[] new_carry;
  }

  delete[] xi;
  delete[] yi;

  return carry;
}

fmpz_t* CorrelatedStore::b2a_daBit_single(const size_t N, const bool* const x) {
  fmpz_t* xp; new_fmpz_array(&xp, N);

  checkDaBits(N);

  bool* v_this = new bool[N];
  for (unsigned int i = 0; i < N; i++) {
    DaBit* dabit = getDaBit();
    v_this[i] = x[i] ^ dabit->b2;

    fmpz_set(xp[i], dabit->bp);
    // consume the daBit
    delete dabit;
  }

  pid_t pid = 0;
  int status = 0;
  if (do_fork) pid = fork();
  if (pid == 0) {
    send_bool_batch(serverfd, v_this, N);

    if (do_fork) exit(EXIT_SUCCESS);
  }
  bool* v_other = new bool[N];
  recv_bool_batch(serverfd, v_other, N);

  for (unsigned int i = 0; i < N; i++) {
    const bool v = v_this[i] ^ v_other[i];

    // [x]_p = v + [b]_p - 2 v [b]_p. Note v only added for one server.
    // So since server_num in {0, 1}, we add it when v = 1
    // Currently, [x]_p is holding [b]_p, which is what we want for v = 0
    if (v) {  // If v = 1, then [x]_p = (0/1) - [b]_p
      fmpz_neg(xp[i], xp[i]);
      fmpz_add_ui(xp[i], xp[i], server_num);
      fmpz_mod(xp[i], xp[i], Int_Modulus);
    }
  }

  delete[] v_this;
  delete[] v_other;

  if (do_fork) waitpid(pid, &status, 0);

  return xp;
}

fmpz_t* CorrelatedStore::b2a_daBit_multi(const size_t N,
                                         const size_t* const num_bits,
                                         const fmpz_t* const x) {
  size_t total_bits = 0;
  for (unsigned int i = 0; i < N; i++)
    total_bits += num_bits[i];

  checkDaBits(total_bits);

  fmpz_t* xp; new_fmpz_array(&xp, N);
  bool* x2 = new bool[total_bits];

  size_t offset = 0;
  for (unsigned int i = 0; i < N; i++) {
    for (unsigned int j = 0; j < num_bits[i]; j++)
      x2[j + offset] = fmpz_tstbit(x[i], j);
    offset += num_bits[i];
  }

  fmpz_t* tmp_xp = b2a_daBit_single(total_bits, x2);

  offset = 0;
  for (unsigned int i = 0; i < N; i++) {
    fmpz_set_ui(xp[i], 0);
    for (unsigned int j = 0; j < num_bits[i]; j++) {
      fmpz_addmul_ui(xp[i], tmp_xp[j + offset], (1ULL << j));
      fmpz_mod(xp[i], xp[i], Int_Modulus);
    }
    offset += num_bits[i];
  }

  delete[] x2;
  clear_fmpz_array(tmp_xp, total_bits);

  return xp;
}

// Using intsum_ot, multiple bits
fmpz_t* CorrelatedStore::b2a_ot(const size_t num_shares, const size_t num_values,
                                const size_t* const num_bits,
                                const fmpz_t* const x, const size_t mod) {
  uint64_t** x2 = new uint64_t*[num_shares];
  bool* const valid = new bool[num_shares];
  for (unsigned int i = 0; i < num_shares; i++) {
    x2[i] = new uint64_t[num_values];
    valid[i] = true;
    for (unsigned int j = 0; j < num_values; j++) {
      x2[i][j] = fmpz_get_ui(x[i * num_values + j]);
    }
  }

  uint64_t** xp;

  if (server_num == 0) {
    xp = intsum_ot_sender(ot0, x2, valid, num_bits, num_shares, num_values, mod);
  } else {
    xp = intsum_ot_receiver(ot0, x2, num_bits, num_shares, num_values, mod);
  }

  // for consistency, flatten and fmpz_t
  fmpz_t* ans; new_fmpz_array(&ans, num_shares * num_values);

  for (unsigned int i = 0; i < num_shares; i++) {
    for (unsigned int j = 0; j < num_values; j++) {
      fmpz_set_ui(ans[i * num_values + j], xp[i][j]);
    }
    delete[] x2[i];
    delete[] xp[i];
  }
  delete[] x2;
  delete[] valid;
  delete[] xp;

  return ans;
}

void CorrelatedStore::heavy_convert(
    const size_t N, const size_t b,
    const bool* const x, const bool* const y,
    const bool* const valid,
    fmpz_t* const bucket0, fmpz_t* const bucket1) {
  const size_t n = N * b;

  // Step 1: convert y to arith shares
  fmpz_t* y_p = b2a_daBit_single(n, y);

  // Step 2: OT setup
  // z = 1 - 2y, as [z] = servernum - 2[y]
  // then (x ^ x')(z + z')
  fmpz_t r; fmpz_init(r);
  fmpz_t z; fmpz_init(z);
  fmpz_t tmp; fmpz_init(tmp);
  // Bucket 0 choices
  uint64_t* const data0 = new uint64_t[n];
  uint64_t* const data1 = new uint64_t[n];
  // Bucket 1 choices
  uint64_t* const data0_1 = new uint64_t[n];
  uint64_t* const data1_1 = new uint64_t[n];

  // [ (r0, r1+z), (r0 + z, r1) ] based on x
  // [ (r0 + xz, r1 + !x z), (r0 + !x z, r1 + x z)]
  // [ (0, 0_1), (1, 1_1)]
  for (unsigned int i = 0; i < N; i++) {
    if (!valid[i]) {
      memset(&data0[i * b], 0, b * sizeof(uint64_t));
      memset(&data0_1[i * b], 0, b * sizeof(uint64_t));
      memset(&data1[i * b], 0, b * sizeof(uint64_t));
      memset(&data1_1[i * b], 0, b * sizeof(uint64_t));
      continue;
    }

    for (unsigned int j = 0; j < b; j++) {
      const size_t idx = i * b + j;

      // Build z = 1 - 2y, as [z] = servernum - 2[y]
      fmpz_set_si(z, server_num);
      fmpz_submul_si(z, y_p[idx], 2);
      fmpz_mod(z, z, Int_Modulus);

      // r0
      fmpz_randm(r, seed, Int_Modulus);
      // fmpz_zero(r);
      fmpz_sub(bucket0[j], bucket0[j], r);
      fmpz_mod(bucket0[j], bucket0[j], Int_Modulus);
      // r0 + xz and r0 + (1-x) z
      fmpz_add(tmp, r, z);
      fmpz_mod(tmp, tmp, Int_Modulus);
      if (x[idx]) {
        data0[idx] = fmpz_get_ui(r);
        data1[idx] = fmpz_get_ui(tmp);
      } else {
        data0[idx] = fmpz_get_ui(tmp);
        data1[idx] = fmpz_get_ui(r);
      }

      // r1
      fmpz_randm(r, seed, Int_Modulus);
      // fmpz_zero(r);
      fmpz_sub(bucket1[j], bucket1[j], r);
      fmpz_mod(bucket1[j], bucket1[j], Int_Modulus);
      // r1 + (1-x)z and r1 + xz
      fmpz_add(tmp, r, z);
      fmpz_mod(tmp, tmp, Int_Modulus);
      if (x[idx]) {
        data0_1[idx] = fmpz_get_ui(tmp);
        data1_1[idx] = fmpz_get_ui(r);
      } else {
        data0_1[idx] = fmpz_get_ui(r);
        data1_1[idx] = fmpz_get_ui(tmp);
      }
    }
  }

  fmpz_clear(r);
  fmpz_clear(z);
  fmpz_clear(tmp);

  // Step 3: OT swap
  uint64_t* received = new uint64_t[n];
  uint64_t* received_1 = new uint64_t[n];
  pid_t pid = 0;
  int status = 0;
  // NOTE: OT forking currently seems bugged. Disable for now.
  const bool do_ot_fork = false;
  if (do_ot_fork) {
    pid = fork();
    if (pid == 0) {
      (server_num == 0 ? ot0 : ot1)->send(data0, data1, n, data0_1, data1_1);
      exit(EXIT_SUCCESS);
    }
    (server_num == 0 ? ot1 : ot0)->recv(received, x, n, received_1);
  } else {
    if (server_num == 0) {
      ot0->send(data0, data1, n, data0_1, data1_1);
      ot1->recv(received, x, n, received_1);
    } else {
      ot0->recv(received, x, n, received_1);
      ot1->send(data0, data1, n, data0_1, data1_1);
    }
  }

  // Step 4: Add to buckets
  for (unsigned int i = 0; i < N; i++) {
    for (unsigned int j = 0; j < b; j++) {
      const size_t idx = i * b + j;
      fmpz_add_ui(bucket0[j], bucket0[j], received[idx]);
      fmpz_mod(bucket0[j], bucket0[j], Int_Modulus);

      fmpz_add_ui(bucket1[j], bucket1[j], received_1[idx]);
      fmpz_mod(bucket1[j], bucket1[j], Int_Modulus);
    }
  }

  delete[] data0;
  delete[] data0_1;
  delete[] data1;
  delete[] data1_1;
  delete[] received;
  delete[] received_1;

  if (do_ot_fork) waitpid(pid, &status, 0);
}

// Use b2A via OT on random bit
// Nearly COT, except delta is changing
// random choice and random base, but also random delta matters
DaBit** CorrelatedStore::generateDaBit(const size_t N) {
  DaBit** const dabit = new DaBit*[N];

  emp::PRG prg;
  const size_t mod = fmpz_get_ui(Int_Modulus);

  bool* const b = new bool[N];
  prg.random_bool(b, N);  // random bits

  uint64_t* const x = new uint64_t[N];

  for (unsigned int i = 0; i < N; i++) {
    dabit[i] = new DaBit();
    dabit[i]->b2 = b[i];
  }

  if (server_num == 0) {
    uint64_t* const b0 = new uint64_t[N];
    uint64_t* const b1 = new uint64_t[N];
    for (unsigned int i = 0; i < N; i++) {
      prg.random_data(&b0[i], sizeof(uint64_t));
      b0[i] %= mod;
      b1[i] = (b0[i] + b[i]) % mod;
      x[i] = mod - b0[i];
    }
    ot0->send(b0, b1, N);
    delete[] b0;
    delete[] b1;
  } else {
    ot0->recv(x, b, N);
  }
  for (unsigned int i = 0; i < N; i++) {
    uint64_t bp = (b[i] + 2 * (mod - x[i])) % mod;
    fmpz_set_ui(dabit[i]->bp, bp);
  }

  delete[] b;
  delete[] x;

  return dabit;
}

// TODO: do better. Currently just in clear.
// [x] > c for known c, shares [x]
// Treats > N/2 as negative
// Maybe also return int (+/-/0) for equality?
bool* CorrelatedStore::cmp_c(const size_t N,
                             const fmpz_t* const x,
                             const fmpz_t* const c) {
  bool* const ans = new bool[N];

  fmpz_t half; fmpz_init(half); fmpz_cdiv_q_ui(half, Int_Modulus, 2);

  // For now, just do in clear. TODO: do securely.
  if (server_num == 0) {
    fmpz_t* x2; new_fmpz_array(&x2, N);
    fmpz_t* c2; new_fmpz_array(&c2, N);
    recv_fmpz_batch(serverfd, x2, N);
    for (unsigned int i = 0; i < N; i++) {
      fmpz_add(x2[i], x2[i], x[i]);
      fmpz_mod(x2[i], x2[i], Int_Modulus);
      if (fmpz_cmp(x2[i], half) > 0) {  // > N/2, so negative
        fmpz_sub(x2[i], x2[i], Int_Modulus);
      }
      if (fmpz_cmp(c[i], half) > 0) {
        fmpz_sub(c2[i], c[i], Int_Modulus);
      } else {
        fmpz_set(c2[i], c[i]);
      }
      ans[i] = fmpz_cmp(x2[i], c2[i]) < 0;
    }
    send_bool_batch(serverfd, ans, N);
  } else {
    send_fmpz_batch(serverfd, x, N);
    recv_bool_batch(serverfd, ans, N);
  }

  fmpz_clear(half);
  return ans;
}

// Just x < y => (x - y) < 0
bool* CorrelatedStore::cmp(const size_t N,
                           const fmpz_t* const x,
                           const fmpz_t* const y) {
  fmpz_t* diff; new_fmpz_array(&diff, N);
  fmpz_t* zeros; new_fmpz_array(&zeros, N);  // zeroed out by default

  fmpz_t half; fmpz_init(half); fmpz_cdiv_q_ui(half, Int_Modulus, 2);
  fmpz_t tmp; fmpz_init(tmp);

  for (unsigned int i = 0; i < N; i++) {
    fmpz_sub(diff[i], x[i], y[i]);
    fmpz_mod(diff[i], diff[i], Int_Modulus);
    // std::cout << "diff_" << server_num << "[" << i << "] = " << fmpz_get_ui(diff[i]) << std::endl;
  }

  bool* const ans = cmp_c(N, diff, zeros);
  fmpz_clear(tmp);
  fmpz_clear(half);
  clear_fmpz_array(diff, N);
  clear_fmpz_array(zeros, N);
  return ans;
}

void CorrelatedStore::abs(const size_t N, const fmpz_t* const x, fmpz_t* const out) {
  fmpz_t* zeros; new_fmpz_array(&zeros, N);
  bool* sign = cmp_c(N, x, zeros);
  for (unsigned int i = 0; i < N; i++) {
    if (sign[i]) {  // negative, flip
      fmpz_sub(out[i], Int_Modulus, x[i]);
    } else {
      fmpz_set(out[i], x[i]);
    }
  }
}

bool* CorrelatedStore::abs_cmp(const size_t N,
                               const fmpz_t* const x, const fmpz_t* const y) {
  fmpz_t* merge; new_fmpz_array(&merge, 2*N);
  fmpz_t* merge_abs; new_fmpz_array(&merge_abs, 2*N);
  for (unsigned int i = 0; i < N; i++) {
    fmpz_set(merge[i], x[i]);
    fmpz_set(merge[i+N], y[i]);
  }
  abs(2*N, merge, merge_abs);
  clear_fmpz_array(merge, 2*N);

  fmpz_t* x2; new_fmpz_array(&x2, N);
  fmpz_t* y2; new_fmpz_array(&y2, N);
  for (unsigned int i = 0; i < N; i++) {
    fmpz_set(x2[i], merge_abs[i]);
    fmpz_set(y2[i], merge_abs[i+N]);
  }
  clear_fmpz_array(merge_abs, N);
  bool* const ans = cmp(N, x2, y2);
  clear_fmpz_array(x2, N);
  clear_fmpz_array(y2, N);
  return ans;
}

// x, y are Nxb shares of N total b-bit numbers. 
// I.e. x[i,j] is additive share of bit j of number i. 
fmpz_t* CorrelatedStore::cmp_bit(const size_t N, const size_t b,
                                 const fmpz_t* const x, const fmpz_t* const y) {
  fmpz_t* ans; new_fmpz_array(&ans, N);
  size_t idx;  // for convenience

  // Total mults: ~3x (N*b)
  checkTriples(3 * N * b);

  // [c] = [x ^ y] = [x] + [y] - 2[xy]
  fmpz_t* c = multiplyArithmeticShares(N * b, x, y);
  for (unsigned int i = 0; i < N * b; i++) {
    fmpz_mul_si(c[i], c[i], -2);
    fmpz_add(c[i], c[i], x[i]);
    fmpz_add(c[i], c[i], y[i]);
    fmpz_mod(c[i], c[i], Int_Modulus);
  }

  // [di] = OR([cj]) from i+1 to b
  // = [ci] OR [d(i+1)], with d_b = cb
  // a OR b = 1-(1-a)(1-b) = a + b - ab
  // TODO: Currently doing b-round version. Can be constant, but lazy fine for now.
  fmpz_t* d; new_fmpz_array(&d, N * b);
  // Start: db = cb
  for (unsigned int i = 0; i < N; i++) {
    idx = i * b + (b-1);  // [i, b-1]
    fmpz_set(d[idx], c[idx]);
  }
  fmpz_t* ci; new_fmpz_array(&ci, N);     // [ci]
  fmpz_t* di1; new_fmpz_array(&di1, N);   // [d(i+1)]
  for (int j = b-2; j >= 0; j--) {
    for (unsigned int i = 0; i < N; i++) {
      idx = i * b + j;
      fmpz_set(ci[i], c[idx]);
      fmpz_set(di1[i], d[idx + 1]);
    }
    fmpz_t* mul = multiplyArithmeticShares(N, ci, di1);
    for (unsigned int i = 0; i < N; i++) {
      idx = i * b + j;
      fmpz_add(d[idx], ci[i], di1[i]);
      fmpz_sub(d[idx], d[idx], mul[i]);
      fmpz_mod(d[idx], d[idx], Int_Modulus);
    }
    clear_fmpz_array(mul, N);
  }
  clear_fmpz_array(ci, N);
  clear_fmpz_array(di1, N);

  // ei = di - d(i+1), with eb = db
  fmpz_t* e; new_fmpz_array(&e, N * b);
  for (unsigned int i = 0; i < N; i++) {
    idx = i * b + (b-1);
    fmpz_set(e[idx], d[idx]);
    for (unsigned int j = 0; j < b - 1; j++) {
      idx = i * b + j;
      fmpz_sub(e[idx], d[idx], d[idx + 1]);
      fmpz_mod(e[idx], e[idx], Int_Modulus);
    }
  }

  // [x < y] = sum ei * yi
  fmpz_t* ey = multiplyArithmeticShares(N * b, e, y);
  for (unsigned int i = 0; i < N; i++) {
    fmpz_zero(ans[i]);
    for (unsigned int j = 0; j < b; j++) {
      fmpz_add(ans[i], ans[i], ey[i * b + j]);
      fmpz_mod(ans[i], ans[i], Int_Modulus);
    }
  }
  return ans;
}

// Make each bit in parallel: [ri in {0,1}]p
// check if [r < p] bitwise, retry if not
// success odds are p/2^b, worst case ~1/2 failure. 
fmpz_t* CorrelatedStore::gen_rand_bitshare(const size_t N, fmpz_t* const r) {
  const size_t b = nbits_mod;
  fmpz_t* rB; new_fmpz_array(&rB, N * b);

  // Assumed tries to succeed. (worst case 1/2 fail, so 2 avg, overestimate)
  const size_t avg_tries = 4;           
  checkDaBits(avg_tries * N * b);       // for gen
  checkTriples(avg_tries * 3 * N * b);  // for cmp
  std::cout << "check done" << std::endl;

  // p bitwise shared, for [r < p]. All bits set on server 0, since public.
  fmpz_t* pB; new_fmpz_array(&pB, N * b);  // Zero by default
  if (server_num == 0)
    for (unsigned int j = 0; j < b; j++)
      if (fmpz_tstbit(Int_Modulus, j))
        for (unsigned int i = 0; i < N; i++)
          fmpz_set_si(pB[i * b + j], 1);

  // for validation checking
  fmpz_t* rB_tocheck; new_fmpz_array(&rB_tocheck, N * b);
  size_t* rB_idx = new size_t[N];
  fmpz_t* r_lt_p_other; new_fmpz_array(&r_lt_p_other, N);

  // Track which are already set
  bool* const valid = new bool[N];
  memset(valid, false, N * sizeof(bool));
  size_t num_invalid;

  size_t log_num_invalid = 0;

  while (true) {
    num_invalid = 0;

    // Compute new (if not valid)
    for (unsigned int i = 0; i < N; i++) {
      if (valid[i])
        continue;

      fmpz_zero(r[i]);
      for (unsigned int j = 0; j < b; j++) {
        // Just need 2 numbers summing to 0 or 1, so bp. b2 not needed.
        DaBit* dabit = getDaBit();
        fmpz_set(rB[i * b + j], dabit->bp);
        fmpz_addmul_ui(r[i], dabit->bp, 1ULL << j);
        fmpz_mod(r[i], r[i], Int_Modulus);
        // consume the dabit
        delete dabit;

        // add to rB_tocheck
        // std::cout << "rB_tocheck[" << num_invalid * b + j << "] := rB[" << i*b+j << "]" << std::endl;
        fmpz_set(rB_tocheck[num_invalid * b + j], rB[i * b + j]);
      }
      rB_idx[num_invalid] = i;

      num_invalid++;
    }

    log_num_invalid += num_invalid;

    // std::cout << "num invalid: " << num_invalid << " / " << N << std::endl;
    if (num_invalid == 0) {
      break;
    }

    // Check [r < p], retry if not, sets "valid" where [r < p]

    // It's fine if arrays are larger, extras get ignored.
    fmpz_t* r_lt_p = cmp_bit(num_invalid, b, rB_tocheck, pB);
    // Get r_lt_p in clear
    // TODO: fork
    if (server_num == 0) {
      send_fmpz_batch(serverfd, r_lt_p, num_invalid);
      recv_fmpz_batch(serverfd, r_lt_p_other, num_invalid);
    } else {
      recv_fmpz_batch(serverfd, r_lt_p_other, num_invalid);
      send_fmpz_batch(serverfd, r_lt_p, num_invalid);
    }
    for (unsigned int i = 0; i < num_invalid; i++) {
      fmpz_add(r_lt_p[i], r_lt_p[i], r_lt_p_other[i]);
      fmpz_mod(r_lt_p[i], r_lt_p[i], Int_Modulus);
      valid[rB_idx[i]] = fmpz_is_one(r_lt_p[i]);
      // std::cout << "check " << i << ": valid[" << rB_idx[i] << "] = " << valid[rB_idx[i]] << std::endl;
    }
    clear_fmpz_array(r_lt_p, num_invalid);
  }

  clear_fmpz_array(rB_tocheck, N * b);
  clear_fmpz_array(pB, N * b);
  clear_fmpz_array(r_lt_p_other, N);
  delete[] rB_idx;

  // std::cout << "total sub-iterations: " << log_num_invalid << ", vs expected: " << avg_tries * N << std::endl;

  return rB;
}
