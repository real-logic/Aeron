/*
 *  Copyright 2014-2020 Real Logic Limited.
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
package io.aeron.cluster;

import io.aeron.*;
import io.aeron.archive.client.*;
import io.aeron.archive.codecs.ControlResponseCode;
import io.aeron.archive.codecs.RecordingSignal;
import io.aeron.archive.codecs.SourceLocation;
import io.aeron.archive.status.RecordingPos;
import io.aeron.cluster.client.AeronCluster;
import io.aeron.cluster.client.ClusterException;
import io.aeron.cluster.codecs.BackupResponseDecoder;
import io.aeron.cluster.codecs.MessageHeaderDecoder;
import io.aeron.cluster.service.ClusterMarkFile;
import io.aeron.exceptions.AeronException;
import io.aeron.exceptions.TimeoutException;
import io.aeron.logbuffer.Header;
import org.agrona.CloseHelper;
import org.agrona.DirectBuffer;
import org.agrona.collections.ArrayUtil;
import org.agrona.collections.Long2LongHashMap;
import org.agrona.concurrent.Agent;
import org.agrona.concurrent.AgentInvoker;
import org.agrona.concurrent.EpochClock;
import org.agrona.concurrent.status.CountersReader;

import java.util.ArrayList;
import java.util.concurrent.TimeUnit;

import static io.aeron.Aeron.NULL_VALUE;
import static io.aeron.CommonContext.ENDPOINT_PARAM_NAME;
import static io.aeron.archive.client.AeronArchive.*;
import static io.aeron.cluster.ClusterBackup.State.*;
import static io.aeron.cluster.ConsensusAdapter.FRAGMENT_POLL_LIMIT;
import static io.aeron.exceptions.AeronException.Category;
import static org.agrona.concurrent.status.CountersReader.NULL_COUNTER_ID;

/**
 * {@link Agent} which backs up a remote cluster by replicating the log and polling for snapshots.
 */
public class ClusterBackupAgent implements Agent, UnavailableCounterHandler
{
    private final MessageHeaderDecoder messageHeaderDecoder = new MessageHeaderDecoder();
    private final BackupResponseDecoder backupResponseDecoder = new BackupResponseDecoder();

    private final ClusterBackup.Context ctx;
    private final ClusterMarkFile markFile;
    private final AgentInvoker aeronClientInvoker;
    private final EpochClock epochClock;
    private final Aeron aeron;
    private final String[] clusterConsensusEndpoints;
    private final ConsensusPublisher consensusPublisher = new ConsensusPublisher();
    private final ArrayList<RecordingLog.Snapshot> snapshotsToRetrieve = new ArrayList<>(4);
    private final ArrayList<RecordingLog.Snapshot> snapshotsRetrieved = new ArrayList<>(4);
    private final Long2LongHashMap snapshotLengthMap = new Long2LongHashMap(NULL_LENGTH);
    private final Counter stateCounter;
    private final Counter liveLogPositionCounter;
    private final Counter nextQueryDeadlineMsCounter;
    private final ClusterBackupEventsListener eventsListener;
    private final long backupResponseTimeoutMs;
    private final long backupQueryIntervalMs;
    private final long backupProgressTimeoutMs;
    private final long coolDownIntervalMs;

    private ClusterBackup.State state = INIT;

    private RecordingLog recordingLog;

    private AeronArchive backupArchive;
    private AeronArchive.AsyncConnect clusterArchiveAsyncConnect;
    private AeronArchive clusterArchive;

    private SnapshotRetrieveMonitor snapshotRetrieveMonitor;

    private final FragmentAssembler consensusFragmentAssembler = new FragmentAssembler(this::onFragment);
    private final Subscription consensusSubscription;
    private ExclusivePublication consensusPublication;
    private ClusterMember[] clusterMembers;
    private ClusterMember leaderMember;
    private RecordingLog.Entry leaderLogEntry;
    private RecordingLog.Entry leaderLastTermEntry;

