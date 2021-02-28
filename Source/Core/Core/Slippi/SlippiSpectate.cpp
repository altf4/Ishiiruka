#include "SlippiSpectate.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "base64.hpp"
#include <Core/ConfigManager.h>

// Networking
#ifdef _WIN32
#include <share.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#endif

// Disable boost exceptions
#define BOOST_NO_EXCEPTIONS
#include <boost/throw_exception.hpp>
void boost::throw_exception(std::exception const & e){
//do nothing
}

// CALLED FROM DOLPHIN MAIN THREAD
SlippiSpectateServer *SlippiSpectateServer::getInstance()
{
	static SlippiSpectateServer instance; // Guaranteed to be destroyed.
	                                      // Instantiated on first use.
	return &instance;
}

// CALLED FROM DOLPHIN MAIN THREAD
void SlippiSpectateServer::write(u8 *payload, u32 length)
{
	if (!SConfig::GetInstance().m_enableSpectator)
	{
		return;
	}
	std::string str_payload((char *)payload, length);
	m_event_queue.Push(str_payload);
}

// CALLED FROM DOLPHIN MAIN THREAD
void SlippiSpectateServer::startGame()
{
	if (!SConfig::GetInstance().m_enableSpectator)
	{
		return;
	}
	m_event_queue.Push("START_GAME");
}

// CALLED FROM DOLPHIN MAIN THREAD
void SlippiSpectateServer::endGame()
{
	if (!SConfig::GetInstance().m_enableSpectator)
	{
		return;
	}
	m_event_queue.Push("END_GAME");
}

// CALLED FROM SERVER THREAD
void SlippiSpectateServer::writeEvents(u16 peer_id)
{
	// Send menu events
	if (!m_in_game && (m_sockets[peer_id]->m_menu_cursor != m_menu_cursor))
	{
		ENetPacket *packet = enet_packet_create(m_menu_event.data(), m_menu_event.length(), ENET_PACKET_FLAG_RELIABLE);
		// Batch for sending
		enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
		// Record for the peer that it was sent
		m_sockets[peer_id]->m_menu_cursor = m_menu_cursor;
	}

	// Send game events
	// Loop through each event that needs to be sent
	//  send all the events starting at their cursor
	// If the client's cursor is beyond the end of the event buffer, then
	//  it's probably left over from an old game. (Or is invalid anyway)
	//  So reset it back to 0
	if (m_sockets[peer_id]->m_cursor > m_event_buffer.size())
	{
		m_sockets[peer_id]->m_cursor = 0;
	}

	for (u64 i = m_sockets[peer_id]->m_cursor; i < m_event_buffer.size(); i++)
	{
		ENetPacket *packet =
		    enet_packet_create(m_event_buffer[i].data(), m_event_buffer[i].size(), ENET_PACKET_FLAG_RELIABLE);
		// Batch for sending
		enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
		m_sockets[peer_id]->m_cursor++;
	}
}

// CALLED FROM SERVER THREAD
void SlippiSpectateServer::popEvents()
{
	// Loop through the event queue and keep popping off events and handling them
	while (!m_event_queue.Empty())
	{
		std::string event;
		m_event_queue.Pop(event);
		// These two are meta-events, used to signify the start/end of a game
		//  They are not sent over the wire
		if (event == "END_GAME")
		{
			m_menu_cursor = 0;
			if (m_event_buffer.size() > 0)
			{
				m_cursor_offset += m_event_buffer.size();
			}
			m_menu_event.clear();
			m_in_game = false;
			continue;
		}
		if (event == "START_GAME")
		{
			m_event_buffer.clear();
			m_in_game = true;
			continue;
		}

		// Make json wrapper for game event
		json game_event;

		// An SLP event with an empty payload is a quasi-event that signifies
		//  the unclean exit of a game. Send this out as its own event
		//  (Since you can't meaningfully concat it with other events)
		if (event.empty())
		{
			game_event["payload"] = "";
			game_event["type"] = "game_event";
			m_event_buffer.push_back(game_event.dump());
			continue;
		}

		if (!m_in_game)
		{
			game_event["payload"] = base64::Base64::Encode(event);
			m_menu_cursor += 1;
			game_event["type"] = "menu_event";
			m_menu_event = game_event.dump();
			continue;
		}

		u8 command = (u8)event[0];
		m_event_concat = m_event_concat + event;

		static std::unordered_map<u8, bool> sendEvents = {
		    {0x36, true}, // GAME_INIT
		    {0x3C, true}, // FRAME_END
		    {0x39, true}, // GAME_END
		    {0x10, true}, // SPLIT_MESSAGE
		};

		if (sendEvents.count(command))
		{
			u32 cursor = (u32)(m_event_buffer.size() + m_cursor_offset);
			game_event["payload"] = base64::Base64::Encode(m_event_concat);
			game_event["type"] = "game_event";
			game_event["cursor"] = cursor;
			game_event["next_cursor"] = cursor + 1;
			m_event_buffer.push_back(game_event.dump());

			m_event_concat = "";
		}
	}
}

