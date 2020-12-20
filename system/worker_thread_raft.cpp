#include "global.h"
#include "message.h"
#include "thread.h"
#include "worker_thread.h"
#include "txn.h"
#include "wl.h"
#include "query.h"
#include "ycsb_query.h"
#include "math.h"
#include "helper.h"
#include "msg_thread.h"
#include "msg_queue.h"
#include "work_queue.h"
#include "message.h"
#include "timer.h"
#include "chain.h"

#if CONSENSUS == RAFT

/**
 * invoke append_entries RPC
 *
 * this function should assume that a client sends a batch of transactions 
 * for each transaction in the batch. it is largely the same as pbft pre-prepare
 * with the main difference being what is sent/invoked
 */
RC WorkerThread::append_entries() {

    cout << "\nAppendEntriesRPC to all nodes\n";
    fflush(stdout);

    assert(g_node_id == get_current_view(get_thd_id()));
    // create the append entries rpc message for each node
    // std::vector<AppendEntriesRPC *> aerpcs;

    for (uint i = 0; i < g_node_cnt; i++) {
        // skip if this node
        if (i != g_node_id) {
            // construct AppendEntriesRPC
            Message *aemsg = Message::create_message(RAFT_AE_RPC);
            AppendEntriesRPC *aerpc = (AppendEntriesRPC *) aemsg;
            aerpc->init();

            aerpc->prevLogIndex = get_node_nextIndex(i) - 1;
            aerpc->prevLogTerm = (BlockChain->get_term_at(aerpc->prevLogIndex));

            // get list of BatchRequests from blockchain
            aerpc->entries = BlockChain->get_batches_since_index(get_node_nextIndex(i));
            aerpc->numEntries = aerpc->entries.size();

            // sign and send AppendEntriesRPC to destination
            vector<string> emptyvec;
            aerpc->sign(i);

            emptyvec.push_back(aerpc->signature);

            vector<uint64_t> dest = nodes_to_send(i, i+1);
            msg_queue.enqueue(get_thd_id(), aerpc, emptyvec, dest);
            emptyvec.clear();
            // aerpcs.push_back(aerpc);
        }
    }

    return RCOK;

}

RC WorkerThread::append_entries(uint64_t node) {

    cout << "\nAppendEntriesRPC to node " << node << "\n";
    fflush(stdout);

    assert(g_node_id == get_current_view(get_thd_id()));
    // create the append entries rpc message for each node

    // construct AppendEntriesRPC
    Message *aemsg = Message::create_message(RAFT_AE_RPC);
    AppendEntriesRPC *aerpc = (AppendEntriesRPC *) aemsg;
    aerpc->init();

    aerpc->prevLogIndex = get_node_nextIndex(node) - 1;
    aerpc->prevLogTerm = (BlockChain->get_term_at(aerpc->prevLogIndex));

    // get list of BatchRequests from blockchain
    aerpc->entries = BlockChain->get_batches_since_index(get_node_nextIndex(node));
    aerpc->numEntries = aerpc->entries.size();

    // sign and send AppendEntriesRPC to destination
    vector<string> emptyvec;
    aerpc->sign(node);

    emptyvec.push_back(aerpc->signature);

    vector<uint64_t> dest = nodes_to_send(node, node+1);
    msg_queue.enqueue(get_thd_id(), aerpc, emptyvec, dest);
    emptyvec.clear();
    // aerpcs.push_back(aerpc);

    return RCOK;

}

/**
 * Process the incoming AppendEntriesRPC
 *
 * this Function should do the following:
 * - reply false to leader/primary if term < currentTerm
 * - reply false if local log has no entry at prevLogIndex whose term matches prevLogTerm
 * - if existing entry conflicts with new entry, (same index different terms),
 *      delete existing entry and all following entries
 * 
 */