    private long timeOfLastTickMs = 0;
    private long timeOfLastBackupQueryMs = 0;
    private long timeOfLastProgressMs = 0;
    private long coolDownDeadlineMs = NULL_VALUE;
    private long correlationId = NULL_VALUE;
    private long leaderLogRecordingId = NULL_VALUE;
    private long liveLogReplaySubscriptionId = NULL_VALUE;
    private long liveLogRecordingId = NULL_VALUE;
    private long liveLogReplayId = NULL_VALUE;
    private int leaderCommitPositionCounterId = NULL_VALUE;
    private int clusterConsensusEndpointsCursor = NULL_VALUE;
    private int snapshotCursor = 0;
    private int liveLogReplaySessionId = NULL_VALUE;
    private int liveLogRecCounterId = NULL_COUNTER_ID;

    ClusterBackupAgent(final ClusterBackup.Context ctx)
    {
        this.ctx = ctx;
        aeron = ctx.aeron();
        epochClock = ctx.epochClock();
        backupResponseTimeoutMs = TimeUnit.NANOSECONDS.toMillis(ctx.clusterBackupResponseTimeoutNs());
        backupQueryIntervalMs = TimeUnit.NANOSECONDS.toMillis(ctx.clusterBackupIntervalNs());
        backupProgressTimeoutMs = TimeUnit.NANOSECONDS.toMillis(ctx.clusterBackupProgressTimeoutNs());
        coolDownIntervalMs = TimeUnit.NANOSECONDS.toMillis(ctx.clusterBackupCoolDownIntervalNs());
        markFile = ctx.clusterMarkFile();
        eventsListener = ctx.eventsListener();

        clusterConsensusEndpoints = ctx.clusterConsensusEndpoints().split(",");

        aeronClientInvoker = aeron.conductorAgentInvoker();
        aeronClientInvoker.invoke();

        aeron.addUnavailableCounterHandler(this);

        consensusSubscription = aeron.addSubscription(ctx.consensusChannel(), ctx.consensusStreamId());

        stateCounter = ctx.stateCounter();
        liveLogPositionCounter = ctx.liveLogPositionCounter();
        nextQueryDeadlineMsCounter = ctx.nextQueryDeadlineMsCounter();
    }

    public void onStart()
    {
        backupArchive = AeronArchive.connect(ctx.archiveContext().clone());
        stateCounter.setOrdered(INIT.code());
        nextQueryDeadlineMsCounter.setOrdered(epochClock.time() - 1);
    }

    public void onClose()
    {
        if (!ctx.ownsAeronClient())
        {
            CloseHelper.close(consensusSubscription);
            CloseHelper.close(consensusPublication);
        }

        if (NULL_VALUE != liveLogReplaySubscriptionId)
        {
            backupArchive.tryStopRecording(liveLogReplaySubscriptionId);
        }

        CloseHelper.close(backupArchive);
        CloseHelper.close(clusterArchiveAsyncConnect);
        CloseHelper.close(clusterArchive);
        CloseHelper.close(recordingLog);
        ctx.close();
    }

    public int doWork()
    {
        final long nowMs = epochClock.time();
        int workCount = INIT == state ? init(nowMs) : 0;

        if (nowMs != timeOfLastTickMs)
        {
            timeOfLastTickMs = nowMs;
            workCount += aeronClientInvoker.invoke();
            markFile.updateActivityTimestamp(nowMs);
        }

        try
        {
            workCount += consensusSubscription.poll(consensusFragmentAssembler, FRAGMENT_POLL_LIMIT);

            switch (state)
            {
                case BACKUP_QUERY:
                    workCount += backupQuery(nowMs);
                    break;

                case SNAPSHOT_LENGTH_RETRIEVE:
                    workCount += snapshotLengthRetrieve(nowMs);
                    break;

                case SNAPSHOT_RETRIEVE:
                    workCount += snapshotRetrieve(nowMs);
                    break;

                case LIVE_LOG_REPLAY:
                    workCount += liveLogReplay(nowMs);
                    break;

                case UPDATE_RECORDING_LOG:
                    workCount += updateRecordingLog(nowMs);
                    break;

                case RESET_BACKUP:
                    workCount += resetBackup(nowMs);
                    break;

                case BACKING_UP:
                    workCount += backingUp(nowMs);
                    break;
            }

            if (hasProgressStalled(nowMs))
            {
                if (null != eventsListener)
                {
                    eventsListener.onPossibleFailure(new TimeoutException("progress has stalled", Category.WARN));
                }

                state(RESET_BACKUP, nowMs);
            }
        }
        catch (final Exception ex)
        {
            if (null != eventsListener)
            {
                eventsListener.onPossibleFailure(ex);
            }

            state(RESET_BACKUP, nowMs);
            throw ex;
        }

        return workCount;
    }

