#include "message.h"
#include <mutex>
#include <random>

#if TIMER_ON

class Timer
{
	uint64_t timestamp;
	string hash;
	Message *msg;

public:
	uint64_t get_timestamp();
	string get_hash();
	Message *get_msg();
	void set_data(uint64_t tst, string hsh, Message *cqry);
};

/********** Raft Timers **********/

// Timer for Primary/Leader (to send AppendEntries)
class LeaderTimer {
	Timer *timer;

	// heartbeat period
	uint64_t period;
	// bool timer_state;

public:
	void init();		// always call once before using
	void startTimer(); // calling startTimer will reset the timer
	bool checkTimer();
	void endTimer();
};

// Timer for ElectionTimeout on Followers
class ElectionTimer {
	// random
	std::random_device rd;
	std::mt19937_64 mt;
	std::uniform_int_distribution<uint64_t> dist;

	Timer *timer;

	// timeout period, randomized on start
	uint64_t timeout;

public:
	ElectionTimer();
	void init();
	void startTimer(); // calling startTimer will reset the timer
	bool checkTimer();
	void endTimer();
};

/*********************************/

// Timer for servers
class ServerTimer
{
	// Stores time of arrival for each transaction.
	std::vector<Timer *> txn_map;
	bool timer_state;

public:
	void startTimer(string digest, Message *clqry);
	void endTimer(string digest);
	bool checkTimer();
	void pauseTimer();
	void resumeTimer();
	Timer *fetchPendingRequests(uint64_t idx);
	uint64_t timerSize();
	void removeAllTimers();
};

// Timer for clients.
class ClientTimer
{
	// Stores time of arrival for each transaction.
	std::vector<Timer *> txn_map;

public:
	void startTimer(uint64_t timestp, ClientQueryBatch *cqry);
	void endTimer(uint64_t timestp);
	bool checkTimer(ClientQueryBatch *&cbatch);
	Timer *fetchPendingRequests();
	void removeAllTimers();
};

/************************************/

extern LeaderTimer *leader_timer;
extern ElectionTimer *election_timer;

extern ServerTimer *server_timer;
extern ClientTimer *client_timer;
extern std::mutex tlock;

#endif // TIMER_ON
