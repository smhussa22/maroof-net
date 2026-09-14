#include <maroof/net/Connection.hh>

namespace maroof::net
{

    Connection::Connection(const Endpoint& peer) : m_peer { peer } { }

    PacketHeader Connection::make_hello()
    {

        m_state = State::Connecting;
        PacketHeader header { m_next_send_seq++, PacketType::Hello, 0, 0 };
        return header;

    }

    PacketHeader Connection::accept_hello()
    {

        m_state = State::Established;
        PacketHeader header { m_next_send_seq++, PacketType::HelloAck, 0, 0 };
        return header;

    }

    void Connection::on_hello_ack()
    {

        //   if state == Disconnected -> ignore (stray packet)

        if(m_state == State::Connecting)
        {

            m_state = State::Established;
            return;

        }

        // duplicate hello ack arrived late
        if(m_state == State::Established)
        {

            return;
        
        }

        if(m_state == State::Disconnected)
        {

            return;

        }

    }

    PacketHeader Connection::make_data_header(std::uint8_t channel_id)
    {

        PacketHeader header { m_next_send_seq++, PacketType::Data, channel_id, 0 };
        return header;
    
    }

    void Connection::note_received(std::uint64_t sequence)
    {

        if (sequence > m_highest_received_seq)
        {

            m_highest_received_seq = sequence;

        }
    
    }

} // namespace maroof::net