    public String roleName()
    {
        return "cluster-backup";
    }

    public void reset()
    {
        clusterMembers = null;
        leaderMember = null;
        snapshotsToRetrieve.clear();
        snapshotsRetrieved.clear();
        snapshotLengthMap.clear();
        leaderLogEntry = null;
        leaderLastTermEntry = null;
        clusterConsensusEndpointsCursor = NULL_VALUE;

        if (null != recordingLog)
        {
            recordingLog.close();
            recordingLog = null;
        }

        consensusFragmentAssembler.clear();
        final ExclusivePublication consensusPublication = this.consensusPublication;
        final AeronArchive clusterArchive = this.clusterArchive;
        final AeronArchive.AsyncConnect clusterArchiveAsyncConnect = this.clusterArchiveAsyncConnect;

        this.consensusPublication = null;
        this.clusterArchive = null;
        this.clusterArchiveAsyncConnect = null;

        correlationId = NULL_VALUE;
        liveLogRecCounterId = NULL_COUNTER_ID;
        liveLogRecordingId = NULL_VALUE;
        liveLogReplayId = NULL_VALUE;
        liveLogReplaySubscriptionId = NULL_VALUE;

        CloseHelper.closeAll(consensusPublication, clusterArchive, clusterArchiveAsyncConnect);
    }

    private void onFragment(final DirectBuffer buffer, final int offset, final int length, final Header header)
    {
        messageHeaderDecoder.wrap(buffer, offset);

        final int schemaId = messageHeaderDecoder.schemaId();
        if (schemaId != MessageHeaderDecoder.SCHEMA_ID)
        {
            throw new ClusterException("expected schemaId=" + MessageHeaderDecoder.SCHEMA_ID + ", actual=" + schemaId);
        }

        if (messageHeaderDecoder.templateId() == BackupResponseDecoder.TEMPLATE_ID)
        {
            backupResponseDecoder.wrap(
                buffer,
                offset + MessageHeaderDecoder.ENCODED_LENGTH,
                messageHeaderDecoder.blockLength(),
                messageHeaderDecoder.version());

            onBackupResponse(
                backupResponseDecoder.correlationId(),
                backupResponseDecoder.logRecordingId(),
                backupResponseDecoder.logLeadershipTermId(),
                backupResponseDecoder.logTermBaseLogPosition(),
                backupResponseDecoder.lastLeadershipTermId(),
                backupResponseDecoder.lastTermBaseLogPosition(),
                backupResponseDecoder.commitPositionCounterId(),
                backupResponseDecoder.leaderMemberId(),
                backupResponseDecoder);
        }
    }

    public void onUnavailableCounter(
        final CountersReader countersReader, final long registrationId, final int counterId)
    {
        if (counterId == liveLogRecCounterId)
        {
            if (null != eventsListener)
            {
                eventsListener.onPossibleFailure(
                    new AeronException("log recording counter became unavailable", Category.WARN));
            }

            state(RESET_BACKUP, epochClock.time());
        }
    }