RC WorkerThread::process_append_entries(Message *msg) {

    cout << "\nReceived AppendEntriesRPC\n";
    fflush(stdout);

#if TIMER_ON
    // TODO: reset timer
    election_timer->startTimer();
#endif

    AppendEntriesRPC *aerpc = (AppendEntriesRPC *)msg;

    // only followers should be using this
    assert(g_node_id != get_current_view(get_thd_id()));

    // authenticate message
    validate_msg(aerpc);
    
    // construct response
    Message *amsg = Message::create_message(RAFT_AE_RESP);
    AppendEntriesResponse *aer = (AppendEntriesResponse *)amsg;
    aer->init();

    // debugging
    cout << "Parameters:" \
            << "\nterm: " << aerpc->term \
            << "\nleaderId: " << aerpc->leaderId \
            << "\nprevLogIndex: " << aerpc->prevLogIndex \
            << "\nnumEntries: " << aerpc->numEntries \
            << "\nleaderCommit: " << aerpc->leaderCommit;
    fflush(stdout);

    // reply false if term < currentTerm
    if (aerpc->term < currentTerm) {
        aer->success = false;
    }

    // reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm
    else if (BlockChain->get_length() > 0 && (aerpc->prevLogIndex + 1 > 0) && !BlockChain->check_term_match_at(aerpc->prevLogIndex, aerpc->prevLogTerm)) {
        aer->success = false;
    }

    // handle heartbeat with no data (might have commit information though)
    else if (!aerpc->numEntries) {
        // send the matchIndex back to primary (primary might have added to chain since last call)
        aer->matchIndex = aerpc->prevLogIndex + aerpc->numEntries;

        if (aerpc->leaderCommit > get_commitIndex()) {
            set_commitIndex(std::min(aerpc->leaderCommit, BlockChain->get_length() - 1));
        }
    }

    // handle entries to append to chain
    else {
        // If an existing enty conflicts with a new one, delete the existing entry and all that follow it
        if (!BlockChain->check_term_match_at(aerpc->prevLogIndex + 1, aerpc->entries[0]->term)) {
            BlockChain->remove_since_index(aerpc->prevLogIndex + 1);
        }

        // need to put this in blockchain since threads do not guarantee order
        // add all batch requests in entries to the blockchain (not committed)
        for (uint64_t i = 0; i < aerpc->entries.size(); i++) {
            // pointer to batch request
            BatchRequests *breq = aerpc->entries[i];

            // allocate transaction managers for all transactions in the batch i
            set_txn_man_fields(breq, 0);

            // Store BatchRequest Message
            txn_man->set_primarybatch(breq);

            // Add BatchRequest to the blockchain (not committed)
            BlockChain->add_block(txn_man);
        }

        // send the matchIndex back to primary (primary might have added to chain since last call)
        aer->matchIndex = aerpc->prevLogIndex + aerpc->numEntries;

        if (aerpc->leaderCommit > get_commitIndex()) {
            set_commitIndex(std::min(aerpc->leaderCommit, BlockChain->get_length() - 1));
        }
    }

    // if lastApplied < commitIndex, execute transactions until caught up
    uint64_t lA;
    while (get_commitIndex() >= 0 && get_commitIndex() > get_lastApplied()) {
        inc_lastApplied();
        lA = get_lastApplied();
        txn_man = get_transaction_manager(BlockChain->get_txn_id_at(lA), 0);
        // txn_man->set_primarybatch(BlockChain->get_batch_at_index(lA));
        txn_man->txn_ready = 1;
        cout << "Committing...\n";
        fflush(stdout);
        // send_execute_msg();
        Message *tmsg = Message::create_message(txn_man, EXECUTE_MSG);
        work_queue.enqueue(get_thd_id(), tmsg, false);
    }

    // debugging
    cout << "\n\nState:" \
        << "\nTerm: " << get_currentTerm() \
        << "\nmatchIndex: " << aer->matchIndex \
        << "\ncommitIndex: " << get_commitIndex() \
        << "\nlastApplied: " << get_lastApplied() \
        << "\nBlockchain Length: " << BlockChain->get_length();
        // << "\nBlockchain:\n";
    // BlockChain->print_chain();

    // sign and send AppendEntriesResponse to leader
    vector<string> emptyvec;
    aer->sign(aerpc->leaderId);

    emptyvec.push_back(aer->signature);

    vector<uint64_t> dest = nodes_to_send(aerpc->leaderId, aerpc->leaderId+1);
    msg_queue.enqueue(get_thd_id(), aer, emptyvec, dest);
    emptyvec.clear();

    cout << "\n" << aer->success << " Response Sent!\n";
    fflush(stdout);

    return RCOK;
}


/**
 * Process the incoming AppendEntriesResponse
 *
 * this Function should do the following:
 * - reply false to leader/primary if term < currentTerm
 * - reply false if local log has no entry at prevLogIndex whose term matches prevLogTerm
 * - if existing entry conflicts with new entry, (same index different terms),
 *      delete existing entry and all following entries
 * 
 */
