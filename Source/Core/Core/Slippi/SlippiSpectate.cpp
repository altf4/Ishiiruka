#include "SlippiSpectate.h"
#include "Common/Logging/Log.h"
#include "Common/CommonTypes.h"
#include <Core/ConfigManager.h>
#include "base64.hpp"

// Networking
#ifdef _WIN32
#include <share.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#endif

SlippiSpectateServer* SlippiSpectateServer::getInstance()
{
    static SlippiSpectateServer instance; // Guaranteed to be destroyed.
                                    // Instantiated on first use.
    return &instance;
}

void SlippiSpectateServer::write(u8 *payload, u32 length)
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    u32 cursor = (u32)m_event_buffer.size();
    u64 offset = m_cursor_offset;
    m_event_buffer_mutex.unlock();

    // Make json wrapper for game event
    json game_event;
    game_event["type"] = "game_event";
    game_event["cursor"] = offset+cursor;
    game_event["next_cursor"] = offset+cursor+1;
    std::string str_payload((char*)payload, length);
    if(str_payload.empty())
    {
        game_event["payload"] = "";
    }
    else
    {
        game_event["payload"] = base64::Base64::Encode(str_payload);
    }
    std::string buffer = game_event.dump();

    // Put this message into the event buffer
    //  This will queue the message up to go out for all clients
    m_event_buffer_mutex.lock();
    m_event_buffer.push_back(buffer);
    m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::writeMenuEvent(u8 *payload, u32 length)
{
  if(!SConfig::GetInstance().m_slippiNetworkingOutput)
  {
      return;
  }

  // Reset the sent menu flag for each peer
  std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
  for(; it != m_sockets.end(); it++)
  {
      it->second->m_sent_menu = false;
  }

  json menu_event;
  std::string str_payload((char*)payload, length);
  menu_event["type"] = "menu_event";
  menu_event["payload"] = base64::Base64::Encode(str_payload);

  // Put this message into the menu event buffer
  //  This will queue the message up to go out for all clients
  m_event_buffer_mutex.lock();
  m_menu_event = menu_event.dump();
  m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::writeEvents(u16 peer_id)
{
    m_event_buffer_mutex.lock();
    bool in_game = m_in_game;
    m_event_buffer_mutex.unlock();

    // Send menu events
    if(!in_game && !m_sockets[peer_id]->m_sent_menu)
    {
        m_event_buffer_mutex.lock();
        ENetPacket *packet = enet_packet_create(m_menu_event.data(),
                                                m_menu_event.length(),
                                                ENET_PACKET_FLAG_RELIABLE);
        // Batch for sending
        enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
        // Record for the peer that it was sent
        m_sockets[peer_id]->m_sent_menu = true;
        m_event_buffer_mutex.unlock();
    }

    // Send game events

    // Get the cursor for this socket
    u64 cursor = m_sockets[peer_id]->m_cursor;

    // Loop through each event that needs to be sent
    //  send all the events starting at their cursor
    m_event_buffer_mutex.lock();
    for(u64 i = cursor; i < m_event_buffer.size(); i++)
    {
        ENetPacket *packet = enet_packet_create(m_event_buffer[i].data(),
                                                m_event_buffer[i].size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        // Batch for sending
        enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
        m_sockets[peer_id]->m_cursor++;
    }
    m_event_buffer_mutex.unlock();
}

// We assume, for the sake of simplicity, that all clients have finished reading
//  from the previous game event buffer by now. At least many seconds will have passed
//  by now, so if a listener is still stuck getting events from the last game,
//  they will get erroneous data.
void SlippiSpectateServer::startGame()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    m_event_buffer.clear();
    m_in_game = true;

    // TODO m_sockets is probably unsafe here :/
    std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    for(; it != m_sockets.end(); it++)
    {
        it->second->m_cursor = 0;
    }

    m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::endGame()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    if(m_event_buffer.size() > 0)
    {
        m_cursor_offset += m_event_buffer.size();
    }
    m_menu_event.clear();
    m_in_game = false;
    m_event_buffer_mutex.unlock();
}

SlippiSpectateServer::SlippiSpectateServer()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    // Init some timestamps
    m_last_broadcast_time = std::chrono::system_clock::now();

    // Spawn thread for socket listener
    m_stop_socket_thread = false;
    m_socketThread = std::thread(&SlippiSpectateServer::SlippicommSocketThread, this);
}

