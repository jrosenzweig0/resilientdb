#include "chain.h"

/****************************************/
/************* BChainStruct *************/
/****************************************/

/* Set the identifier for the block. */
void BChainStruct::set_txn_id(uint64_t tid)
{
	txn_id = tid;
}	

/* Get the identifier of this block. */
uint64_t BChainStruct::get_txn_id()
{
	return txn_id;
}

/* Store the BatchRequests to this block. */
void BChainStruct::add_batch(BatchRequests *msg) {
	char *buf = create_msg_buffer(msg);
	Message *deepMsg = deep_copy_msg(buf, msg);
	batch_info = (BatchRequests *)deepMsg;
	delete_msg_buffer(buf);
}	

/* Store the commit messages to this block. */
void BChainStruct::add_commit_proof(Message *msg) {
	char *buf = create_msg_buffer(msg);
	Message *deepMsg = deep_copy_msg(buf, msg);
	commit_proof.push_back(deepMsg);
	delete_msg_buffer(buf);
}

/* Release the contents of the block. */
void BChainStruct::release_data() {
	Message::release_message(this->batch_info);

	PBFTCommitMessage *cmsg;
	while(this->commit_proof.size()>0)
	{
		cmsg = (PBFTCommitMessage *)this->commit_proof[0];
		this->commit_proof.erase(this->commit_proof.begin());
		Message::release_message(cmsg);
	}	
}

/************* RAFT ADDITIONS *************/

#if CONSENSUS == RAFT

/* set the term this block was committed */
void BChainStruct::set_term(uint64_t t) 
{
	term = t;
}

/* get the term this block was committed */
uint64_t BChainStruct::get_term()
{
	return term;
}

/* Get the batch request from this block (returns a deep copy of the batch request) */
BatchRequests *BChainStruct::get_batch_request() 
{
	char *buf = create_msg_buffer(batch_info);
	Message *deepMsg = deep_copy_msg(buf, batch_info);
	return (BatchRequests *) deepMsg; // !!! this might not work !!!
}

/* Get the number of commit messages on the block */
uint64_t BChainStruct::get_commit_len() {
	return commit_proof.size();
}

#endif

/****************************************/
/**************** BChain ****************/
/****************************************/

/* Add a block to the chain. */
void BChain::add_block(TxnManager *txn) {
	BChainStruct *blk = (BChainStruct *)mem_allocator.alloc(sizeof(BChainStruct));
	new (blk) BChainStruct();

	blk->set_txn_id(txn->get_txn_id());
	blk->add_batch(txn->batchreq);

#if CONSENSUS == RAFT
	blk->set_term(txn->batchreq->term)
#endif

	for(uint64_t i=0; i<txn->commit_msgs.size(); i++) {
		blk->add_commit_proof(txn->commit_msgs[i]);
	}	
	
	chainLock.lock();
	   bchain_map.push_back(blk);
	chainLock.unlock();
}

/* Remove a block from the chain bbased on its identifier. */
void BChain::remove_block(uint64_t tid)
{
	BChainStruct *blk;
	bool found = false;

	chainLock.lock();
	   for (uint64_t i = 0; i < bchain_map.size(); i++)
	   {
	   	blk = bchain_map[i];
	   	if (blk->get_txn_id() == tid)
	   	{
	   		bchain_map.erase(bchain_map.begin() + i);
	   		found = true;
	   		break;
	   	}
	   }
	chainLock.unlock();

	if(found) {
		blk->release_data();
		mem_allocator.free(blk, sizeof(BChainStruct));
	}	
}

/************* RAFT ADDITIONS *************/

// Some blockchain functions that will be helpful for the raft implementation
#if CONSENSUS == RAFT

/* Removes the last block currently on the blockchain */
void BChain::remove_last() {
	BChainStruct *blk;
	bool found = false;

	chainLock.lock();
	if (bchain_map.size() > 0) 
	{
		found = true;
		blk = bchain_map.back();
		bchain_map.pop_back();
	}
	chainLock.unlock();

	if (found) {
		blk->release_data();
		mem_allocator.free(blk, sizeof(BChainStruct));
	}
}

