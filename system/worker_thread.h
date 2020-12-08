#ifndef _WORKERTHREAD_H_
#define _WORKERTHREAD_H_

#include "global.h"
#include "message.h"
#include "crypto.h"

class Workload;
class Message;

class WorkerThread : public Thread
{
public:
    RC run();
    void setup();
    void send_key();
    RC process_key_exchange(Message *msg);

    void process(Message *msg);
    TxnManager *get_transaction_manager(uint64_t txn_id, uint64_t batch_id);
    RC init_phase();
    bool is_cc_new_timestamp();
    bool exception_msg_handling(Message *msg);

    uint64_t next_set;
    uint64_t get_next_txn_id();

    void release_txn_man(uint64_t txn_id, uint64_t batch_id);
    void algorithm_specific_update(Message *msg, uint64_t idx);
    void create_and_send_batchreq(ClientQueryBatch *msg, uint64_t tid);
    void set_txn_man_fields(BatchRequests *breq, uint64_t bid);

    bool validate_msg(Message *msg);
    bool checkMsg(Message *msg);

    /******* RAFT *******/

    RC process_apnd_entry(Message *msg);        // worker_thread_raft
    RC process_apnd_entry_resp(Message *msg);   // worker_thread_raft

    /********************/

    RC process_client_batch(Message *msg);      // worker_thread_pbft (primary receive client request, pre-prepare)
    RC process_batch(Message *msg);             // worker_thread_pbft (non-primary receive client request, send prepare O(n^2))
    void send_checkpoints(uint64_t txn_id); 
    RC process_pbft_chkpt_msg(Message *msg);    
#if BANKING_SMART_CONTRACT
    void init_txn_man(BankingSmartContractMessage *bscm);
#else
    void init_txn_man(YCSBClientQueryMessage *msg);
#endif
#if EXECUTION_THREAD
    void send_execute_msg();
    RC process_execute_msg(Message *msg);
#endif

#if TIMER_ON
    void add_timer(Message *msg, string qryhash);
#endif

#if VIEW_CHANGES
    void client_query_check(ClientQueryBatch *clbtch);
    void check_for_timeout();
    void check_switch_view();
    void store_batch_msg(BatchRequests *breq);
    RC process_view_change_msg(Message *msg);
    RC process_new_view_msg(Message *msg);
    void reset();
    void fail_primary(Message *msg, uint64_t batch_to_fail);
#endif

#if LOCAL_FAULT
    void fail_nonprimary();
#endif

    bool prepared(PBFTPrepMessage *msg);            // moved to worker_thread_pbft (checks for 2f+1)
    RC process_pbft_prep_msg(Message *msg);         // worker_thread_pbft (receive prepare, send commit)

    bool committed_local(PBFTCommitMessage *msg);   // worker_thread_pbft (checks for 2f+1)
    RC process_pbft_commit_msg(Message *msg);       // worker_thread_pbft (receive commit)

#if TESTING_ON
    void testcases(Message *msg);
#if TEST_CASE == ONLY_PRIMARY_NO_EXECUTE
    void test_no_execution(Message *msg);
#elif TEST_CASE == ONLY_PRIMARY_EXECUTE
    void test_only_primary_execution(Message *msg);
#elif TEST_CASE == ONLY_PRIMARY_BATCH_EXECUTE
    void test_only_primary_batch_execution(Message *msg);
#endif
#endif

private:
    uint64_t _thd_txn_id;
    ts_t _curr_ts;
    TxnManager *txn_man;
};

#endif