SlippiSpectateServer::~SlippiSpectateServer()
{
    // The socket thread will be blocked waiting for input
    // So to wake it up, let's connect to the socket!
    m_stop_socket_thread = true;
    if (m_socketThread.joinable())
    {
        m_socketThread.join();
    }
}

void SlippiSpectateServer::writeBroadcast()
{
    sendto(m_broadcast_socket, (char*)&m_broadcast_message, sizeof(m_broadcast_message), 0,
        (struct sockaddr *)&m_broadcastAddr, sizeof(m_broadcastAddr));

    m_last_broadcast_time = std::chrono::system_clock::now();
}

void SlippiSpectateServer::handleMessage(u8 *buffer, u32 length, u16 peer_id)
{
    // Unpack the message
    std::string message((char*)buffer, length);
    json json_message = json::parse(message);
    if(!json_message.is_discarded() && (json_message.find("type") != json_message.end()))
    {
        // Check what type of message this is
        if(!json_message["type"].is_string())
        {
            return;
        }

        if(json_message["type"] == "connect_request")
        {
            // Get the requested cursor
            if(json_message.find("cursor") == json_message.end())
            {
                return;
            }
            if(!json_message["cursor"].is_number_integer())
            {
                return;
            }
            u32 requested_cursor = json_message["cursor"];
            u32 sent_cursor = 0;
            // Set the user's cursor position
            m_event_buffer_mutex.lock();
            if(requested_cursor >= m_cursor_offset)
            {
                // If the requested cursor is past what events we even have, then just tell them to start over
                if(requested_cursor > m_event_buffer.size() + m_cursor_offset)
                {
                    m_sockets[peer_id]->m_cursor = 0;
                }
                // Requested cursor is in the middle of a live match, events that we have
                else
                {
                    m_sockets[peer_id]->m_cursor = requested_cursor - m_cursor_offset;
                }
            }
            else
            {
                // The client requested a cursor that was too low. Bring them up to the present
                m_sockets[peer_id]->m_cursor = 0;
            }

            sent_cursor = (u32)m_sockets[peer_id]->m_cursor + (u32)m_cursor_offset;

            // If someone joins while at the menu, don't catch them up
            //  set their cursor to the end
            if(!m_in_game)
            {
                m_sockets[peer_id]->m_cursor = m_event_buffer.size();
            }

            m_event_buffer_mutex.unlock();

            json reply;
            reply["type"] = "connect_reply";
            reply["nick"] = SConfig::GetInstance().m_slippiConsoleName;
            reply["version"] = "1.9.0-dev-2";
            reply["cursor"] = sent_cursor;

            std::string packet_buffer = reply.dump();

            ENetPacket *packet = enet_packet_create(packet_buffer.data(),
                          (u32)packet_buffer.length(),
                          ENET_PACKET_FLAG_RELIABLE);

            // Batch for sending
            enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
            // Put the client in the right in_game state
            m_sockets[peer_id]->m_shook_hands = true;
        }
    }
}

void SlippiSpectateServer::sendHolePunchMsg(ENetHost *host, std::string remoteIp, u16 remotePort)
{
  	ENetAddress addr;
  	enet_address_set_host(&addr, remoteIp.c_str());
  	addr.port = remotePort;

  	auto server = enet_host_connect(host, &addr, 3, 0);
  	if (server == nullptr)
  	{
    		// Failed to connect to server
    		return;
  	}

  	enet_host_flush(host);
  	enet_peer_reset(server);
}

