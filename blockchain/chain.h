#include "message.h"
#include "txn.h"

/*
 * This class provides an implementation for a block to be part of the chain.
 * Each block is identified using the identifier for the last transaction in 
 * its batch. Further, each block consists of the signed BatchRequests msg from
 * the primary replica, which also includes the client request. Each block also
 * includes the signed Commit messages from other replicas.
 */
class BChainStruct
{
	uint64_t txn_id;
	BatchRequests *batch_info;	// BatchRequests msg from primary.
	vector<Message *> commit_proof; // Signed commit messages.

public:
	void set_txn_id(uint64_t tid);
	uint64_t get_txn_id();
	void add_batch(BatchRequests *bmsg);
	void add_commit_proof(Message *proof);
	void release_data();

#if CONSENSUS == RAFT

	uint64_t term;
	void set_term(uint64_t t);
	uint64_t get_term();
	BatchRequests *get_batch_request();

#endif
};


class BChain 
{
	// The actual chain is implemented as a vector of blocks.
	std::vector<BChainStruct *> bchain_map;

public:
	void add_block(TxnManager *txn);
	void remove_block(uint64_t tid);

#if CONSENSUS == RAFT

	void remove_last();
	BatchRequests *get_batch_at_index(uint64_t i);
	std::vector<BatchRequests *> get_batches_since_index(uint64_t start);
	bool check_term_match_at(uint64_t i, uint64_t t);

#endif

};	

extern BChain *BlockChain;	// Global variable to access the chain.
extern std::mutex chainLock;
