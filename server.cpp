#include "server.h"

#include <math.h>  // sqrt
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <string>

#include "edabit.h"
#include "net_share.h"
#include "proto.h"
#include "types.h"

#define SERVER0_IP "127.0.0.1"
#define SERVER1_IP "127.0.0.1"

// #define SERVER0_IP "52.87.230.64"
// #define SERVER1_IP "54.213.189.18"

#define INVALID_THRESHOLD 0.5

uint64_t int_sum_max;
uint32_t num_bits;

// Can keep the same random X for a while. TODO: how much?
#define RANDOMX_THRESHOLD 1e6
uint32_t randx_uses = 0;
fmpz_t randomX;
// Precomputes for random X
std::unordered_map<size_t, CheckerPreComp*> precomp_store;

// Precompute cache of edabits and beaver triples
CorrelatedStore* correlated_store;
#define CACHE_SIZE 65536
// If set, does fast but insecure offline precompute.
#define LAZY_PRECOMPUTE true

// TODO: const 60051 for netio?

void error_exit(const char* const msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

size_t send_out(const int sockfd, const void* const buf, const size_t len) {
    size_t ret = send(sockfd, buf, len, 0);
    if (ret <= 0) error_exit("Failed to send");
    return ret;
}

void bind_and_listen(sockaddr_in& addr, int& sockfd, const int port, const int reuse = 1) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1)
        error_exit("Socket creation failed");

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)))
        error_exit("Sockopt failed");
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)))
        error_exit("Sockopt failed");

    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind to port: " << port << std::endl;
        error_exit("Bind to port failed");
    }

    if (listen(sockfd, 2) < 0)
        error_exit("Listen failed");   
}

// Asymmetric: 1 connects to 0, 0 listens to 1.
void server0_listen(int& sockfd, int& newsockfd, const int port, const int reuse = 0) {
    sockaddr_in addr;
    bind_and_listen(addr, sockfd, port, reuse);

    socklen_t addrlen = sizeof(addr);
    std::cout << "  Waiting to accept\n";
    newsockfd = accept(sockfd, (sockaddr*)&addr, &addrlen);
    if (newsockfd < 0) error_exit("Accept failure");
    std::cout << "  Accepted\n";
}

void server1_connect(int& sockfd, const int port, const int reuse = 0) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) error_exit("Socket creation failed");

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)))
        error_exit("Sockopt failed");
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)))
        error_exit("Sockopt failed");

    sockaddr_in addr;
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, SERVER0_IP, &addr.sin_addr);

    std::cout << "  Trying to connect...\n";
    if (connect(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0)
        error_exit("Can't connect to other server");
    std::cout << "  Connected\n";
}

// Gets a new randomX, syncs it between servers.
void sync_randomX(const int serverfd, const int server_num, fmpz_t randomX) {
    if (server_num == 0) {
        recv_fmpz(serverfd, randomX);
        std::cout << "Got randomX: "; fmpz_print(randomX); std::cout << std::endl;
    } else {
        fmpz_randm(randomX, seed, Int_Modulus);
        std::cout << "Sending randomX: "; fmpz_print(randomX); std::cout << std::endl;
        send_fmpz(serverfd, randomX);
    }
}

std::string get_pk(const int serverfd) {
    char pk_buf[PK_LENGTH];
    recv_in(serverfd, &pk_buf[0], PK_LENGTH);
    std::string pk(pk_buf, pk_buf + PK_LENGTH);
    return pk;
}

CheckerPreComp* getPrecomp(const size_t N) {
    CheckerPreComp* pre;
    if (precomp_store.find(N) == precomp_store.end()) {
        pre = new CheckerPreComp(N);
        pre->setCheckerPrecomp(randomX);
        precomp_store[N] = pre;
    } else {
        pre = precomp_store[N];
    }
    return pre;
}