void SlippiSpectateServer::SlippicommSocketThread(void)
{
    // Setup the broadcast advertisement message and socket
    m_broadcast_socket = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable=1;
    if(setsockopt(m_broadcast_socket, SOL_SOCKET, SO_BROADCAST,
       (char*)&broadcastEnable, sizeof(broadcastEnable)))
    {
        WARN_LOG(SLIPPI, "Failed configuring Slippi braodcast socket");
        return;
    }
    memset(&m_broadcastAddr, 0, sizeof(m_broadcastAddr));
    m_broadcastAddr.sin_family = AF_INET;
    m_broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    m_broadcastAddr.sin_port = htons(20582);

    // Setup the broadcast message
    //  It never changes, so let's just do this once
    const char* nickname = SConfig::GetInstance().m_slippiConsoleName.c_str();
    char cmd[] = "SLIP_READY";

    memcpy(m_broadcast_message.cmd, cmd, sizeof(m_broadcast_message.cmd));
    memset(m_broadcast_message.mac_addr, 0, sizeof(m_broadcast_message.mac_addr));
    strncpy(m_broadcast_message.nickname, nickname, sizeof(m_broadcast_message.nickname));

    if (enet_initialize () != 0) {
        // TODO replace all printfs with logs
        printf("An error occurred while initializing ENet.\n");
        return;
    }

    ENetAddress server_address = {0};
    server_address.host = ENET_HOST_ANY;
    server_address.port = SLIPPI_PORT;

    /* create a server */
    ENetHost *server = enet_host_create(&server_address, MAX_CLIENTS, 2, 0, 0);

    if (server == NULL) {
        printf("An error occurred while trying to create an ENet server host.\n");
        return;
    }

    printf("Started a server...\n");
    // Main slippicomm server loop
    while(1)
    {
        // If we're told to stop, then quit
        if(m_stop_socket_thread)
        {
            enet_host_destroy(server);
            enet_deinitialize();
            return;
        }

        std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
        for(; it != m_sockets.end(); it++)
        {
            if(it->second->m_shook_hands)
            {
                writeEvents(it->first);
            }
        }

        // Write any broadcast messages if we haven't in two seconds
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        if(now - std::chrono::seconds(2) > m_last_broadcast_time)
        {
            writeBroadcast();
            // Also take this time to punch a connection out to the spectator
            if(!SConfig::GetInstance().m_spectator_IP.empty())
            {
                // TODO don't send this hole punch if we're already connected to that machine
                //  look that in the sockets somehow
                sendHolePunchMsg(server,
                  SConfig::GetInstance().m_spectator_IP,
                  SConfig::GetInstance().m_spectator_port);
            }
        }

        ENetEvent event;
        while (enet_host_service (server, &event, 1) > 0)
        {
            switch (event.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    printf ("A new client connected from %x:%u.\n",
                            event.peer -> address.host,
                            event.peer -> address.port);
                    std::shared_ptr<SlippiSocket> newSlippiSocket(new SlippiSocket());
                    newSlippiSocket->m_peer = event.peer;
                    m_sockets[event.peer->outgoingPeerID] = newSlippiSocket;

                    // New incoming client connection
                    //  I don't think there's any special logic that we need to do here
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    handleMessage(event.packet->data, (u32)event.packet->dataLength, event.peer->outgoingPeerID);
                    /* Clean up the packet now that we're done using it. */
                    enet_packet_destroy (event.packet);

                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    printf ("%s disconnected.\n", (char*)event.peer -> data);
                    /* Reset the peer's client information. */
                    event.peer -> data = NULL;
                    break;
                }
                default:
                {
                    printf ("%s Unknown message came in.\n", (char*)event.peer -> data);
                    break;
                }
            }
        }
    }

    enet_host_destroy(server);
    enet_deinitialize();
}