    @SuppressWarnings("MethodLength")
    private void onBackupResponse(
        final long correlationId,
        final long logRecordingId,
        final long logLeadershipTermId,
        final long logTermBaseLogPosition,
        final long lastLeadershipTermId,
        final long lastTermBaseLogPosition,
        final int commitPositionCounterId,
        final int leaderMemberId,
        final BackupResponseDecoder backupResponseDecoder)
    {
        if (BACKUP_QUERY == state && correlationId == this.correlationId)
        {
            final BackupResponseDecoder.SnapshotsDecoder snapshotsDecoder = backupResponseDecoder.snapshots();

            if (snapshotsDecoder.count() > 0)
            {
                for (final BackupResponseDecoder.SnapshotsDecoder snapshot : snapshotsDecoder)
                {
                    final RecordingLog.Entry entry = recordingLog.getLatestSnapshot(snapshot.serviceId());

                    if (null != entry)
                    {
                        if (snapshot.logPosition() == entry.logPosition)
                        {
                            continue;
                        }
                    }

                    snapshotsToRetrieve.add(new RecordingLog.Snapshot(
                        snapshot.recordingId(),
                        snapshot.leadershipTermId(),
                        snapshot.termBaseLogPosition(),
                        snapshot.logPosition(),
                        snapshot.timestamp(),
                        snapshot.serviceId()));
                }
            }

            final RecordingLog.Entry lastTerm = recordingLog.findLastTerm();

            if (null == leaderMember || leaderMember.id() != leaderMemberId || logRecordingId != leaderLogRecordingId)
            {
                leaderLogRecordingId = logRecordingId;

                leaderLogEntry = new RecordingLog.Entry(
                    logRecordingId,
                    logLeadershipTermId,
                    logTermBaseLogPosition,
                    NULL_POSITION,
                    NULL_TIMESTAMP,
                    NULL_VALUE,
                    RecordingLog.ENTRY_TYPE_TERM,
                    true, -1
                );
            }

            if (null == lastTerm ||
                lastLeadershipTermId != lastTerm.leadershipTermId ||
                lastTermBaseLogPosition != lastTerm.termBaseLogPosition)
            {
                leaderLastTermEntry = new RecordingLog.Entry(
                    logRecordingId,
                    lastLeadershipTermId,
                    lastTermBaseLogPosition,
                    NULL_POSITION,
                    NULL_TIMESTAMP,
                    NULL_VALUE,
                    RecordingLog.ENTRY_TYPE_TERM,
                    true,
                    -1
                );
            }

            timeOfLastBackupQueryMs = 0;
            snapshotCursor = 0;
            this.correlationId = NULL_VALUE;
            leaderCommitPositionCounterId = commitPositionCounterId;

            clusterMembers = ClusterMember.parse(backupResponseDecoder.clusterMembers());
            leaderMember = ClusterMember.findMember(clusterMembers, leaderMemberId);

            if (null != eventsListener)
            {
                eventsListener.onBackupResponse(clusterMembers, leaderMember, snapshotsToRetrieve);
            }

            if (null == clusterArchive)
            {
                final ChannelUri leaderArchiveUri = ChannelUri.parse(ctx.archiveContext().controlRequestChannel());
                leaderArchiveUri.put(ENDPOINT_PARAM_NAME, leaderMember.archiveEndpoint());

                final AeronArchive.Context leaderArchiveCtx = new AeronArchive.Context()
                    .aeron(ctx.aeron())
                    .controlRequestChannel(leaderArchiveUri.toString())
                    .controlRequestStreamId(ctx.archiveContext().controlRequestStreamId())
                    .controlResponseChannel(ctx.archiveContext().controlResponseChannel())
                    .controlResponseStreamId(ctx.archiveContext().controlResponseStreamId());

                CloseHelper.close(clusterArchiveAsyncConnect);
                clusterArchiveAsyncConnect = AeronArchive.asyncConnect(leaderArchiveCtx);
            }

            final long nowMs = epochClock.time();

            timeOfLastProgressMs = nowMs;

            if (snapshotsToRetrieve.isEmpty())
            {
                state(LIVE_LOG_REPLAY, nowMs);
            }
            else
            {
                state(SNAPSHOT_LENGTH_RETRIEVE, nowMs);
            }
        }
    }