/* Removes all blocks from index i to the end of the chain */
void BChain::remove_since_index(uint64_t i) {
	std::vector<BChainStruct *> blks;
	bool found = false;

	chainLock.lock();
	if (bchain_map.size() > i) {
		found = true;
		for (uint64_t j = i; j < bchain_map.size(); j++) {
			blks.push_back(bchain_map[i]);
		}
		bchain_map.erase(bchain_map.begin()+i, bchain_map.end());
	}
	chainLock.unlock();

	if (found) {
		for (uint64_t j = 0; j < blks.size(); j++) {
			blks[i]->release_data();
			mem_allocator.free(blks[i], sizeof(BChainStruct));
		}
	}
	blks.clear()
}

/* Gets the batchrequest stored on the i-th block of the blockchain */
BatchRequests *BChain::get_batch_at_index(uint64_t i) {
	BatchRequests *breq;
	bool found = false;

	chainLock.lock();
	if (i < bchain_map.size()) {
		breq = bchain_map[i]->get_batch_request();
		found = true;
	}
	chainLock.unlock();

	if (found) {
		return breq;
	} else {
		return (BatchRequests *) NULL;
	}
}

/* Gets all the blocks since the given index until the end of the blockchain 
	and returns a vector of the batchrequests stored in them 
	(used by primary to forward committed transactions to workers) */
std::vector<BatchRequests *> BChain::get_batches_since_index(uint64_t start) {
	std::vector<BatchRequests *> batches;
	bool success = false;

	chainLock.lock();
	if (start < bchain_map.size()) 
	{
		for (uint64_t i = start; i < bchain_map.size(); i++)
		{
			batches.push_back(bchain_map[i]->get_batch_request());
		}
		success = true;
	}
	chainLock.unlock();

	return batches;
}

/* Get the current length of the blockchain */
uint64_t BChain::get_length() {
	uint64_t val;
	chainLock.lock();
	val = bchain_map.size();
	chainLock.unlock();
	return val;
}

/* Gets term block i was committed, term cannot be 0 */
uint64_t BChain::get_term_at(uint64_t i) {
	uint64_t term = 0;
	chainLock.lock();
	if (i < bchain_map.size()) {
		term = bchain_map[i]->get_term();
	}
	chainLock.unlock();

	return term;
}

/* Get transaction id at block i */
uint64_t get_txn_id_at(uint64_t i) {
	uint64_t val = UINT64_MAX;
	chainLock.lock();
	if (i < bchain_map.size()) {
		val = bchain_map[i]->get_txn_id();
	}
	chainLock.unlock();
	return val;
}

/* Checks that the term of block i matches the given term t */
bool BChain::check_term_match_at(uint64_t i, uint64_t t) {
	bool ret = false;

	chainLock.lock();
	if (i < bchain_map.size()) {
		ret = (t == bchain_map[i]->get_term());
	}
	chainLock.unlock();

	return ret;
}

/* add proof to the block at index i (AppendEntriesResponse for Raft) */
void BChain::add_proof_at(uint64_t i, Message *proof) {
	chainLock.lock();
	if (i < bchain_map.size()) {
		bchain_map[i]->add_commit_proof(proof);
	}
	chainLock.unlock();
}

void add_proof_for_range(uint64_t start, uint64_t end, Message *proof) {
	assert(start < end);
	chainLock.lock();
	if (end <= bchain_map.size()) {
		for (uint64_t i = start; i < end; i++) {
			bchain_map[i]->add_commit_proof(proof);
		}
	}
	chainLock.unlock();
}


bool BChain::check_proof_at(uint64_t i, uint64_t half) {
	bool ret = false;
	chainLock.lock();
	if (i < bchain_map.size()) {
		ret = (bchain_map[i]->get_commit_len() > half);
	}
	chainLock.unlock();
	return ret;
}

#endif

/*****************************************/

BChain *BlockChain;
std::mutex chainLock;