// Batch of N (snips + num_input wire/share) validations
bool* validate_snips(const size_t N,
                     const size_t num_inputs,
                     const int serverfd,
                     const int server_num,
                     const size_t num_bits,
                     Circuit* const * const circuit,
                     const ClientPacket* const * const packet,
                     const uint64_t* const share
                     ) {
    bool* ans = new bool[N];
    for (unsigned int i = 0; i < N; i++)
        ans[i] = true;

    // validate wires
    // Ensures [share]_2 and [wire]_p encode the same value.

    fmpz_t* fshare; new_fmpz_array(&fshare, N * num_inputs);
    fmpz_t* wireshare; new_fmpz_array(&wireshare, N * num_inputs);

    for (unsigned int i = 0; i < N; i++) {
        for (unsigned int j = 0; j < num_inputs; j++) {
            int k = i * num_inputs + j;
            fmpz_set_ui(fshare[k], share[k]);
            fmpz_set(wireshare[k], packet[i]->WireShares[j]);
        }
    }

    bool* valid = correlated_store->validateSharesMatch(
            N * num_inputs, fshare, wireshare);

    for (unsigned int i = 0; i < N; i++)
        for (unsigned int j = 0; j < num_inputs; j++)
            ans[i] &= valid[i * num_inputs + j];

    clear_fmpz_array(fshare, N * num_inputs);
    clear_fmpz_array(wireshare, N * num_inputs);
    delete[] valid;

    // run SNIPs

    init_roots(circuit[0]->N());

    Checker** const checker = new Checker*[N];
    for (unsigned int i = 0; i < N; i++)
        checker[i] = new Checker(circuit[i], server_num, packet[i]);
    CheckerPreComp* pre = getPrecomp(circuit[0]->N());
    randx_uses += N;

    CorShare** cor_share = new CorShare*[N];
    for (unsigned int i = 0; i < N; i++) {
        cor_share[i] = checker[i]->CorShareFn(pre);
        send_CorShare(serverfd, cor_share[i]);
    }
    CorShare** cor_share_other = new CorShare*[N];
    for (unsigned int i = 0; i < N; i++) {
        cor_share_other[i] = new CorShare();
        recv_CorShare(serverfd, cor_share_other[i]);
    }

    Cor** cor = new Cor*[N];
    fmpz_t* valid_share; new_fmpz_array(&valid_share, N);
    for (unsigned int i = 0; i < N; i++) {
        cor[i] = checker[i]->CorFn(cor_share[i], cor_share_other[i]);
        checker[i]->OutShare(valid_share[i], cor[i]);
        send_fmpz(serverfd, valid_share[i]);
    }
    fmpz_t valid_share_other; fmpz_init(valid_share_other);
    for (unsigned int i = 0; i < N; i++) {
        recv_fmpz(serverfd, valid_share_other);
        ans[i] &= checker[i]->OutputIsValid(valid_share[i], valid_share_other);
        // if (!ans[i])
        //     std::cout << "snip[" << i << "] invalid" << std::endl;
    }

    for (unsigned int i = 0; i < N; i++) {
        delete cor[i];
        delete cor_share_other[i];
        delete cor_share[i];
        delete checker[i];
    }
    delete[] cor;
    delete[] cor_share_other;
    delete[] cor_share;
    delete[] checker;
    clear_fmpz_array(valid_share, N);
    fmpz_clear(valid_share_other);
    return ans;
}

