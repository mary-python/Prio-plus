#include "share.h"

#include "fmpz_utils.h"
#include "prio.h"

extern "C" {
  #include "flint/flint.h"
  #include "flint/fmpz.h"
};

fmpz_t Int_Modulus;
flint_rand_t seed;

void init_client_packet(ClientPacket &p, int N){
    p = new client_packet();

    p->N = N;

    new_fmpz_array(&p->WireShares, N);
    
    fmpz_init(p->f0_s);
    fmpz_init(p->g0_s);
    fmpz_init(p->h0_s);

    new_fmpz_array(&p->h_points, N);
}

void SplitShare(fmpz_t val, fmpz_t A, fmpz_t B){
    fmpz_randm(A,seed,Int_Modulus);
    fmpz_sub(B,val,A);
    fmpz_mod(B,B,Int_Modulus);
}

BeaverTriple* NewBeaverTriple() {
    BeaverTriple* out = new BeaverTriple();

    fmpz_randm(out->A,seed,Int_Modulus);
    fmpz_randm(out->B,seed,Int_Modulus);
    fmpz_mul(out->C,out->A,out->B);
    fmpz_mod(out->C,out->C,Int_Modulus);

    return out;
}

BeaverTripleShare* BeaverTripleShares(BeaverTriple* inp) {
    BeaverTripleShare* out = new BeaverTripleShare[2];

    SplitShare(inp->A,out[0].shareA,out[1].shareA);
    SplitShare(inp->B,out[0].shareB,out[1].shareB);
    SplitShare(inp->C,out[0].shareC,out[1].shareC);

    return out;
}