    private int init(final long nowMs)
    {
        CloseHelper.close(recordingLog);
        recordingLog = new RecordingLog(ctx.clusterDir());
        timeOfLastProgressMs = nowMs;
        state(BACKUP_QUERY, nowMs);
        return 1;
    }

    private int resetBackup(final long nowMs)
    {
        timeOfLastProgressMs = nowMs;

        if (NULL_VALUE == coolDownDeadlineMs)
        {
            coolDownDeadlineMs = nowMs + coolDownIntervalMs;
            reset();
            return 1;
        }
        else if (nowMs > coolDownDeadlineMs)
        {
            coolDownDeadlineMs = NULL_VALUE;
            state(INIT, nowMs);
            return 1;
        }

        return 0;
    }

    private int backupQuery(final long nowMs)
    {
        if (null == consensusPublication || nowMs > (timeOfLastBackupQueryMs + backupResponseTimeoutMs))
        {
            int cursor = ++clusterConsensusEndpointsCursor;
            if (cursor >= clusterConsensusEndpoints.length)
            {
                clusterConsensusEndpointsCursor = 0;
                cursor = 0;
            }

            CloseHelper.close(clusterArchiveAsyncConnect);
            clusterArchiveAsyncConnect = null;
            CloseHelper.close(clusterArchive);
            clusterArchive = null;

            CloseHelper.close(consensusPublication);
            final ChannelUri uri = ChannelUri.parse(ctx.consensusChannel());
            uri.put(ENDPOINT_PARAM_NAME, clusterConsensusEndpoints[cursor]);
            consensusPublication = aeron.addExclusivePublication(uri.toString(), ctx.consensusStreamId());
            correlationId = NULL_VALUE;
            timeOfLastBackupQueryMs = nowMs;

            return 1;
        }
        else if (NULL_VALUE == correlationId && consensusPublication.isConnected())
        {
            final long correlationId = aeron.nextCorrelationId();

            if (consensusPublisher.backupQuery(
                consensusPublication,
                correlationId,
                ctx.consensusStreamId(),
                AeronCluster.Configuration.PROTOCOL_SEMANTIC_VERSION,
                ctx.consensusChannel(),
                ArrayUtil.EMPTY_BYTE_ARRAY))
            {
                timeOfLastBackupQueryMs = nowMs;
                this.correlationId = correlationId;

                return 1;
            }
        }

        return 0;
    }

    private int snapshotLengthRetrieve(final long nowMs)
    {
        int workCount = 0;

        if (null == clusterArchive)
        {
            clusterArchive = clusterArchiveAsyncConnect.poll();
            return null == clusterArchive ? 0 : 1;
        }

        if (NULL_VALUE == correlationId)
        {
            final long stopPositionCorrelationId = ctx.aeron().nextCorrelationId();
            final RecordingLog.Snapshot snapshot = snapshotsToRetrieve.get(snapshotCursor);

            if (clusterArchive.archiveProxy().getStopPosition(
                snapshot.recordingId,
                stopPositionCorrelationId,
                clusterArchive.controlSessionId()))
            {
                correlationId = stopPositionCorrelationId;
                timeOfLastProgressMs = nowMs;
                workCount++;
            }
        }
        else if (pollForResponse(clusterArchive, correlationId))
        {
            final long snapshotStopPosition = (int)clusterArchive.controlResponsePoller().relevantId();
            correlationId = NULL_VALUE;

            if (NULL_POSITION == snapshotStopPosition)
            {
                state(RESET_BACKUP, nowMs);
            }

            snapshotLengthMap.put(snapshotCursor, snapshotStopPosition);
            if (++snapshotCursor >= snapshotsToRetrieve.size())
            {
                snapshotCursor = 0;
                state(SNAPSHOT_RETRIEVE, nowMs);
            }

            timeOfLastProgressMs = nowMs;
            workCount++;
        }

        return workCount;
    }

