#pragma once

#include <map>
#include <chrono>
#include <thread>
#include <mutex>

#include <enet/enet.h>
#include "slippicomm.pb.h"

// Sockets in windows are unsigned
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
typedef int SOCKET;
#endif

#define MAX_CLIENTS 4
#define SLIPPI_PORT 51441

#define HANDSHAKE_MSG_BUF_SIZE 128
#define HANDSHAKE_TYPE 1
#define PAYLOAD_TYPE 2
#define KEEPALIVE_TYPE 3
#define MENU_TYPE 4

// Actual socket value is not here since that's the key of the map
class SlippiSocket
{
public:
    u64 m_cursor = 0;
    bool m_sent_menu = false;
    bool m_shook_hands = false;
    ENetPeer *m_peer = NULL;
};

class SlippicommServer
{
public:
    // Singleton. Get an instance of the class here
    //   When SConfig::GetInstance().m_slippiNetworkingOutput is false, this
    //  instance exists and is callable, but does nothing
    static SlippicommServer* getInstance();

    // Write the given game payload data to all listening sockets
    void write(u8 *payload, u32 length);

    // Write a menu state payload to all listening sockets
    void writeMenuEvent(u8 *payload, u32 length);

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
    SlippicommServer(SlippicommServer const&) = delete;
    void operator=(SlippicommServer const&)  = delete;

  private:
    std::map<u16, std::shared_ptr<SlippiSocket>> m_sockets;
    bool m_stop_socket_thread;
    std::vector<std::string> m_event_buffer;
    std::string m_menu_event;
    std::mutex m_event_buffer_mutex;

    std::thread m_socketThread;
    SOCKET m_server_fd;
    std::chrono::system_clock::time_point m_last_broadcast_time;
    std::string m_broadcast_message;
    SOCKET m_broadcast_socket;
    struct sockaddr_in m_broadcastAddr;
    // In order to emulate Wii behavior, the cursor position should be strictly
    //  increasing. But internally, we need to index arrays by the cursor value.
    //  To solve this, we keep an "offset" value that is added to all outgoing
    //  cursor positions to give the appearance like it's going up
    u64 m_cursor_offset = 0;
    // Keep track of what the current state of the emulator is. Are we in the middle
    //  of a game or not?
    bool m_in_game = false;

    // Private constructor to avoid making another instance
    SlippicommServer();
    ~SlippicommServer();

    // Server thread. Accepts new incoming connections and goes back to sleep
    void SlippicommSocketThread(void);
    // Handle an incoming message on a socket
    void handleMessage(u8 *buffer, u32 length, u16 peer_id);
    // Send keepalive messages to all clients
    void writeKeepalives();
    // Send broadcast advertisement of the slippi server
    void writeBroadcast();
    // Catch up given socket to the latest events
    //  Does nothing if they're already caught up.
    void writeEvents(u16 peer_id);
};