returnType bit_sum(const initMsg msg, const int clientfd, const int serverfd, const int server_num, uint64_t& ans) {
    std::unordered_map<std::string, bool> share_map;
    auto start = clock_start();

    BitShare share;
    const unsigned int total_inputs = msg.num_of_inputs;

    int num_bytes = 0;
    for (unsigned int i = 0; i < total_inputs; i++) {
        num_bytes += recv_in(clientfd, &share, sizeof(BitShare));
        std::string pk(share.pk, share.pk + PK_LENGTH);
        if (share_map.find(pk) != share_map.end())
            continue;
        share_map[pk] = share.val;
    }

    std::cout << "Received " << total_inputs << " total shares" << std::endl;
    std::cout << "bytes from client: " << num_bytes << std::endl;
    std::cout << "receive time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
    start = clock_start();

    // if (fork() > 0) return RET_NO_ANS;

    if (server_num == 1) {
        const unsigned int num_inputs = share_map.size();
        send_size(serverfd, num_inputs);
        bool* shares = new bool[num_inputs];
        int i = 0;
        for (const auto& share : share_map) {
            send_out(serverfd, &share.first[0], PK_LENGTH);
            shares[i] = share.second;
            i++;
        }
        NetIO* const io = new NetIO(SERVER0_IP, 60051, true);
        uint64_t b = bitsum_ot_receiver(io, &shares[0], num_inputs);
        delete io;
        delete[] shares;

        send_uint64(serverfd, b);
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        return RET_NO_ANS;
    } else {
        size_t num_inputs, num_valid = 0;
        recv_size(serverfd, num_inputs);
        bool* shares = new bool[num_inputs];
        bool* valid = new bool[num_inputs];

        for (unsigned int i = 0; i < num_inputs; i++) {
            std::string pk = get_pk(serverfd);

            bool is_valid = (share_map.find(pk) != share_map.end());
            valid[i] = is_valid;
            if (!is_valid)
                continue;
            num_valid++;
            shares[i] = share_map[pk];
        }
        NetIO* const io = new NetIO(nullptr, 60051, true);
        uint64_t a = bitsum_ot_sender(io, &shares[0], &valid[0], num_inputs);
        delete io;
        delete[] shares;
        delete[] valid;

        uint64_t b;
        recv_uint64(serverfd, b);

        std::cout << "Final valid count: " << num_valid << " / " << total_inputs << std::endl;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        if (num_valid < total_inputs * (1 - INVALID_THRESHOLD)) {
            std::cout << "Failing, This is less than the invalid threshold of " << INVALID_THRESHOLD << std::endl;
            return RET_INVALID;
        }

        ans = a + b;
        return RET_ANS;
    }
}

returnType int_sum(const initMsg msg, const int clientfd, const int serverfd, const int server_num, uint64_t& ans) {
    std::unordered_map<std::string, uint64_t> share_map;
    auto start = clock_start();

    IntShare share;
    const uint64_t max_val = 1ULL << num_bits;
    const unsigned int total_inputs = msg.num_of_inputs;

    int num_bytes = 0;
    for (unsigned int i = 0; i < total_inputs; i++) {
        num_bytes += recv_in(clientfd, &share, sizeof(IntShare));
        std::string pk(share.pk, share.pk + PK_LENGTH);

        if (share_map.find(pk) != share_map.end()
            or share.val >= max_val)
            continue;
        share_map[pk] = share.val;

        // std::cout << "share[" << i << "] = " << share.val << std::endl;
    }

    std::cout << "Received " << total_inputs << " total shares" << std::endl;
    std::cout << "bytes from client: " << num_bytes << std::endl;
    std::cout << "receive time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
    start = clock_start();

    // if (fork() > 0) return RET_NO_ANS;

    if (server_num == 1) {
        const unsigned int num_inputs = share_map.size();
        send_size(serverfd, num_inputs);
        uint64_t* shares = new uint64_t[num_inputs];
        int i = 0;
        for (const auto& share : share_map) {
            send_out(serverfd, &share.first[0], PK_LENGTH);
            shares[i] = share.second;
            i++;
        }
        NetIO* const io = new NetIO(SERVER0_IP, 60051, true);
        uint64_t b = intsum_ot_receiver(io, &shares[0], num_inputs, num_bits);
        delete io;
        delete[] shares;

        send_uint64(serverfd, b);
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        return RET_NO_ANS;
    } else {
        size_t num_inputs, num_valid = 0;
        recv_size(serverfd, num_inputs);
        uint64_t* shares = new uint64_t[num_inputs];
        bool* valid = new bool[num_inputs];

        for (unsigned int i = 0; i < num_inputs; i++) {
            std::string pk = get_pk(serverfd);

            bool is_valid = (share_map.find(pk) != share_map.end());
            valid[i] = is_valid;
            if (!is_valid)
                continue;
            num_valid++;
            shares[i] = share_map[pk];
        }
        NetIO* const io = new NetIO(nullptr, 60051, true);
        uint64_t a = intsum_ot_sender(io, &shares[0], &valid[0], num_inputs, num_bits);
        delete io;
        delete[] shares;
        delete[] valid;

        uint64_t b;
        recv_uint64(serverfd, b);
        std::cout << "Final valid count: " << num_valid << " / " << total_inputs << std::endl;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        if (num_valid < total_inputs * (1 - INVALID_THRESHOLD)) {
            std::cout << "Failing, This is less than the invalid threshold of " << INVALID_THRESHOLD << std::endl;
            return RET_INVALID;
        }

        ans = a + b;
        return RET_ANS;
    }
}

