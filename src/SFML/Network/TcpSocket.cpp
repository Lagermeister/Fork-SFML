////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2023 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Packet.hpp>
#include <SFML/Network/SocketImpl.hpp>
#include <SFML/Network/TcpSocket.hpp>

#include <SFML/System/Err.hpp>

#include <algorithm>
#include <ostream>

#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4127) // "conditional expression is constant" generated by the FD_SET macro
#endif


namespace
{
// Define the low-level send/receive flags, which depend on the OS
#ifdef SFML_SYSTEM_LINUX
const int flags = MSG_NOSIGNAL;
#else
const int flags = 0;
#endif
} // namespace

namespace sf
{
////////////////////////////////////////////////////////////
TcpSocket::TcpSocket() : Socket(Type::Tcp)
{
}


////////////////////////////////////////////////////////////
unsigned short TcpSocket::getLocalPort() const
{
    if (getNativeHandle() != priv::SocketImpl::invalidSocket())
    {
        // Retrieve information about the local end of the socket
        sockaddr_in                  address{};
        priv::SocketImpl::AddrLength size = sizeof(address);
        if (getsockname(getNativeHandle(), reinterpret_cast<sockaddr*>(&address), &size) != -1)
        {
            return ntohs(address.sin_port);
        }
    }

    // We failed to retrieve the port
    return 0;
}


////////////////////////////////////////////////////////////
std::optional<IpAddress> TcpSocket::getRemoteAddress() const
{
    if (getNativeHandle() != priv::SocketImpl::invalidSocket())
    {
        // Retrieve information about the remote end of the socket
        sockaddr_in                  address{};
        priv::SocketImpl::AddrLength size = sizeof(address);
        if (getpeername(getNativeHandle(), reinterpret_cast<sockaddr*>(&address), &size) != -1)
        {
            return IpAddress(ntohl(address.sin_addr.s_addr));
        }
    }

    // We failed to retrieve the address
    return std::nullopt;
}


////////////////////////////////////////////////////////////
unsigned short TcpSocket::getRemotePort() const
{
    if (getNativeHandle() != priv::SocketImpl::invalidSocket())
    {
        // Retrieve information about the remote end of the socket
        sockaddr_in                  address{};
        priv::SocketImpl::AddrLength size = sizeof(address);
        if (getpeername(getNativeHandle(), reinterpret_cast<sockaddr*>(&address), &size) != -1)
        {
            return ntohs(address.sin_port);
        }
    }

    // We failed to retrieve the port
    return 0;
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::connect(const IpAddress& remoteAddress, unsigned short remotePort, Time timeout)
{
    // Disconnect the socket if it is already connected
    disconnect();

    // Create the internal socket if it doesn't exist
    create();

    // Create the remote address
    sockaddr_in address = priv::SocketImpl::createAddress(remoteAddress.toInteger(), remotePort);

    if (timeout <= Time::Zero)
    {
        // ----- We're not using a timeout: just try to connect -----

        // Connect the socket
        if (::connect(getNativeHandle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1)
            return priv::SocketImpl::getErrorStatus();

        // Connection succeeded
        return Status::Done;
    }
    else
    {
        // ----- We're using a timeout: we'll need a few tricks to make it work -----

        // Save the previous blocking state
        const bool blocking = isBlocking();

        // Switch to non-blocking to enable our connection timeout
        if (blocking)
            setBlocking(false);

        // Try to connect to the remote address
        if (::connect(getNativeHandle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) >= 0)
        {
            // We got instantly connected! (it may no happen a lot...)
            setBlocking(blocking);
            return Status::Done;
        }

        // Get the error status
        Status status = priv::SocketImpl::getErrorStatus();

        // If we were in non-blocking mode, return immediately
        if (!blocking)
            return status;

        // Otherwise, wait until something happens to our socket (success, timeout or error)
        if (status == Socket::Status::NotReady)
        {
            // Setup the selector
            fd_set selector;
            FD_ZERO(&selector);
            FD_SET(getNativeHandle(), &selector);

            // Setup the timeout
            timeval time{};
            time.tv_sec  = static_cast<long>(timeout.asMicroseconds() / 1000000);
            time.tv_usec = static_cast<int>(timeout.asMicroseconds() % 1000000);

            // Wait for something to write on our socket (which means that the connection request has returned)
            if (select(static_cast<int>(getNativeHandle() + 1), nullptr, &selector, nullptr, &time) > 0)
            {
                // At this point the connection may have been either accepted or refused.
                // To know whether it's a success or a failure, we must check the address of the connected peer
                if (getRemoteAddress().has_value())
                {
                    // Connection accepted
                    status = Status::Done;
                }
                else
                {
                    // Connection refused
                    status = priv::SocketImpl::getErrorStatus();
                }
            }
            else
            {
                // Failed to connect before timeout is over
                status = priv::SocketImpl::getErrorStatus();
            }
        }

        // Switch back to blocking mode
        setBlocking(true);

        return status;
    }
}


////////////////////////////////////////////////////////////
void TcpSocket::disconnect()
{
    // Close the socket
    close();

    // Reset the pending packet data
    m_pendingPacket = PendingPacket();
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::send(const std::byte* data, std::size_t size)
{
    if (!isBlocking())
        err() << "Warning: Partial sends might not be handled properly." << std::endl;

    std::size_t sent;

    return send(data, size, sent);
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::send(const std::byte* data, std::size_t size, std::size_t& sent)
{
    // Check the parameters
    if (!data || (size == 0))
    {
        err() << "Cannot send data over the network (no data to send)" << std::endl;
        return Status::Error;
    }

    // Loop until every byte has been sent
    int result = 0;
    for (sent = 0; sent < size; sent += static_cast<std::size_t>(result))
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
        // Send a chunk of data
        result = static_cast<int>(::send(getNativeHandle(),
                                         reinterpret_cast<const char*>(data) + sent,
                                         static_cast<priv::SocketImpl::Size>(size - sent),
                                         flags));
#pragma GCC diagnostic pop

        // Check for errors
        if (result < 0)
        {
            const Status status = priv::SocketImpl::getErrorStatus();

            if ((status == Status::NotReady) && sent)
                return Status::Partial;

            return status;
        }
    }

    return Status::Done;
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::receive(void* data, std::size_t size, std::size_t& received)
{
    // First clear the variables to fill
    received = 0;

    // Check the destination buffer
    if (!data)
    {
        err() << "Cannot receive data from the network (the destination buffer is invalid)" << std::endl;
        return Status::Error;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
    // Receive a chunk of bytes
    const int sizeReceived = static_cast<int>(
        recv(getNativeHandle(), static_cast<char*>(data), static_cast<priv::SocketImpl::Size>(size), flags));
#pragma GCC diagnostic pop

    // Check the number of bytes received
    if (sizeReceived > 0)
    {
        received = static_cast<std::size_t>(sizeReceived);
        return Status::Done;
    }
    else if (sizeReceived == 0)
    {
        return Socket::Status::Disconnected;
    }
    else
    {
        return priv::SocketImpl::getErrorStatus();
    }
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::send(Packet& packet)
{
    // TCP is a stream protocol, it doesn't preserve messages boundaries.
    // This means that we have to send the packet size first, so that the
    // receiver knows the actual end of the packet in the data stream.

    // We allocate an extra memory block so that the size can be sent
    // together with the data in a single call. This may seem inefficient,
    // but it is actually required to avoid partial send, which could cause
    // data corruption on the receiving end.

    // Get the data to send from the packet
    std::size_t size = 0;
    const auto* data = packet.onSend(size);

    // First convert the packet size to network byte order
    std::uint32_t packetSize = htonl(static_cast<std::uint32_t>(size));

    // Allocate memory for the data block to send
    m_blockToSendBuffer.resize(sizeof(packetSize) + size);

// Copy the packet size and data into the block to send
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference" // False positive.
    std::memcpy(m_blockToSendBuffer.data(), &packetSize, sizeof(packetSize));
#pragma GCC diagnostic pop
    if (size > 0)
        std::memcpy(m_blockToSendBuffer.data() + sizeof(packetSize), data, size);

// These warnings are ignored here for portability, as even on Windows the
// signature of `send` might change depending on whether Win32 or MinGW is
// being used.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
    // Send the data block
    std::size_t  sent;
    const Status status = send(m_blockToSendBuffer.data() + packet.m_sendPos,
                               static_cast<priv::SocketImpl::Size>(m_blockToSendBuffer.size() - packet.m_sendPos),
                               sent);
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

    // In the case of a partial send, record the location to resume from
    if (status == Status::Partial)
    {
        packet.m_sendPos += sent;
    }
    else if (status == Status::Done)
    {
        packet.m_sendPos = 0;
    }

    return status;
}


////////////////////////////////////////////////////////////
Socket::Status TcpSocket::receive(Packet& packet)
{
    // First clear the variables to fill
    packet.clear();

    // We start by getting the size of the incoming packet
    std::uint32_t packetSize = 0;
    std::size_t   received   = 0;
    if (m_pendingPacket.SizeReceived < sizeof(m_pendingPacket.Size))
    {
        // Loop until we've received the entire size of the packet
        // (even a 4 byte variable may be received in more than one call)
        while (m_pendingPacket.SizeReceived < sizeof(m_pendingPacket.Size))
        {
            char*        data   = reinterpret_cast<char*>(&m_pendingPacket.Size) + m_pendingPacket.SizeReceived;
            const Status status = receive(data, sizeof(m_pendingPacket.Size) - m_pendingPacket.SizeReceived, received);
            m_pendingPacket.SizeReceived += received;

            if (status != Status::Done)
                return status;
        }

        // The packet size has been fully received
        packetSize = ntohl(m_pendingPacket.Size);
    }
    else
    {
        // The packet size has already been received in a previous call
        packetSize = ntohl(m_pendingPacket.Size);
    }

    // Loop until we receive all the packet data
    char buffer[1024];
    while (m_pendingPacket.Data.size() < packetSize)
    {
        // Receive a chunk of data
        const std::size_t sizeToGet = std::min(packetSize - m_pendingPacket.Data.size(), sizeof(buffer));
        const Status      status    = receive(buffer, sizeToGet, received);
        if (status != Status::Done)
            return status;

        // Append it into the packet
        if (received > 0)
        {
            m_pendingPacket.Data.resize(m_pendingPacket.Data.size() + received);
            std::byte* begin = m_pendingPacket.Data.data() + m_pendingPacket.Data.size() - received;
            std::memcpy(begin, buffer, received);
        }
    }

    // We have received all the packet data: we can copy it to the user packet
    if (!m_pendingPacket.Data.empty())
        packet.onReceive(m_pendingPacket.Data.data(), m_pendingPacket.Data.size());

    // Clear the pending packet data
    m_pendingPacket = PendingPacket();

    return Status::Done;
}

} // namespace sf