    private int snapshotRetrieve(final long nowMs)
    {
        int workCount = 0;

        if (null == clusterArchive)
        {
            clusterArchive = clusterArchiveAsyncConnect.poll();
            return null == clusterArchive ? 0 : 1;
        }

        if (null != snapshotRetrieveMonitor)
        {
            workCount += snapshotRetrieveMonitor.poll();

            timeOfLastProgressMs = nowMs;

            if (snapshotRetrieveMonitor.isDone())
            {
                final RecordingLog.Snapshot snapshot = snapshotsToRetrieve.get(snapshotCursor);

                snapshotsRetrieved.add(new RecordingLog.Snapshot(
                    snapshotRetrieveMonitor.recordingId,
                    snapshot.leadershipTermId,
                    snapshot.termBaseLogPosition,
                    snapshot.logPosition,
                    snapshot.timestamp,
                    snapshot.serviceId));

                snapshotRetrieveMonitor = null;
                correlationId = NULL_VALUE;
                timeOfLastProgressMs = nowMs;

                if (++snapshotCursor >= snapshotsToRetrieve.size())
                {
                    state(LIVE_LOG_REPLAY, nowMs);
                    workCount++;
                }
            }
        }
        else if (NULL_VALUE == correlationId)
        {
            final long replayId = ctx.aeron().nextCorrelationId();
            final RecordingLog.Snapshot snapshot = snapshotsToRetrieve.get(snapshotCursor);
            final String catchupChannel = "aeron:udp?endpoint=" + ctx.catchupEndpoint();

            if (clusterArchive.archiveProxy().replay(
                snapshot.recordingId,
                0,
                NULL_LENGTH,
                catchupChannel,
                ctx.replayStreamId(),
                replayId,
                clusterArchive.controlSessionId()))
            {
                correlationId = replayId;
                timeOfLastProgressMs = nowMs;
                workCount++;
            }
        }
        else if (pollForResponse(clusterArchive, correlationId))
        {
            final int replaySessionId = (int)clusterArchive.controlResponsePoller().relevantId();
            final String catchupChannel =
                "aeron:udp?endpoint=" + ctx.catchupEndpoint() + "|session-id=" + replaySessionId;

            snapshotRetrieveMonitor = new SnapshotRetrieveMonitor(backupArchive, snapshotLengthMap.get(snapshotCursor));

            backupArchive.startRecording(catchupChannel, ctx.replayStreamId(), SourceLocation.REMOTE, true);

            timeOfLastProgressMs = nowMs;
            workCount++;
        }

        return workCount;
    }

    private int liveLogReplay(final long nowMs)
    {
        int workCount = 0;

        if (NULL_VALUE == liveLogRecordingId)
        {
            if (null == clusterArchive)
            {
                clusterArchive = clusterArchiveAsyncConnect.poll();
                return null == clusterArchive ? 0 : 1;
            }

            if (NULL_VALUE == correlationId)
            {
                final long replayId = ctx.aeron().nextCorrelationId();
                final RecordingLog.Entry logEntry = recordingLog.findLastTerm();
                final long startPosition = null == logEntry ?
                    NULL_POSITION : backupArchive.getStopPosition(logEntry.recordingId);

                final String catchupChannel = "aeron:udp?endpoint=" + ctx.catchupEndpoint();

                if (clusterArchive.archiveProxy().boundedReplay(
                    leaderLogRecordingId,
                    startPosition,
                    NULL_LENGTH,
                    leaderCommitPositionCounterId,
                    catchupChannel,
                    ctx.logStreamId(),
                    replayId,
                    clusterArchive.controlSessionId()))
                {
                    correlationId = replayId;
                    timeOfLastProgressMs = nowMs;
                    workCount++;
                }
            }
            else if (NULL_VALUE != liveLogReplaySubscriptionId && NULL_COUNTER_ID == liveLogRecCounterId)
            {
                final CountersReader countersReader = aeron.countersReader();

                if ((liveLogRecCounterId = RecordingPos.findCounterIdBySession(
                    countersReader, liveLogReplaySessionId)) != NULL_COUNTER_ID)
                {
                    liveLogPositionCounter.setOrdered(countersReader.getCounterValue(liveLogRecCounterId));

                    liveLogRecordingId = RecordingPos.getRecordingId(countersReader, liveLogRecCounterId);
                    timeOfLastBackupQueryMs = nowMs;
                    timeOfLastProgressMs = nowMs;

                    state(UPDATE_RECORDING_LOG, nowMs);
                }
            }
            else if (pollForResponse(clusterArchive, correlationId))
            {
                final RecordingLog.Entry logEntry = recordingLog.findLastTerm();
                liveLogReplayId = clusterArchive.controlResponsePoller().relevantId();
                liveLogReplaySessionId = (int)liveLogReplayId;
                final String catchupChannel =
                    "aeron:udp?endpoint=" + ctx.catchupEndpoint() + "|session-id=" + liveLogReplaySessionId;

                timeOfLastProgressMs = nowMs;

                if (null == logEntry)
                {
                    liveLogReplaySubscriptionId = backupArchive.startRecording(
                        catchupChannel, ctx.logStreamId(), SourceLocation.REMOTE, true);
                }
                else
                {
                    liveLogReplaySubscriptionId = backupArchive.extendRecording(
                        logEntry.recordingId, catchupChannel, ctx.logStreamId(), SourceLocation.REMOTE, true);
                }
            }
        }
        else
        {
            timeOfLastProgressMs = nowMs;
            state(UPDATE_RECORDING_LOG, nowMs);
        }

        return workCount;
    }