// CALLED ONCE EVER, DOLPHIN MAIN THREAD
SlippiSpectateServer::SlippiSpectateServer()
{
	std::cout << "SlippiSpectateServer" << std::endl;

	if (!SConfig::GetInstance().m_enableSpectator)
	{
		std::cout << "disabled" << std::endl;
		return;
	}

	m_in_game = false;
	m_menu_cursor = 0;

	// Initialize Asio Transport
	websocketpp::lib::error_code error_code;
	m_server.init_asio(error_code);
	if (error_code) {
		//TODO
		std::cout << "init error " << error_code << std::endl;
		return;
	}
	// Register handler callbacks
	m_server.set_open_handler(bind(&SlippiSpectateServer::on_open,this,::_1));
	m_server.set_close_handler(bind(&SlippiSpectateServer::on_close,this,::_1));
	m_server.set_message_handler(bind(&SlippiSpectateServer::on_message,this,::_1,::_2));

	// Start a thread to run the processing loop
	m_server_thread = thread(bind(&SlippiSpectateServer::SlippicommSocketThread, this));

	// t.join();

	// Spawn thread for socket listener
	m_stop_socket_thread = false;
	m_acceptThread = std::thread(&SlippiSpectateServer::SlippicommAcceptThread, this);
	std::cout << "return constructor" << std::endl;
}

// CALLED FROM DOLPHIN MAIN THREAD
SlippiSpectateServer::~SlippiSpectateServer()
{
	// Stop the server
	m_server.stop();
	m_acceptThread.join();
	std::cout << "accept joined" << std::endl;

	// The socket thread will be blocked waiting for input
	// So to wake it up, let's connect to the socket!
	m_stop_socket_thread = true;
	if (m_server_thread.joinable())
	{
		m_server_thread.join();
	}
	std::cout << "fin" << std::endl;

}

