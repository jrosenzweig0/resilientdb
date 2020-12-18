## Attention: This is a Fork of [ResilientDB](https://github.com/resilientdb/resilientdb)
For the purposes of our ECS265 Final Project

# ECS265 CAPsized Final Project: Raft in ResilientDB
## Lead: Jonathan Rosenzweig 
## Members: Dan Hoang, David Pham, Di Zhao, Ryan Tsang

This project aims to implement the Raft consensus protocol using the ResilientDB platform. Currently, ResilientDB reaches local consensus using PBFT. Our group intends to modify the existing codebase to use Raft instead of PBFT

## Implementation Notes

- We want to change the preprepare stage of pbft so that instead of communicating with each other, worker nodes only communicate with the leader. 

- leader id is located in global.cpp, saved as local_view, initialized to be 0 

- process_client_batch receives client request and forwards pbft pre-prepare message, we edit this so it sends append_entries message instead

- change process_batch to append_entries 
- send_pbft_prep_msgs -> append_entries_response

- the ledger is the BlockChain class (`chain.h`)
- the txn_table is the pool of all the active txns that are on the node
    - get_transaction_manager creates a new one if not already present

- append_entries behavior changes depending on if leader or worker

- in worker_append_entries:
	- first check commit index and commit/execute and outstanding transactions


worker node:
txn table - pool of all active transaction managers on the node
    each txn manager handles a single transaction. it also sends the pbft messages in the msg queue
    how does it copy the txn data and send it?

**12/5**
- using process_client_batch to respond to client requests
- want to use `create_and_send_batchreq` from `worker_thread.cpp` to invoke the `append_entries` rpc by editing `algorithm_specific_update` to include the necessary parameters to invoke function call.
- need to define some global variables for the node's state, currentTerm, (confirm that local_view represents the current leader), find a way to get commitIndex from the local blockchain, and the lastApplied.
- need to add the term variable to the blocks on the blockchain
- the commit index represents the true end of the blockchain, anthing after may not stay on the chain

