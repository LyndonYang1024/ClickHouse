#include <Formats/ReadSchemaUtils.h>

#include <IO/ReadBufferFromString.h>

#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>

#include <Parsers/ASTLiteral.h>

#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Formats/IInputFormat.h>

#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/QueryPipelineBuilder.h>

#include <Storages/StorageValues.h>
#include <Storages/checkAndGetLiteralArgument.h>

#include <TableFunctions/TableFunctionFormat.h>
#include <TableFunctions/TableFunctionFactory.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

void TableFunctionFormat::parseArguments(const ASTPtr & ast_function, ContextPtr context)
{
    ASTs & args_func = ast_function->children;

    if (args_func.size() != 1)
        throw Exception("Table function '" + getName() + "' must have arguments", ErrorCodes::LOGICAL_ERROR);

    ASTs & args = args_func.at(0)->children;

    if (args.size() != 2)
        throw Exception("Table function '" + getName() + "' requires 2 arguments: format and data", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    for (auto & arg : args)
        arg = evaluateConstantExpressionOrIdentifierAsLiteral(arg, context);

    format = checkAndGetLiteralArgument<String>(args[0], "format");
    data = checkAndGetLiteralArgument<String>(args[1], "data");
}

ColumnsDescription TableFunctionFormat::getActualTableStructure(ContextPtr context) const
{
    ReadBufferIterator read_buffer_iterator = [&](ColumnsDescription &)
    {
        return std::make_unique<ReadBufferFromString>(data);
    };
    return readSchemaFromFormat(format, std::nullopt, read_buffer_iterator, false, context);
}

Block TableFunctionFormat::parseData(ColumnsDescription columns, ContextPtr context) const
{
    Block block;
    for (const auto & name_and_type : columns.getAllPhysical())
        block.insert({name_and_type.type->createColumn(), name_and_type.type, name_and_type.name});

    auto read_buf = std::make_unique<ReadBufferFromString>(data);
    auto input_format = context->getInputFormat(format, *read_buf, block, context->getSettingsRef().max_block_size);
    auto pipeline = std::make_unique<QueryPipeline>(input_format);
    auto reader = std::make_unique<PullingPipelineExecutor>(*pipeline);

    std::vector<Block> blocks;
    while (reader->pull(block))
        blocks.push_back(std::move(block));

    if (blocks.size() == 1)
        return blocks[0];

    /// In case when data contains more then 1 block we combine
    /// them all to one big block (this is considered a rare case).
    return concatenateBlocks(blocks);
}

StoragePtr TableFunctionFormat::executeImpl(const ASTPtr & /*ast_function*/, ContextPtr context, const std::string & table_name, ColumnsDescription /*cached_columns*/) const
{
    auto columns = getActualTableStructure(context);
    Block res_block = parseData(columns, context);
    auto res = std::make_shared<StorageValues>(StorageID(getDatabaseName(), table_name), columns, res_block);
    res->startup();
    return res;
}

static const Documentation format_table_function_documentation =
{
    R"(
Extracts table structure from data and parses it according to specified input format.
Syntax: `format(format_name, data)`.
Parameters:
    - `format_name` - the format of the data.
    - `data ` - String literal or constant expression that returns a string containing data in specified format.
Returned value: A table with data parsed from `data` argument according specified format and extracted schema.
)",
    Documentation::Examples
    {
        {
            "First example",
            R"(
Query:
```
:) select * from format(JSONEachRow,
$$
{"a": "Hello", "b": 111}
{"a": "World", "b": 123}
{"a": "Hello", "b": 112}
{"a": "World", "b": 124}
$$)
```

Result:
```
┌───b─┬─a─────┐
│ 111 │ Hello │
│ 123 │ World │
│ 112 │ Hello │
│ 124 │ World │
└─────┴───────┘
```
)"
        },
        {
            "Second example",
            R"(
Query:
```
:) desc format(JSONEachRow,
$$
{"a": "Hello", "b": 111}
{"a": "World", "b": 123}
{"a": "Hello", "b": 112}
{"a": "World", "b": 124}
$$)
```

Result:
```
┌─name─┬─type──────────────┬─default_type─┬─default_expression─┬─comment─┬─codec_expression─┬─ttl_expression─┐
│ b    │ Nullable(Float64) │              │                    │         │                  │                │
│ a    │ Nullable(String)  │              │                    │         │                  │                │
└──────┴───────────────────┴──────────────┴────────────────────┴─────────┴──────────────────┴────────────────┘
```
)"
        },
    },
    Documentation::Categories{"format", "table-functions"}
};

void registerTableFunctionFormat(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionFormat>({format_table_function_documentation, false}, TableFunctionFactory::CaseInsensitive);
}
}
