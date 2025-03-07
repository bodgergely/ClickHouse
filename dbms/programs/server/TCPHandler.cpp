#include <iomanip>
#include <ext/scope_guard.h>
#include <Poco/Net/NetException.h>
#include <Common/ClickHouseRevision.h>
#include <Common/CurrentThread.h>
#include <Common/Stopwatch.h>
#include <Common/NetException.h>
#include <Common/setThreadName.h>
#include <Common/config_version.h>
#include <IO/Progress.h>
#include <Compression/CompressedReadBuffer.h>
#include <Compression/CompressedWriteBuffer.h>
#include <IO/ReadBufferFromPocoSocket.h>
#include <IO/WriteBufferFromPocoSocket.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <IO/copyData.h>
#include <DataStreams/AsynchronousBlockInputStream.h>
#include <DataStreams/NativeBlockInputStream.h>
#include <DataStreams/NativeBlockOutputStream.h>
#include <Interpreters/executeQuery.h>
#include <Interpreters/Quota.h>
#include <Interpreters/TablesStatus.h>
#include <Interpreters/InternalTextLogsQueue.h>
#include <Storages/StorageMemory.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Core/ExternalTable.h>
#include <Storages/ColumnDefault.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Compression/CompressionFactory.h>
#include <common/logger_useful.h>

#include <Processors/Formats/LazyOutputFormat.h>

#include "TCPHandler.h"