// For AND and OR
returnType xor_op(const initMsg msg, const int clientfd, const int serverfd, const int server_num, bool& ans) {
    std::unordered_map<std::string, uint64_t> share_map;
    auto start = clock_start();

    IntShare share;
    const unsigned int total_inputs = msg.num_of_inputs;

    int num_bytes = 0;
    for (unsigned int i = 0; i < total_inputs; i++) {
        num_bytes += recv_in(clientfd, &share, sizeof(IntShare));
        std::string pk(share.pk, share.pk + PK_LENGTH);

        if (share_map.find(pk) != share_map.end())
            continue;
        share_map[pk] = share.val;
    }

    std::cout << "Received " << total_inputs << " total shares" << std::endl;
    std::cout << "bytes from client: " << num_bytes << std::endl;
    std::cout << "receive time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
    start = clock_start();

    // if (fork() > 0) return RET_NO_ANS;

    if (server_num == 1) {
        const unsigned int num_inputs = share_map.size();
        send_size(serverfd, num_inputs);
        uint64_t b = 0;
        std::string* pk_list = new std::string[num_inputs];
        size_t idx = 0;
        for (const auto& share : share_map) {
            send_out(serverfd, &share.first[0], PK_LENGTH);
            pk_list[idx] = share.first;
            idx++;
        }
        for (unsigned int i = 0; i < num_inputs; i++) {
            bool other_valid;
            recv_bool(serverfd, other_valid);
            if (!other_valid)
                continue;
            b ^= share_map[pk_list[i]];
        }
        send_uint64(serverfd, b);
        delete[] pk_list;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        return RET_NO_ANS;
    } else {
        size_t num_inputs, num_valid = 0;
        recv_size(serverfd, num_inputs);
        uint64_t a = 0;
        bool* valid = new bool[num_inputs];

        for (unsigned int i = 0; i < num_inputs; i++) {
            std::string pk = get_pk(serverfd);
            valid[i] = (share_map.find(pk) != share_map.end());
            if (!valid[i])
                continue;
            num_valid++;
            a ^= share_map[pk];
        }
        for (unsigned int i = 0; i < num_inputs; i++) {
            send_bool(serverfd, valid[i]);
        }
        delete[] valid;

        uint64_t b;
        recv_uint64(serverfd, b);
        uint64_t aggr = a ^ b;

        std::cout << "Final valid count: " << num_valid << " / " << total_inputs << std::endl;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        if (num_valid < total_inputs * (1 - INVALID_THRESHOLD)) {
            std::cout << "Failing, This is less than the invalid threshold of " << INVALID_THRESHOLD << std::endl;
            return RET_INVALID;
        }

        if (msg.type == AND_OP) {
            ans = (aggr == 0);
        } else if (msg.type == OR_OP) {
            ans = (aggr != 0);
        } else {
            error_exit("Message type incorrect for xor_op");
        }
        return RET_ANS;
    }
}