RC WorkerThread::process_append_entries_resp(Message *msg) {

    cout << "\nreceived AppendEntriesResponse!\n";
    fflush(stdout);

    AppendEntriesResponse *aer = (AppendEntriesResponse *) msg;

    // only the leader calls this function
    assert(g_node_id == get_current_view(get_thd_id()));

    // validate the AppendEntriesResponse message
    validate_msg(aer);

    // get the id of the node that sent the response message
    uint64_t node = aer->return_node_id;

    cout << "From Node " << node << "\n";
    fflush(stdout);

    if (aer->success) {
        // if success, 
        cout << "Append Entries Success!\n";
        fflush(stdout);
        // set_node_matchIndex(node, BlockChain->get_length()-1);
        // set_node_nextIndex(node, BlockChain->get_length());
        set_node_matchIndex(node, aer->matchIndex);
        set_node_nextIndex(node, aer->matchIndex + 1);
    } else if (get_node_nextIndex(node) > 0) {
        decr_node_nextIndex(node);
    }

    //if majority of replicas have matching index N with primary, set commitIndex to N
    uint64_t N = get_median_matchIndex();
    if ((N > get_commitIndex()) && (BlockChain->check_term_match_at(N, get_currentTerm()))) {
        set_commitIndex(N);
    }

    // if lastApplied < commitIndex, execute transactions until caught up
    uint64_t lA;
    while (get_commitIndex() >= get_lastApplied()) {
        cout << "Committing Transactions...\n";
        fflush(stdout);
        lA = get_lastApplied();
        txn_man = get_transaction_manager(BlockChain->get_txn_id_at(lA), 0);
        cout << "Commiting txn id: " << txn_man->get_txn_id() << "\n";
        fflush(stdout);
        // txn_man->set_primarybatch(BlockChain->get_batch_at_index(lA));
        txn_man->txn_ready = 1;
        // send_execute_msg();
        Message *tmsg = Message::create_message(txn_man, EXECUTE_MSG);
        work_queue.enqueue(get_thd_id(), tmsg, false);
        inc_lastApplied();
    }

    // debugging
    cout << "\nThread: " << get_thd_id() \
        << "\nCurrent Term: " << get_currentTerm() \
        << "\nChain Length: " << BlockChain->get_length() \
        << "\nCommit Index: " << get_commitIndex() << "\n";
    for (uint i = 0; i < g_node_cnt; i++) {
        cout << i << " nextIndex: " << get_node_nextIndex(i) << "\n";
        cout << i << " matchIndex: " << get_node_matchIndex(i) << "\n";
    }
    fflush(stdout);

#if !TIMER_ON
    // broadcast append entries
    append_entries(node);
#endif

    return RCOK;
}

/**
 * Processes an incoming client batch and sends a AppendEntriesRPC to all replicas.
 *
 * This function assumes that a client sends a batch of transactions and 
 * for each transaction in the batch, a separate transaction manager is created. 
 * Next, this batch added to the log (uncommitted) and sent to all other nodes 
 * via AppendEntriesRPC
 *
 * @param msg Batch of Transactions of type CientQueryBatch from the client.
 * @return RC
 */
