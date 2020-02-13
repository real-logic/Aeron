/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.aeron.driver.media;

import io.aeron.AeronCloseHelper;
import org.agrona.ErrorHandler;
import org.agrona.collections.ArrayUtil;
import org.agrona.concurrent.NanoClock;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;

import static io.aeron.driver.media.UdpChannelTransport.sendError;

final class MultiRcvDestination
{
    private static final ReceiveDestinationTransport[] EMPTY_TRANSPORTS = new ReceiveDestinationTransport[0];

    private final long destinationEndpointTimeoutNs;
    private final ErrorHandler errorHandler;
    private final NanoClock nanoClock;
    private ReceiveDestinationTransport[] transports = EMPTY_TRANSPORTS;
    private int numDestinations = 0;

    MultiRcvDestination(final NanoClock nanoClock, final long timeoutNs, final ErrorHandler errorHandler)
    {
        this.nanoClock = nanoClock;
        this.destinationEndpointTimeoutNs = timeoutNs;
        this.errorHandler = errorHandler;
    }

    void close(final DataTransportPoller poller)
    {
        for (final ReceiveDestinationTransport transport : transports)
        {
            AeronCloseHelper.close(errorHandler, transport);
            if (null != poller)
            {
                AeronCloseHelper.close(errorHandler, poller::selectNowWithoutProcessing);
            }
        }
    }

    int addDestination(final ReceiveDestinationTransport transport)
    {
        int index = transports.length;

        for (int i = 0, length = transports.length; i < length; i++)
        {
            if (null == transports[i])
            {
                index = i;
                break;
            }
        }

        transports = ArrayUtil.ensureCapacity(transports, index + 1);
        transports[index] = transport;
        numDestinations++;

        return index;
    }

    void removeDestination(final int transportIndex)
    {
        transports[transportIndex] = null;
        numDestinations--;
    }

    boolean hasDestination(final int transportIndex)
    {
        return numDestinations > transportIndex && null != transports[transportIndex];
    }

    ReceiveDestinationTransport transport(final int transportIndex)
    {
        return transports[transportIndex];
    }

    int transport(final UdpChannel udpChannel)
    {
        final ReceiveDestinationTransport[] transports = this.transports;
        int index = ArrayUtil.UNKNOWN_INDEX;

        for (int i = 0, length = transports.length; i < length; i++)
        {
            final ReceiveDestinationTransport transport = transports[i];

            if (null != transport && transport.udpChannel().equals(udpChannel))
            {
                index = i;
                break;
            }
        }

        return index;
    }

    int sendToAll(
        final ImageConnection[] imageConnections,
        final ByteBuffer buffer,
        final int bufferPosition,
        final int bytesToSend)
    {
        final ReceiveDestinationTransport[] transports = this.transports;
        final long nowNs = nanoClock.nanoTime();
        int minBytesSent = bytesToSend;

        for (int lastIndex = imageConnections.length - 1, i = lastIndex; i >= 0; i--)
        {
            final ImageConnection connection = imageConnections[i];

            if (null != connection)
            {
                final UdpChannelTransport transport = transports[i];
                if (null != transport && ((connection.timeOfLastActivityNs + destinationEndpointTimeoutNs) - nowNs > 0))
                {
                    buffer.position(bufferPosition);
                    minBytesSent = Math.min(minBytesSent, sendTo(transport, buffer, connection.controlAddress));
                }
            }
        }

        return minBytesSent;
    }

    static int sendTo(
        final UdpChannelTransport transport, final ByteBuffer buffer, final InetSocketAddress remoteAddress)
    {
        final int remaining = buffer.remaining();
        int bytesSent = 0;
        try
        {
            if (null != transport && null != transport.sendDatagramChannel && transport.sendDatagramChannel.isOpen())
            {
                transport.sendHook(buffer, remoteAddress);
                bytesSent = transport.sendDatagramChannel.send(buffer, remoteAddress);
            }
        }
        catch (final IOException ex)
        {
            sendError(remaining, ex, remoteAddress);
        }

        return bytesSent;
    }
}