    private int updateRecordingLog(final long nowMs)
    {
        boolean wasRecordingLogUpdated = false;
        final long snapshotLeadershipTermId = snapshotsRetrieved.isEmpty() ?
            NULL_VALUE : snapshotsRetrieved.get(0).leadershipTermId;

        if (null != leaderLogEntry &&
            recordingLog.isUnknown(leaderLogEntry.leadershipTermId) &&
            leaderLogEntry.leadershipTermId <= snapshotLeadershipTermId)
        {
            recordingLog.appendTerm(
                liveLogRecordingId,
                leaderLogEntry.leadershipTermId,
                leaderLogEntry.termBaseLogPosition,
                leaderLogEntry.timestamp);

            wasRecordingLogUpdated = true;
            leaderLogEntry = null;
        }

        if (!snapshotsRetrieved.isEmpty())
        {
            for (int i = snapshotsRetrieved.size() - 1; i >= 0; i--)
            {
                final RecordingLog.Snapshot snapshot = snapshotsRetrieved.get(i);

                recordingLog.appendSnapshot(
                    snapshot.recordingId,
                    snapshot.leadershipTermId,
                    snapshot.termBaseLogPosition,
                    snapshot.logPosition,
                    snapshot.timestamp,
                    snapshot.serviceId);
            }

            wasRecordingLogUpdated = true;
        }

        if (null != leaderLastTermEntry && recordingLog.isUnknown(leaderLastTermEntry.leadershipTermId))
        {
            recordingLog.appendTerm(
                liveLogRecordingId,
                leaderLastTermEntry.leadershipTermId,
                leaderLastTermEntry.termBaseLogPosition,
                leaderLastTermEntry.timestamp);

            wasRecordingLogUpdated = true;
            leaderLastTermEntry = null;
        }

        if (wasRecordingLogUpdated && null != eventsListener)
        {
            eventsListener.onUpdatedRecordingLog(recordingLog, snapshotsRetrieved);
        }

        snapshotsRetrieved.clear();
        snapshotsToRetrieve.clear();
        snapshotLengthMap.clear();

        timeOfLastProgressMs = nowMs;

        nextQueryDeadlineMsCounter.setOrdered(nowMs + backupQueryIntervalMs);
        state(BACKING_UP, nowMs);

        return 1;
    }