RC WorkerThread::process_client_batch(Message *msg)
{
    //printf("ClientQueryBatch: %ld, THD: %ld :: CL: %ld :: RQ: %ld\n",msg->txn_id, get_thd_id(), msg->return_node_id, clbtch->cqrySet[0]->requests[0]->key);
    //fflush(stdout);

    ClientQueryBatch *clbtch = (ClientQueryBatch *)msg;

    // Authenticate the client signature.
    validate_msg(clbtch);

#if VIEW_CHANGES
    // If message forwarded to the non-primary.
    if (g_node_id != get_current_view(get_thd_id()))
    {
        client_query_check(clbtch);
        return RCOK;
    }

    // Partial failure of Primary 0.
    fail_primary(msg, 9);
#endif

    // Initialize all transaction mangers and construct the batchrequest
    // create_and_send_batchreq(clbtch, clbtch->txn_id);

    /*** taken from create_and_send_batch_requests ****/

    Message *bmsg = Message::create_message(BATCH_REQ);
    BatchRequests *breq = (BatchRequests *)bmsg;
    breq->init(get_thd_id());

    next_set = clbtch->txn_id;

    string batchStr;

     for (uint64_t i = 0; i < get_batch_size(); i++)
    {
        uint64_t txn_id = get_next_txn_id() + i;

        //cout << "Txn: " << txn_id << " :: Thd: " << get_thd_id() << "\n";
        //fflush(stdout);
        txn_man = get_transaction_manager(txn_id, 0);

        // Unset this txn man so that no other thread can concurrently use.
        while (true)
        {
            bool ready = txn_man->unset_ready();
            if (!ready)
            {
                continue;
            }
            else
            {
                break;
            }
        }

        txn_man->register_thread(this);
        txn_man->return_id = clbtch->return_node;

        // Fields that need to updated according to the specific algorithm.
        algorithm_specific_update(clbtch, i);

        init_txn_man(clbtch->cqrySet[i]);

        // Append string representation of this txn.
        batchStr += clbtch->cqrySet[i]->getString();

        // Setting up data for BatchRequests Message.
        breq->copy_from_txn(txn_man, clbtch->cqrySet[i]);

        // Reset this txn manager.
        bool ready = txn_man->set_ready();
        assert(ready);
    }

    // Now we need to unset the txn_man again for the last txn of batch.
    while (true)
    {
        bool ready = txn_man->unset_ready();
        if (!ready)
        {
            continue;
        }
        else
        {
            break;
        }
    }

    // Generating the hash representing the whole batch in last txn man.
    txn_man->set_hash(calculateHash(batchStr));
    txn_man->hashSize = txn_man->hash.length();

    breq->copy_from_txn(txn_man);

    // Storing the BatchRequests message.
    txn_man->set_primarybatch(breq);

    // Storing all the signatures.
    vector<string> emptyvec;
    TxnManager *tman = get_transaction_manager(txn_man->get_txn_id() - 2, 0);
    for (uint64_t i = 0; i < g_node_cnt; i++)
    {
        if (i == g_node_id)
        {
            continue;
        }
        breq->sign(i);
        tman->allsign.push_back(breq->signature); // Redundant
        emptyvec.push_back(breq->signature);
    }

    /**********************************************/

    // add the resulting batch request to the log
    cout << "Adding block with txn id: " << txn_man->get_txn_id() << "\n";
    fflush(stdout);
    BlockChain->add_block(txn_man);
    // inc_node_nextIndex(g_node_id);
    set_node_nextIndex(g_node_id, BlockChain->get_length());

    // debugging
    // BlockChain->print_chain();
    // cout << "\nChain Length: " << BlockChain->get_length() << "\n";

    // broadcast AppendEntriesRPC if this is the first block on the chain
#if !TIMER_ON
    if (is_first_block()) {
        first_block_sent();
        append_entries();
    }
#endif

    return RCOK;
}

// /**
//  * Process incoming BatchRequests message from the Primary. (Prepare)
//  *
//  * This function is used by the non-primary or backup replicas to process an incoming
//  * BatchRequests message sent by the primary replica. This processing would require 
//  * sending messages of type PBFTPrepMessage, which correspond to the Prepare phase of 
//  * the PBFT protocol. Due to network delays, it is possible that a repica may have 
//  * received some messages of type PBFTPrepMessage and PBFTCommitMessage, prior to 
//  * receiving this BatchRequests message.
//  *
//  * @param msg Batch of Transactions of type BatchRequests from the primary.
//  * @return RC
//  */
// RC WorkerThread::process_batch(Message *msg)
// {
//     uint64_t cntime = get_sys_clock();

//     BatchRequests *breq = (BatchRequests *)msg;

//     //printf("BatchRequests: TID:%ld : VIEW: %ld : THD: %ld\n",breq->txn_id, breq->view, get_thd_id());
//     //fflush(stdout);

//     // Assert that only a non-primary replica has received this message.
//     assert(g_node_id != get_current_view(get_thd_id()));

//     // Check if the message is valid.
//     validate_msg(breq);

// #if VIEW_CHANGES
//     // Store the batch as it could be needed during view changes.
//     store_batch_msg(breq);
// #endif

//     // Allocate transaction managers for all the transactions in the batch.
//     set_txn_man_fields(breq, 0);

// #if TIMER_ON
//     // The timer for this client batch stores the hash of last request.
//     add_timer(breq, txn_man->get_hash());
// #endif

//     // Storing the BatchRequests message.
//     txn_man->set_primarybatch(breq);

