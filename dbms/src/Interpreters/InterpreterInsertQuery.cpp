#include <IO/ConcatReadBuffer.h>
#include <IO/ReadBufferFromMemory.h>

#include <Common/typeid_cast.h>
#include <Common/checkStackSize.h>

#include <DataStreams/AddingDefaultBlockOutputStream.h>
#include <DataStreams/AddingDefaultsBlockInputStream.h>
#include <DataStreams/CheckConstraintsBlockOutputStream.h>
#include <DataStreams/OwningBlockInputStream.h>
#include <DataStreams/ConvertingBlockInputStream.h>
#include <DataStreams/CountingBlockOutputStream.h>
#include <DataStreams/NullAndDoCopyBlockInputStream.h>
#include <DataStreams/PushingToViewsBlockOutputStream.h>
#include <DataStreams/SquashingBlockOutputStream.h>
#include <DataStreams/InputStreamFromASTInsertQuery.h>
#include <DataStreams/copyData.h>

#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>

#include <Interpreters/InterpreterInsertQuery.h>
#include <Interpreters/InterpreterSelectWithUnionQuery.h>

#include <TableFunctions/TableFunctionFactory.h>
#include <Parsers/ASTFunction.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int READONLY;
    extern const int ILLEGAL_COLUMN;
}


InterpreterInsertQuery::InterpreterInsertQuery(
    const ASTPtr & query_ptr_, const Context & context_, bool allow_materialized_, bool no_squash_)
    : query_ptr(query_ptr_), context(context_), allow_materialized(allow_materialized_), no_squash(no_squash_)
{
    checkStackSize();
}


StoragePtr InterpreterInsertQuery::getTable(const ASTInsertQuery & query)
{
    if (query.table_function)
    {
        const auto * table_function = query.table_function->as<ASTFunction>();
        const auto & factory = TableFunctionFactory::instance();
        TableFunctionPtr table_function_ptr = factory.get(table_function->name, context);
        return table_function_ptr->execute(query.table_function, context, table_function_ptr->getName());
    }

    /// Into what table to write.
    return context.getTable(query.database, query.table);
}

Block InterpreterInsertQuery::getSampleBlock(const ASTInsertQuery & query, const StoragePtr & table)
{
    Block table_sample_non_materialized = table->getSampleBlockNonMaterialized();
    /// If the query does not include information about columns
    if (!query.columns)
    {
        /// Format Native ignores header and write blocks as is.
        if (query.format == "Native")
            return {};
        else if (query.no_destination)
            return table->getSampleBlockWithVirtuals();
        else
            return table_sample_non_materialized;
    }

    Block table_sample = table->getSampleBlock();
    /// Form the block based on the column names from the query
    Block res;
    for (const auto & identifier : query.columns->children)
    {
        std::string current_name = identifier->getColumnName();

        /// The table does not have a column with that name
        if (!table_sample.has(current_name))
            throw Exception("No such column " + current_name + " in table " + query.table, ErrorCodes::NO_SUCH_COLUMN_IN_TABLE);

        if (!allow_materialized && !table_sample_non_materialized.has(current_name))
            throw Exception("Cannot insert column " + current_name + ", because it is MATERIALIZED column.", ErrorCodes::ILLEGAL_COLUMN);

        res.insert(ColumnWithTypeAndName(table_sample.getByName(current_name).type, current_name));
    }
    return res;
}


BlockIO InterpreterInsertQuery::execute()
{
    const auto & query = query_ptr->as<ASTInsertQuery &>();
    checkAccess(query);
    StoragePtr table = getTable(query);

    auto table_lock = table->lockStructureForShare(true, context.getInitialQueryId());

    BlockIO res;
    Block query_sample_block = getSampleBlock(query, table);

    /// NOTE:
    /// For the log family engine(this may be true for all engines, because clickhouse reads are always based on snapshot)
    /// the read always holds lock resource only in the Interpreter and releases it during data processing,
    /// but for the write, the lock resource is held until the query is completed.
    /// To avoid deadlocks, we should first create BockInputStream for INSERT INTO SELECT
    if (BlockInputStreamPtr source_input = tryCreateSourceInputStream(query, table, query_sample_block))
        res.in = std::make_shared<NullAndDoCopyBlockInputStream>(source_input, createOutputStream(query, table, query_sample_block));
    else
        res.out = createOutputStream(query, table, query_sample_block);

    return res;
}


void InterpreterInsertQuery::checkAccess(const ASTInsertQuery & query)
{
    const Settings & settings = context.getSettingsRef();
    auto readonly = settings.readonly;

    if (!readonly || (query.database.empty() && context.tryGetExternalTable(query.table) && readonly >= 2))
    {
        return;
    }

    throw Exception("Cannot insert into table in readonly mode", ErrorCodes::READONLY);
}