// CALLED FROM SERVER THREAD
void SlippiSpectateServer::handleMessage(u8 *buffer, u32 length, u16 peer_id)
{
	// Unpack the message
	std::string message((char *)buffer, length);
	json json_message = json::parse(message);
	if (!json_message.is_discarded() && (json_message.find("type") != json_message.end()))
	{
		// Check what type of message this is
		if (!json_message["type"].is_string())
		{
			return;
		}

		if (json_message["type"] == "connect_request")
		{
			// Get the requested cursor
			if (json_message.find("cursor") == json_message.end())
			{
				return;
			}
			if (!json_message["cursor"].is_number_integer())
			{
				return;
			}
			u32 requested_cursor = json_message["cursor"];
			u32 sent_cursor = 0;
			// Set the user's cursor position
			if (requested_cursor >= m_cursor_offset)
			{
				// If the requested cursor is past what events we even have, then just tell them to start over
				if (requested_cursor > m_event_buffer.size() + m_cursor_offset)
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
			if (!m_in_game)
			{
				m_sockets[peer_id]->m_cursor = m_event_buffer.size();
			}

			json reply;
			reply["type"] = "connect_reply";
			reply["nick"] = "Slippi Online";
			reply["version"] = scm_slippi_semver_str;
			reply["cursor"] = sent_cursor;

			std::string packet_buffer = reply.dump();

			ENetPacket *packet =
			    enet_packet_create(packet_buffer.data(), (u32)packet_buffer.length(), ENET_PACKET_FLAG_RELIABLE);

			// Batch for sending
			enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
			// Put the client in the right in_game state
			m_sockets[peer_id]->m_shook_hands = true;
		}
	}
}

void SlippiSpectateServer::on_open(connection_hdl hdl) {
		{
				lock_guard<mutex> guard(m_action_lock);
				//std::cout << "on_open" << std::endl;
				m_actions.push(action(SUBSCRIBE,hdl));
		}
		m_action_cond.notify_one();
}

void SlippiSpectateServer::on_close(connection_hdl hdl) {
		{
				lock_guard<mutex> guard(m_action_lock);
				//std::cout << "on_close" << std::endl;
				m_actions.push(action(UNSUBSCRIBE,hdl));
		}
		m_action_cond.notify_one();
}

void SlippiSpectateServer::on_message(connection_hdl hdl, server::message_ptr msg) {
		// queue message up for sending by processing thread
		{
				lock_guard<mutex> guard(m_action_lock);
				//std::cout << "on_message" << std::endl;
				m_actions.push(action(MESSAGE,hdl,msg));
		}
		m_action_cond.notify_one();
}

void SlippiSpectateServer::SlippicommAcceptThread(void)
{
	std::cout << "socket thread start" << std::endl;

	// Run the asio loop with the main thread
	// listen on specified port
	std::cout << "listen" << std::endl;
	websocketpp::lib::error_code error_code;
	m_server.listen(51551, error_code);
	if (error_code) {
		//TODO
		std::cout << "listen error" << std::endl;
		return;
	}
	// Start the server accept loop
	m_server.start_accept(error_code);
	if (error_code) {
		//TODO
		std::cout << "accept error" << std::endl;
		return;
	}

	// Start the ASIO io_service run loop
	std::cout << "run" << std::endl;
	m_server.run();
	std::cout << "running" << std::endl;
}

void SlippiSpectateServer::SlippicommSocketThread(void)
{
	// Main slippicomm server loop
	while (1)
	{
		// If we're told to stop, then quit
		if (m_stop_socket_thread)
		{
			return;
		}

		unique_lock<mutex> lock(m_action_lock);

		while(m_actions.empty()) {
				m_action_cond.wait(lock);
		}

		action a = m_actions.front();
		m_actions.pop();

		lock.unlock();

		if (a.type == SUBSCRIBE) {
				lock_guard<mutex> guard(m_connection_lock);
				m_connections.insert(a.hdl);
		} else if (a.type == UNSUBSCRIBE) {
				lock_guard<mutex> guard(m_connection_lock);
				m_connections.erase(a.hdl);
		} else if (a.type == MESSAGE) {
				lock_guard<mutex> guard(m_connection_lock);

				con_list::iterator it;
				for (it = m_connections.begin(); it != m_connections.end(); ++it) {
						websocketpp::lib::error_code error_code;
						m_server.send(*it, a.msg, error_code);
						if (error_code) {
							// TODO
							return;
						}
				}
		}
	}



	// 	// Pop off any events in the queue
	// 	popEvents();
	//
	// 	std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
	// 	for (; it != m_sockets.end(); it++)
	// 	{
	// 		if (it->second->m_shook_hands)
	// 		{
	// 			writeEvents(it->first);
	// 		}
	// 	}
	//
	// 	ENetEvent event;
	// 	while (enet_host_service(server, &event, 1) > 0)
	// 	{
	// 		switch (event.type)
	// 		{
	// 		case ENET_EVENT_TYPE_CONNECT:
	// 		{
	//
	// 			INFO_LOG(SLIPPI, "A new spectator connected from %x:%u.\n", event.peer->address.host,
	// 			         event.peer->address.port);
	//
	// 			std::shared_ptr<SlippiSocket> newSlippiSocket(new SlippiSocket());
	// 			newSlippiSocket->m_peer = event.peer;
	// 			m_sockets[event.peer->incomingPeerID] = newSlippiSocket;
	// 			break;
	// 		}
	// 		case ENET_EVENT_TYPE_RECEIVE:
	// 		{
	// 			handleMessage(event.packet->data, (u32)event.packet->dataLength, event.peer->incomingPeerID);
	// 			/* Clean up the packet now that we're done using it. */
	// 			enet_packet_destroy(event.packet);
	//
	// 			break;
	// 		}
	// 		case ENET_EVENT_TYPE_DISCONNECT:
	// 		{
	// 			INFO_LOG(SLIPPI, "A spectator disconnected from %x:%u.\n", event.peer->address.host,
	// 			         event.peer->address.port);
	//
	// 			// Delete the item in the m_sockets map
	// 			m_sockets.erase(event.peer->incomingPeerID);
	// 			/* Reset the peer's client information. */
	// 			event.peer->data = NULL;
	// 			break;
	// 		}
	// 		default:
	// 		{
	// 			INFO_LOG(SLIPPI, "Spectator sent an unknown ENet event type");
	// 			break;
	// 		}
	// 		}
	// 	}
	// }
	//
	// enet_host_destroy(server);
	// enet_deinitialize();
}