//     // Send Prepare messages.
//     txn_man->send_pbft_prep_msgs();

//     // End the counter for pre-prepare phase as prepare phase starts next.
//     double timepre = get_sys_clock() - cntime;
//     INC_STATS(get_thd_id(), time_pre_prepare, timepre);

//     // Only when BatchRequests message comes after some Prepare message.
//     for (uint64_t i = 0; i < txn_man->info_prepare.size(); i++)
//     {
//         // Decrement.
//         uint64_t num_prep = txn_man->decr_prep_rsp_cnt();
//         if (num_prep == 0)
//         {
//             txn_man->set_prepared();
//             break;
//         }
//     }

//     // If enough Prepare messages have already arrived.
//     if (txn_man->is_prepared())
//     {
//         // Send Commit messages.
//         txn_man->send_pbft_commit_msgs();

//         double timeprep = get_sys_clock() - txn_man->txn_stats.time_start_prepare - timepre;
//         INC_STATS(get_thd_id(), time_prepare, timeprep);
//         double timediff = get_sys_clock() - cntime;

//         // Check if any Commit messages arrived before this BatchRequests message.
//         for (uint64_t i = 0; i < txn_man->info_commit.size(); i++)
//         {
//             uint64_t num_comm = txn_man->decr_commit_rsp_cnt();
//             if (num_comm == 0)
//             {
//                 txn_man->set_committed();
//                 break;
//             }
//         }

//         // If enough Commit messages have already arrived.
//         if (txn_man->is_committed())
//         {
// #if TIMER_ON
//             // End the timer for this client batch.
//             server_timer->endTimer(txn_man->hash);
// #endif
//             // Proceed to executing this batch of transactions.
//             send_execute_msg();

//             // End the commit counter.
//             INC_STATS(get_thd_id(), time_commit, get_sys_clock() - txn_man->txn_stats.time_start_commit - timediff);
//         }
//     }
//     else
//     {
//         // Although batch has not prepared, still some commit messages could have arrived.
//         for (uint64_t i = 0; i < txn_man->info_commit.size(); i++)
//         {
//             txn_man->decr_commit_rsp_cnt();
//         }
//     }

//     // Release this txn_man for other threads to use.
//     bool ready = txn_man->set_ready();
//     assert(ready);

//     // UnSetting the ready for the txn id representing this batch.
//     txn_man = get_transaction_manager(msg->txn_id, 0);
//     while (true)
//     {
//         bool ready = txn_man->unset_ready();
//         if (!ready)
//         {
//             continue;
//         }
//         else
//         {
//             break;
//         }
//     }

//     return RCOK;
// }

// /**
//  * Checks if the incoming PBFTPrepMessage can be accepted.
//  *
//  * This functions checks if the hash and view of the commit message matches that of 
//  * the Pre-Prepare message. Once 2f messages are received it returns a true and 
//  * sets the `is_prepared` flag for furtue identification.
//  *
//  * @param msg PBFTPrepMessage.
//  * @return bool True if the transactions of this batch are prepared.
//  */
// bool WorkerThread::prepared(PBFTPrepMessage *msg)
// {
//     //cout << "Inside PREPARED: " << txn_man->get_txn_id() << "\n";
//     //fflush(stdout);

//     // Once prepared is set, no processing for further messages.
//     if (txn_man->is_prepared())
//     {
//         return false;
//     }

//     // If BatchRequests messages has not arrived yet, then return false.
//     if (txn_man->get_hash().empty())
//     {
//         // Store the message.
//         txn_man->info_prepare.push_back(msg->return_node);
//         return false;
//     }
//     else
//     {
//         if (!checkMsg(msg))
//         {
//             // If message did not match.
//             cout << txn_man->get_hash() << " :: " << msg->hash << "\n";
//             cout << get_current_view(get_thd_id()) << " :: " << msg->view << "\n";
//             fflush(stdout);
//             return false;
//         }
//     }

//     uint64_t prep_cnt = txn_man->decr_prep_rsp_cnt();
//     if (prep_cnt == 0)
//     {
//         txn_man->set_prepared();
//         return true;
//     }

//     return false;
// }