std::pair<String, String> InterpreterInsertQuery::getDatabaseTable() const
{
    const auto & query = query_ptr->as<ASTInsertQuery &>();
    return {query.database, query.table};
}

/*
<<<<<<< HEAD
    /// We create a pipeline of several streams, into which we will write data.
    BlockOutputStreamPtr out;

    out = std::make_shared<PushingToViewsBlockOutputStream>(query.database, query.table, table, context, query_ptr, query.no_destination);

    /// Do not squash blocks if it is a sync INSERT into Distributed, since it lead to double bufferization on client and server side.
    /// Client-side bufferization might cause excessive timeouts (especially in case of big blocks).
    if (!(context.getSettingsRef().insert_distributed_sync && table->isRemote()) && !no_squash)
    {
        out = std::make_shared<SquashingBlockOutputStream>(
            out, out->getHeader(), context.getSettingsRef().min_insert_block_size_rows, context.getSettingsRef().min_insert_block_size_bytes);
    }
    auto query_sample_block = getSampleBlock(query, table);

    /// Actually we don't know structure of input blocks from query/table,
    /// because some clients break insertion protocol (columns != header)
    out = std::make_shared<AddingDefaultBlockOutputStream>(
        out, query_sample_block, out->getHeader(), table->getColumns().getDefaults(), context);

    if (const auto & constraints = table->getConstraints(); !constraints.empty())
        out = std::make_shared<CheckConstraintsBlockOutputStream>(query.table,
            out, query_sample_block, table->getConstraints(), context);

    auto out_wrapper = std::make_shared<CountingBlockOutputStream>(out);
    out_wrapper->setProcessListElement(context.getProcessListElement());
    out = std::move(out_wrapper);

=======
>>>>>>> ff54f3a10b3d1d89c099cff3c3b5eb6ebcf27972
*/

BlockOutputStreamPtr InterpreterInsertQuery::createOutputStream(const ASTInsertQuery & query, const StoragePtr & table, const Block & sample_block)
{
    const Block & table_sample_block = table->getSampleBlock();
    const ColumnDefaults & table_default_columns = table->getColumns().getDefaults();

    /// We create a pipeline of several streams, into which we will write data.
    BlockOutputStreamPtr out = std::make_shared<PushingToViewsBlockOutputStream>(
        query.database, query.table, table, context, query_ptr, query.no_destination);

    /// Do not squash blocks if it is a sync INSERT into Distributed, since it lead to double bufferization on client and server side.
    /// Client-side bufferization might cause excessive timeouts (especially in case of big blocks).
    if (!(context.getSettingsRef().insert_distributed_sync && table->isRemote()) && !no_squash)
    {
        UInt64 min_insert_block_size_rows = context.getSettingsRef().min_insert_block_size_rows;
        UInt64 min_insert_block_size_bytes = context.getSettingsRef().min_insert_block_size_bytes;
        out = std::make_shared<SquashingBlockOutputStream>(out, table_sample_block, min_insert_block_size_rows, min_insert_block_size_bytes);
    }

    /// Actually we don't know structure of input blocks from query/table,
    /// because some clients break insertion protocol (columns != header)
    out = std::make_shared<AddingDefaultBlockOutputStream>(out, sample_block, table_sample_block, table_default_columns, context);

    if (const auto & constraints = table->getConstraints(); !constraints.empty())
        out = std::make_shared<CheckConstraintsBlockOutputStream>(query.table,
            out, sample_block, table->getConstraints(), context);

    return std::make_shared<CountingBlockOutputStream>(out, context.getProcessListElement());
}

BlockInputStreamPtr InterpreterInsertQuery::tryCreateSourceInputStream(
    const ASTInsertQuery & query, const StoragePtr & table, const Block & sample_block)
{
    BlockInputStreamPtr in;

    if (query.data && !query.has_tail) /// can execute without additional data
    {
        in = std::make_shared<InputStreamFromASTInsertQuery>(query_ptr, nullptr, sample_block, context);
    }
    else if (query.select)
    {
        /// Passing 1 as subquery_depth will disable limiting size of intermediate result.
        SelectQueryOptions select_query_options = SelectQueryOptions(QueryProcessingStage::Complete, 1);
        InterpreterSelectWithUnionQuery interpreter_select{query.select, context, select_query_options};

        in = interpreter_select.execute().in;
        in = std::make_shared<ConvertingBlockInputStream>(context, in, sample_block, ConvertingBlockInputStream::MatchColumnsMode::Position);

        if (!allow_materialized)
        {
            Block in_header = in->getHeader();
            for (const auto & column : table->getColumns())
                if (column.default_desc.kind == ColumnDefaultKind::Materialized && in_header.has(column.name))
                    throw Exception("Cannot insert column " + column.name + ", because it is MATERIALIZED column.",
                                    ErrorCodes::ILLEGAL_COLUMN);
        }
    }

    return in;
}

}