// For MAX and MIN
// TODO: does this need htonl/ntohl wrappers for int arrays?
returnType max_op(const initMsg msg, const int clientfd, const int serverfd, const int server_num, uint32_t& ans) {
    std::unordered_map<std::string, uint32_t*> share_map;
    auto start = clock_start();

    MaxShare share;
    const unsigned int total_inputs = msg.num_of_inputs;
    const unsigned int B = msg.max_inp;
    const size_t share_sz = (B+1) * sizeof(uint32_t);
    // Need this to have all share arrays stay in memory, for server1 later.
    uint32_t* shares = new uint32_t[total_inputs * (B + 1)];

    int num_bytes = 0;
    for (unsigned int i = 0; i < total_inputs; i++) {
        num_bytes += recv_in(clientfd, &share, sizeof(MaxShare));
        std::string pk(share.pk, share.pk + PK_LENGTH);

        num_bytes += recv_in(clientfd, &shares[i*(B+1)], share_sz);

        if (share_map.find(pk) != share_map.end())
            continue;
        share_map[pk] = &shares[i*(B+1)];
    }

    std::cout << "Received " << total_inputs << " total shares" << std::endl;
    std::cout << "bytes from client: " << num_bytes << std::endl;
    std::cout << "receive time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
    start = clock_start();

    // if (fork() > 0) return RET_NO_ANS;

    if (server_num == 1) {
        const unsigned int num_inputs = share_map.size();
        send_size(serverfd, num_inputs);
        uint32_t b[B+1];
        memset(b, 0, sizeof(b));
        std::string* pk_list = new std::string[num_inputs];
        size_t idx = 0;
        for (const auto& share : share_map) {
            send_out(serverfd, &share.first[0], PK_LENGTH);
            pk_list[idx] = share.first;
            idx++;
        }
        for (unsigned int i = 0; i < num_inputs; i++) {
            bool other_valid;
            recv_bool(serverfd, other_valid);
            if (!other_valid)
                continue;
            for (unsigned int j = 0; j <= B; j++)
                b[j] ^= share_map[pk_list[i]][j];
        }
        send_out(serverfd, &b[0], share_sz);
        delete[] shares;
        delete[] pk_list;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        return RET_NO_ANS;
    } else {
        size_t num_inputs, num_valid = 0;;
        recv_size(serverfd, num_inputs);
        uint32_t a[B+1];
        memset(a, 0, sizeof(a));
        bool* valid = new bool[num_inputs];

        for (unsigned int i =0; i < num_inputs; i++) {
            std::string pk = get_pk(serverfd);
            valid[i] = (share_map.find(pk) != share_map.end());
            if (!valid[i])
                continue;
            num_valid++;
            for (unsigned int j = 0; j <= B; j++)
                a[j] ^= share_map[pk][j];
        }
        for (unsigned int i = 0; i < num_inputs; i++) {
            send_bool(serverfd, valid[i]);
        }
        delete[] shares;
        delete[] valid;
        uint32_t b[B+1];
        recv_in(serverfd, &b[0], share_sz);

        std::cout << "Final valid count: " << num_valid << " / " << total_inputs << std::endl;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        if (num_valid < total_inputs * (1 - INVALID_THRESHOLD)) {
            std::cout << "Failing, This is less than the invalid threshold of " << INVALID_THRESHOLD << std::endl;
            std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
            return RET_INVALID;
        }

        for (unsigned int j = B; j >= 0; j--) {
            // std::cout << "a, b[" << j << "] = " << a[j] << ", " << b[j] << std::endl;
            if (a[j] != b[j]) {
                if (msg.type == MAX_OP) {
                    ans = j;
                } else if (msg.type == MIN_OP) {
                    ans = B - j;
                } else {
                    error_exit("Message type incorrect for max_op");
                }
                return RET_ANS;
            }
        }
        // Shouldn't reach.
        return RET_INVALID;
    }

    return RET_INVALID;
}