namespace DB
{

namespace ErrorCodes
{
    extern const int CLIENT_HAS_CONNECTED_TO_WRONG_PORT;
    extern const int UNKNOWN_DATABASE;
    extern const int UNKNOWN_EXCEPTION;
    extern const int UNKNOWN_PACKET_FROM_CLIENT;
    extern const int POCO_EXCEPTION;
    extern const int STD_EXCEPTION;
    extern const int SOCKET_TIMEOUT;
    extern const int UNEXPECTED_PACKET_FROM_CLIENT;
}


void TCPHandler::runImpl()
{
    setThreadName("TCPHandler");
    ThreadStatus thread_status;

    connection_context = server.context();
    connection_context.makeSessionContext();

    /// These timeouts can be changed after receiving query.

    auto global_receive_timeout = connection_context.getSettingsRef().receive_timeout;
    auto global_send_timeout = connection_context.getSettingsRef().send_timeout;

    socket().setReceiveTimeout(global_receive_timeout);
    socket().setSendTimeout(global_send_timeout);
    socket().setNoDelay(true);

    in = std::make_shared<ReadBufferFromPocoSocket>(socket());
    out = std::make_shared<WriteBufferFromPocoSocket>(socket());

    if (in->eof())
    {
        LOG_WARNING(log, "Client has not sent any data.");
        return;
    }

    /// User will be authenticated here. It will also set settings from user profile into connection_context.
    try
    {
        receiveHello();
    }
    catch (const Exception & e) /// Typical for an incorrect username, password, or address.
    {
        if (e.code() == ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT)
        {
            LOG_DEBUG(log, "Client has connected to wrong port.");
            return;
        }

        if (e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF)
        {
            LOG_WARNING(log, "Client has gone away.");
            return;
        }

        try
        {
            /// We try to send error information to the client.
            sendException(e, connection_context.getSettingsRef().calculate_text_stack_trace);
        }
        catch (...) {}

        throw;
    }

    /// When connecting, the default database can be specified.
    if (!default_database.empty())
    {
        if (!connection_context.isDatabaseExist(default_database))
        {
            Exception e("Database " + default_database + " doesn't exist", ErrorCodes::UNKNOWN_DATABASE);
            LOG_ERROR(log, "Code: " << e.code() << ", e.displayText() = " << e.displayText()
                << ", Stack trace:\n\n" << e.getStackTrace().toString());
            sendException(e, connection_context.getSettingsRef().calculate_text_stack_trace);
            return;
        }

        connection_context.setCurrentDatabase(default_database);
    }

    Settings connection_settings = connection_context.getSettings();

    sendHello();

    connection_context.setProgressCallback([this] (const Progress & value) { return this->updateProgress(value); });

    while (1)
    {
        /// We are waiting for a packet from the client. Thus, every `poll_interval` seconds check whether we need to shut down.
        {
            Stopwatch idle_time;
            while (!server.isCancelled() && !static_cast<ReadBufferFromPocoSocket &>(*in).poll(
                std::min(connection_settings.poll_interval, connection_settings.idle_connection_timeout) * 1000000))
            {
                if (idle_time.elapsedSeconds() > connection_settings.idle_connection_timeout)
                {
                    LOG_TRACE(log, "Closing idle connection");
                    return;
                }
            }
        }

        /// If we need to shut down, or client disconnects.
        if (server.isCancelled() || in->eof())
            break;

        /// Set context of request.
        query_context = connection_context;

        Stopwatch watch;
        state.reset();

        /// Initialized later.
        std::optional<CurrentThread::QueryScope> query_scope;

        /** An exception during the execution of request (it must be sent over the network to the client).
         *  The client will be able to accept it, if it did not happen while sending another packet and the client has not disconnected yet.
         */
        std::unique_ptr<Exception> exception;
        bool network_error = false;

        bool send_exception_with_stack_trace = connection_context.getSettingsRef().calculate_text_stack_trace;

        try
        {
            /// If a user passed query-local timeouts, reset socket to initial state at the end of the query
            SCOPE_EXIT({state.timeout_setter.reset();});

            /** If Query - process it. If Ping or Cancel - go back to the beginning.
             *  There may come settings for a separate query that modify `query_context`.
             */
            if (!receivePacket())
                continue;

            query_scope.emplace(*query_context);

            send_exception_with_stack_trace = query_context->getSettingsRef().calculate_text_stack_trace;

            /// Should we send internal logs to client?
            const auto client_logs_level = query_context->getSettingsRef().send_logs_level;
            if (client_revision >= DBMS_MIN_REVISION_WITH_SERVER_LOGS
                && client_logs_level != LogsLevel::none)
            {
                state.logs_queue = std::make_shared<InternalTextLogsQueue>();
                state.logs_queue->max_priority = Poco::Logger::parseLevel(client_logs_level.toString());
                CurrentThread::attachInternalTextLogsQueue(state.logs_queue, client_logs_level);
            }

            query_context->setExternalTablesInitializer([&connection_settings, this] (Context & context)
            {
                if (&context != &*query_context)
                    throw Exception("Unexpected context in external tables initializer", ErrorCodes::LOGICAL_ERROR);

                /// Get blocks of temporary tables
                readData(connection_settings);

                /// Reset the input stream, as we received an empty block while receiving external table data.
                /// So, the stream has been marked as cancelled and we can't read from it anymore.
                state.block_in.reset();
                state.maybe_compressed_in.reset(); /// For more accurate accounting by MemoryTracker.
            });

            customizeContext(*query_context);

            bool may_have_embedded_data = client_revision >= DBMS_MIN_REVISION_WITH_CLIENT_SUPPORT_EMBEDDED_DATA;
            /// Processing Query
            state.io = executeQuery(state.query, *query_context, false, state.stage, may_have_embedded_data);

            if (state.io.out)
                state.need_receive_data_for_insert = true;

            after_check_cancelled.restart();
            after_send_progress.restart();

            /// Does the request require receive data from client?
            if (state.need_receive_data_for_insert)
                processInsertQuery(connection_settings);
            else if (state.io.pipeline.initialized())
                processOrdinaryQueryWithProcessors(query_context->getSettingsRef().max_threads);
            else
                processOrdinaryQuery();

            /// Do it before sending end of stream, to have a chance to show log message in client.
            query_scope->logPeakMemoryUsage();

            sendLogs();
            sendEndOfStream();

            query_scope.reset();
            state.reset();
        }
        catch (const Exception & e)
        {
            state.io.onException();
            exception.reset(e.clone());

            if (e.code() == ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT)
                throw;

            /// If a timeout occurred, try to inform client about it and close the session
            if (e.code() == ErrorCodes::SOCKET_TIMEOUT)
                network_error = true;
        }
        catch (const Poco::Net::NetException & e)
        {
            /** We can get here if there was an error during connection to the client,
             *  or in connection with a remote server that was used to process the request.
             *  It is not possible to distinguish between these two cases.
             *  Although in one of them, we have to send exception to the client, but in the other - we can not.
             *  We will try to send exception to the client in any case - see below.
             */
            state.io.onException();
            exception = std::make_unique<Exception>(e.displayText(), ErrorCodes::POCO_EXCEPTION);
        }
        catch (const Poco::Exception & e)
        {
            state.io.onException();
            exception = std::make_unique<Exception>(e.displayText(), ErrorCodes::POCO_EXCEPTION);
        }
        catch (const std::exception & e)
        {
            state.io.onException();
            exception = std::make_unique<Exception>(e.what(), ErrorCodes::STD_EXCEPTION);
        }
        catch (...)
        {
            state.io.onException();
            exception = std::make_unique<Exception>("Unknown exception", ErrorCodes::UNKNOWN_EXCEPTION);
        }

        try
        {
            if (exception)
            {
                try
                {
                    /// Try to send logs to client, but it could be risky too
                    /// Assume that we can't break output here
                    sendLogs();
                }
                catch (...)
                {
                    tryLogCurrentException(log, "Can't send logs to client");
                }

                sendException(*exception, send_exception_with_stack_trace);
            }
        }
        catch (...)
        {
            /** Could not send exception information to the client. */
            network_error = true;
            LOG_WARNING(log, "Client has gone away.");
        }

        try
        {
            query_scope.reset();
            state.reset();
        }
        catch (...)
        {
            /** During the processing of request, there was an exception that we caught and possibly sent to client.
             *  When destroying the request pipeline execution there was a second exception.
             *  For example, a pipeline could run in multiple threads, and an exception could occur in each of them.
             *  Ignore it.
             */
        }

        watch.stop();

        LOG_INFO(log, std::fixed << std::setprecision(3)
            << "Processed in " << watch.elapsedSeconds() << " sec.");

        /// It is important to destroy query context here. We do not want it to live arbitrarily longer than the query.
        query_context.reset();

        if (network_error)
            break;
    }
}


void TCPHandler::readData(const Settings & connection_settings)
{
    const auto receive_timeout = query_context->getSettingsRef().receive_timeout.value;

    /// Poll interval should not be greater than receive_timeout
    const size_t default_poll_interval = connection_settings.poll_interval * 1000000;
    size_t current_poll_interval = static_cast<size_t>(receive_timeout.totalMicroseconds());
    constexpr size_t min_poll_interval = 5000; // 5 ms
    size_t poll_interval = std::max(min_poll_interval, std::min(default_poll_interval, current_poll_interval));

    sendLogs();

    while (true)
    {
        Stopwatch watch(CLOCK_MONOTONIC_COARSE);

        /// We are waiting for a packet from the client. Thus, every `POLL_INTERVAL` seconds check whether we need to shut down.
        while (true)
        {
            if (static_cast<ReadBufferFromPocoSocket &>(*in).poll(poll_interval))
                break;

            /// Do we need to shut down?
            if (server.isCancelled())
                return;

            /** Have we waited for data for too long?
             *  If we periodically poll, the receive_timeout of the socket itself does not work.
             *  Therefore, an additional check is added.
             */
            double elapsed = watch.elapsedSeconds();
            if (elapsed > receive_timeout.totalSeconds())
            {
                std::stringstream ss;
                ss << "Timeout exceeded while receiving data from client.";
                ss << " Waited for " << static_cast<size_t>(elapsed) << " seconds,";
                ss << " timeout is " << receive_timeout.totalSeconds() << " seconds.";

                throw Exception(ss.str(), ErrorCodes::SOCKET_TIMEOUT);
            }
        }

        /// If client disconnected.
        if (in->eof())
            return;

        /// We accept and process data. And if they are over, then we leave.
        if (!receivePacket())
            break;

        sendLogs();
    }
}


void TCPHandler::processInsertQuery(const Settings & connection_settings)
{
    /** Made above the rest of the lines, so that in case of `writePrefix` function throws an exception,
      *  client receive exception before sending data.
      */
    state.io.out->writePrefix();

    /// Send ColumnsDescription for insertion table
    if (client_revision >= DBMS_MIN_REVISION_WITH_COLUMN_DEFAULTS_METADATA)
    {
        const auto & db_and_table = query_context->getInsertionTable();
        if (query_context->getSettingsRef().input_format_defaults_for_omitted_fields)
        {
            if (!db_and_table.second.empty())
                sendTableColumns(query_context->getTable(db_and_table.first, db_and_table.second)->getColumns());
        }
    }

    /// Send block to the client - table structure.
    sendData(state.io.out->getHeader());

    readData(connection_settings);
    state.io.out->writeSuffix();
    state.io.onFinish();
}


void TCPHandler::processOrdinaryQuery()
{
    /// Pull query execution result, if exists, and send it to network.
    if (state.io.in)
    {
        /// Send header-block, to allow client to prepare output format for data to send.
        {
            Block header = state.io.in->getHeader();

            if (header)
                sendData(header);
        }

        AsynchronousBlockInputStream async_in(state.io.in);
        async_in.readPrefix();

        while (true)
        {
            Block block;

            while (true)
            {
                if (isQueryCancelled())
                {
                    /// A packet was received requesting to stop execution of the request.
                    async_in.cancel(false);
                    break;
                }
                else
                {
                    if (after_send_progress.elapsed() / 1000 >= query_context->getSettingsRef().interactive_delay)
                    {
                        /// Some time passed and there is a progress.
                        after_send_progress.restart();
                        sendProgress();
                    }

                    sendLogs();

                    if (async_in.poll(query_context->getSettingsRef().interactive_delay / 1000))
                    {
                        /// There is the following result block.
                        block = async_in.read();
                        break;
                    }
                }
            }

            /** If data has run out, we will send the profiling data and total values to
              * the last zero block to be able to use
              * this information in the suffix output of stream.
              * If the request was interrupted, then `sendTotals` and other methods could not be called,
              *  because we have not read all the data yet,
              *  and there could be ongoing calculations in other threads at the same time.
              */
            if (!block && !isQueryCancelled())
            {
                sendTotals(state.io.in->getTotals());
                sendExtremes(state.io.in->getExtremes());
                sendProfileInfo(state.io.in->getProfileInfo());
                sendProgress();
                sendLogs();
            }

            sendData(block);
            if (!block)
                break;
        }

        async_in.readSuffix();
    }

    state.io.onFinish();
}

void TCPHandler::processOrdinaryQueryWithProcessors(size_t num_threads)
{
    auto & pipeline = state.io.pipeline;

    /// Send header-block, to allow client to prepare output format for data to send.
    {
        auto & header = pipeline.getHeader();

        if (header)
            sendData(header);
    }

    auto lazy_format = std::make_shared<LazyOutputFormat>(pipeline.getHeader());
    pipeline.setOutput(lazy_format);

    {
        auto thread_group = CurrentThread::getGroup();
        ThreadPool pool(1);
        auto executor = pipeline.execute();
        std::atomic_bool exception = false;

        pool.schedule([&]()
        {
            /// ThreadStatus thread_status;

            if (thread_group)
                CurrentThread::attachTo(thread_group);

            SCOPE_EXIT(
                    if (thread_group)
                        CurrentThread::detachQueryIfNotDetached();
            );

            CurrentMetrics::Increment query_thread_metric_increment{CurrentMetrics::QueryThread};
            setThreadName("QueryPipelineEx");

            try
            {
                executor->execute(num_threads);
            }
            catch (...)
            {
                exception = true;
                throw;
            }
        });

        /// Wait in case of exception. Delete pipeline to release memory.
        SCOPE_EXIT(
                /// Clear queue in case if somebody is waiting lazy_format to push.
                lazy_format->finish();
                lazy_format->clearQueue();

                pool.wait();
                pipeline = QueryPipeline()
        );

        while (true)
        {
            Block block;

            while (true)
            {
                if (isQueryCancelled())
                {
                    /// A packet was received requesting to stop execution of the request.
                    executor->cancel();

                    break;
                }
                else
                {
                    if (after_send_progress.elapsed() / 1000 >= query_context->getSettingsRef().interactive_delay)
                    {
                        /// Some time passed and there is a progress.
                        after_send_progress.restart();
                        sendProgress();
                    }

                    sendLogs();

                    if ((block = lazy_format->getBlock(query_context->getSettingsRef().interactive_delay / 1000)))
                        break;

                    if (lazy_format->isFinished())
                        break;

                    if (exception)
                    {
                        pool.wait();
                        break;
                    }
                }
            }

            /** If data has run out, we will send the profiling data and total values to
              * the last zero block to be able to use
              * this information in the suffix output of stream.
              * If the request was interrupted, then `sendTotals` and other methods could not be called,
              *  because we have not read all the data yet,
              *  and there could be ongoing calculations in other threads at the same time.
              */
            if (!block && !isQueryCancelled())
            {
                pool.wait();
                pipeline.finalize();

                sendTotals(lazy_format->getTotals());
                sendExtremes(lazy_format->getExtremes());
                sendProfileInfo(lazy_format->getProfileInfo());
                sendProgress();
                sendLogs();
            }

            sendData(block);
            if (!block)
                break;
        }
    }

    state.io.onFinish();
}


void TCPHandler::processTablesStatusRequest()
{
    TablesStatusRequest request;
    request.read(*in, client_revision);

    TablesStatusResponse response;
    for (const QualifiedTableName & table_name: request.tables)
    {
        StoragePtr table = connection_context.tryGetTable(table_name.database, table_name.table);
        if (!table)
            continue;

        TableStatus status;
        if (auto * replicated_table = dynamic_cast<StorageReplicatedMergeTree *>(table.get()))
        {
            status.is_replicated = true;
            status.absolute_delay = replicated_table->getAbsoluteDelay();
        }
        else
            status.is_replicated = false;

        response.table_states_by_id.emplace(table_name, std::move(status));
    }

    writeVarUInt(Protocol::Server::TablesStatusResponse, *out);
    response.write(*out, client_revision);
}

void TCPHandler::receiveUnexpectedTablesStatusRequest()
{
    TablesStatusRequest skip_request;
    skip_request.read(*in, client_revision);

    throw NetException("Unexpected packet TablesStatusRequest received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
}

void TCPHandler::sendProfileInfo(const BlockStreamProfileInfo & info)
{
    writeVarUInt(Protocol::Server::ProfileInfo, *out);
    info.write(*out);
    out->next();
}


void TCPHandler::sendTotals(const Block & totals)
{
    if (totals)
    {
        initBlockOutput(totals);

        writeVarUInt(Protocol::Server::Totals, *out);
        writeStringBinary("", *out);

        state.block_out->write(totals);
        state.maybe_compressed_out->next();
        out->next();
    }
}


void TCPHandler::sendExtremes(const Block & extremes)
{
    if (extremes)
    {
        initBlockOutput(extremes);

        writeVarUInt(Protocol::Server::Extremes, *out);
        writeStringBinary("", *out);

        state.block_out->write(extremes);
        state.maybe_compressed_out->next();
        out->next();
    }
}


void TCPHandler::receiveHello()
{
    /// Receive `hello` packet.
    UInt64 packet_type = 0;
    String user = "default";
    String password;

    readVarUInt(packet_type, *in);
    if (packet_type != Protocol::Client::Hello)
    {
        /** If you accidentally accessed the HTTP protocol for a port destined for an internal TCP protocol,
          * Then instead of the packet type, there will be G (GET) or P (POST), in most cases.
          */
        if (packet_type == 'G' || packet_type == 'P')
        {
            writeString("HTTP/1.0 400 Bad Request\r\n\r\n"
                "Port " + server.config().getString("tcp_port") + " is for clickhouse-client program.\r\n"
                "You must use port " + server.config().getString("http_port") + " for HTTP.\r\n",
                *out);

            throw Exception("Client has connected to wrong port", ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT);
        }
        else
            throw NetException("Unexpected packet from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
    }

    readStringBinary(client_name, *in);
    readVarUInt(client_version_major, *in);
    readVarUInt(client_version_minor, *in);
    // NOTE For backward compatibility of the protocol, client cannot send its version_patch.
    readVarUInt(client_revision, *in);
    readStringBinary(default_database, *in);
    readStringBinary(user, *in);
    readStringBinary(password, *in);

    LOG_DEBUG(log, "Connected " << client_name
        << " version " << client_version_major
        << "." << client_version_minor
        << "." << client_version_patch
        << ", revision: " << client_revision
        << (!default_database.empty() ? ", database: " + default_database : "")
        << (!user.empty() ? ", user: " + user : "")
        << ".");

    connection_context.setUser(user, password, socket().peerAddress(), "");
}


void TCPHandler::receiveUnexpectedHello()
{
    UInt64 skip_uint_64;
    String skip_string;

    readStringBinary(skip_string, *in);
    readVarUInt(skip_uint_64, *in);
    readVarUInt(skip_uint_64, *in);
    readVarUInt(skip_uint_64, *in);
    readStringBinary(skip_string, *in);
    readStringBinary(skip_string, *in);
    readStringBinary(skip_string, *in);

    throw NetException("Unexpected packet Hello received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
}


void TCPHandler::sendHello()
{
    writeVarUInt(Protocol::Server::Hello, *out);
    writeStringBinary(DBMS_NAME, *out);
    writeVarUInt(DBMS_VERSION_MAJOR, *out);
    writeVarUInt(DBMS_VERSION_MINOR, *out);
    writeVarUInt(ClickHouseRevision::get(), *out);
    if (client_revision >= DBMS_MIN_REVISION_WITH_SERVER_TIMEZONE)
        writeStringBinary(DateLUT::instance().getTimeZone(), *out);
    if (client_revision >= DBMS_MIN_REVISION_WITH_SERVER_DISPLAY_NAME)
        writeStringBinary(server_display_name, *out);
    if (client_revision >= DBMS_MIN_REVISION_WITH_VERSION_PATCH)
        writeVarUInt(DBMS_VERSION_PATCH, *out);
    out->next();
}


bool TCPHandler::receivePacket()
{
    UInt64 packet_type = 0;
    readVarUInt(packet_type, *in);

//    std::cerr << "Server got packet: " << Protocol::Client::toString(packet_type) << "\n";

    switch (packet_type)
    {
        case Protocol::Client::Query:
            if (!state.empty())
                receiveUnexpectedQuery();
            receiveQuery();
            return true;

        case Protocol::Client::Data:
            if (state.empty())
                receiveUnexpectedData();
            return receiveData();

        case Protocol::Client::Ping:
            writeVarUInt(Protocol::Server::Pong, *out);
            out->next();
            return false;

        case Protocol::Client::Cancel:
            return false;

        case Protocol::Client::Hello:
            receiveUnexpectedHello();

        case Protocol::Client::TablesStatusRequest:
            if (!state.empty())
                receiveUnexpectedTablesStatusRequest();
            processTablesStatusRequest();
            out->next();
            return false;

        default:
            throw Exception("Unknown packet " + toString(packet_type) + " from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
    }
}


void TCPHandler::receiveQuery()
{
    UInt64 stage = 0;
    UInt64 compression = 0;

    state.is_empty = false;
    readStringBinary(state.query_id, *in);

    query_context->setCurrentQueryId(state.query_id);

    /// Client info
    {
        ClientInfo & client_info = query_context->getClientInfo();
        if (client_revision >= DBMS_MIN_REVISION_WITH_CLIENT_INFO)
            client_info.read(*in, client_revision);

        /// For better support of old clients, that does not send ClientInfo.
        if (client_info.query_kind == ClientInfo::QueryKind::NO_QUERY)
        {
            client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
            client_info.client_name = client_name;
            client_info.client_version_major = client_version_major;
            client_info.client_version_minor = client_version_minor;
            client_info.client_version_patch = client_version_patch;
            client_info.client_revision = client_revision;
        }

        /// Set fields, that are known apriori.
        client_info.interface = ClientInfo::Interface::TCP;

        if (client_info.query_kind == ClientInfo::QueryKind::INITIAL_QUERY)
        {
            /// 'Current' fields was set at receiveHello.
            client_info.initial_user = client_info.current_user;
            client_info.initial_query_id = client_info.current_query_id;
            client_info.initial_address = client_info.current_address;
        }
    }

    /// Per query settings.
    Settings & settings = query_context->getSettingsRef();
    settings.deserialize(*in);

    /// Sync timeouts on client and server during current query to avoid dangling queries on server
    /// NOTE: We use settings.send_timeout for the receive timeout and vice versa (change arguments ordering in TimeoutSetter),
    ///  because settings.send_timeout is client-side setting which has opposite meaning on the server side.
    /// NOTE: these settings are applied only for current connection (not for distributed tables' connections)
    state.timeout_setter = std::make_unique<TimeoutSetter>(socket(), settings.receive_timeout, settings.send_timeout);

    readVarUInt(stage, *in);
    state.stage = QueryProcessingStage::Enum(stage);

    readVarUInt(compression, *in);
    state.compression = static_cast<Protocol::Compression>(compression);

    readStringBinary(state.query, *in);
}

void TCPHandler::receiveUnexpectedQuery()
{
    UInt64 skip_uint_64;
    String skip_string;

    readStringBinary(skip_string, *in);

    ClientInfo & skip_client_info = query_context->getClientInfo();
    if (client_revision >= DBMS_MIN_REVISION_WITH_CLIENT_INFO)
        skip_client_info.read(*in, client_revision);

    Settings & skip_settings = query_context->getSettingsRef();
    skip_settings.deserialize(*in);

    readVarUInt(skip_uint_64, *in);
    readVarUInt(skip_uint_64, *in);
    readStringBinary(skip_string, *in);

    throw NetException("Unexpected packet Query received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
}

bool TCPHandler::receiveData()
{
    initBlockInput();

    /// The name of the temporary table for writing data, default to empty string
    String external_table_name;
    readStringBinary(external_table_name, *in);

    /// Read one block from the network and write it down
    Block block = state.block_in->read();

    if (block)
    {
        /// If there is an insert request, then the data should be written directly to `state.io.out`.
        /// Otherwise, we write the blocks in the temporary `external_table_name` table.
        if (!state.need_receive_data_for_insert)
        {
            StoragePtr storage;
            /// If such a table does not exist, create it.
            if (!(storage = query_context->tryGetExternalTable(external_table_name)))
            {
                NamesAndTypesList columns = block.getNamesAndTypesList();
                storage = StorageMemory::create("_external", external_table_name, ColumnsDescription{columns}, ConstraintsDescription{});
                storage->startup();
                query_context->addExternalTable(external_table_name, storage);
            }
            /// The data will be written directly to the table.
            state.io.out = storage->write(ASTPtr(), *query_context);
        }
        if (block)
            state.io.out->write(block);
        return true;
    }
    else
        return false;
}

void TCPHandler::receiveUnexpectedData()
{
    String skip_external_table_name;
    readStringBinary(skip_external_table_name, *in);

    std::shared_ptr<ReadBuffer> maybe_compressed_in;

    if (last_block_in.compression == Protocol::Compression::Enable)
        maybe_compressed_in = std::make_shared<CompressedReadBuffer>(*in);
    else
        maybe_compressed_in = in;

    auto skip_block_in = std::make_shared<NativeBlockInputStream>(
            *maybe_compressed_in,
            last_block_in.header,
            client_revision,
            !connection_context.getSettingsRef().low_cardinality_allow_in_native_format);

    Block skip_block = skip_block_in->read();
    throw NetException("Unexpected packet Data received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
}

void TCPHandler::initBlockInput()
{
    if (!state.block_in)
    {
        if (state.compression == Protocol::Compression::Enable)
            state.maybe_compressed_in = std::make_shared<CompressedReadBuffer>(*in);
        else
            state.maybe_compressed_in = in;

        Block header;
        if (state.io.out)
            header = state.io.out->getHeader();

        last_block_in.header = header;
        last_block_in.compression = state.compression;

        state.block_in = std::make_shared<NativeBlockInputStream>(
            *state.maybe_compressed_in,
            header,
            client_revision,
            !connection_context.getSettingsRef().low_cardinality_allow_in_native_format);
    }
}


void TCPHandler::initBlockOutput(const Block & block)
{
    if (!state.block_out)
    {
        if (!state.maybe_compressed_out)
        {
            std::string method = Poco::toUpper(query_context->getSettingsRef().network_compression_method.toString());
            std::optional<int> level;
            if (method == "ZSTD")
                level = query_context->getSettingsRef().network_zstd_compression_level;

            if (state.compression == Protocol::Compression::Enable)
                state.maybe_compressed_out = std::make_shared<CompressedWriteBuffer>(
                    *out, CompressionCodecFactory::instance().get(method, level));
            else
                state.maybe_compressed_out = out;
        }

        state.block_out = std::make_shared<NativeBlockOutputStream>(
            *state.maybe_compressed_out,
            client_revision,
            block.cloneEmpty(),
            !connection_context.getSettingsRef().low_cardinality_allow_in_native_format);
    }
}

void TCPHandler::initLogsBlockOutput(const Block & block)
{
    if (!state.logs_block_out)
    {
        /// Use uncompressed stream since log blocks usually contain only one row
        state.logs_block_out = std::make_shared<NativeBlockOutputStream>(
            *out,
            client_revision,
            block.cloneEmpty(),
            !connection_context.getSettingsRef().low_cardinality_allow_in_native_format);
    }
}


bool TCPHandler::isQueryCancelled()
{
    if (state.is_cancelled || state.sent_all_data)
        return true;

    if (after_check_cancelled.elapsed() / 1000 < query_context->getSettingsRef().interactive_delay)
        return false;

    after_check_cancelled.restart();

    /// During request execution the only packet that can come from the client is stopping the query.
    if (static_cast<ReadBufferFromPocoSocket &>(*in).poll(0))
    {
        UInt64 packet_type = 0;
        readVarUInt(packet_type, *in);

        switch (packet_type)
        {
            case Protocol::Client::Cancel:
                if (state.empty())
                    throw NetException("Unexpected packet Cancel received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
                LOG_INFO(log, "Query was cancelled.");
                state.is_cancelled = true;
                return true;

            default:
                throw NetException("Unknown packet from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
        }
    }

    return false;
}


void TCPHandler::sendData(const Block & block)
{
    initBlockOutput(block);

    writeVarUInt(Protocol::Server::Data, *out);
    /// Send external table name (empty name is the main table)
    writeStringBinary("", *out);

    state.block_out->write(block);
    state.maybe_compressed_out->next();
    out->next();
}


void TCPHandler::sendLogData(const Block & block)
{
    initLogsBlockOutput(block);

    writeVarUInt(Protocol::Server::Log, *out);
    /// Send log tag (empty tag is the default tag)
    writeStringBinary("", *out);

    state.logs_block_out->write(block);
    out->next();
}

void TCPHandler::sendTableColumns(const ColumnsDescription & columns)
{
    writeVarUInt(Protocol::Server::TableColumns, *out);

    /// Send external table name (empty name is the main table)
    writeStringBinary("", *out);
    writeStringBinary(columns.toString(), *out);

    out->next();
}

void TCPHandler::sendException(const Exception & e, bool with_stack_trace)
{
    writeVarUInt(Protocol::Server::Exception, *out);
    writeException(e, *out, with_stack_trace);
    out->next();
}


void TCPHandler::sendEndOfStream()
{
    state.sent_all_data = true;
    writeVarUInt(Protocol::Server::EndOfStream, *out);
    out->next();
}


void TCPHandler::updateProgress(const Progress & value)
{
    state.progress.incrementPiecewiseAtomically(value);
}


void TCPHandler::sendProgress()
{
    writeVarUInt(Protocol::Server::Progress, *out);
    auto increment = state.progress.fetchAndResetPiecewiseAtomically();
    increment.write(*out, client_revision);
    out->next();
}


void TCPHandler::sendLogs()
{
    if (!state.logs_queue)
        return;

    MutableColumns logs_columns;
    MutableColumns curr_logs_columns;
    size_t rows = 0;

    for (; state.logs_queue->tryPop(curr_logs_columns); ++rows)
    {
        if (rows == 0)
        {
            logs_columns = std::move(curr_logs_columns);
        }
        else
        {
            for (size_t j = 0; j < logs_columns.size(); ++j)
                logs_columns[j]->insertRangeFrom(*curr_logs_columns[j], 0, curr_logs_columns[j]->size());
        }
    }

    if (rows > 0)
    {
        Block block = InternalTextLogsQueue::getSampleBlock();
        block.setColumns(std::move(logs_columns));
        sendLogData(block);
    }
}


void TCPHandler::run()
{
    try
    {
        runImpl();

        LOG_INFO(log, "Done processing connection.");
    }
    catch (Poco::Exception & e)
    {
        /// Timeout - not an error.
        if (!strcmp(e.what(), "Timeout"))
        {
            LOG_DEBUG(log, "Poco::Exception. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
                << ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what());
        }
        else
            throw;
    }
}

}