// /**
//  * Processes incoming Prepare message.
//  *
//  * This functions precessing incoming messages of type PBFTPrepMessage. If a replica 
//  * received 2f identical Prepare messages from distinct replicas, then it creates 
//  * and sends a PBFTCommitMessage to all the other replicas.
//  *
//  * @param msg Prepare message of type PBFTPrepMessage from a replica.
//  * @return RC
//  */
// RC WorkerThread::process_pbft_prep_msg(Message *msg)
// {
//     //cout << "PBFTPrepMessage: TID: " << msg->txn_id << " FROM: " << msg->return_node_id << endl;
//     //fflush(stdout);

//     // Start the counter for prepare phase.
//     if (txn_man->prep_rsp_cnt == 2 * g_min_invalid_nodes)
//     {
//         txn_man->txn_stats.time_start_prepare = get_sys_clock();
//     }

//     // Check if the incoming message is valid.
//     PBFTPrepMessage *pmsg = (PBFTPrepMessage *)msg;
//     validate_msg(pmsg);

//     // Check if sufficient number of Prepare messages have arrived.
//     if (prepared(pmsg))
//     {
//         // Send Commit messages.
//         txn_man->send_pbft_commit_msgs();

//         // End the prepare counter.
//         INC_STATS(get_thd_id(), time_prepare, get_sys_clock() - txn_man->txn_stats.time_start_prepare);
//     }

//     return RCOK;
// }

// /**
//  * Checks if the incoming PBFTCommitMessage can be accepted.
//  *
//  * This functions checks if the hash and view of the commit message matches that of 
//  * the Pre-Prepare message. Once 2f+1 messages are received it returns a true and 
//  * sets the `is_committed` flag for furtue identification.
//  *
//  * @param msg PBFTCommitMessage.
//  * @return bool True if the transactions of this batch can be executed.
//  */
// bool WorkerThread::committed_local(PBFTCommitMessage *msg)
// {
//     //cout << "Check Commit: TID: " << txn_man->get_txn_id() << "\n";
//     //fflush(stdout);

//     // Once committed is set for this transaction, no further processing.
//     if (txn_man->is_committed())
//     {
//         return false;
//     }

//     // If BatchRequests messages has not arrived, then hash is empty; return false.
//     if (txn_man->get_hash().empty())
//     {
//         //cout << "hash empty: " << txn_man->get_txn_id() << "\n";
//         //fflush(stdout);
//         txn_man->info_commit.push_back(msg->return_node);
//         return false;
//     }
//     else
//     {
//         if (!checkMsg(msg))
//         {
//             // If message did not match.
//             //cout << txn_man->get_hash() << " :: " << msg->hash << "\n";
//             //cout << get_current_view(get_thd_id()) << " :: " << msg->view << "\n";
//             //fflush(stdout);
//             return false;
//         }
//     }

//     uint64_t comm_cnt = txn_man->decr_commit_rsp_cnt();
//     if (comm_cnt == 0 && txn_man->is_prepared())
//     {
//         txn_man->set_committed();
//         return true;
//     }

//     return false;
// }

// /**
//  * Processes incoming Commit message.
//  *
//  * This functions precessing incoming messages of type PBFTCommitMessage. If a replica 
//  * received 2f+1 identical Commit messages from distinct replicas, then it asks the 
//  * execute-thread to execute all the transactions in this batch.
//  *
//  * @param msg Commit message of type PBFTCommitMessage from a replica.
//  * @return RC
//  */
// RC WorkerThread::process_pbft_commit_msg(Message *msg)
// {
//     //cout << "PBFTCommitMessage: TID " << msg->txn_id << " FROM: " << msg->return_node_id << "\n";
//     //fflush(stdout);

//     if (txn_man->commit_rsp_cnt == 2 * g_min_invalid_nodes + 1)
//     {
//         txn_man->txn_stats.time_start_commit = get_sys_clock();
//     }

//     // Check if message is valid.
//     PBFTCommitMessage *pcmsg = (PBFTCommitMessage *)msg;
//     validate_msg(pcmsg);

//     txn_man->add_commit_msg(pcmsg);

//     // Check if sufficient number of Commit messages have arrived.
//     if (committed_local(pcmsg))
//     {
// #if TIMER_ON
//         // End the timer for this client batch.
//         server_timer->endTimer(txn_man->hash);
// #endif

//         // Add this message to execute thread's queue.
//         send_execute_msg();

//         INC_STATS(get_thd_id(), time_commit, get_sys_clock() - txn_man->txn_stats.time_start_commit);
//     }

//     return RCOK;
// }

#endif