    private int backingUp(final long nowMs)
    {
        int workCount = 0;

        if (nowMs > nextQueryDeadlineMsCounter.get())
        {
            timeOfLastBackupQueryMs = nowMs;
            timeOfLastProgressMs = nowMs;
            state(BACKUP_QUERY, nowMs);
            workCount += 1;
        }

        if (NULL_COUNTER_ID != liveLogRecCounterId)
        {
            final long liveLogPosition = aeron.countersReader().getCounterValue(liveLogRecCounterId);

            if (liveLogPositionCounter.proposeMaxOrdered(liveLogPosition))
            {
                if (null != eventsListener)
                {
                    eventsListener.onLiveLogProgress(liveLogRecordingId, liveLogRecCounterId, liveLogPosition);
                }

                workCount += 1;
            }
        }

        return workCount;
    }

    private void state(final ClusterBackup.State newState, final long nowMs)
    {
        stateChange(state, newState, nowMs);

        if (BACKUP_QUERY == newState && null != eventsListener)
        {
            eventsListener.onBackupQuery();
        }

        stateCounter.setOrdered(newState.code());
        state = newState;
    }

    @SuppressWarnings("unused")
    private void stateChange(final ClusterBackup.State oldState, final ClusterBackup.State newState, final long nowMs)
    {
        //System.out.println(nowMs + ": " + oldState + " -> " + newState);
    }

    private static boolean pollForResponse(final AeronArchive archive, final long correlationId)
    {
        final ControlResponsePoller poller = archive.controlResponsePoller();

        if (poller.poll() > 0 && poller.isPollComplete())
        {
            if (poller.controlSessionId() == archive.controlSessionId() && poller.correlationId() == correlationId)
            {
                if (poller.code() == ControlResponseCode.ERROR)
                {
                    throw new ClusterException(
                        "archive response for correlationId=" + correlationId + ", error: " + poller.errorMessage());
                }

                return true;
            }
        }

        return false;
    }

    private boolean hasProgressStalled(final long nowMs)
    {
        return (NULL_COUNTER_ID == liveLogRecCounterId) && (nowMs > (timeOfLastProgressMs + backupProgressTimeoutMs));
    }

    static class SnapshotRetrieveMonitor implements ControlEventListener, RecordingSignalConsumer
    {
        private final long expectedStopPosition;
        private final RecordingSignalAdapter recordingSignalAdapter;

        private long recordingId = RecordingPos.NULL_RECORDING_ID;
        private boolean isDone = false;
        private String errorMessage;

        SnapshotRetrieveMonitor(final AeronArchive clusterArchive, final long expectedStopPosition)
        {
            this.expectedStopPosition = expectedStopPosition;

            final Subscription subscription = clusterArchive.controlResponsePoller().subscription();
            final long controlSessionId = clusterArchive.controlSessionId();
            recordingSignalAdapter = new RecordingSignalAdapter(
                controlSessionId, this, this, subscription, FRAGMENT_POLL_LIMIT);
        }

        boolean isDone()
        {
            return isDone;
        }

        public int poll()
        {
            final int poll = recordingSignalAdapter.poll();

            if (null != errorMessage)
            {
                throw new AeronException("error occurred while transferring snapshot: " + errorMessage);
            }

            return poll;
        }

        public void onResponse(
            final long controlSessionId,
            final long correlationId,
            final long relevantId,
            final ControlResponseCode code,
            final String errorMessage)
        {
            if (code == ControlResponseCode.ERROR)
            {
                this.errorMessage = errorMessage;
            }
        }

        public void onSignal(
            final long controlSessionId,
            final long correlationId,
            final long recordingId,
            final long subscriptionId,
            final long position,
            final RecordingSignal signal)
        {
            if (signal == RecordingSignal.START && this.recordingId == RecordingPos.NULL_RECORDING_ID)
            {
                if (0 != position)
                {
                    errorMessage = "unexpected start position expected = 0, actual = " + position;
                }
                else
                {
                    this.recordingId = recordingId;
                }
            }
            else if (signal == RecordingSignal.STOP && this.recordingId == recordingId)
            {
                if (expectedStopPosition == position)
                {
                    isDone = true;
                }
                else
                {
                    errorMessage = "unexpected stop position expected = " + expectedStopPosition +
                        ", actual = " + position;
                }
            }
        }
    }
}
