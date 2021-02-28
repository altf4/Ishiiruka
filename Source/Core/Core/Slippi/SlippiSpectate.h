#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <thread>

#include "Common/FifoQueue.h"
#include "nlohmann/json.hpp"
#include <enet/enet.h>
using json = nlohmann::json;

#define _WEBSOCKETPP_NO_EXCEPTIONS_

#include <iostream>
#include <set>

/*#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>*/
#include <websocketpp/config/asio_no_tls.hpp>
// #include <websocketpp/config/core.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/common/thread.hpp>

using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

using websocketpp::lib::thread;
using websocketpp::lib::mutex;
using websocketpp::lib::lock_guard;
using websocketpp::lib::unique_lock;
using websocketpp::lib::condition_variable;

// Sockets in windows are unsigned
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
typedef int SOCKET;
#endif

#define MAX_CLIENTS 4

#define HANDSHAKE_MSG_BUF_SIZE 128
#define HANDSHAKE_TYPE 1
#define PAYLOAD_TYPE 2
#define KEEPALIVE_TYPE 3
#define MENU_TYPE 4

typedef websocketpp::server<websocketpp::config::asio> server;

enum action_type {
    SUBSCRIBE,
    UNSUBSCRIBE,
    MESSAGE
};

struct action {
    action(action_type t, websocketpp::connection_hdl h) : type(t), hdl(h) {}
    action(action_type t, websocketpp::connection_hdl h, server::message_ptr m)
      : type(t), hdl(h), msg(m) {}

    action_type type;
    websocketpp::connection_hdl hdl;
    server::message_ptr msg;
};

class SlippiSocket
{
  public:
	u64 m_cursor = 0;           // Index of the last game event this client sent
	u64 m_menu_cursor = 0;      // The latest menu event that this socket has sent
	bool m_shook_hands = false; // Has this client shaken hands yet?
	ENetPeer *m_peer = NULL;    // The ENet peer object for the socket
};

class SlippiSpectateServer
{
  public:
	// Singleton. Get an instance of the class here
	//   When SConfig::GetInstance().m_slippiNetworkingOutput is false, this
	//  instance exists and is callable, but does nothing
	static SlippiSpectateServer *getInstance();

	// Write the given game payload data to all listening sockets
	void write(u8 *payload, u32 length);

	// Should be called each time a new game starts.
	//  This will clear out the old game event buffer and start a new one
	void startGame();

	// Clear the game event history buffer. Such as when a game ends.
	//  The slippi server keeps a history of events in a buffer. So that
	//  when a new client connects to the server mid-match, it can recieve all
	//  the game events that have happened so far. This buffer needs to be
	//  cleared when a match ends.
	void endGame();

	// Don't try to copy the class. Delete those functions
	SlippiSpectateServer(SlippiSpectateServer const &) = delete;
	void operator=(SlippiSpectateServer const &) = delete;

  private:
	// ACCESSED FROM BOTH DOLPHIN AND SERVER THREADS
	// This is a lockless queue that bridges the gap between the main
	//  dolphin thread and the spectator server thread. The purpose here
	//  is to avoid blocking (even if just for a brief mutex) on the main
	//  dolphin thread.
	Common::FifoQueue<std::string> m_event_queue;
	// Bool gets flipped by the destrctor to tell the server thread to shut down
	//  bools are probably atomic by default, but just for safety...
	std::atomic<bool> m_stop_socket_thread;

	// ONLY ACCESSED FROM SERVER THREAD
	bool m_in_game;
	std::map<u16, std::shared_ptr<SlippiSocket>> m_sockets;
	std::string m_event_concat = "";
	std::vector<std::string> m_event_buffer;
	std::string m_menu_event;
	// In order to emulate Wii behavior, the cursor position should be strictly
	//  increasing. But internally, we need to index arrays by the cursor value.
	//  To solve this, we keep an "offset" value that is added to all outgoing
	//  cursor positions to give the appearance like it's going up
	u64 m_cursor_offset = 0;
	//  How many menu events have we sent so far? (Reset between matches)
	//    Is used to know when a client hasn't been sent a menu event
	u64 m_menu_cursor;

	std::thread m_socketThread;

	// Private constructor to avoid making another instance
	SlippiSpectateServer();
	~SlippiSpectateServer();

	// FUNCTIONS CALLED ONLY FROM SERVER THREAD
	// Server thread. Accepts new incoming connections and goes back to sleep
	void SlippicommSocketThread(void);
	// Handle an incoming message on a socket
	void handleMessage(u8 *buffer, u32 length, u16 peer_id);
	// Catch up given socket to the latest events
	//  Does nothing if they're already caught up.
	void writeEvents(u16 peer_id);
	// Pop events
	void popEvents();
  void on_open(connection_hdl hdl);
  void on_close(connection_hdl hdl);
  void on_message(connection_hdl hdl, server::message_ptr msg);


  typedef std::set<websocketpp::connection_hdl,std::owner_less<websocketpp::connection_hdl> > con_list;

  server m_server;
  con_list m_connections;
  std::queue<action> m_actions;

  mutex m_action_lock;
  mutex m_connection_lock;
  condition_variable m_action_cond;

};