**12/6**
- I think these are the state variables:
    - currentTerm (new)
    - local_view[]                  votedFor (current leader/primary) (dunno why it's an array)
    - (?) g_last_stable_chkpt       commitIndex (unless g_next_index is 0, need to handle that)
    - (?) g_next_index - 1          lastApplied

    - nextIndex[]
    - matchIndex[]


- local_view in global.cpp keeps track of the current view of each thread. why?
- commitIndex is g_next_index in global.cpp
- worker threads assume they themselves are the primary if VIEW_CHANGE is not invoked and will act as such if they receive a client request?
- I need to add the append_entries rpc arguments to the BatchRequests message subclass, since that's what will be getting sent to a node (with a list of YSBCQueryMessage? objects)

- leaderCommit is leader commit index comes from curr_next_index
- entries[] is the YSBCQueryMessage
    - wait, this can't work, entries might need to have previous batches in it if a node needs updating...

- I do need to create an append_entries_rpc message subclass after all? i need to figure out how transactions are stored on the blockchain so they can be forwarded to nodes that are behind primary if necessary. ugh.

**12/8**
- blockchain stored batchrequests on each block
- let's treat the batchrequest as the transaction itself

- ~~**!!! is the blockchain the same as the log or the state machine!! i think it's actually the state machine...**~~ It's the same thing. the state machine is the whole system...

- todo:
    - add term variable to blockchain
    - rewrite process_client_batch, instead of immediately sending, it should append the batchrequest to a log without execution that will get forwarded during next heartbeat
    - write append entries, sends multiple batches at a time
    - modify create_and_send_batch to create TxnManagers without sending
    - modify process_client_batch to process batches without sending prepare message but only responding to primary

**12/9**
- updating batchrequests message subclass to be used with Raft (term variable)
    - need to update all helper functions

- todo:
    - finish writing AppendEntriesRPC message subclass
    - write AppendEntriesResp message subclass
    - write append_entries
    - write process_append_entries
    - write process_append_entries_resp

**12/10**
- make sure that the batchrequests term field is initialized on client request
- docker 3.0.0 is [broken](https://github.com/docker/for-mac/issues/5115)

#### Changelog

`global`:
- (12/6) added currentTerm and helper functions
- (12/8) added commitIndex and helper functions
- (12/8) added lastApplied and helper functions
- (12/8) added nextIndex[] and helper functions
- (12/8) added matchIndex[] and helper functions
- (12/11) more helper functions

`chain`
- (12/8) added additional helper functions for accessing the blockchain log
- (12/9) added term and helper functions for it
- (12/11) more helper functions

`messages`:
- (12/5) ~~add the AppendEntriesRPC message subclass~~
- (12/5) ~~add AppendEntriesResponse~~ 
- (12/9) added AppendEntriesRPC message subclass, started helper functions
- (12/10) continuing to fill out helper functions for AppendEntriesRPC
- (12/17) added matchIndex to AppendEntriesResponse (primary adds to its blockchain too quickly)

`worker_thread`:
- (12/4) added macros to toggle pbft and raft

`worker_thread_pbft`:
- (12/4) moved all pbft functions here, added macros to toggle if pbft

`worker_thread_raft`:
- (12/4) created to replace `worker_thread_pbft` when RAFT invoked. currently a copy of `worker_thread_pbft`
- (12/11) most of append_entries, process_append_entries, and process_append_entries_response done. tweaked process_client_batch

`msg_queue`: 
- (12/10) updated msq_queue to send AppendEntriesRPCs and AppendEntriesResponse

#### Useful Commands

`docker exec -it <repilica id> bash`        starts a bash in docker replica (eg s1, c1, etc.)

In container:

`./rundb -nid#`                 runs replica with nid # (eg nid0, nid1, etc) needs to match format
                                s1 -> nid0, s<X> -> nid<X>, c1 -> nid<X+1>
`./runcl -nid#`                 runs client with nid #, needs to match format and be in correct 
                                container

gdb
`gdb --args ./rundb nid0`
`(gdb) b <function name>`
`(gdb) run -nid0`
`(gdb) layout src`      shows where you are in code
`(gdb) p <var>`         prints var
`(gdb) c`               continue until next breakpoint
`(gdb) n`               step over
`(gdb) s`               step in


#### Notes from [Sajjad's ResilientDB Tutorial](https://www.youtube.com/watch?v=cBn142Uz_J0&feature=youtu.be)
Everything we need is probably in `system` and `client` folders


---
# ResilientDB: A High-throughput yielding Permissioned Blockchain Fabric.

### ResilientDB aims at *Making Permissioned Blockchain Systems Fast Again*. ResilientDB makes *system-centric* design decisions by adopting a *multi-thread architecture* that encompasses *deep-pipelines*. Further, we *separate* the ordering of client transactions from their execution, which allows us to perform *out-of-order processing of messages*.

### Quick Facts about Version 2.0 of ResilientDB
1. ResilientDB supports a **Dockerized** implementation, which allows specifying the number of clients and replicas.
2. **PBFT** [Castro and Liskov, 1998] protocol is used to achieve consensus among the replicas.
3. ResilientDB expects minimum **3f+1** replicas, where **f** is the maximum number of byzantine (or malicious) replicas.
4. ReslientDB designates one of its replicas as the **primary** (replicas with identifier **0**), which is also responsible for initiating the consensus.
5. At present, each client only sends YCSB-style transactions for processing, to the primary.
6. Each client transaction has an associated **transaction manager**, which stores all the data related to the transaction.
7. Depending on the type of replica (primary or non-primary), we associate different a number of threads and queues with each replica.
8. ResilientDB allows easy implementation of **Smart Contracts**. At present, we provide a comprehensive implementation of **Banking Smart Contracts**.
9. To facilitate data storage and persistence, ResilientDB provides support for an **in-memory key-value store**. Further, users can take advantage of **SQL query** execution through the fully-integrated APIs for **SQLite**.
10. With ResilientDB we also provide a seamless **GUI display**. This display generates a status log and also accesses **Grafana to plot the results**. Further details regarding the setup of GUI display are available in the **dashboard** folder.

---

### Steps to Run and Compile through Docker
First, install docker and docker-compose:

- [Install Docker-CE](https://docs.docker.com/install/)
- [Install Docker-Compose](https://docs.docker.com/compose/install/)


Use the Script ``resilientDB-docker``

    Usage:
     ./resilientDB-docker --clients=1 --replicas=4
     ./resilientDB-docker -d [default 4 replicas and 1 client]

## Result
-   The result will be printed on STDOUT and also ``res.out`` file. It contains the Throughputs and Latencies for the run and summary of each thread in replicas.
## warning:
-   Using docker, all replicas and clients will be running on one machine as containers, so a large number of replicas would degrade the performance of your system

---

## Steps to Run and Compile without Docker <br/>

#### We strongly recommend that first try the docker version, Here are the steps to run on a real environment:

* First Step is to untar the dependencies:

        cd deps && \ls | xargs -i tar -xvf {} && cd ..
* Create **obj** folder inside **resilientdb** folder, to store object files. And **results** to store the results.

        mkdir obj
        mkdir results
* Create a folder named **results** inside **resilientdb** to store the results.
        
* We provide a script **startResilientDB.sh** to compile and run the code. To run **ResilientDB** on a cluster such as AWS, Azure or Google Cloud, you need to specify the **Private IP Addresses** of each replica. 
* The code will be compiled on the machine that is running the **startResilientDB.sh** and send the binary files over the SSH to the **resilientdb** folder in all other  nodes. the directory which contains the **resilientdb** in nodes should be set as ``home_directory`` in following files as :
    1. scripts/scp_binaries.sh
    2. scripts/scp_results.sh
    3. scripts/simRun.py
* **change the ``CNODES`` and ``SNODES`` arrays in ``scripts/startResilientDB.sh`` and put IP Addresses.**
* Adjust the parameters in ``config.h`` such as number of replicas and clients
* Run script as: **./scripts/startResilientDB.sh \<number of servers\> \<number of clients\> \<batch size\>**

* All the results after running the script will be stored inside the **results** folder.


#### What is happening behind the scenes?

* The code is compiled using command: **make clean; make**
* On compilation, two new files are created: **runcl** and **rundb**.
* Each machine is going to act as a client needs to execute **runcl**.
* Each machine is going to act as a replica needs to execute **rundb**. 
* The script runs each binary as: **./rundb -nid\<numeric identifier\>**
* This numeric identifier starts from **0** (for the primary) and increases as **1,2,3...** for subsequent replicas and clients.



---


### Relevant Parameters of "config.h"
<pre>
* NODE_CNT			Total number of replicas, minimum 4, that is, f=1.  
* THREAD_CNT			Total number of threads at primary (at least 5)
* REM_THREAD_CNT		Total number of input threads at a replica (set it to 3)
* SEND_THREAD_CNT		Total number of output threads at a replica (at least 1)
* CLIENT_NODE_CNT		Total number of clients (at least 1).  
* CLIENT_THREAD_CNT		Total number of threads at a client (at least 1)
* CLIENT_REM_THREAD_CNT		Total number of input threads at a client (set it to 1)
* SEND_THREAD_CNT		Total number of output threads at a client (set it to 1)
* MAX_TXN_IN_FLIGHT		Multiple of Batch Size
* DONE_TIMER			Amount of time to run the system.
* WARMUP_TIMER			Amount of time to warmup the system (No statistics collected).
* BATCH_THREADS			Number of threads at primary to batch client transactions.
* BATCH_SIZE			Number of transactions in a batch (at least 10)
* ENABLE_CHAIN			Set it to true if blocks need to be stored in a ledger.
* TXN_PER_CHKPT			Frequency at which garbage collection is done.
* USE_CRYPTO			To switch on and off cryptographic signing of messages.
* CRYPTO_METHOD_RSA		To use RSA based digital signatures.
* CRYPTO_METHOD_ED25519		To use ED25519 based digital signatures.
* CRYPTO_METHOD_CMAC_AES	To use CMAC + AES combination for authentication.
* SYNTH_TABLE_SIZE		The range of keys for clients to select.
* EXT_DB MEMORY			To specify the type of memory storage (in-memory of SQLite)..
* BANKING_SMART_CONTRACT	To allow usage of smart contraacts instead of YCSB bechmarks.



</pre>

<br/>

---

* There are several other parameters in *config.h*, which are unusable (or not fully tested) in the current version.