// For var, stddev
returnType var_op(const initMsg msg, const int clientfd, const int serverfd, const int server_num, double& ans) {
    typedef std::tuple <uint64_t, uint64_t, ClientPacket*> sharetype;
    std::unordered_map<std::string, sharetype> share_map;
    auto start = clock_start();

    VarShare share; 
    // We have x^2 < max, so we want x < sqrt(max)
    const uint64_t small_max = 1ULL << (num_bits / 2);
    const uint64_t square_max = 1ULL << num_bits;
    const unsigned int total_inputs = msg.num_of_inputs;

    // Just for getting sizes
    Circuit* const mock_circuit = CheckVar();
    const size_t N = mock_circuit->N();
    const size_t NWires = mock_circuit->NumMulInpGates();
    delete mock_circuit;

    int num_bytes = 0;
    for (unsigned int i = 0; i < total_inputs; i++) {
        num_bytes += recv_in(clientfd, &share, sizeof(VarShare));
        std::string pk(share.pk, share.pk + PK_LENGTH);

        ClientPacket* packet = new ClientPacket(N, NWires);
        int packet_bytes = recv_ClientPacket(clientfd, packet);
        num_bytes += packet_bytes;

        // std::cout << "share[" << i << "] = " << share.val << ", " << share.val_squared << std::endl;

        if ((share_map.find(pk) != share_map.end())
            or (share.val >= small_max)
            or (share.val_squared >= square_max)
            or (packet_bytes <= 0)
            ) {
            delete packet;
            continue;
        }
        share_map[pk] = {share.val, share.val_squared, packet};
    }

    std::cout << "Received " << total_inputs << " total shares" << std::endl;
    std::cout << "bytes from client: " << num_bytes << std::endl;
    std::cout << "receive time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
    start = clock_start();

    // if (fork() > 0) return RET_NO_ANS;

    if (server_num == 1) {
        const unsigned int num_inputs = share_map.size();
        send_size(serverfd, num_inputs);

        // For OT
        uint64_t* shares = new uint64_t[num_inputs];
        uint64_t* shares_squared = new uint64_t[num_inputs];
        ClientPacket** packet = new ClientPacket*[num_inputs];

        std::string* pk_list = new std::string[num_inputs];
        Circuit** circuit = new Circuit*[num_inputs];
        uint64_t* wire_shares = new uint64_t[2 * num_inputs];

        size_t idx = 0;
        for (const auto& share : share_map) {
            send_out(serverfd, &share.first[0], PK_LENGTH);
            pk_list[idx] = share.first;
            idx++;
        }

        for (unsigned int i = 0; i < num_inputs; i++) {
            std::tie(shares[i], shares_squared[i], packet[i]) = share_map[pk_list[i]];
            circuit[i] = CheckVar();
            wire_shares[2 * i] = shares[i];
            wire_shares[2 * i + 1] = shares_squared[i];
        }
        bool* snip_valid = validate_snips(num_inputs, 2, serverfd, server_num,
                                          num_bits, circuit, packet, wire_shares);
        for (unsigned int i = 0; i < num_inputs; i++) {
            send_bool(serverfd, snip_valid[i]);
        }

        // Compute result
        NetIO* const io = new NetIO(SERVER0_IP, 60051, true);
        uint64_t b = intsum_ot_receiver(io, &shares[0], num_inputs, num_bits);
        uint64_t b2 = intsum_ot_receiver(io, &shares_squared[0], num_inputs, num_bits);

        send_uint64(serverfd, b);
        send_uint64(serverfd, b2);

        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;

        for (unsigned int i = 0; i < num_inputs; i++) {
            delete circuit[i];
            delete packet[i];
        }
        delete io;
        delete[] shares;
        delete[] shares_squared;
        delete[] packet;
        delete[] pk_list;
        delete[] circuit;
        delete[] wire_shares;
        delete[] snip_valid;
        return RET_NO_ANS;
    } else {
        size_t num_inputs, num_valid = 0;
        recv_size(serverfd, num_inputs);

        // For OT
        uint64_t* shares = new uint64_t[num_inputs];
        uint64_t* shares_squared = new uint64_t[num_inputs];
        ClientPacket** packet = new ClientPacket*[num_inputs];

        bool* valid = new bool[num_inputs];
        std::string* pk_list = new std::string[num_inputs];
        Circuit** circuit = new Circuit*[num_inputs];
        uint64_t* wire_shares = new uint64_t[2 * num_inputs];

        std::cout << "getting PKs" << std::endl;
        for (unsigned int i = 0; i < num_inputs; i++) {
            std::string pk = get_pk(serverfd);
            pk_list[i] = pk;
            valid[i] = (share_map.find(pk) != share_map.end());
        }
        std::cout << "unpacking" << std::endl;
        for (unsigned int i = 0; i < num_inputs; i++) {
            if (valid[i]) {
                std::tie(shares[i], shares_squared[i], packet[i]) = share_map[pk_list[i]];
            } else {
                packet[i] = new ClientPacket(N, NWires);  // mock empty packet
            }
            circuit[i] = CheckVar();
            wire_shares[2 * i] = shares[i];
            wire_shares[2 * i + 1] = shares_squared[i];
        }
        std::cout << "validating snips" << std::endl;
        bool* snip_valid = validate_snips(num_inputs, 2, serverfd, server_num,
                                          num_bits, circuit, packet, wire_shares);
        std::cout << "getting valid" << std::endl;
        for (unsigned int i = 0; i < num_inputs; i++) {
            bool other_valid;
            recv_bool(serverfd, other_valid);
            valid[i] &= snip_valid[i];
            valid[i] &= other_valid;
            if (valid[i])
                num_valid++;
        }

        // Compute result
        NetIO* const io = new NetIO(nullptr, 60051, true);
        uint64_t a = intsum_ot_sender(io, &shares[0], &valid[0], num_inputs, num_bits);
        uint64_t a2 = intsum_ot_sender(io, &shares_squared[0], &valid[0], num_inputs, num_bits);

        for (unsigned int i = 0; i < num_inputs; i++) {
            delete circuit[i];
            delete packet[i];
        }
        delete io;
        delete[] shares;
        delete[] shares_squared;
        delete[] valid;
        delete[] pk_list;
        delete[] circuit;
        delete[] wire_shares;
        delete[] snip_valid;

        uint64_t b, b2;
        recv_uint64(serverfd, b);
        recv_uint64(serverfd, b2);

        std::cout << "Final valid count: " << num_valid << " / " << total_inputs << std::endl;
        std::cout << "compute time: " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        if (num_valid < total_inputs * (1 - INVALID_THRESHOLD)) {
            std::cout << "Failing, This is less than the invalid threshold of " << INVALID_THRESHOLD << std::endl;
            return RET_INVALID;
        }

        const double ex = 1.0 * (a + b) / num_valid;
        const double ex2 = 1.0 * (a2 + b2) / num_valid;
        ans = ex2 - (ex * ex);
        if (msg.type == VAR_OP) {
            std::cout << "Ans: " << ex2 << " - (" << ex << ")^2 = " << ans << std::endl;
        }
        if (msg.type == STDDEV_OP) {
            ans = sqrt(ans);
            std::cout << "Ans: sqrt(" << ex2 << " - (" << ex << ")^2) = " << ans << std::endl;
        }
        return RET_ANS;
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "Usage: ./bin/server server_num(0/1) this_client_port server0_port INT_SUM_MAX_bits" << endl;
    }

    const int server_num = atoi(argv[1]);  // Server # 1 or # 2
    const int client_port = atoi(argv[2]); // port of this server, for the client
    const int server_port = atoi(argv[3]); // port of this server, for the other server

    std::cout << "This server is server # " << server_num << std::endl;
    std::cout << "  Listening for client on " << client_port << std::endl;
    std::cout << "  Listening for server on " << server_port << std::endl;

    if (argc >= 5)
        num_bits = atoi(argv[4]);

    init_constants();

    // Set serverfd. 
    // Server 0 listens, via newsockfd_server.
    // Server 1 connects, via sockfd_server
    int sockfd_server, newsockfd_server, serverfd = 0;
    if (server_num == 0) {
        server0_listen(sockfd_server, newsockfd_server, server_port, 1);
        serverfd = newsockfd_server;
    } else if (server_num == 1) {
        server1_connect(sockfd_server, server_port, 1);
        serverfd = sockfd_server;
    } else {
        error_exit("Can only handle servers #0 and #1");
    }

    fmpz_init(randomX);
    sync_randomX(serverfd, server_num, randomX);

    correlated_store = new CorrelatedStore(serverfd, server_num, SERVER0_IP, SERVER1_IP, num_bits, CACHE_SIZE, LAZY_PRECOMPUTE);

    int sockfd, newsockfd;
    sockaddr_in addr;

    bind_and_listen(addr, sockfd, client_port, 1);

    while(1) {
        // Refresh randomX if used too much
        if (randx_uses > RANDOMX_THRESHOLD) {
            randx_uses -= RANDOMX_THRESHOLD;
            sync_randomX(serverfd, server_num, randomX);
            // Update precomps
            for (const auto& pair : precomp_store) {
                pair.second -> clear_preX();
                pair.second -> setCheckerPrecomp(randomX);
            }
        }

        correlated_store->maybeUpdate();

        socklen_t addrlen = sizeof(addr);

        std::cout << "waiting for connection..." << std::endl;

        newsockfd = accept(sockfd, (struct sockaddr*)&addr, &addrlen);
        if (newsockfd < 0) error_exit("Connection creation failure");

        // Get an initMsg
        initMsg msg;
        recv_in(newsockfd, &msg, sizeof(initMsg));

        if (msg.type == BIT_SUM) {
            std::cout << "BIT_SUM" << std::endl;
            auto start = clock_start();

            uint64_t ans;
            returnType ret = bit_sum(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == INT_SUM) {
            std::cout << "INT_SUM" << std::endl;
            auto start = clock_start();

            uint64_t ans;
            returnType ret = int_sum(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == AND_OP) {
            std::cout << "AND_OP" << std::endl;
            auto start = clock_start();

            bool ans;
            returnType ret = xor_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << std::boolalpha << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == OR_OP) {
            std::cout << "OR_OP" << std::endl;
            auto start = clock_start();

            bool ans;
            returnType ret = xor_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << std::boolalpha << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == MAX_OP) {
            std::cout << "MAX_OP" << std::endl;
            auto start = clock_start();

            uint32_t ans;
            returnType ret = max_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == MIN_OP) {
            std::cout << "MIN_OP" << std::endl;
            auto start = clock_start();

            uint32_t ans;
            returnType ret = max_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == VAR_OP) {
            std::cout << "VAR_OP" << std::endl;
            auto start = clock_start();

            double ans;
            returnType ret = var_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == STDDEV_OP) {
            std::cout << "STDDEV_OP" << std::endl;
            auto start = clock_start();

            double ans;
            returnType ret = var_op(msg, newsockfd, serverfd, server_num, ans);
            if (ret == RET_ANS)
                std::cout << "Ans: " << ans << std::endl;

            std::cout << "Total time  : " << (((float)time_from(start))/CLOCKS_PER_SEC) << std::endl;
        } else if (msg.type == NONE_OP) {
            std::cout << "Empty client message" << std::endl;
        } else {
            std::cout << "Unrecognized message type: " << msg.type << std::endl;
        }
        close(newsockfd);
    }

    delete correlated_store;
    for (const auto& precomp : precomp_store)
        delete precomp.second;
    return 0;
}
